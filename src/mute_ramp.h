/*
* Copyright (c) 2026 Toshimiyu
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef MUTE_RAMP_H
#define MUTE_RAMP_H

#include <stdbool.h>
#include <stdint.h>

#define USB_MUTE_RAMP_MS 5u

typedef struct
{
	uint32_t frames;
	uint32_t position;
} mute_ramp_t;

static inline void mute_ramp_reset(mute_ramp_t *ramp, uint32_t sample_rate, bool muted)
{
	ramp->frames = (uint32_t)(((uint64_t)sample_rate * USB_MUTE_RAMP_MS + 999u) / 1000u);
	if (ramp->frames == 0u)
		ramp->frames = 1u;
	ramp->position = muted ? 0u : ramp->frames;
}

static inline uint32_t mute_ramp_next(mute_ramp_t *ramp, bool muted)
{
	if (muted)
	{
		if (ramp->position != 0u)
			ramp->position--;
	}
	else if (ramp->position < ramp->frames)
		ramp->position++;

	return ramp->position;
}

#endif
