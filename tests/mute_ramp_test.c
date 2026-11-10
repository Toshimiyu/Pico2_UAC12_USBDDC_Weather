#include <stdbool.h>
#include <stdint.h>
#include "mute_ramp.h"

static int test_rate(uint32_t rate, uint32_t expected_frames)
{
	mute_ramp_t ramp;
	mute_ramp_reset(&ramp, rate, false);
	if (ramp.frames != expected_frames || ramp.position != expected_frames)
		return 1;
	if (mute_ramp_next(&ramp, false) != expected_frames)
		return 2;

	for (uint32_t i = 1; i <= expected_frames; i++)
		if (mute_ramp_next(&ramp, true) != expected_frames - i)
			return 3;
	if (mute_ramp_next(&ramp, true) != 0u)
		return 4;

	for (uint32_t i = 1; i <= expected_frames; i++)
		if (mute_ramp_next(&ramp, false) != i)
			return 5;

	mute_ramp_reset(&ramp, rate, true);
	if (ramp.position != 0u || mute_ramp_next(&ramp, true) != 0u)
		return 6;

	const uint32_t halfway = expected_frames / 2u;
	for (uint32_t i = 0; i < halfway; i++)
		mute_ramp_next(&ramp, false);
	if (ramp.position != halfway || mute_ramp_next(&ramp, true) != halfway - 1u ||
		mute_ramp_next(&ramp, false) != halfway)
		return 7;

	mute_ramp_reset(&ramp, rate, false);
	if (ramp.position != expected_frames)
		return 8;
	return 0;
}

int main(void)
{
	static const struct {
		uint32_t rate;
		uint32_t frames;
	} cases[] = {
		{44100u, 221u}, {48000u, 240u}, {88200u, 441u},
		{96000u, 480u}, {176400u, 882u}, {192000u, 960u}
	};

	for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		const int result = test_rate(cases[i].rate, cases[i].frames);
		if (result != 0)
			return (int)(10u * (i + 1u)) + result;
	}
	return 0;
}
