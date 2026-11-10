/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#include <string.h>
#include "transmit_to_dac.h"
#include "usb_device_control.h"
#include "usb_device_control.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/regs/dma.h"
#include "hardware/regs/pio.h"
#include "i2s_pio_interface.h"

static const PIO pio = pio0;
static const uint sm = 0;
static pio_sm_config sm_config;
static uint offset;

#define DMA_TX_CHAIN_CHANNELS (2)
#define DMA_TX_SRAM_BANK_BYTES (64u * 1024u)

static uint32_t __attribute__((section(".dma_sram_bank7")))
    dma_tx_buffers[DEPTH_DMA_TX_BUFFER][SIZE_DMA_TX_BUF];

_Static_assert(sizeof(dma_tx_buffers) <= DMA_TX_SRAM_BANK_BYTES,
               "DMA buffers exceed SRAM bank 7");

typedef enum
{
    DMA_CH_STATE_IDLE = 0,
    DMA_CH_STATE_ARMED,
    DMA_CH_STATE_RUNNING
} dma_ch_state_t;

static volatile int dma_ch[DMA_TX_CHAIN_CHANNELS];
static dma_channel_config c_dma[DMA_TX_CHAIN_CHANNELS];
static volatile uint8_t dma_ch_state[DMA_TX_CHAIN_CHANNELS];
static bool dma_ahb_error_seen[DMA_TX_CHAIN_CHANNELS];
DMA_TX_STRUCTURE dma_tx;

static void dma_tx_reset(void)
{
    dma_tx.wp = 0;
    dma_tx.rp = 0;
    dma_tx.using = 0;
    dma_tx.prev_write_length = 0;
    for (uint32_t i = 0; i < DEPTH_DMA_TX_BUFFER; i++)
    {
        dma_tx.data[i].tx_buf = dma_tx_buffers[i];
        dma_tx.data[i].tx_size = 0;
    }
}

volatile bool enable_output = false;

// Ver1.2 diagnostics -------------------------------------------------------------------------
DMA_DIAG dma_diag = {.armed = false, .level_min = INT32_MAX, .level_max = 0, .fetch_count = 0, .output_stop_count = 0,
                     .loop_prev_us = 0, .loop_max_us = 0, .loop_over_count = 0, .tail_block_count = 0};

void dma_diag_reset(void)
{
    dma_diag.level_min = INT32_MAX;
    dma_diag.level_max = 0;
    dma_diag.loop_max_us = 0;
    dma_diag.loop_over_count = 0;
}

void dma_diag_reset_counters(void)
{
    dma_diag_reset();
    dma_diag.fetch_count = 0;
    dma_diag.output_stop_count = 0;
    dma_diag.loop_prev_us = 0;
    dma_diag.tail_block_count = 0;
}

// Blocks in use are the last dma_tx.using blocks written (oldest first). The oldest one may be partly
// played already, so this is an upper bound. Read from Core0 without locking: approximate by design.
uint32_t dma_tx_pool_frames(void)
{
    uint32_t n = dma_tx.using;
    if (n > DEPTH_DMA_TX_BUFFER)
        n = DEPTH_DMA_TX_BUFFER;
    uint32_t idx = (dma_tx.wp + DEPTH_DMA_TX_BUFFER - n) % DEPTH_DMA_TX_BUFFER;
    uint32_t frames = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        frames += dma_tx.data[idx].tx_size / 2u;
        idx = (idx + 1u) % DEPTH_DMA_TX_BUFFER;
    }
    return frames;
}

// PWM
static uint16_t pwm_slice;

void __not_in_flash_func(dma_tx_start)(void);
static inline uint32_t dma_tx_active_count(void);
static inline bool dma_tx_any_running(void);
static void __not_in_flash_func(dma_tx_chain_service)(bool ring_paused);
static bool dma_tail_seen = false; // lone-block episode already counted
static bool prefill_waiting_for_pio = false;
static uint32_t output_start_us;

bool calc_pwm_clkdiv_and_wrap_us(float target_period_us, uint16_t *wrap_out, float *clkdiv_out);
void set_pwm_isr_1(float period_us);
void set_period_pwm_us(float period_us);
void pwm_i2s_streaming_rate_change(void);
void __not_in_flash_func(pwm_wrap_handler)(void);

static inline uint32_t dma_tx_active_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
    {
        if (dma_ch_state[i] != DMA_CH_STATE_IDLE)
            count++;
    }
    return count;
}

static inline bool dma_tx_any_running(void)
{
    for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
    {
        if (dma_ch_state[i] == DMA_CH_STATE_RUNNING)
            return true;
    }
    return false;
}

static void __not_in_flash_func(dma_tx_check_ahb_errors)(void)
{
    for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
    {
        const bool error =
            (dma_hw->ch[dma_ch[i]].ctrl_trig & DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS) != 0u;
        if (error && !dma_ahb_error_seen[i])
            usb_audio_log_anomaly(10, (uint8_t)dma_ch[i], "CHANNEL", (uint32_t)dma_ch[i]);
        dma_ahb_error_seen[i] = error;
    }
}

static void __not_in_flash_func(dma_tx_check_pio_stall)(uint32_t now_us)
{
    const uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    if ((pio->fdebug & stall_mask) == 0u)
        return;

    pio->fdebug = stall_mask;
    if (!enable_output || usb_audio_get_state() != USB_AUDIO_STREAMING ||
        (uint32_t)(now_us - output_start_us) < 10000u)
        return;

    const uint32_t fifo_level = pio_sm_get_tx_fifo_level(pio, sm);
    usb_audio_log_anomaly(9, (uint8_t)fifo_level, "TXFIFO", fifo_level);
}

static void __not_in_flash_func(dma_tx_chain_service)(bool ring_paused)
{
    dma_tx_check_ahb_errors();

    for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
    {
        bool busy = dma_channel_is_busy(dma_ch[i]);
        if (dma_ch_state[i] == DMA_CH_STATE_ARMED)
        {
            if (busy)
                dma_ch_state[i] = DMA_CH_STATE_RUNNING;
        }
        else if (dma_ch_state[i] == DMA_CH_STATE_RUNNING)
        {
            if (!busy)
            {
                dma_ch_state[i] = DMA_CH_STATE_IDLE;
                if (dma_tx.using > 0)
                    dma_tx.using--;
            }
        }
    }

    uint32_t active = dma_tx_active_count();
    int32_t unassigned = (int32_t)dma_tx.using - (int32_t)active;
    if (unassigned < 0)
        unassigned = 0;

    while (unassigned > 0)
    {
        int idle_idx = -1;
        for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
        {
            if (dma_ch_state[i] == DMA_CH_STATE_IDLE)
            {
                idle_idx = i;
                break;
            }
        }
        if (idle_idx < 0)
            break;

        uint32_t rp = dma_tx.rp;
        dma_channel_set_read_addr(dma_ch[idle_idx], dma_tx.data[rp].tx_buf, false);
        dma_channel_set_trans_count(dma_ch[idle_idx], dma_tx.data[rp].tx_size, false);
        dma_tx.rp++;
        if (dma_tx.rp >= DEPTH_DMA_TX_BUFFER)
            dma_tx.rp = 0;
        dma_ch_state[idle_idx] = DMA_CH_STATE_ARMED;
        active++;
        unassigned--;
    }

    if (!dma_tx_any_running())
    {
        int armed_count = 0;
        int start_idx = -1;
        for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
        {
            if (dma_ch_state[i] == DMA_CH_STATE_ARMED)
            {
                armed_count++;
                if (start_idx < 0)
                    start_idx = i;
            }
        }
        bool start = (armed_count >= 2) && (start_idx >= 0);

        // A paused or empty Ring cannot supply another block; drain the lone queued block.
        const bool tail = (armed_count == 1) && (start_idx >= 0) &&
                          (ring_paused || get_size_using(&buffer_audio) == 0);
        if (tail)
        {
            if (!dma_tail_seen)
            {
                dma_tail_seen = true;
                if (dma_diag.armed)
                    dma_diag.tail_block_count++;
            }
#if DMA_START_TAIL_BLOCK
            start = true;
#endif
        }
        else
        {
            dma_tail_seen = false;
        }

        if (start)
        {
            dma_channel_start(dma_ch[start_idx]);
            dma_ch_state[start_idx] = DMA_CH_STATE_RUNNING;
        }
    }
    else
    {
        dma_tail_seen = false;
    }
}

void reset_i2s_freq(void)
{
    I2S_freq_init(pio, sm, &sm_config, offset);
}

void init_i2s_interface(void)
{
    if(I2S_SLEWRATE_FAST_ENABLE)
    {
        gpio_set_slew_rate(I2S_DATA_PIN, GPIO_SLEW_RATE_SLOW);
        gpio_set_slew_rate(I2S_SIDESET_BASE, GPIO_SLEW_RATE_SLOW);
        gpio_set_slew_rate(I2S_SIDESET_BASE + 1, GPIO_SLEW_RATE_SLOW);
    }

    if(I2S_STRENGTH_REINFORCE_ENABLE)
    {
        gpio_set_drive_strength(I2S_DATA_PIN, GPIO_DRIVE_STRENGTH_2MA);
        gpio_set_drive_strength(I2S_SIDESET_BASE, GPIO_DRIVE_STRENGTH_2MA);
        gpio_set_drive_strength(I2S_SIDESET_BASE + 1, GPIO_DRIVE_STRENGTH_2MA);
    }

    // Initialize PIO I2S.
    if(I2S_SIDESET_CHANGE)
    {
        I2S_32bit_inv_program_init(pio, sm, I2S_DATA_PIN, I2S_SIDESET_BASE, AUDIO_INITIAL_FREQ, &sm_config, &offset);
    }
    else
    {
        I2S_32bit_program_init(pio, sm, I2S_DATA_PIN, I2S_SIDESET_BASE, AUDIO_INITIAL_FREQ, &sm_config, &offset);
    }

    reset_i2s_freq();

    // DMA setup (chained, no IRQ)
    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);

    c_dma[0] = dma_channel_get_default_config(dma_ch[0]);
    channel_config_set_transfer_data_size(&c_dma[0], DMA_SIZE_32);
    channel_config_set_dreq(&c_dma[0], pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c_dma[0], dma_ch[1]);

    c_dma[1] = dma_channel_get_default_config(dma_ch[1]);
    channel_config_set_transfer_data_size(&c_dma[1], DMA_SIZE_32);
    channel_config_set_dreq(&c_dma[1], pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c_dma[1], dma_ch[0]);

    dma_tx_reset();
    memset((void *)dma_ch_state, 0, sizeof(dma_ch_state));

    dma_channel_set_irq0_enabled(dma_ch[0], false);
    dma_channel_set_irq0_enabled(dma_ch[1], false);

    dma_channel_configure(dma_ch[0], &c_dma[0],
                          &pio->txf[sm], dma_tx.data[0].tx_buf, 0, false);
    dma_channel_configure(dma_ch[1], &c_dma[1],
                          &pio->txf[sm], dma_tx.data[0].tx_buf, 0, false);
}

// Notify the I2S transmit timing.
void __not_in_flash_func(pwm_wrap_handler)(void)
{
    pwm_clear_irq(pwm_slice); // Clear the PWM interrupt flag.
}

// Maximum number of stereo frames copied into one DMA buffer.
#define DMA_MAX_FRAMES (SIZE_DMA_TX_BUF / 2)

static volatile bool enable_output_prev = false;
extern volatile absolute_time_t time_start_output;

void __not_in_flash_func(dma_tx_start)(void)
{
    // Loop-interval diagnostics: gap between two calls (this loop also runs the FFT)
    const uint32_t loop_now_us = time_us_32();
    static uint32_t anomaly_loop_prev_us;
    if (usb_audio_get_state() == USB_AUDIO_STREAMING && anomaly_loop_prev_us != 0)
    {
        const uint32_t gap_us = loop_now_us - anomaly_loop_prev_us;
        if (gap_us >= 5000u)
            usb_audio_log_anomaly(7, (uint8_t)(gap_us / 1000u), "GAP_US", gap_us);
    }
    anomaly_loop_prev_us = loop_now_us;

    if (dma_diag.armed && dma_diag.loop_prev_us != 0)
    {
        const uint32_t gap_us = loop_now_us - dma_diag.loop_prev_us;
        if (gap_us > dma_diag.loop_max_us)
            dma_diag.loop_max_us = gap_us;
        if (gap_us > LOOP1_WARN_US)
            dma_diag.loop_over_count++;
    }
    dma_diag.loop_prev_us = dma_diag.armed ? loop_now_us : 0;

    const usb_audio_state_t state = usb_audio_get_state();
    const bool ring_paused = state == USB_AUDIO_BUFFERING || state == USB_AUDIO_UNDERFLOW_FATAL;
    if (ring_paused)
    {
        if (enable_output)
        {
            dma_tx_chain_service(true);
            if (dma_tx.using != 0)
                prefill_waiting_for_pio = false;
            else if (!prefill_waiting_for_pio)
            {
                // DMA completion precedes playback of the words still in the PIO.
                pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
                prefill_waiting_for_pio = true;
            }
            else if ((pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + sm))) != 0u)
                dma_stop_and_clear();
        }
        return;
    }
    prefill_waiting_for_pio = false;

    int32_t level = (int32_t)get_size_using(&buffer_audio) / 2;
    if ((state == USB_AUDIO_STREAMING && level > 0) ||
        (state != USB_AUDIO_STREAMING && level >= SIZE_AUDIO_PREFILL_FRAMES))
        enable_output = true;
    else if (level == 0 && dma_tx.using == 0)
    {
        if (enable_output && usb_audio_get_state() == USB_AUDIO_STREAMING)
        {
            dma_diag.output_stop_count++;
            usb_audio_log_anomaly(5, 0, NULL, 0);
        }
        enable_output = false;
    }


    if (enable_output && !enable_output_prev)
    {
        output_start_us = loop_now_us;
        pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        time_start_output = get_absolute_time();
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_clear_fifos(pio, sm);
        pio_sm_restart(pio, sm);
        reset_i2s_freq();
    }
    enable_output_prev = enable_output;

    if (!enable_output)
    {
        dma_stop_and_clear();
        return;
    }

    while (dma_tx.using < SIZE_DMA_TX_BUF_STACK)
    {
        const usb_audio_state_t fetch_state = usb_audio_get_state();
        if (fetch_state == USB_AUDIO_BUFFERING || fetch_state == USB_AUDIO_UNDERFLOW_FATAL)
            break;

        int32_t frames = (int32_t)get_size_using(&buffer_audio) / 2;
        if (frames <= 0)
            break;
        const int32_t level_before = frames; // Ring level just before this fetch
        if (frames > DMA_MAX_FRAMES)
            frames = DMA_MAX_FRAMES;

        const uint32_t wp = dma_tx.wp;
        uint32_t *tx = dma_tx.data[wp].tx_buf;
        if (ringbuf_read_array_spinlock((int32_t *)tx, (uint32_t)frames * 2u, &buffer_audio) < 0)
            break;

        dma_diag.fetch_count++;
        if (dma_diag.armed)
        {
            const int32_t level_after = (int32_t)get_size_using(&buffer_audio) / 2;
            if (level_before > dma_diag.level_max)
                dma_diag.level_max = level_before;
            if (level_after < dma_diag.level_min)
                dma_diag.level_min = level_after;
        }

        const uint32_t irq_state = save_and_disable_interrupts();
        dma_tx.data[wp].tx_size = (uint32_t)frames * 2u;
        dma_tx.wp = (wp + 1u) % DEPTH_DMA_TX_BUFFER;
        dma_tx.using++;
        restore_interrupts(irq_state);
    }

    dma_tx_chain_service(false);
    dma_tx_check_pio_stall(time_us_32());
}

void dma_stop_and_clear(void)
{
    // Force-stop DMA.
    for (int i = 0; i < DMA_TX_CHAIN_CHANNELS; i++)
        dma_channel_abort(dma_ch[i]);

    // Reset the DMA status.
    dma_hw->ints0 = (1u << dma_ch[0]) | (1u << dma_ch[1]);

    // Clear the FIFO buffer.
    pio_sm_clear_fifos(pio0, 0);
    pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);

    // Reset queue metadata without clearing the DMA buffer pointers.
    dma_tx_reset();
    memset((void *)dma_ch_state, 0, sizeof(dma_ch_state));

    // A clock/rate change starts a fresh playback epoch. Require the normal
    // buffer threshold and I2S phase resynchronization before output resumes.
    enable_output = false;
    enable_output_prev = false;
    prefill_waiting_for_pio = false;
}
