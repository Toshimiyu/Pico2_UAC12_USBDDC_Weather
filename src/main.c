/*
 * Copyright (c) 2026 Toshimiyu
 *
 * Released under the MIT license
 * https://opensource.org/licenses/mit-license.php
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"
#include "hardware/regs/intctrl.h"
#include "common.h"
#include "usb_device_control.h"
#include "cdc_diagnostics.h"
#include "transmit_to_dac.h"
#include "ringbuffer.h"
#include "display_rta.h"
#include "spectrum.h"
#include "debug_gauge.h"
#include "weather_rx.h"

#define IDLE_CLEANUP_TIMEOUT_US (500000u)

// ring buffer
RINGBUFFER buffer_audio;

// Audio State
AUDIO_STATE audio_state;
uint32_t now_playing = 0;
static uint32_t last_observed_now_playing = 0;
static uint32_t last_audio_activity_us = 0;
static bool idle_cleanup_initialized = false;
static bool is_cleared_buffer = false;

// Output start time.
volatile absolute_time_t time_start_output;

static void display_service(void);
static void service_idle_cleanup(void);

// TinyUSB must be polled during startup animation; keep servicing USB while waiting.
static void boot_wait_ms(uint32_t ms)
{
	absolute_time_t end = make_timeout_time_ms(ms);
	while (!time_reached(end))
	{
		usb_audio_task();
		weather_rx_task();
		cdc_diagnostics_task();
		service_idle_cleanup();
		display_service();
		sleep_us(1);
	}
}


// Store the audio format shown in the previous frame (added).
static uint32_t last_display_freq = 0;
static uint8_t last_display_bit_depth = 0;
extern volatile uint32_t audio_last_data_us;
extern volatile bool audio_data_seen;

// Core1 main function.
extern void core1_main();

// Check when to unmute.
extern volatile bool enable_output;

static void display_service(void)
{
	const display_button_action_t button_action = display_rta_process_buttons();
	const bool diagnostic_mode = display_rta_is_diagnostic_mode();
	switch (button_action)
	{
	case DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_3:
		debug_gauge_set_reset_countdown(3);
		break;
	case DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_2:
		debug_gauge_set_reset_countdown(2);
		break;
	case DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_1:
		debug_gauge_set_reset_countdown(1);
		break;
	case DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_CLEARED:
		debug_gauge_set_reset_countdown(0);
		break;
	case DISPLAY_BUTTON_ACTION_RESET_DIAGNOSTICS:
		debug_gauge_reset();
		break;
	case DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_3:
		display_rta_set_mode_countdown(3);
		break;
	case DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_2:
		display_rta_set_mode_countdown(2);
		break;
	case DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_1:
		display_rta_set_mode_countdown(1);
		break;
	case DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_CLEARED:
		display_rta_set_mode_countdown(0);
		if (diagnostic_mode)
			debug_gauge_restore_log_header();
		break;
	case DISPLAY_BUTTON_ACTION_MODE_SWITCH:
		display_rta_set_mode(!diagnostic_mode);
		display_rta_set_mode_countdown(0);
		break;
	default:
		break;
	}
	const bool active_diagnostic_mode = display_rta_is_diagnostic_mode();
	static bool mode_initialized;
	static bool last_diagnostic_mode;
	if (!mode_initialized || active_diagnostic_mode != last_diagnostic_mode)
	{
		mode_initialized = true;
		last_diagnostic_mode = active_diagnostic_mode;
		weather_rx_set_enabled(true);
		cdc_diagnostics_set_enabled(active_diagnostic_mode);
		usb_audio_set_diagnostics_enabled(active_diagnostic_mode);
	}
	static bool weather_initialized;
	static uint32_t last_weather_generation;
	const uint32_t weather_generation = weather_rx_generation();
	if (!weather_initialized || weather_generation != last_weather_generation)
	{
		weather_snapshot_t weather_snapshot;
		weather_rx_get_snapshot(&weather_snapshot);
		display_rta_set_weather_data(&weather_snapshot);
		last_weather_generation = weather_generation;
		weather_initialized = true;
	}

	if (display_rta_page_service())
	{
		const display_page_t page = display_rta_get_page();
		if (page == DISPLAY_PAGE_DIAGNOSTICS)
		{
			debug_gauge_init();
		}
		else if (page == DISPLAY_PAGE_PLAYBACK)
		{
			display_rta_set_format(audio_state.bit_depth, (float)audio_state.freq / 1000.0f);
		}
	}

	if (!display_rta_page_ready())
	{
		return;
	}

	const display_page_t page = display_rta_get_page();

	if (page != DISPLAY_PAGE_BOOT)
		display_rta_render_service();

	if (page == DISPLAY_PAGE_DIAGNOSTICS)
	{
		debug_gauge_service();
	}
}

static void service_idle_cleanup(void)
{
	const uint32_t now_us = time_us_32();
	const uint32_t current_now_playing = now_playing;

	if (!idle_cleanup_initialized)
	{
		last_observed_now_playing = current_now_playing;
		last_audio_activity_us = now_us;
		idle_cleanup_initialized = true;
		return;
	}

	if (current_now_playing != last_observed_now_playing)
	{
		last_observed_now_playing = current_now_playing;
		last_audio_activity_us = now_us;
		is_cleared_buffer = false;
		return;
	}

	if (is_cleared_buffer ||
		(uint32_t)(now_us - last_audio_activity_us) < IDLE_CLEANUP_TIMEOUT_US)
		return;

	multicore_lockout_start_blocking();
	if (now_playing == last_observed_now_playing && !is_cleared_buffer)
	{
		clear_ringbuffer(&buffer_audio);
		renew_clock();
		now_playing = 0;
		last_observed_now_playing = 0;
		last_audio_activity_us = time_us_32();
		is_cleared_buffer = true;
	}
	multicore_lockout_end_blocking();
}

int main(void)
{
	// External power enable pin.
	if (USE_EXT_POWER_ENABLE)
	{
		gpio_init(EXT_POWER_ENABLE_PIN);
		gpio_set_dir(EXT_POWER_ENABLE_PIN, GPIO_OUT);
		gpio_put(EXT_POWER_ENABLE_PIN, false);
		sleep_us(BOOT_WAIT_TIME_US);
		gpio_put(EXT_POWER_ENABLE_PIN, true);
	}
	else
	{
		sleep_us(BOOT_WAIT_TIME_US);
	}

	// Increase the operating voltage and clock speed.
	vreg_set_voltage(V_CORE_HI);
	sleep_ms(2);
	set_sys_clock_khz(SYS_CLOCK_KHZ_44K, true);
	sleep_us(1);

	// Enable the test mode pins.
	if (TEST_MODE)
	{
		gpio_init(TEST_PIN1);
		gpio_init(TEST_PIN2);
		gpio_set_dir(TEST_PIN1, GPIO_OUT);
		gpio_set_dir(TEST_PIN2, GPIO_OUT);
	}

	// Initialize buffers.
	initialize_ringbuffer(SIZE_AUDIO_RING_FRAMES * 2u, false, &buffer_audio);

	// Initialize audio status.
	audio_state.freq = AUDIO_INITIAL_FREQ;
	audio_state.bit_depth = 16;
	audio_state.mute = false;

	// Show the boot screen before the slower peripheral and DSP setup. The splash
	// routine returns immediately, so initialization continues with it visible.
	display_rta_hw_init();
	display_rta_clear_screen();
	
	display_rta_draw_splash_screen();

	// Turn on the onboard LED.
	gpio_init(ONBOARD_LED_PIN);
	gpio_set_dir(ONBOARD_LED_PIN, GPIO_OUT);
	gpio_put(ONBOARD_LED_PIN, true);

	// DCDC operating mode: true for FPWM, false for PFM.
	gpio_init(DCDC_MODE_PIN);
	gpio_set_dir(DCDC_MODE_PIN, true);
	gpio_put(DCDC_MODE_PIN, true);

	cdc_diagnostics_init();
	weather_rx_init();

	// Start Core1, which handles I2S output.
	multicore_launch_core1(core1_main);


	spectrum_init();
	spectrum_set_sample_rate(audio_state.freq);



	// Make USB available first.
	usb_sound_card_init();
	boot_wait_ms(100);

	// Draw the boot screen.
	display_rta_set_boot_stage(0);
	boot_wait_ms(1000);

	display_rta_set_boot_stage(1);
	boot_wait_ms(1000);

	display_rta_set_boot_stage(2);
	boot_wait_ms(1000);

	display_rta_set_boot_stage(3);
	boot_wait_ms(1000);

	display_rta_set_boot_stage(4);
	boot_wait_ms(1000);

	display_rta_set_boot_stage(5);
	boot_wait_ms(1000);

	// GADGET MODE (the standard mode) starts on the weather screen; hold B for 3 seconds to enter DIAG MODE.
	display_rta_set_mode(false);

	// watchdog_enable(50, 1);

	while (true)
	{
		// watchdog_update();

		// TinyUSB device task + direct PCM receive-to-I2S ring buffer + feedback/state management
		usb_audio_task();
		weather_rx_task();
		cdc_diagnostics_task();
		service_idle_cleanup();

		sleep_us(1);

		// --- Display control ---
		
		// Update the display when the USB audio format (sample rate / bit depth) changes.
    	if (audio_state.freq != last_display_freq || audio_state.bit_depth != last_display_bit_depth)
    	{
        last_display_freq = audio_state.freq;
        last_display_bit_depth = audio_state.bit_depth;
        
        // Update the displayed sample rate and bit depth.
        display_rta_set_format(audio_state.bit_depth, (float)audio_state.freq / 1000.0f);
        spectrum_set_sample_rate(audio_state.freq);
    	}

	    // Monitor input inactivity on Core0; reset drawing is queued for the render service below.
	    static bool inactivity_screen_reset = false;
	    const bool audio_timed_out = audio_data_seen &&
	        ((uint32_t)(time_us_32() - audio_last_data_us) >= 200000u);
	    if (!audio_timed_out) inactivity_screen_reset = false;

	    // Hold the last meter/FFT frame long enough to observe short system test tones.
	    const bool display_inactive = audio_data_seen &&
	        ((uint32_t)(time_us_32() - audio_last_data_us) >= 1000000u);
	    if (display_inactive && !inactivity_screen_reset)
	    {
        display_rta_reset_levels();
	        inactivity_screen_reset = true;
	    }

	    // Only the active page is drawn; diagnostics no longer overlays the playback screen.
	    display_service();
	}
}
