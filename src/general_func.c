/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#include <math.h>
#include "common.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "transmit_to_dac.h"

static void renew_cpu_clock(void)
{
	// CPU clock is kept at the high-performance voltage for the selected rate family.
	switch (audio_state.freq)
	{
	case 192000:
	case 96000:
	case 48000:
		vreg_set_voltage(V_CORE_HI);
		busy_wait_us(100);
		set_sys_clock_khz(SYS_CLOCK_KHZ_48K, true);
		busy_wait_us(50);
		break;
	case 176400:
	case 88200:
	case 44100:
	default:
		vreg_set_voltage(V_CORE_HI);
		busy_wait_us(100);
		set_sys_clock_khz(SYS_CLOCK_KHZ_44K, true);
		busy_wait_us(50);
		break;
	}
}

void renew_clock(void)
{
	// Reconfigure the CPU clock.
	renew_cpu_clock();

	// Clear DMA.
	dma_stop_and_clear();

	// Reconfigure the PIO clock divider.
	reset_i2s_freq();

}


void volume_control(void)
{
	audio_state.vol_float = saturation_f32(powf(10.0f, (float)audio_state.acq_volume / (float)VOLUME_RESOLUTION / 20.0f), 1.0f, 0.0f);
}
