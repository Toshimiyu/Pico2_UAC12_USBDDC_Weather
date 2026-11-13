/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef DISPLAY_RTA_H
#define DISPLAY_RTA_H

#include <stdbool.h>
#include <stdint.h>
#include "spectrum.h"
#include "weather_rx.h"

typedef enum
{
	DISPLAY_PAGE_BOOT = 0,
	DISPLAY_PAGE_PLAYBACK,
	DISPLAY_PAGE_DIAGNOSTICS,
	DISPLAY_PAGE_WEATHER
} display_page_t;

typedef enum
{
	DISPLAY_BUTTON_ACTION_NONE = 0,
	DISPLAY_BUTTON_ACTION_PAGE_CHANGED,
	DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_3,
	DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_2,
	DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_1,
	DISPLAY_BUTTON_ACTION_RESET_COUNTDOWN_CLEARED,
	DISPLAY_BUTTON_ACTION_RESET_DIAGNOSTICS,
	DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_3,
	DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_2,
	DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_1,
	DISPLAY_BUTTON_ACTION_MODE_COUNTDOWN_CLEARED,
	DISPLAY_BUTTON_ACTION_MODE_SWITCH
} display_button_action_t;

void display_rta_init(void);
void display_rta_hw_init(void);
void display_rta_clear_screen(void);
void display_rta_set_page(display_page_t page);
display_page_t display_rta_get_page(void);
void display_rta_set_mode(bool diagnostic_mode);
bool display_rta_is_diagnostic_mode(void);
void display_rta_set_mode_countdown(uint8_t seconds_remaining);
void display_rta_set_weather_data(const weather_snapshot_t *snapshot);
bool display_rta_page_service(void);
bool display_rta_page_ready(void);
bool display_rta_spectrum_active(void);
display_button_action_t display_rta_process_buttons(void);
void display_rta_splash_service(void);
void display_rta_update(void);
// Core1 FFT, split per channel so dma_tx_start() can run in between (see core1.c).
// _left() returns true when a frame is in progress; _right() must then follow.
bool display_rta_process_fft_left(void);
void display_rta_process_fft_right(void);
void display_rta_render_service(void);
#if LOOP_MEASUREMENT
void display_rta_set_loop_measurement(bool valid, uint32_t core0_us, uint32_t core1_us);
void display_rta_loop_render_service(void);
#endif
void display_rta_set_format(uint8_t bit_depth, float sample_rate_khz);

// Boot screen (10-second Quad Heritage theme animation).
void display_rta_draw_splash_screen(void);
void display_rta_set_boot_stage(uint8_t stage);

// Functions for supplying actual audio data.
void display_rta_set_levels(
    const uint8_t l_bands[SPECTRUM_BAND_COUNT],
    const uint8_t r_bands[SPECTRUM_BAND_COUNT],
    uint8_t l_peak,
    uint8_t r_peak);
void display_rta_set_bands(const uint8_t l_bands[SPECTRUM_BAND_COUNT], const uint8_t r_bands[SPECTRUM_BAND_COUNT]);
void display_rta_reset_levels(void);

// Update only the L/R peaks.
void display_rta_set_peak(
    uint8_t l_peak,
    uint8_t r_peak);

#endif // DISPLAY_RTA_H
