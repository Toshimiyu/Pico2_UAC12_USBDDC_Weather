/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef _TRANSMIT_TO_DAC_H_
#define _TRANSMIT_TO_DAC_H_

#include "pico/stdlib.h"
#include "common.h"

#define SIZE_DMA_TX_BUF ((96 * 2 * DMA_BLOCK_US) / 1000 + 64)

// Two DMA transfer buffers are sufficient because they are used as a double buffer.
#define SIZE_DMA_TX_BUF_STACK (DEPTH_DMA_TX_BUFFER)

typedef struct
{
    uint32_t *tx_buf;
    uint32_t tx_size;
} DMA_TX_DATA;

typedef struct
{
    DMA_TX_DATA data[DEPTH_DMA_TX_BUFFER];
    volatile uint32_t wp;
    volatile uint32_t rp;
    volatile uint32_t using;
    volatile uint32_t prev_write_length;
} DMA_TX_STRUCTURE;

extern void init_i2s_interface(void);
extern void reset_i2s_freq(void);
extern void __not_in_flash_func(dma_tx_start)(void);
extern void dma_stop_and_clear(void);
extern void pwm_i2s_streaming_rate_change(void);
extern void set_pwm_isr_1(float period_us);
extern DMA_TX_STRUCTURE dma_tx;
extern volatile bool enable_output;

// ---- Ver1.2 diagnostics (written by Core1 in dma_tx_start(), read by Core0) ----
typedef struct
{
    volatile bool armed;                 // Core0 sets it while STREAMING: MIN/MAX are only collected then
    volatile int32_t level_min;          // Ring level [frames] right after a fetch (local minimum)
    volatile int32_t level_max;          // Ring level [frames] right before a fetch (local maximum)
    volatile uint32_t fetch_count;       // blocks moved from the Ring to the DMA pool
    volatile uint32_t output_stop_count; // underruns: enable_output true -> false while armed (STREAMING)
    volatile uint32_t loop_prev_us;      // Core1 private: time of the previous dma_tx_start() call
    volatile uint32_t loop_max_us;       // longest gap between two dma_tx_start() calls while armed
    volatile uint32_t loop_over_count;   // gaps longer than LOOP1_WARN_US while armed
    volatile uint32_t tail_block_count;  // a lone last block waited with an empty Ring (see DMA_START_TAIL_BLOCK)
} DMA_DIAG;
extern DMA_DIAG dma_diag;
extern void dma_diag_reset(void);        // restart MIN/MAX collection (Core0)
extern void dma_diag_reset_counters(void);
extern uint32_t dma_tx_pool_frames(void); // frames queued in the DMA pool (upper bound, includes the block being played)

#endif /* _TRANSMIT_TO_DAC_H_ */
