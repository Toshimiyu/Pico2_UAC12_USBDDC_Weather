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
#include "hardware/vreg.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

#include "common.h"
#include "transmit_to_dac.h"
#include "ringbuffer.h"
#include "display_rta.h"

void core1_main()
{
	// Allow Core0 to pause this core while changing the shared audio clock,
	// resetting DMA, or clearing the stereo ring buffers.
	multicore_lockout_victim_init();

	// Initialize I2S.
	init_i2s_interface();

	while (true)
	{
		if(TEST_MODE) gpio_put(TEST_PIN2, true);
		dma_tx_start();
		// FFT calculation only; all blocking TFT writes are serviced by Core0.
		// One channel per step, with a DMA service in between, so the DMA chain is never left
		// unattended for a whole two-channel FFT.
		if (display_rta_process_fft_left())
		{
			dma_tx_start();
			display_rta_process_fft_right();
		}
		if(TEST_MODE) gpio_put(TEST_PIN2, false);
		sleep_us(1);
	}
}
