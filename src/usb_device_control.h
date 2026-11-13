/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef _USB_DEVICE_CONTROL_H_
#define _USB_DEVICE_CONTROL_H_

#include <stdint.h>
#include <stdbool.h>
#include "usb_audio_config.h"

// ---- Bring-up level (no debug equipment needed: flash and observe) -----------------------------
//   0 : USB completely OFF  (original boot flow only: splash -> FFT screen). If this does not boot,
//       the problem is NOT in the USB code.
//   1 : TinyUSB init + tud_task only (audio device, no CDC; no audio processing)
//   2 : full firmware without a CDC interface
//   3 : full firmware with a CDC interface (default)

// Streaming state (XMOS-style buffering control)
typedef enum
{
	USB_AUDIO_IDLE = 0,		   // no stream (AS interface alt0 / not configured)
	USB_AUDIO_BUFFERING,	   // stream open, nominal feedback only until the prefill level is reached
	USB_AUDIO_STREAMING,	   // normal operation, PI feedback active
	USB_AUDIO_OVERFLOW_FATAL,  // ring buffer too full : new USB data is discarded, playback continues
	USB_AUDIO_UNDERFLOW_FATAL, // Ring + DMA pool too empty : accumulate again
	USB_AUDIO_SWITCHING,	   // sample-rate change: USB data discarded, Ring + DMA pool are played out, then clock switch -> BUFFERING
} usb_audio_state_t;

// Diagnostic counters (read-only)
typedef struct
{
	volatile usb_audio_state_t state;
	volatile uint32_t overflow_count;
	volatile uint32_t underflow_count;
	volatile uint32_t fifo_resync_count;	// TinyUSB software FIFO had to be flushed
	volatile uint32_t rate_limit_violation; // host selected a bit depth / rate pair that Full-Speed cannot carry
	volatile int32_t level_now;				// audio ring-buffer usage [samples]
	volatile int32_t level_min;				// min / max since STREAMING started
	volatile int32_t level_max;
	volatile float feedback_hz;				// effective sample rate currently requested from the host
	volatile float integ_hz;				// PI integrator [Hz] = learned clock offset (saturated at +-limit -> control failing)
	volatile int32_t win_avg;				// 1 s window statistics of the level (valid while STREAMING):
	volatile int32_t win_min;				//   a steadily rising / falling win_avg = drift = control failure
	volatile int32_t win_max;
	volatile int32_t win_slope;				// win_avg - previous win_avg  [samples per second]				//   win_avg / min / max staying near TARGET = control working
	// ---- Ver1.2 ----
	// level_min / level_max are collected by Core1 at every Ring fetch (min: right after, max: right before)
	// since STREAMING started, so they include the sawtooth that Core0 loop sampling would miss.
	volatile int32_t ring_pool_now;		   // Ring level + frames queued in the DMA pool [frames] (upper bound)
	volatile int32_t max_hf_diff;		   // max |host rate H - feedback rate F| [Hz], excluding the first STREAMING second
	volatile uint32_t output_stop_count;   // underruns: Core1 output stopped because Ring and pool ran dry while STREAMING
	volatile uint32_t switch_count;		   // completed sample-rate changes
	volatile uint32_t switch_timeout_count; // switches whose drain hit SWITCH_DRAIN_TIMEOUT_US and was forced
	volatile uint32_t switch_drain_us;	   // drain time of the last switch [us]

	// ---- Drop accounting [stereo frames, since boot] ----
	// Healthy playback: drop_ovf / drop_fifo / output_stop_count / tail_block_count stay 0.
	// drop_switch / drop_clear grow by design at every rate change or (re)open.
	volatile uint32_t rx_frames;		   // frames pulled from the TinyUSB FIFO (including the discarded ones)
	volatile uint32_t drop_ovf_frames;	   // Ring full: frames lost (failed Ring write + discard in OVERFLOW_FATAL)
	volatile uint32_t drop_fifo_frames;	   // TinyUSB FIFO resync flush (Core0 did not read fast enough)
	volatile uint32_t drop_switch_frames;  // discarded while SWITCHING (+ FIFO content flushed at completion)
	volatile uint32_t drop_idle_frames;	   // discarded in IDLE (no stream / rate unsupported)
	volatile uint32_t drop_clear_frames;   // Ring / DMA pool contents thrown away by a restart or forced stop, FIFO flush at open / close
	volatile uint32_t invalid_format_count; // unsupported PCM bytes-per-sample value at the receive boundary
	volatile uint32_t invalid_frame_count;  // malformed PCM frame boundaries or impossible FIFO read lengths
	volatile uint32_t drop_invalid_frames;  // complete-frame equivalents discarded during PCM input resynchronization
	volatile uint32_t click_candidate_count; // PCM blocks whose peak second difference crossed the diagnostic threshold
	volatile uint32_t max_click_curvature_q31; // maximum candidate curvature, in Q31 sample units

	// ---- Loop intervals while STREAMING (since STREAMING started) ----
	volatile uint32_t loop0_max_us;		   // Core0: longest gap between two usb_audio_task() calls (SPI drawing, FFT feed, ...)
	volatile uint32_t loop0_over_count;	   // Core0: gaps longer than LOOP0_WARN_US
	volatile uint32_t loop1_max_us;		   // Core1: longest gap between two dma_tx_start() calls (FFT runs in this loop)
	volatile uint32_t loop1_over_count;	   // Core1: gaps longer than LOOP1_WARN_US
	volatile uint32_t tail_block_count;	   // a lone last DMA block was found waiting with an empty Ring (see DMA_START_TAIL_BLOCK)
} usb_audio_stats_t;

typedef struct
{
	uint32_t timestamp_us;
	uint16_t ring_frames;
	uint16_t dma_frames;
	usb_audio_state_t state;
	bool output_active;
} usb_audio_history_sample_t;

// ---- Application API (main.c) -----------------------------------------------
// Initialise TinyUSB and publish the UAC2 device (name kept for main.c compatibility)
extern void usb_sound_card_init(void);

// Must be called from the Core0 main loop as often as possible:
// tud_task + PCM receive/convert -> audio ring buffer + feedback control + state machine
extern void usb_audio_task(void);

extern usb_audio_state_t usb_audio_get_state(void);
extern const usb_audio_stats_t *usb_audio_get_stats(void);
extern void usb_audio_set_diagnostics_enabled(bool enabled);
extern void usb_audio_reset_diagnostics(void);

// ---- USB Audio class adapter API ---------------------------------------------
extern bool usb_audio_rate_supported(uint32_t freq);
extern void usb_audio_apply_sample_rate(uint32_t freq); // reconfigure I2S and clear queued PCM
extern bool usb_audio_stream_open(uint8_t alt);			 // AS interface alternate setting
extern void usb_audio_stream_close(void);				 // AS interface alt 0 / unmount

// ----- Diagnostic API (debug_gauge.c) -----

uint32_t usb_audio_get_host_rate_hz(void);
uint32_t usb_audio_get_feedback_hz(void);
uint32_t usb_audio_get_feedback_limit_hz(void);
uint32_t usb_audio_get_packets_per_sec(void);
uint32_t usb_audio_get_avg_packet_bytes(void);
bool usb_audio_get_history_sample(uint32_t age, usb_audio_history_sample_t *sample);

void usb_audio_get_event(uint32_t age, char out[8]);
uint32_t usb_audio_get_event_time_us(uint32_t age); // time_us_32() stamp of the same event (0 = none)
bool usb_audio_get_event_entry(uint32_t age, char out[8], uint32_t *timestamp_us);
void usb_audio_record_event(char kind, uint8_t a, uint8_t b, uint8_t c, bool ok, bool has_c);
void usb_audio_log_anomaly(uint8_t code, uint8_t detail,
						   const char *detail_name, uint32_t detail_value);

// ----- Switch sequence (Ver1.2) -----
// Rate to report for GET_CUR: the pending rate while SWITCHING, otherwise the rate in use
uint32_t usb_audio_get_current_rate(void);
// Append an internal event (e.g. 'W' switch begin, 'D' drained, 'K' clock done) to the event log
void usb_audio_log_event(char kind, uint8_t a, uint8_t b);

#endif /* _USB_DEVICE_CONTROL_H_ */