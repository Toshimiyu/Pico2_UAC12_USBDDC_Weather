/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>
#include <stdbool.h>

#define SPECTRUM_BAND_COUNT 11

void spectrum_init(void);
void spectrum_set_sample_rate(uint32_t sample_rate);
void spectrum_feed_restart(void);
uint32_t spectrum_generation(void);
uint32_t spectrum_processing_generation(void);
void spectrum_feed(float left_q31, float right_q31);
void spectrum_feed_block(const float *left_q31, const float *right_q31, uint32_t length);
// One analysis frame is processed in two calls so the caller can service the DMA chain in between.
// _left() takes ownership of a ready frame (true = frame in progress); _right() must then be called
// before _left() will accept another frame. _right() returns false if no frame is in progress.
bool spectrum_process_left(uint8_t left[SPECTRUM_BAND_COUNT]);
bool spectrum_process_right(uint8_t right[SPECTRUM_BAND_COUNT]);

#endif
