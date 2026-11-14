/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

// USB Audio Class 2.0 (TinyUSB uac2_speaker_fb based) front-end of the DDC.
//
//   Host -> UAC2 async OUT EP -> TinyUSB sw FIFO -> usb_audio_task()
//        -> PCM convert (16/24/32bit -> float in int32 full-scale)
//        -> direct PCM-to-int32 conversion -> audio ring buffer -> DMA -> PIO-I2S
//
// Feedback (explicit, 16.16 samples/ms) is computed from the fill level of buffer_audio
// with a small PI controller (XMOS-style), NOT from the TinyUSB FIFO count.

#include <math.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "device/dcd.h"
#include "usb_device_control.h"
#include "cdc_diagnostics.h"
#include "usb_audio_class.h"
#include "common_types.h"
#include "common.h"
#include "mute_ramp.h"
#if LOOP_MEASUREMENT
#include "loop_measurement.h"
#endif
#include "ringbuffer.h"
#include "display_rta.h"
#include "transmit_to_dac.h"

extern uint32_t now_playing;

// Referenced from main.c / display_rta.c (activity monitor)
volatile uint32_t audio_last_data_us = 0;
volatile bool audio_data_seen = false;

//--------------------------------------------------------------------+
// Tunables
//--------------------------------------------------------------------+
#define RX_CHUNK_FRAMES (192)
#if (RX_CHUNK_FRAMES > SIZE_EP_BUFFER)
#error RX_CHUNK_FRAMES must not exceed SIZE_EP_BUFFER
#endif

// Ring buffer watermarks (buffer_audio, PCM frames)
#define FB_TARGET_LEVEL (SIZE_BUFFER_FB_THRESHOLD)
#define LEVEL_LOW_MARK (SIZE_AUDIO_RING_FRAMES / 32)						   // below -> UNDERFLOW_FATAL
#define LEVEL_HIGH_MARK (SIZE_AUDIO_RING_FRAMES - SIZE_AUDIO_RING_FRAMES / 32)   // above -> OVERFLOW_FATAL
#define LEVEL_OVERFLOW_RECOVER (FB_TARGET_LEVEL + SIZE_AUDIO_RING_FRAMES / 8) // OVERFLOW_FATAL -> BUFFERING
#define LEVEL_UNDERFLOW_RECOVER (FB_TARGET_LEVEL / 2)					   // UNDERFLOW_FATAL -> BUFFERING

// PI controller (units: Hz of requested sample rate per input sample of level error)
#define FB_KP (0.50f)			   // proportional [Hz / sample]
#define FB_KI (0.0625f)			   // integral     [Hz / (sample * s)]  -> zeta ~= 1, wn ~= 0.25 rad/s
#define FB_HF_GAIN (0.01f)         // H-F difference correction gain.
#define FB_LEVEL_TAU_S (0.064f)	   // level low-pass time constant (removes block-size sawtooth)
#define FB_LIMIT_PPM (3000.0f)	   // hard limit of the correction : +-0.3 % of fs (never exceeds FB_ADJ_LIMIT Hz)
#define FB_UPDATE_PERIOD_US (1000) // one update per USB frame
#define HOST_RATE_UPDATE_US (250000u)
#define HF_DIFF_SETTLE_TIME_US (1000000u)
#define FB_BASE_LIMIT_PPM  (50.0f)
#define FB_BOOST_START_ERR (128.0f)
#define FB_BOOST_FULL_ERR  (512.0f)
#define FB_BOOST_MAX_PPM   (100.0f)
#define EVT_LOG_N 64
#define AUDIO_HISTORY_N 64
#define AUDIO_HISTORY_PRETRIGGER_N 48
#define AUDIO_HISTORY_POSTTRIGGER_N (AUDIO_HISTORY_N - AUDIO_HISTORY_PRETRIGGER_N)
#define AUDIO_HISTORY_PERIOD_US 100000u
#define ANOMALY_LOG_INTERVAL_US 250000u
#define USB_OUT_GAP_WARN_US 3000u
#define PCM_CLICK_CANDIDATE_THRESHOLD_Q31 (0x60000000u)
#define PCM_CLICK_CANDIDATE_MAX_HITS_PER_BLOCK (4u)

// Sample-rate change: how long to wait for the Ring + DMA pool to play out before the stop is forced.
// Worst case drain = (Ring 2048 + DMA pool 4 x 243 frames) / 44.1 kHz ~= 69 ms.
#define SWITCH_DRAIN_TIMEOUT_US (100000u)

// ---- Diagnostic build switches (NEVER ship with FIXED_FEEDBACK_TEST = 1) -------------------------
// FIXED_FEEDBACK_TEST = 1 : feedback control OFF. The requested rate is fs + FIXED_FEEDBACK_OFFSET_HZ.
//   OFFSET = 0     -> measure the natural drift of the buffer level (win_slope)
//   OFFSET = +-100 -> if the host follows feedback, win_slope changes by about the offset [samples/s]
//                     (ratio 1); if it does not change, the host ignores the feedback endpoint.
// Normal operation (PI control) is unchanged when the switch is 0.
#ifndef FIXED_FEEDBACK_TEST
#define FIXED_FEEDBACK_TEST (0)
#endif
#ifndef FIXED_FEEDBACK_OFFSET_HZ
#define FIXED_FEEDBACK_OFFSET_HZ (0.0f)
#endif
#if FIXED_FEEDBACK_TEST
#warning "FIXED_FEEDBACK_TEST is enabled: feedback control is OFF (diagnostic build only)"
#endif

// (UART diagnostics removed: the buffer level is shown by debug_gauge.c on the LCD)

//--------------------------------------------------------------------+
// Module state
//--------------------------------------------------------------------+
static usb_audio_stats_t stats;
static volatile uint8_t cur_alt = 0;
static volatile uint8_t bytes_per_sample = 2;
static volatile bool rate_unsupported = false;
static char evt_log[EVT_LOG_N][8];
static uint32_t evt_time_us[EVT_LOG_N];
static volatile uint32_t evt_total = 0;
static volatile bool diagnostics_enabled = false;
static spin_lock_t *event_history_lock;
static usb_audio_history_sample_t audio_history[AUDIO_HISTORY_N];
static uint32_t audio_history_total;
static uint32_t audio_history_last_us;
static usb_audio_history_sample_t fault_history[AUDIO_HISTORY_N];
static uint32_t fault_history_count;
static uint32_t fault_history_post_remaining;
static bool fault_history_valid;
static uint32_t anomaly_log_last_us[14];
static volatile uint32_t rx_packet_last_us;
static volatile uint32_t rx_packet_gap_pending_us;
static volatile bool rx_packet_gap_pending;

typedef struct
{
	int32_t previous_left;
	int32_t previous2_left;
	int32_t previous_right;
	int32_t previous2_right;
	uint8_t history_count;
} pcm_candidate_history_t;

static pcm_candidate_history_t pcm_candidate_history;
static mute_ramp_t usb_mute_ramp;
static void reset_pcm_candidate_history(void);

static char hex_digit(uint8_t value)
{
	value &= 0x0F;
	return (char)(value < 10 ? ('0' + value) : ('A' + value - 10));
}

void usb_audio_record_event(char kind, uint8_t a, uint8_t b, uint8_t c, bool ok, bool has_c)
{
	const uint32_t owner = spin_lock_blocking(event_history_lock);
	const uint32_t index = evt_total % EVT_LOG_N;
	char *event = evt_log[index];
	evt_time_us[index] = time_us_32();
	event[0] = kind;
	event[1] = hex_digit(a >> 4);
	event[2] = hex_digit(a);
	event[3] = hex_digit(b >> 4);
	event[4] = hex_digit(b);
	event[5] = has_c ? hex_digit(c) : ' ';
	event[6] = ok ? '+' : '-';
	event[7] = '\0';
	evt_total++;
	spin_unlock(event_history_lock, owner);
}

void usb_audio_get_event(uint32_t age, char out[8])
{
	uint32_t timestamp_us;
	usb_audio_get_event_entry(age, out, &timestamp_us);
}

bool usb_audio_get_event_entry(uint32_t age, char out[8], uint32_t *timestamp_us)
{
	const uint32_t owner = spin_lock_blocking(event_history_lock);
	if (age >= EVT_LOG_N || age >= evt_total)
	{
		memset(out, ' ', 7);
		out[7] = '\0';
		*timestamp_us = 0;
		spin_unlock(event_history_lock, owner);
		return false;
	}

	const uint32_t index = (evt_total - 1u - age) % EVT_LOG_N;
	memcpy(out, evt_log[index], 8);
	*timestamp_us = evt_time_us[index];
	spin_unlock(event_history_lock, owner);
	return true;
}

uint32_t usb_audio_get_event_time_us(uint32_t age)
{
	char event[8];
	uint32_t timestamp_us;
	usb_audio_get_event_entry(age, event, &timestamp_us);
	return timestamp_us;
}

void usb_audio_log_event(char kind, uint8_t a, uint8_t b)
{
	usb_audio_record_event(kind, a, b, 0, true, false);
}

void usb_audio_log_anomaly(uint8_t code, uint8_t detail,
						   const char *detail_name, uint32_t detail_value)
{
	if (code >= sizeof(anomaly_log_last_us) / sizeof(anomaly_log_last_us[0]))
		return;

	const uint32_t now = time_us_32();
	const uint32_t owner = spin_lock_blocking(event_history_lock);
	const uint32_t last_us = anomaly_log_last_us[code];
	if (last_us != 0 && (uint32_t)(now - last_us) < ANOMALY_LOG_INTERVAL_US)
	{
		spin_unlock(event_history_lock, owner);
		return;
	}

	anomaly_log_last_us[code] = now;
	if (fault_history_post_remaining == 0)
	{
		const uint32_t pre_count = audio_history_total < AUDIO_HISTORY_PRETRIGGER_N
			? audio_history_total : AUDIO_HISTORY_PRETRIGGER_N;
		for (uint32_t i = 0; i < pre_count; i++)
		{
			const uint32_t age = pre_count - 1u - i;
			fault_history[i] = audio_history[(audio_history_total - 1u - age) % AUDIO_HISTORY_N];
		}
		fault_history_count = pre_count;
		fault_history_post_remaining = AUDIO_HISTORY_POSTTRIGGER_N;
		fault_history_valid = true;
	}
	spin_unlock(event_history_lock, owner);
	usb_audio_record_event('E', code, (uint8_t)stats.state, detail, false, true);
	cdc_diagnostics_log_anomaly(code, detail_name, detail_value);
}

bool usb_audio_get_history_sample(uint32_t age, usb_audio_history_sample_t *sample)
{
	const uint32_t owner = spin_lock_blocking(event_history_lock);
	const uint32_t history_count = fault_history_valid
		? fault_history_count
		: (audio_history_total < AUDIO_HISTORY_N ? audio_history_total : AUDIO_HISTORY_N);
	if (age >= history_count)
	{
		spin_unlock(event_history_lock, owner);
		return false;
	}

	if (fault_history_valid && age < fault_history_count)
		*sample = fault_history[fault_history_count - 1u - age];
	else
		*sample = audio_history[(audio_history_total - 1u - age) % AUDIO_HISTORY_N];
	spin_unlock(event_history_lock, owner);
	return true;
}

static void record_audio_history(void)
{
	const uint32_t now = time_us_32();
	if ((uint32_t)(now - audio_history_last_us) < AUDIO_HISTORY_PERIOD_US)
		return;

	audio_history_last_us = now;
	const uint32_t ring_frames = audio_ring_total_frames();
	const uint32_t dma_frames = dma_tx_pool_frames();
	const usb_audio_history_sample_t sample = {
		.timestamp_us = now,
		.ring_frames = (uint16_t)(ring_frames > UINT16_MAX ? UINT16_MAX : ring_frames),
		.dma_frames = (uint16_t)(dma_frames > UINT16_MAX ? UINT16_MAX : dma_frames),
		.state = stats.state,
		.output_active = enable_output,
	};
	const uint32_t owner = spin_lock_blocking(event_history_lock);
	audio_history[audio_history_total % AUDIO_HISTORY_N] = sample;
	audio_history_total++;
	if (fault_history_post_remaining > 0 && fault_history_count < AUDIO_HISTORY_N)
	{
		fault_history[fault_history_count++] = sample;
		fault_history_post_remaining--;
	}
	spin_unlock(event_history_lock, owner);
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
	(void)in_isr;
	if (eventid == DCD_EVENT_BUS_RESET_START ||
		eventid == DCD_EVENT_BUS_RESET_END ||
		eventid == DCD_EVENT_UNPLUGGED)
	{
		usb_audio_log_anomaly(4, (uint8_t)eventid, "EVENT_ID", eventid);
		usb_audio_record_event('B', (uint8_t)eventid, rhport, 0, true, true);
	}
}

typedef struct
{
	float level_f;	 // filtered buffer level
	float integ_hz;	 // integrator [Hz]
	uint32_t last_us;
	uint32_t last_fb;
	bool force_send;
	uint32_t transient_counter_us; // 【追加】切り替え直後の過渡期タイマー (us)
} fb_ctrl_t;
static fb_ctrl_t fb;

// Sample-rate switch sequence (Ver1.2), Core0 only
static uint32_t pending_freq = 0;	// rate requested by SET_CUR, applied when the output is quiet
static uint32_t switch_t0_us = 0;	// time the SWITCHING state was entered
static uint32_t loop0_prev_us = 0;	// previous usb_audio_task() call (loop-interval diagnostics)

static uint8_t rx_raw[RX_CHUNK_FRAMES * 8];
static float ep_Lch[RX_CHUNK_FRAMES];
static float ep_Rch[RX_CHUNK_FRAMES];
static int32_t pcm_stereo[RX_CHUNK_FRAMES * 2u];

// ---- Host sample-rate measurement ----------------------------------

// Counts the frames actually received from the host

static uint32_t rx_frames_counter = 0;
static uint32_t host_win_t0_us = 0;
static uint32_t host_win_frames0 = 0;
static uint32_t host_rate_hz = 0;
static uint32_t streaming_started_us = 0;

// ---- Packet statistics ---------------------------------------------

static volatile uint32_t pkt_count_total = 0;
static volatile uint32_t pkt_bytes_total = 0;

static uint32_t pkt_win_count0 = 0;
static uint32_t pkt_win_bytes0 = 0;

static uint32_t pkt_per_sec = 0;
static uint32_t pkt_avg_bytes = 0;

#if defined(TUD_AUDIO_DESC_IAD)

bool tud_audio_rx_done_pre_read_cb(
    uint8_t rhport,
    uint16_t n_bytes_received,
    uint8_t func_id,
    uint8_t ep_out,
    uint8_t cur_alt_setting)

#else

bool tud_audio_rx_done_isr(
    uint8_t rhport,
    uint16_t n_bytes_received,
    uint8_t func_id,
    uint8_t ep_out,
    uint8_t cur_alt_setting)

#endif
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    if (diagnostics_enabled)
    {
        pkt_count_total++;
        pkt_bytes_total += n_bytes_received;
    }

    if (diagnostics_enabled && stats.state == USB_AUDIO_STREAMING)
    {
        const uint32_t now_us = time_us_32();
        const uint32_t previous_us = rx_packet_last_us;
        if (previous_us != 0u)
        {
            const uint32_t gap_us = now_us - previous_us;
            if (gap_us >= USB_OUT_GAP_WARN_US &&
                (!rx_packet_gap_pending || gap_us > rx_packet_gap_pending_us))
            {
                rx_packet_gap_pending_us = gap_us;
                rx_packet_gap_pending = true;
            }
        }
        rx_packet_last_us = now_us;
    }
    else
    {
        rx_packet_last_us = 0u;
    }

    return true;
}

static void service_rx_packet_gap(void)
{
    uint32_t gap_us = 0;
    const uint32_t irq_state = save_and_disable_interrupts();
    if (rx_packet_gap_pending)
    {
        gap_us = rx_packet_gap_pending_us;
        rx_packet_gap_pending = false;
        rx_packet_gap_pending_us = 0;
    }
    restore_interrupts(irq_state);

    if (gap_us != 0u)
        usb_audio_log_anomaly(8, (uint8_t)(gap_us / 1000u), "GAP_US", gap_us);
}

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+
bool usb_audio_rate_supported(uint32_t freq)
{
	uint8_t rate_count;
	const uint32_t *rates = usb_audio_class_sample_rates(&rate_count);
	for (uint8_t i = 0; i < rate_count; i++)
		if (rates[i] == freq)
			return true;
	return false;
}

static bool stream_rate_supported(uint8_t alt, uint32_t rate)
{
	usb_audio_format_t format;
	return usb_audio_class_get_format(alt, &format) && rate <= format.max_rate_hz;
}

static void level_stats_reset(void);

static void set_state(usb_audio_state_t s)
{
    const usb_audio_state_t previous_state = stats.state;
    stats.state = s;

    if (previous_state != s && s == USB_AUDIO_STREAMING)
        streaming_started_us = time_us_32();

    dma_diag.armed = diagnostics_enabled &&
                     (s == USB_AUDIO_STREAMING);

    if (previous_state != s)
    {
#if LOOP_MEASUREMENT
        loop_measurement_invalidate();
#endif
        usb_audio_record_event(
            'S',
            (uint8_t)previous_state,
            (uint8_t)s,
            0,
            true,
            false);

        cdc_diagnostics_log_state_change(previous_state, s, false);
    }
}

void usb_audio_set_diagnostics_enabled(bool enabled)
{
	if (diagnostics_enabled == enabled)
		return;

	diagnostics_enabled = enabled;
	reset_pcm_candidate_history();
	if (enabled)
	{
		host_win_t0_us = time_us_32();
		host_win_frames0 = rx_frames_counter;
		pkt_win_count0 = pkt_count_total;
		pkt_win_bytes0 = pkt_bytes_total;
		loop0_prev_us = 0;
		dma_diag.loop_prev_us = 0;
		dma_diag.armed = stats.state == USB_AUDIO_STREAMING;
	}
	else
	{
		dma_diag.armed = false;
		loop0_prev_us = 0;
		dma_diag.loop_prev_us = 0;
	}
}

static void fb_reset(void)
{
	fb.level_f = (float)audio_ring_total_frames();
	fb.integ_hz = 0.0f;
	fb.last_us = time_us_32();
	fb.force_send = true;
	fb.transient_counter_us = 1000000u; // 【追加】切替後 1.0秒間は過渡期としてゲインを絞る
}

static void service_rx(void);

static int32_t win_prev_avg = 0; // previous 1 s window average (for win_slope)
static bool win_prev_valid = false;
static uint32_t win_start_us = 0;
static uint32_t win_count = 0;
static int64_t win_sum = 0;
static int32_t wmin = INT32_MAX;
static int32_t wmax = 0;

static void level_stats_reset(void)
{
	win_prev_valid = false;
	win_start_us = 0;
	win_count = 0;
	win_sum = 0;
	wmin = INT32_MAX;
	wmax = 0;
	stats.win_slope = 0;
	stats.level_min = INT32_MAX;
	stats.level_max = 0;
	stats.max_hf_diff = 0;
	stats.loop0_max_us = 0;
	stats.loop0_over_count = 0;
	loop0_prev_us = 0; // the next pass only sets the baseline
	dma_diag_reset();
	stats.win_avg = stats.win_min = stats.win_max = 0;
}

void usb_audio_reset_diagnostics(void)
{
	const uint32_t now = time_us_32();

	level_stats_reset();
	dma_diag_reset_counters();

	stats.overflow_count = 0;
	stats.underflow_count = 0;
	stats.fifo_resync_count = 0;
	stats.rate_limit_violation = 0;
	stats.switch_count = 0;
	stats.switch_timeout_count = 0;
	stats.switch_drain_us = 0;
	stats.output_stop_count = 0;
	stats.tail_block_count = 0;
	stats.drop_ovf_frames = 0;
	stats.drop_fifo_frames = 0;
	stats.drop_switch_frames = 0;
	stats.drop_idle_frames = 0;
	stats.drop_clear_frames = 0;
	stats.invalid_format_count = 0;
	stats.invalid_frame_count = 0;
	stats.drop_invalid_frames = 0;
	stats.click_candidate_count = 0;
	stats.max_click_curvature_q31 = 0;
	stats.loop1_max_us = 0;
	stats.loop1_over_count = 0;
	stats.rx_frames = rx_frames_counter;
	stats.level_now = (int32_t)audio_ring_total_frames();
	stats.ring_pool_now = stats.level_now + (int32_t)dma_tx_pool_frames();

	host_win_t0_us = now;
	host_win_frames0 = rx_frames_counter;
	host_rate_hz = 0;
	pkt_win_count0 = pkt_count_total;
	pkt_win_bytes0 = pkt_bytes_total;
	pkt_per_sec = 0;
	pkt_avg_bytes = 0;

	if (event_history_lock != NULL)
	{
		const uint32_t owner = spin_lock_blocking(event_history_lock);
		memset(evt_log, 0, sizeof(evt_log));
		memset(evt_time_us, 0, sizeof(evt_time_us));
		evt_total = 0;
		audio_history_total = 0;
		audio_history_last_us = now;
		memset(fault_history, 0, sizeof(fault_history));
		fault_history_count = 0;
		fault_history_post_remaining = 0;
		fault_history_valid = false;
		memset(anomaly_log_last_us, 0, sizeof(anomaly_log_last_us));
		spin_unlock(event_history_lock, owner);
	}
}

// Frames thrown away together with the Ring contents are accounted (drop_clear_frames)
static void clear_audio_buffers(void)
{
	reset_pcm_candidate_history();
	mute_ramp_reset(&usb_mute_ramp, audio_state.freq, audio_state.mute);
	const int64_t used = get_size_using(&buffer_audio);
	if (used > 0)
		stats.drop_clear_frames += (uint32_t)(used / 2);
	clear_ringbuffer(&buffer_audio);
}

// Flush the TinyUSB FIFO and return how many stereo frames it held
static uint32_t fifo_flush_frames(void)
{
	const uint32_t bytes = tud_audio_available();
	tud_audio_clear_ep_out_ff();
	return bytes / (2u * bytes_per_sample);
}

//--------------------------------------------------------------------+
// Sample-rate change (Ver1.2 switch state machine)
//
//   SET_CUR       -> remember pending_freq, state = SWITCHING (nothing else is touched yet)
//   SWITCHING     -> USB data is discarded and feedback is paused. Core1 keeps playing the Ring and the
//                    DMA pool with the OLD clock and stops by itself (enable_output = false) when both
//                    are empty (dma_tx_start()).
//   quiet         -> [Core1 locked out] renew_clock, clear Ring / USB FIFO, fb_reset
//   BUFFERING     -> the existing gate refills to the FB target and restarts PIO (reset_i2s_freq)
//
// Only Core0 takes part: no change is needed in dma_tx_start().
//--------------------------------------------------------------------+
static void switch_begin(uint32_t freq)
{
	pending_freq = freq;
	if (stats.state == USB_AUDIO_SWITCHING)
		return; // already draining: only the target rate changes, keep the start time

	switch_t0_us = time_us_32();
	set_state(USB_AUDIO_SWITCHING);
	usb_audio_log_event('W', (uint8_t)(freq / 1000u), 0);
}

// GET_CUR must already report the new rate while the switch is pending
uint32_t usb_audio_get_current_rate(void)
{
	return (stats.state == USB_AUDIO_SWITCHING) ? pending_freq : audio_state.freq;
}

void usb_audio_apply_sample_rate(uint32_t new_freq)
{
	    
	usb_audio_record_event(
        'R',
        (uint8_t)(new_freq / 1000u),
        0,
        0,
        true,
        false);

	if (stats.state != USB_AUDIO_SWITCHING && audio_state.freq == new_freq)
		return;

	switch_begin(new_freq);
}

// Called every usb_audio_task() pass, right after tud_task()
static void service_switch(void)
{
	if (stats.state != USB_AUDIO_SWITCHING)
		return;

	const uint32_t waited_us = time_us_32() - switch_t0_us;
	const bool quiet = !enable_output && (dma_tx.using == 0);
	const bool timeout = !quiet && (waited_us >= SWITCH_DRAIN_TIMEOUT_US);
	if (!quiet && !timeout)
		return;

	usb_audio_log_event('D', (uint8_t)(waited_us / 1000u), timeout ? 1 : 0);

	const bool freq_changed = (pending_freq != audio_state.freq);
	bool forced = timeout;

	multicore_lockout_start_blocking();

	// Check again with Core1 frozen: it may have (re)started output after the check above.
	if (enable_output || dma_tx.using != 0)
	{
		stats.drop_clear_frames += dma_tx_pool_frames(); // queued PCM that will never be played
		dma_stop_and_clear();
		forced = true;
	}

	if (freq_changed)
	{
		audio_state.freq = pending_freq;
		renew_clock();
	}
	clear_audio_buffers();
	const uint32_t flushed_frames = fifo_flush_frames();
	stats.drop_switch_frames += flushed_frames;
	fb_reset();
	level_stats_reset();

	multicore_lockout_end_blocking();

	if (freq_changed)
		stats.switch_count++;
	if (timeout)
		stats.switch_timeout_count++;
	stats.switch_drain_us = waited_us;

	// Full-Speed can not carry 24/32bit above 96kHz : do not pretend to play it
	rate_unsupported = (cur_alt != 0) && !stream_rate_supported(cur_alt, audio_state.freq);
	if (rate_unsupported)
		stats.rate_limit_violation++;

	usb_audio_log_event('K',
		 (uint8_t)(audio_state.freq / 1000u),
		  forced ? 1 : 0);

	usb_audio_record_event(
    'W',
    cur_alt,
    rate_unsupported ? 1 : 0,
    0,
    true,
    false);

	set_state((cur_alt != 0 && !rate_unsupported) ? USB_AUDIO_BUFFERING : USB_AUDIO_IDLE);
}

//--------------------------------------------------------------------+
// Stream open / close (AS interface alternate setting)
//--------------------------------------------------------------------+
bool usb_audio_stream_open(uint8_t alt)
{
	    usb_audio_record_event(
        'O',
        alt,
    stats.state,
        0,
        true,
        false);

    usb_audio_format_t format;
    if (!usb_audio_class_get_format(alt, &format))
        return false;

    const uint8_t prev_alt = cur_alt;

    cur_alt = alt;
    bytes_per_sample = format.bytes_per_sample;
    audio_state.bit_depth = format.bit_depth;

    if (prev_alt == alt &&
        (stats.state == USB_AUDIO_STREAMING ||
         stats.state == USB_AUDIO_BUFFERING))
    {
        return true;
    }

	// A rate change is in progress: service_switch() finishes it and picks up the alt recorded above.
	if (stats.state == USB_AUDIO_SWITCHING)
		return true;

	// Same-rate (re)open: the clock does not change, so there is nothing to wait for. Start a fresh
	// buffer epoch, but with Core1 frozen: clear_ringbuffer() takes no spinlock, so clearing while
	// Core1 reads the Ring could leave size_using negative.
	// Full-Speed can not carry 24/32bit above 96kHz : do not pretend to play it
	rate_unsupported = !stream_rate_supported(alt, audio_state.freq);
	if (rate_unsupported)
		stats.rate_limit_violation++;

	multicore_lockout_start_blocking();
	const uint32_t flushed_frames = fifo_flush_frames();
	stats.drop_clear_frames += flushed_frames;
	clear_audio_buffers();
	fb_reset();
	level_stats_reset();
	multicore_lockout_end_blocking();
	set_state(rate_unsupported ? USB_AUDIO_IDLE : USB_AUDIO_BUFFERING);
	return true;
}

void usb_audio_stream_close(void)
{
	    usb_audio_record_event(
        'C',
        cur_alt,
        stats.state,
        0,
        true,
        false);

	cur_alt = 0;
	rate_unsupported = false;
	// A pending rate change keeps running: it will end in IDLE because cur_alt is 0 now.
	// Otherwise Core1 simply plays out what is left in the Ring / DMA pool.
	if (stats.state != USB_AUDIO_SWITCHING)
		set_state(USB_AUDIO_IDLE);
	const uint32_t flushed_frames = fifo_flush_frames(); // FIFO tail the host sent just before alt 0
	stats.drop_clear_frames += flushed_frames;
}

//--------------------------------------------------------------------+
// PCM receive: TinyUSB FIFO -> 32-bit I2S samples -> audio ring buffer
//--------------------------------------------------------------------+
static inline int32_t rd_s16(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24));
}

static inline int32_t rd_s24(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
}

static inline int32_t rd_s32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void reset_pcm_candidate_history(void)
{
	memset(&pcm_candidate_history, 0, sizeof(pcm_candidate_history));
}

static uint32_t __not_in_flash_func(scan_pcm_click_candidate)(const uint8_t *src,
															 uint32_t frames, uint8_t bps,
															 uint8_t *threshold_hits)
{
	uint64_t max_curvature = 0;
	*threshold_hits = 0;

	for (uint32_t i = 0; i < frames; i++, src += 2u * bps)
	{
		int32_t left;
		int32_t right;
		if (bps == 4u)
		{
			left = rd_s32(src);
			right = rd_s32(src + 4);
		}
		else if (bps == 3u)
		{
			left = rd_s24(src);
			right = rd_s24(src + 3);
		}
		else
		{
			left = rd_s16(src);
			right = rd_s16(src + 2);
		}

		if (pcm_candidate_history.history_count >= 2u)
		{
			const int64_t curvature_left = (int64_t)left -
				(2 * (int64_t)pcm_candidate_history.previous_left) +
				pcm_candidate_history.previous2_left;
			const int64_t curvature_right = (int64_t)right -
				(2 * (int64_t)pcm_candidate_history.previous_right) +
				pcm_candidate_history.previous2_right;
			const uint64_t abs_left = curvature_left < 0
				? (uint64_t)(-curvature_left) : (uint64_t)curvature_left;
			const uint64_t abs_right = curvature_right < 0
				? (uint64_t)(-curvature_right) : (uint64_t)curvature_right;
			const uint64_t curvature = abs_left > abs_right ? abs_left : abs_right;
			if (curvature > max_curvature)
				max_curvature = curvature;
			if ((curvature + 1u) / 2u >= PCM_CLICK_CANDIDATE_THRESHOLD_Q31 &&
				*threshold_hits <= PCM_CLICK_CANDIDATE_MAX_HITS_PER_BLOCK)
				(*threshold_hits)++;
		}

		pcm_candidate_history.previous2_left = pcm_candidate_history.previous_left;
		pcm_candidate_history.previous_left = left;
		pcm_candidate_history.previous2_right = pcm_candidate_history.previous_right;
		pcm_candidate_history.previous_right = right;
		if (pcm_candidate_history.history_count < 2u)
			pcm_candidate_history.history_count++;
	}

	const uint64_t curvature_q31 = (max_curvature + 1u) / 2u;
	return curvature_q31 > UINT32_MAX ? UINT32_MAX : (uint32_t)curvature_q31;
}

static void __not_in_flash_func(pcm_to_float)(const uint8_t *src, uint32_t frames, uint8_t bps, float *left, float *right)
{
	switch (bps)
	{
	case 4:
		for (uint32_t i = 0; i < frames; i++, src += 8)
		{
			left[i] = (float)rd_s32(src);
			right[i] = (float)rd_s32(src + 4);
		}
		break;
	case 3:
		for (uint32_t i = 0; i < frames; i++, src += 6)
		{
			left[i] = (float)rd_s24(src);
			right[i] = (float)rd_s24(src + 3);
		}
		break;
	case 2:
	default:
		for (uint32_t i = 0; i < frames; i++, src += 4)
		{
			left[i] = (float)rd_s16(src);
			right[i] = (float)rd_s16(src + 2);
		}
		break;
	}
}

static void report_peak(const float *left, const float *right, uint32_t frames)
{
	float peak_l = 0.0f, peak_r = 0.0f;
	for (uint32_t i = 0; i < frames; i++)
	{
		float abs_l = fabsf(left[i]);
		float abs_r = fabsf(right[i]);
		if (abs_l > peak_l)
			peak_l = abs_l;
		if (abs_r > peak_r)
			peak_r = abs_r;
	}

	float db_l = 20.0f * log10f(fmaxf(peak_l, 1.0f) / 2147483647.0f);
	float db_r = 20.0f * log10f(fmaxf(peak_r, 1.0f) / 2147483647.0f);
	// -80dBFS .. +3dBFS -> 0..100
	float level_l_f = ((db_l + 80.0f) / 83.0f) * 100.0f;
	float level_r_f = ((db_r + 80.0f) / 83.0f) * 100.0f;
	uint8_t level_l = (uint8_t)fminf(fmaxf(level_l_f, 0.0f), 100.0f);
	uint8_t level_r = (uint8_t)fminf(fmaxf(level_r_f, 0.0f), 100.0f);
	display_rta_set_peak(level_l, level_r);
}

static void discard_invalid_pcm(uint8_t anomaly_code, const char *detail_name,
								uint32_t detail_value, uint32_t frame_bytes,
								uint32_t bytes_already_read)
{
	const uint32_t remaining_bytes = tud_audio_available();
	reset_pcm_candidate_history();
	if (frame_bytes != 0)
	{
		const uint32_t dropped_frames = (bytes_already_read + remaining_bytes) / frame_bytes;
		rx_frames_counter += dropped_frames;
		stats.drop_invalid_frames += dropped_frames;
	}

	tud_audio_clear_ep_out_ff();
	if (anomaly_code == 11)
		stats.invalid_format_count++;
	else
		stats.invalid_frame_count++;
	stats.fifo_resync_count++;
	usb_audio_log_anomaly(anomaly_code, (uint8_t)detail_value, detail_name, detail_value);
}

static void service_rx(void)
{
	const uint8_t bps = bytes_per_sample;
	if (bps != 2u && bps != 3u && bps != 4u)
	{
		if (tud_audio_available() != 0)
			discard_invalid_pcm(11, "BYTES_PER_SAMPLE", bps, 0, 0);
		return;
	}

	const uint32_t frame_bytes = 2u * bps;
	const usb_audio_state_t st = stats.state;
	if (st == USB_AUDIO_IDLE || st == USB_AUDIO_OVERFLOW_FATAL || st == USB_AUDIO_SWITCHING)
	{
    	const uint16_t discard = tud_audio_available();

    	if (discard)
    	{
			reset_pcm_candidate_history();
        	{
				const uint32_t lost = discard / frame_bytes;
				rx_frames_counter += lost;
				if (st == USB_AUDIO_SWITCHING)
					stats.drop_switch_frames += lost;
				else if (st == USB_AUDIO_IDLE)
					stats.drop_idle_frames += lost;
				else
				{
					stats.drop_ovf_frames += lost;
					usb_audio_log_anomaly(2, (uint8_t)(lost / 16u), "DROP_FRAMES", lost);
				}
			}
        	tud_audio_clear_ep_out_ff();
    	}

    	return;
	}

	uint16_t avail = tud_audio_available();
	if (avail % frame_bytes != 0)
	{
		discard_invalid_pcm(12, "AVAILABLE_BYTES", avail, frame_bytes, 0);
		return;
	}

	const uint16_t fifo_depth = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
	if (avail > (fifo_depth / 4u) * 3u)
	{
		reset_pcm_candidate_history();
    	{
			const uint32_t lost = avail / frame_bytes;
			rx_frames_counter += lost;
			stats.drop_fifo_frames += lost;
		}

    	tud_audio_clear_ep_out_ff();

		stats.fifo_resync_count++;
		usb_audio_log_anomaly(3, (uint8_t)(avail / 16u), "BYTES", avail);

    	return;
	}

	while (avail >= frame_bytes)
	{
		int64_t space = get_size_remain(&buffer_audio) / 2;
		if (space <= 0)
			break;

		uint32_t frames = avail / frame_bytes;
		if (frames > RX_CHUNK_FRAMES)
			frames = RX_CHUNK_FRAMES;
		if ((int64_t)frames > space)
			frames = (uint32_t)space;

		const uint16_t requested_bytes = (uint16_t)(frames * frame_bytes);
		const uint16_t got = tud_audio_read(rx_raw, requested_bytes);
		if (got > requested_bytes)
		{
			discard_invalid_pcm(12, "READ_BYTES", got, frame_bytes, got);
			return;
		}
		if (got % frame_bytes != 0)
		{
			discard_invalid_pcm(12, "READ_REMAINDER", got % frame_bytes,
								frame_bytes, got);
			return;
		}
		frames = got / frame_bytes;

			if (frames == 0)
    		break;

		rx_frames_counter += frames;

		if (diagnostics_enabled)
		{
			uint8_t threshold_hits;
			const uint32_t curvature_q31 = scan_pcm_click_candidate(rx_raw, frames, bps,
																	&threshold_hits);
			if (curvature_q31 >= PCM_CLICK_CANDIDATE_THRESHOLD_Q31 &&
				threshold_hits != 0 &&
				threshold_hits <= PCM_CLICK_CANDIDATE_MAX_HITS_PER_BLOCK)
			{
				stats.click_candidate_count++;
				if (curvature_q31 > stats.max_click_curvature_q31)
					stats.max_click_curvature_q31 = curvature_q31;
				usb_audio_log_anomaly(13, (uint8_t)(curvature_q31 >> 24),
									  "CURVATURE_Q31", curvature_q31);
			}
		}

		pcm_to_float(rx_raw, frames, bps, ep_Lch, ep_Rch);
		const float scale = audio_state.vol_float * DEFAULT_GAIN_RATIO;
		const bool muted = audio_state.mute;
		const mute_ramp_t ramp_before = usb_mute_ramp;
		if ((!muted && usb_mute_ramp.position == usb_mute_ramp.frames) ||
			(muted && usb_mute_ramp.position == 0u))
		{
			const float sample_scale = muted ? 0.0f : scale;
			for (uint32_t i = 0; i < frames; ++i)
			{
				ep_Lch[i] *= sample_scale;
				ep_Rch[i] *= sample_scale;
				pcm_stereo[2u * i] = (int32_t)ep_Lch[i];
				pcm_stereo[2u * i + 1u] = (int32_t)ep_Rch[i];
			}
		}
		else
		{
			const float ramp_unit = 1.0f / (float)usb_mute_ramp.frames;
			for (uint32_t i = 0; i < frames; ++i)
			{
				const uint32_t position = mute_ramp_next(&usb_mute_ramp, muted);
				const float sample_scale = position == usb_mute_ramp.frames ? scale :
					(position == 0u ? 0.0f : scale * ((float)position * ramp_unit));
				ep_Lch[i] *= sample_scale;
				ep_Rch[i] *= sample_scale;
				pcm_stereo[2u * i] = (int32_t)ep_Lch[i];
				pcm_stereo[2u * i + 1u] = (int32_t)ep_Rch[i];
			}
		}

		if (display_rta_spectrum_active())
		{
			report_peak(ep_Lch, ep_Rch, frames);
			spectrum_feed_block(ep_Lch, ep_Rch, frames);
		}
		if (ringbuf_write_array_spinlock(pcm_stereo, frames * 2u, &buffer_audio) < 0)
		{
			reset_pcm_candidate_history();
			usb_mute_ramp = ramp_before;
			stats.drop_ovf_frames += frames; // already read from the FIFO, the Ring refused it
			stats.overflow_count++;
			usb_audio_log_anomaly(2, (uint8_t)(frames / 16u), "DROP_FRAMES", frames);
			set_state(USB_AUDIO_OVERFLOW_FATAL);
			break;
		}

		audio_last_data_us = time_us_32();
		audio_data_seen = true;
		now_playing++;
		avail = tud_audio_available();
	}
}

//--------------------------------------------------------------------+
// State machine (IDLE / BUFFERING / STREAMING / OVERFLOW_FATAL / UNDERFLOW_FATAL)
//--------------------------------------------------------------------+
static void service_state(void)
{
	const int32_t level = (int32_t)audio_ring_total_frames();
	const int32_t dma_pool_frames = (int32_t)dma_tx_pool_frames();
	const int32_t ring_pool_level = level + dma_pool_frames;
	if (diagnostics_enabled)
	{
		stats.level_now = level;
		stats.ring_pool_now = ring_pool_level;
		stats.output_stop_count = dma_diag.output_stop_count;
		stats.rx_frames = rx_frames_counter;
		stats.loop1_max_us = dma_diag.loop_max_us;
		stats.loop1_over_count = dma_diag.loop_over_count;
		stats.tail_block_count = dma_diag.tail_block_count;
	}

	switch (stats.state)
	{
	case USB_AUDIO_BUFFERING:
	{
    	uint32_t prefill_frames =
        	(audio_state.freq * TARGET_PREFILL_MS) / 1000u;

		if (prefill_frames < FB_TARGET_LEVEL)
			prefill_frames = FB_TARGET_LEVEL;

    	if (prefill_frames >= SIZE_AUDIO_RING_FRAMES)
        	prefill_frames = SIZE_AUDIO_RING_FRAMES - 1u;

    	if ((uint32_t)level >= prefill_frames)
    	{
        	level_stats_reset();
        	set_state(USB_AUDIO_STREAMING);
    	}
    	break;
	}

	case USB_AUDIO_STREAMING:
		// MIN / MAX are collected by Core1 at every Ring fetch (dma_tx_start). The level sampled here
		// would miss the minimum right after a block was taken.
		if (diagnostics_enabled)
		{
			stats.level_min = dma_diag.level_min;
			stats.level_max = dma_diag.level_max;
		}

		// 1 s window statistics (diagnostics only)
		if (diagnostics_enabled)
		{
			const uint32_t now_us = time_us_32();
			if (win_count == 0)
				win_start_us = now_us;
			win_sum += level;
			win_count++;
			if (level < wmin)
				wmin = level;
			if (level > wmax)
				wmax = level;
			if ((uint32_t)(now_us - win_start_us) >= 1000000u)
			{
				const int32_t avg = (int32_t)(win_sum / (int64_t)win_count);
				stats.win_slope = win_prev_valid ? (avg - win_prev_avg) : 0;
				win_prev_avg = avg;
				win_prev_valid = true;
				stats.win_avg = avg;
				stats.win_min = wmin;
				stats.win_max = wmax;
				win_sum = 0;
				win_count = 0;
				wmin = INT32_MAX;
				wmax = 0;
			}
		}

		if (level >= LEVEL_HIGH_MARK)
		{
			stats.overflow_count++;
			usb_audio_log_anomaly(2, (uint8_t)(level / 16), "LEVEL", (uint32_t)level);
			set_state(USB_AUDIO_OVERFLOW_FATAL);
		}
		else if (ring_pool_level <= LEVEL_LOW_MARK)
		{
			stats.underflow_count++;
			usb_audio_log_anomaly(1, (uint8_t)(ring_pool_level / 16),
								  "RING_POOL", (uint32_t)ring_pool_level);
			set_state(USB_AUDIO_UNDERFLOW_FATAL);
		}
		break;

	case USB_AUDIO_OVERFLOW_FATAL:
		// DMA / PIO keep draining the buffer; USB data is discarded in service_rx()
		if (level <= LEVEL_OVERFLOW_RECOVER)
			set_state(USB_AUDIO_BUFFERING);
		break;

	case USB_AUDIO_UNDERFLOW_FATAL:
		// USB data keeps accumulating
		if (level >= LEVEL_UNDERFLOW_RECOVER)
			set_state(USB_AUDIO_BUFFERING);
		break;

	case USB_AUDIO_SWITCHING: // handled by service_switch()
	case USB_AUDIO_IDLE:
	default:
		break;
	}
}

//--------------------------------------------------------------------+
// Feedback (PI control on buffer_audio fill level)
//--------------------------------------------------------------------+
static void service_feedback(void)
{
const usb_audio_state_t st = stats.state;
if (st == USB_AUDIO_IDLE || st == USB_AUDIO_SWITCHING)
return;

const uint32_t now = time_us_32();
const uint32_t dt_us = now - fb.last_us;
if (!fb.force_send && dt_us < FB_UPDATE_PERIOD_US)
    return;
fb.last_us = now;

float dt = (float)dt_us * 1e-6f;
if (dt > 0.02f)
    dt = 0.02f;
// 【ここから追加】過渡期カウンタの減衰処理とゲインのスケール
    float transient_scale = 1.0f;
    if (fb.transient_counter_us > 0)
    {
        if (fb.transient_counter_us > dt_us)
            fb.transient_counter_us -= dt_us;
        else
            fb.transient_counter_us = 0;

        // 切替直後はゲインを 0.25倍からスタートし、1秒かけて 1.0倍 に滑らかに戻す
        float progress = 1.0f - ((float)fb.transient_counter_us / 1000000.0f);
        transient_scale = 0.25f + 0.75f * progress;
    }

    float effective_ki = FB_KI * transient_scale;
    float effective_kp = FB_KP * transient_scale;
const uint32_t fs = audio_state.freq;
const float level = (float)audio_ring_total_frames();

// Low-pass the level so that block-wise Core0 / DMA access does not leak into the feedback
const float alpha = dt / (FB_LEVEL_TAU_S + dt);
fb.level_f += (level - fb.level_f) * alpha;

float adj_hz = 0.0f;
#if FIXED_FEEDBACK_TEST
// Diagnostic: control OFF, constant (usually zero) offset
fb.integ_hz = 0.0f;
if (st != USB_AUDIO_BUFFERING)
adj_hz = FIXED_FEEDBACK_OFFSET_HZ;
#else
if (st == USB_AUDIO_BUFFERING)
{
    fb.integ_hz = 0.0f;
	// BUFFERING中はバッファを早く溜めるため、強制的に定常レートより +0.025% 高い要求を送る
    // 例: 192000Hz なら +0.00025f (+0.025%)250 ppm
    adj_hz = (float)fs * 0.00030f;
}
else
{

// error in input-rate samples
const float err = (float)FB_TARGET_LEVEL - fb.level_f;

float dynamic_limit_ppm = FB_BASE_LIMIT_PPM;

if (err > FB_BOOST_START_ERR)
{
    float ratio =
        (err - FB_BOOST_START_ERR) /
        (FB_BOOST_FULL_ERR - FB_BOOST_START_ERR);

    if (ratio > 1.0f)
        ratio = 1.0f;

    dynamic_limit_ppm =
        FB_BASE_LIMIT_PPM +
        (FB_BOOST_MAX_PPM - FB_BASE_LIMIT_PPM) * ratio;
}

float lim =
    (float)fs * dynamic_limit_ppm * 1e-6f;

if (lim > (float)FB_ADJ_LIMIT)
    lim = (float)FB_ADJ_LIMIT;

// H-F difference correction.
const float host_rate = (float)host_rate_hz;
const float fb_rate   = stats.feedback_hz;

float hf_error = 0.0f;

if (host_rate > 0.0f && fb_rate > 0.0f)
{
    hf_error = host_rate - fb_rate;
}

float p =
    effective_kp * err +     // 【変更】FB_KP から effective_kp に変更
    FB_HF_GAIN * hf_error;

float integ_next = fb.integ_hz + effective_ki * err * dt; // 【変更】FB_KI から effective_ki に変更

float integ_lim =
    (float)fs * FB_BASE_LIMIT_PPM * 1e-6f;

if (integ_lim > (float)FB_ADJ_LIMIT)
    integ_lim = (float)FB_ADJ_LIMIT;

if (integ_next > integ_lim)
    integ_next = integ_lim;

if (integ_next < -integ_lim)
    integ_next = -integ_lim;

adj_hz = p + integ_next;

if (adj_hz > lim)
{
    adj_hz = lim;
}
else if (adj_hz < -lim)
{
    adj_hz = -lim;
}

fb.integ_hz = integ_next;

}

#endif

const float rate_hz = (float)fs + adj_hz;
stats.feedback_hz = rate_hz;

// 16.16 samples per 1 ms frame
const uint32_t value = (uint32_t)((double)rate_hz * 65.536 + 0.5);
if (fb.force_send || value != fb.last_fb)
{
    tud_audio_fb_set(value);
    fb.last_fb = value;
    fb.force_send = false;
}
}

static void service_host_rate(void)
{
    const uint32_t now = time_us_32();

    if (stats.state == USB_AUDIO_IDLE || stats.state == USB_AUDIO_SWITCHING)
    {
        host_rate_hz = 0;
        pkt_per_sec = 0;
        pkt_avg_bytes = 0;

        host_win_t0_us = now;
        host_win_frames0 = rx_frames_counter;

        pkt_win_count0 = pkt_count_total;
        pkt_win_bytes0 = pkt_bytes_total;

        return;
    }

    const uint32_t dt = now - host_win_t0_us;

    if (dt < HOST_RATE_UPDATE_US)
        return;

    host_rate_hz =
        (uint32_t)(
            ((uint64_t)(rx_frames_counter - host_win_frames0) * 1000000ull
             + dt / 2u) / dt);

    {
        const uint32_t cnt   = pkt_count_total;
        const uint32_t bytes = pkt_bytes_total;

        const uint32_t dcnt   = cnt   - pkt_win_count0;
        const uint32_t dbytes = bytes - pkt_win_bytes0;

        pkt_per_sec =
            (uint32_t)(((uint64_t)dcnt * 1000000ull + dt / 2u) / dt);

        pkt_avg_bytes =
            dcnt ? (dbytes + dcnt / 2u) / dcnt : 0u;

        pkt_win_count0 = cnt;
        pkt_win_bytes0 = bytes;
    }

    // MAX HF: largest |H - F| after the first second of STREAMING.
    if (diagnostics_enabled && stats.state == USB_AUDIO_STREAMING &&
        (uint32_t)(now - streaming_started_us) >= HF_DIFF_SETTLE_TIME_US &&
        host_rate_hz != 0u && stats.feedback_hz > 0.0f)
    {
        int32_t d = (int32_t)host_rate_hz - (int32_t)(stats.feedback_hz + 0.5f);
        if (d < 0)
            d = -d;
        if (d > stats.max_hf_diff)
            stats.max_hf_diff = d;
    }

    host_win_t0_us = now;
    host_win_frames0 = rx_frames_counter;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+
void usb_sound_card_init(void)
{
#if USB_BRINGUP_LEVEL == 0
	return;
#endif
	event_history_lock = spin_lock_init(spin_lock_claim_unused(true));
	memset(&stats, 0, sizeof(stats));
	level_stats_reset();
	set_state(USB_AUDIO_IDLE);

	audio_state.acq_volume = DEFAULT_VOLUME;
	volume_control();
	mute_ramp_reset(&usb_mute_ramp, audio_state.freq, audio_state.mute);

	tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
}

void usb_audio_task(void)
{
#if USB_BRINGUP_LEVEL == 0
    return;
#endif

    // Loop-interval diagnostics: gap between two calls = everything else Core0 does in main()
    // (SPI drawing, ...) plus the work inside this function
    const uint32_t loop_now_us = time_us_32();
#if LOOP_MEASUREMENT
    loop_measurement_sample_core0(loop_now_us);
#endif
    static uint32_t anomaly_loop_prev_us;
    if (stats.state == USB_AUDIO_STREAMING && anomaly_loop_prev_us != 0)
    {
        const uint32_t gap_us = loop_now_us - anomaly_loop_prev_us;
        if (gap_us >= 5000u)
            usb_audio_log_anomaly(6, (uint8_t)(gap_us / 1000u), "GAP_US", gap_us);
    }
    anomaly_loop_prev_us = loop_now_us;
    if (diagnostics_enabled && stats.state == USB_AUDIO_STREAMING && loop0_prev_us != 0)
    {
        const uint32_t gap_us = loop_now_us - loop0_prev_us;
        if (gap_us > stats.loop0_max_us)
            stats.loop0_max_us = gap_us;
        if (gap_us > LOOP0_WARN_US)
            stats.loop0_over_count++;
    }
    loop0_prev_us = diagnostics_enabled ? loop_now_us : 0;

    tud_task();
    service_rx_packet_gap();

#if USB_BRINGUP_LEVEL == 1
    return;
#endif

    service_switch();
    service_rx();
    service_state();
    service_feedback();
    if (diagnostics_enabled)
        service_host_rate();
    record_audio_history();
}

usb_audio_state_t usb_audio_get_state(void)
{
	return stats.state;
}

const usb_audio_stats_t *usb_audio_get_stats(void)
{
	return &stats;
}

uint32_t usb_audio_get_host_rate_hz(void)
{
    return host_rate_hz;
}

uint32_t usb_audio_get_feedback_hz(void)
{
    return (stats.state == USB_AUDIO_IDLE || stats.state == USB_AUDIO_SWITCHING)
        ? 0u
        : (uint32_t)(stats.feedback_hz + 0.5f);
}

uint32_t usb_audio_get_feedback_limit_hz(void)
{
    float lim = (float)audio_state.freq * FB_LIMIT_PPM * 1e-6f;

    if (lim > (float)FB_ADJ_LIMIT)
        lim = (float)FB_ADJ_LIMIT;

    return (uint32_t)(lim + 0.5f);
}

uint32_t usb_audio_get_packets_per_sec(void)
{
    return pkt_per_sec;
}

uint32_t usb_audio_get_avg_packet_bytes(void)
{
    return pkt_avg_bytes;
}