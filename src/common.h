/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ringbuffer.h"
#include "usb_audio_config.h"

// User Configurable ------------------------------------------------------------------

// Test mode
#define TEST_MODE (false)
#define TEST_PIN1 (1)
#define TEST_PIN2 (2)

// Enable CDC diagnostics only in the build mode that exposes the virtual COM port.
#define COM_PORT_OUTPUT_ENABLE USB_CDC_ENABLED

// String Desc.
#define MFG_NAME ("Toshimiyu")
#define DEVICE_NAME ("Pico2 UAC12 USB DDC")
#define WEBSITE_ADDR ("toshimi.yuze@gmail.com:")

// Faster I2S slew rate
#define I2S_SLEWRATE_FAST_ENABLE (false)

// Enhancement I2S signal output current
#define I2S_STRENGTH_REINFORCE_ENABLE (false)

// I2S Pin : sideset0:BCLK, sideset1:LRCK (if No Changed)
#define I2S_SIDESET_CHANGE (false)
#define I2S_DATA_PIN (9)
#define I2S_SIDESET_BASE (10)

// Output level scale retained from the previous PCM-to-DAC path.
#define DEFAULT_GAIN_RATIO (0.80f)

// Other Function
#define USE_EXT_POWER_ENABLE (false)
#define EXT_POWER_ENABLE_PIN (3)
#define BOOT_WAIT_TIME_US (50000)

// Pico Display Pack 2.0 wiring shared by the playback and diagnostics screens.
#ifndef LCD_SCK_PIN
#define LCD_SCK_PIN (18)
#endif
#ifndef LCD_MOSI_PIN
#define LCD_MOSI_PIN (19)
#endif
#ifndef LCD_CS_PIN
#define LCD_CS_PIN (17)
#endif
#ifndef LCD_DC_PIN
#define LCD_DC_PIN (16)
#endif
#ifndef LCD_BL_PIN
#define LCD_BL_PIN (20)
#endif
#ifndef DISPLAY_BUTTON_A_PIN
#define DISPLAY_BUTTON_A_PIN (12)
#endif
#ifndef DISPLAY_BUTTON_B_PIN
#define DISPLAY_BUTTON_B_PIN (13)
#endif
#ifndef DISPLAY_BUTTON_X_PIN
#define DISPLAY_BUTTON_X_PIN (14)
#endif
#ifndef DISPLAY_BUTTON_Y_PIN
#define DISPLAY_BUTTON_Y_PIN (15)
#endif
#define DISPLAY_BUTTON_DEBOUNCE_US (30000u)
#define MODE_SWITCH_HOLD_MS (3000u)

// User Configurable end ------------------------------------------------------------

// System Clock
#define SYS_CLOCK_KHZ_44K (336000)
#define SYS_CLOCK_KHZ_48K (360000)

// Core Voltage
#define V_CORE_HI VREG_VOLTAGE_1_25


// Initial Audio Sample Frequency
#define AUDIO_INITIAL_FREQ (44100)

// FFT length shared by the FFT task and spectrum analyzer.
#define FFT_LEN (2048)

// DCDC Control
#define DCDC_MODE_PIN (23)

// LED
#define ONBOARD_LED_PIN (25)

// DMA block duration used to size the interleaved I2S transfer buffers.
#define DMA_BLOCK_US (2200)

// Endpoint buffer size (at least 97 is needed for (96+1) kHz * 1 ms).
#define SIZE_EP_BUFFER (896)

// Audio ring capacity in stereo frames. 2048 frames provides about 10.7 ms of
// buffering at 192 kHz; the feedback target follows at 50% capacity.
#define SIZE_AUDIO_RING_FRAMES (2048)
#define TARGET_PREFILL_MS 6

// Legacy start threshold for draining queued audio in non-buffering states.
// Normal playback starts only after TARGET_PREFILL_MS has accumulated.
#ifndef USB_AUDIO_PREFILL_FRAMES
#define USB_AUDIO_PREFILL_FRAMES (1536)
#endif
#if USB_AUDIO_PREFILL_FRAMES <= 0 || USB_AUDIO_PREFILL_FRAMES >= SIZE_AUDIO_RING_FRAMES
#error "USB_AUDIO_PREFILL_FRAMES must be between 1 and SIZE_AUDIO_RING_FRAMES - 1"
#endif
#define SIZE_AUDIO_PREFILL_FRAMES USB_AUDIO_PREFILL_FRAMES

// DMA transfer buffer size (at least 3 is needed).
#define DEPTH_DMA_TX_BUFFER (4)

// Ver1.2: start the DMA with a single queued block when nothing else is coming (Ring empty).
// Before, output only started with >= 2 armed blocks, so the lone last block of a stream (or of a drain)
// was never played. 0 = old behaviour (dma_diag.tail_block_count still counts the situation).
#define DMA_START_TAIL_BLOCK (1)

// Diagnostics: a main-loop pass longer than this counts as 'over' (loop0/loop1_over_count) [us]
#define LOOP0_WARN_US (1000)
#define LOOP1_WARN_US (1000)

// Feedback target level (50%) threshold. LCD drawing is allowed only at or above this level.
#define SIZE_BUFFER_FB_THRESHOLD (SIZE_AUDIO_RING_FRAMES / 2)

// LCD drawing (blocking SPI on Core0) is only allowed while the ring holds at least this many frames.
// Defined once here: display services are allowed when at least half the feedback target remains buffered.
#define DISPLAY_MIN_AUDIO_HEADROOM_SAMPLES (SIZE_BUFFER_FB_THRESHOLD / 2)

// Feedback adjustment limit (returns a value corresponding to +/-1 sample; the reference is 1000).
#define FB_ADJ_LIMIT (1000)

// Volume management.
#define VOLUME_RESOLUTION (256)
#define MIN_VOLUME (-64 * VOLUME_RESOLUTION)
#define MAX_VOLUME (0 * VOLUME_RESOLUTION)
#define DEFAULT_VOLUME (0 * VOLUME_RESOLUTION)

typedef struct
{
	bool raw_high;
	bool stable_high;
	uint32_t raw_changed_us;
} display_button_t;

static inline bool display_button_pressed(display_button_t *button, uint pin, uint32_t now)
{
	const bool raw_high = gpio_get(pin);
	if (raw_high != button->raw_high)
	{
		button->raw_high = raw_high;
		button->raw_changed_us = now;
	}

	if (button->stable_high != button->raw_high &&
		(uint32_t)(now - button->raw_changed_us) >= DISPLAY_BUTTON_DEBOUNCE_US)
	{
		button->stable_high = button->raw_high;
		return !button->stable_high;
	}
	return false;
}

typedef struct
{
	volatile uint32_t freq;		// USB control callback updates this asynchronously
	volatile uint32_t bit_depth; // USB alternate-setting callback updates this asynchronously
	int16_t now_volume;
	int16_t acq_volume;
	float vol_float;
	int16_t vol_mul;
	uint32_t vol_shift;
	bool mute;
} AUDIO_STATE;

extern RINGBUFFER buffer_audio;

extern AUDIO_STATE audio_state;
extern uint32_t now_playing;
extern uint16_t length_remain_to_I2S_FIFO;
static inline uint32_t audio_ring_total_frames(void)
{
	return (uint32_t)get_size_using(&buffer_audio) / 2u;
}

extern uint32_t calc_pwm_period_us(float period_us, uint16_t prescale);
extern void volume_control(void);

extern void renew_clock(void);

inline int32_t saturation_i32(int32_t in, int32_t max, int32_t min)
{
	if (in > max)
		return max;
	else if (in < min)
		return min;
	return in;
}

inline float saturation_f32(float in, float max, float min)
{
	if (in > max)
		return max;
	else if (in < min)
		return min;
	return in;
}

#endif
