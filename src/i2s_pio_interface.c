/*
* Copyright (c) 2026 Toshimiyu
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#include "i2s_pio_interface.h"
#include "common.h"

// PIO

// I2S_16bit_program -> 16-bit I2S (primarily for NOS DACs such as the TDA1543).
// I2S_32bit_program -> 32-bit I2S (standard 32-bit I2S format).
// I2S_32bit_inv_program -> 32-bit I2S (with the BCLK and LRCK GPIO signals inverted).
void I2S_16bit_program_init(PIO pio, uint sm, uint data_pin, uint sideset_base, uint freq, pio_sm_config *sm_config_out, uint *offset_out)
{
	uint offset = pio_add_program(pio, &I2S_16bit_program);
	pio_sm_config sm_config = I2S_16bit_program_get_default_config(offset);

	pio_gpio_init(pio, data_pin);
	pio_gpio_init(pio, sideset_base);
	pio_gpio_init(pio, sideset_base + 1);

	pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 1, true);
	pio_sm_set_consecutive_pindirs(pio, sm, sideset_base, 2, true);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX); // Disable the RX FIFO and use twice the TX FIFO capacity.

	sm_config_set_out_pins(&sm_config, data_pin, 1);
	sm_config_set_sideset_pins(&sm_config, sideset_base);
	sm_config_set_out_shift(&sm_config, false, true, 32);
	float div = (float)clock_get_hz(clk_sys) / (freq << 7u);
	sm_config_set_clkdiv(&sm_config, div);

	pio_sm_init(pio, sm, offset, &sm_config);
	pio_sm_set_enabled(pio, sm, true);

	*sm_config_out = sm_config;
	*offset_out = offset;
}

void I2S_32bit_program_init(PIO pio, uint sm, uint data_pin, uint sideset_base, uint freq, pio_sm_config *sm_config_out, uint *offset_out)
{
	uint offset = pio_add_program(pio, &I2S_32bit_program);
	pio_sm_config sm_config = I2S_32bit_program_get_default_config(offset);

	pio_gpio_init(pio, data_pin);
	pio_gpio_init(pio, sideset_base);
	pio_gpio_init(pio, sideset_base + 1);

	pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 1, true);
	pio_sm_set_consecutive_pindirs(pio, sm, sideset_base, 2, true);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX); // Disable the RX FIFO and use twice the TX FIFO capacity.

	sm_config_set_out_pins(&sm_config, data_pin, 1);
	sm_config_set_sideset_pins(&sm_config, sideset_base);
	sm_config_set_out_shift(&sm_config, false, true, 32);
	float div = (float)clock_get_hz(clk_sys) / (freq << 7u);
	sm_config_set_clkdiv(&sm_config, div);

	pio_sm_init(pio, sm, offset, &sm_config);
	pio_sm_set_enabled(pio, sm, true);

	*sm_config_out = sm_config;
	*offset_out = offset;
}

void I2S_32bit_inv_program_init(PIO pio, uint sm, uint data_pin, uint sideset_base, uint freq, pio_sm_config *sm_config_out, uint *offset_out)
{
	uint offset = pio_add_program(pio, &I2S_32bit_inv_program);
	pio_sm_config sm_config = I2S_32bit_program_get_default_config(offset);

	pio_gpio_init(pio, data_pin);
	pio_gpio_init(pio, sideset_base);
	pio_gpio_init(pio, sideset_base + 1);

	pio_sm_set_consecutive_pindirs(pio, sm, data_pin, 1, true);
	pio_sm_set_consecutive_pindirs(pio, sm, sideset_base, 2, true);
	sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX); // Disable the RX FIFO and use twice the TX FIFO capacity.

	sm_config_set_out_pins(&sm_config, data_pin, 1);
	sm_config_set_sideset_pins(&sm_config, sideset_base);
	sm_config_set_out_shift(&sm_config, false, true, 32);
	float div = (float)clock_get_hz(clk_sys) / (freq << 7u);
	sm_config_set_clkdiv(&sm_config, div);

	pio_sm_init(pio, sm, offset, &sm_config);
	pio_sm_set_enabled(pio, sm, true);

	*sm_config_out = sm_config;
	*offset_out = offset;
}

// Reconfigure the PIO clock divider when the sample rate changes.
void I2S_freq_init(PIO pio, uint sm, pio_sm_config *sm_config, uint offset)
{
	pio_sm_set_enabled(pio, sm, false);

	float div = (float)clock_get_hz(clk_sys) / (float)(audio_state.freq << 7u);
	sm_config_set_clkdiv(sm_config, div);
	pio_sm_init(pio, sm, offset, sm_config);
	pio_sm_set_enabled(pio, sm, true);
}