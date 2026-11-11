#include <limits.h>
#include <stdio.h>
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "cdc_diagnostics.h"
#include "common.h"
#include "transmit_to_dac.h"
#include "usb_audio_config.h"
#include "usb_device_control.h"

#if USB_CDC_ENABLED

#define CDC_LOG_PERIOD_US 100000u
#define CDC_EVENT_QUEUE_SIZE 16u
#define CDC_CSV_LINE_SIZE 192u
#define STRINGIFY_DETAIL(value) #value
#define STRINGIFY(value) STRINGIFY_DETAIL(value)

#if USB_AUDIO_CLASS_VERSION == 1
#define USB_MODE_NAME "UAC1"
#else
#define USB_MODE_NAME "UAC2"
#endif

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

typedef struct
{
	uint32_t timestamp_us;
	uint32_t detail_value;
	uint32_t sample_rate;
	uint32_t ring_frames;
	uint32_t dma_pool_frames;
	uint8_t code;
	usb_audio_state_t state;
	char detail_name[20];
} cdc_anomaly_t;

static spin_lock_t *event_lock;
static cdc_anomaly_t event_queue[CDC_EVENT_QUEUE_SIZE];
static uint8_t event_head;
static uint8_t event_count;
static uint32_t event_drop_count;
static uint32_t row_drop_count;
static volatile bool session_active;
static uint32_t session_start_us;
static uint32_t next_sample_us;
static int32_t ring_min;
static int32_t ring_max;
static uint32_t max_hf_diff_session;
static bool diagnostics_enabled;

static const char *anomaly_name(uint8_t code)
{
	switch (code)
	{
	case 1: return "UNDERFLOW";
	case 2: return "OVERFLOW";
	case 3: return "FIFO_RESYNC";
	case 4: return "USB_EVENT";
	case 5: return "OUTPUT_STOP";
	case 6: return "CORE0_LOOP_GAP";
	case 7: return "CORE1_LOOP_GAP";
	case 8: return "USB_OUT_GAP";
	case 9: return "PIO_TXSTALL";
	case 10: return "DMA_AHB_ERROR";
	case 11: return "PCM_FORMAT";
	case 12: return "PCM_FRAME";
	case 13: return "PCM_CLICK_CANDIDATE";
	default: return "UNKNOWN";
	}
}

static const char *audio_state_name(usb_audio_state_t state)
{
	switch (state)
	{
	case USB_AUDIO_IDLE: return "IDLE";
	case USB_AUDIO_BUFFERING: return "BUFFERING";
	case USB_AUDIO_STREAMING: return "STREAMING";
	case USB_AUDIO_OVERFLOW_FATAL: return "OVERFLOW_FATAL";
	case USB_AUDIO_UNDERFLOW_FATAL: return "UNDERFLOW_FATAL";
	case USB_AUDIO_SWITCHING: return "SWITCHING";
	default: return "UNKNOWN";
	}
}

static bool cdc_write(const char *text, uint32_t length)
{
	if (tud_cdc_n_write_available(1) < length)
		return false;

	if (tud_cdc_n_write(1, text, length) != length)
		return false;

	tud_cdc_n_write_flush(1);
	return true;
}

static uint32_t session_milliseconds(uint32_t timestamp_us)
{
	return (timestamp_us - session_start_us) / 1000u;
}

static void reset_event_queue(void)
{
	const uint32_t owner = spin_lock_blocking(event_lock);
	event_head = 0;
	event_count = 0;
	event_drop_count = 0;
	spin_unlock(event_lock, owner);
}

static bool start_session(uint32_t now)
{
	static const char headers[] =
		"#FW_VERSION=" FW_VERSION ",PREFILL_FRAMES=" STRINGIFY(USB_AUDIO_PREFILL_FRAMES) ",PREFILL_MS=" STRINGIFY(TARGET_PREFILL_MS) ",USB_MODE=" USB_MODE_NAME "\r\n"
		"MS,FREQ,BIT_DEPTH,USB_MODE,HOST_RATE,FEEDBACK_RATE,HF_DIFF_NOW,RING_NOW,RING_MIN,RING_MAX,MAX_HF_DIFF_SESSION,MAX_HF_DIFF_STREAM,STATE\r\n"
		"STATE,MS,FROM,TO,RATE,RING,DMA_POOL,USB_MODE\r\n"
		"ANOMALY,MS,CODE,DETAIL,USB_MODE,STATE,RATE,RING,DMA_POOL\r\n";

	if (!cdc_write(headers, sizeof(headers) - 1u))
		return false;

	reset_event_queue();
	row_drop_count = 0;
	session_start_us = now;
	next_sample_us = now;
	ring_min = INT32_MAX;
	ring_max = 0;
	max_hf_diff_session = 0;
	session_active = true;
	cdc_diagnostics_log_state_change(USB_AUDIO_IDLE, usb_audio_get_state(), true);
	return true;
}

static void sample_ring_level(void)
{
	const int32_t level = (int32_t)audio_ring_total_frames();
	if (level < ring_min)
		ring_min = level;
	if (level > ring_max)
		ring_max = level;

	const uint32_t host_rate = usb_audio_get_host_rate_hz();
	const uint32_t feedback_rate = usb_audio_get_feedback_hz();
	if (host_rate != 0u && feedback_rate != 0u)
	{
		const uint32_t difference = host_rate > feedback_rate
			? host_rate - feedback_rate
			: feedback_rate - host_rate;
		if (difference > max_hf_diff_session)
			max_hf_diff_session = difference;
	}
}

static bool write_sample(uint32_t now)
{
	char line[CDC_CSV_LINE_SIZE];
	const uint32_t host_rate = usb_audio_get_host_rate_hz();
	const uint32_t feedback_rate = usb_audio_get_feedback_hz();
	const int32_t hf_diff_now = (int32_t)host_rate - (int32_t)feedback_rate;
	char hf_diff_text[16];
	if (host_rate == 0u || feedback_rate == 0u)
		hf_diff_text[0] = '\0';
	else
		snprintf(hf_diff_text, sizeof(hf_diff_text), "%ld", (long)hf_diff_now);
	const uint32_t milliseconds = session_milliseconds(now);
	const int32_t level = (int32_t)audio_ring_total_frames();
	const usb_audio_stats_t *stats = usb_audio_get_stats();
	const int written = snprintf(line, sizeof(line),
		"%lu,%lu,%lu,%s,%lu,%lu,%s,%ld,%ld,%ld,%lu,%ld,%s\r\n",
		(unsigned long)milliseconds,
		(unsigned long)audio_state.freq,
		(unsigned long)audio_state.bit_depth,
		USB_MODE_NAME,
		(unsigned long)host_rate,
		(unsigned long)feedback_rate,
		hf_diff_text,
		(long)level,
		(long)(ring_min == INT32_MAX ? level : ring_min),
		(long)ring_max,
		(unsigned long)max_hf_diff_session,
		(long)stats->max_hf_diff,
		audio_state_name(stats->state));

	if (written < 0 || (size_t)written >= sizeof(line))
		return false;
	const bool sent = cdc_write(line, (uint32_t)written);

	ring_min = level;
	ring_max = level;
	next_sample_us = now + CDC_LOG_PERIOD_US;
	return sent;
}

static bool peek_event(cdc_anomaly_t *event)
{
	const uint32_t owner = spin_lock_blocking(event_lock);
	if (event_count == 0)
	{
		spin_unlock(event_lock, owner);
		return false;
	}

	*event = event_queue[event_head];
	spin_unlock(event_lock, owner);
	return true;
}

static void pop_event(void)
{
	const uint32_t owner = spin_lock_blocking(event_lock);
	if (event_count != 0)
	{
		event_head = (uint8_t)((event_head + 1u) % CDC_EVENT_QUEUE_SIZE);
		event_count--;
	}
	spin_unlock(event_lock, owner);
}

static void write_pending_anomalies(uint32_t now)
{
	cdc_anomaly_t event;
	char line[CDC_CSV_LINE_SIZE];

	while (peek_event(&event))
	{
		const int written = event.code == 0
			? snprintf(line, sizeof(line),
				"STATE,%lu,%s,%s,%lu,%lu,%lu,%s\r\n",
				(unsigned long)session_milliseconds(event.timestamp_us),
				event.detail_name,
				audio_state_name(event.state),
				(unsigned long)event.sample_rate,
				(unsigned long)event.ring_frames,
				(unsigned long)event.dma_pool_frames,
				USB_MODE_NAME)
			: snprintf(line, sizeof(line),
				"ANOMALY,%lu,%s,%s=%lu,%s,%s,%lu,%lu,%lu\r\n",
				(unsigned long)session_milliseconds(event.timestamp_us),
				anomaly_name(event.code),
				event.detail_name[0] ? event.detail_name : "VALUE",
				(unsigned long)event.detail_value,
				USB_MODE_NAME,
				audio_state_name(event.state),
				(unsigned long)event.sample_rate,
				(unsigned long)event.ring_frames,
				(unsigned long)event.dma_pool_frames);
		if (written < 0 || (size_t)written >= sizeof(line) ||
			!cdc_write(line, (uint32_t)written))
			return;
		pop_event();
	}

	const uint32_t owner = spin_lock_blocking(event_lock);
	const uint32_t dropped = event_drop_count;
	spin_unlock(event_lock, owner);
	if (dropped != 0)
	{
		const int written = snprintf(line, sizeof(line),
			"ANOMALY,%lu,EVENT_QUEUE_DROPPED,COUNT=%lu,%s,%s,%lu,%lu,%lu\r\n",
			(unsigned long)session_milliseconds(now), (unsigned long)dropped, USB_MODE_NAME,
			audio_state_name(usb_audio_get_state()), (unsigned long)audio_state.freq,
			(unsigned long)audio_ring_total_frames(), (unsigned long)dma_tx_pool_frames());
		if (written > 0 && (size_t)written < sizeof(line) &&
			cdc_write(line, (uint32_t)written))
		{
			const uint32_t clear_owner = spin_lock_blocking(event_lock);
			event_drop_count = event_drop_count >= dropped
				? event_drop_count - dropped : 0;
			spin_unlock(event_lock, clear_owner);
		}
	}

	if (row_drop_count != 0)
	{
		const int written = snprintf(line, sizeof(line),
			"ANOMALY,%lu,LOG_ROWS_DROPPED,COUNT=%lu,%s,%s,%lu,%lu,%lu\r\n",
			(unsigned long)session_milliseconds(now), (unsigned long)row_drop_count, USB_MODE_NAME,
			audio_state_name(usb_audio_get_state()), (unsigned long)audio_state.freq,
			(unsigned long)audio_ring_total_frames(), (unsigned long)dma_tx_pool_frames());
		if (written > 0 && (size_t)written < sizeof(line) &&
			cdc_write(line, (uint32_t)written))
			row_drop_count = 0;
	}
}

void cdc_diagnostics_init(void)
{
	if (!COM_PORT_OUTPUT_ENABLE)
		return;

	event_lock = spin_lock_init(spin_lock_claim_unused(true));
}

void cdc_diagnostics_set_enabled(bool enabled)
{
	diagnostics_enabled = enabled && COM_PORT_OUTPUT_ENABLE;
}

bool cdc_diagnostics_is_connected(void)
{
	return COM_PORT_OUTPUT_ENABLE && tud_cdc_n_connected(1);
}

void cdc_diagnostics_log_anomaly(uint8_t code, const char *detail_name, uint32_t detail_value)
{
	if (!COM_PORT_OUTPUT_ENABLE || !diagnostics_enabled || !session_active || event_lock == NULL)
		return;

	const uint32_t owner = spin_lock_blocking(event_lock);
	if (event_count == CDC_EVENT_QUEUE_SIZE)
	{
		event_drop_count++;
		spin_unlock(event_lock, owner);
		return;
	}

	const uint8_t index = (uint8_t)((event_head + event_count) % CDC_EVENT_QUEUE_SIZE);
	cdc_anomaly_t *event = &event_queue[index];
	event->timestamp_us = time_us_32();
	event->code = code;
	event->detail_value = detail_value;
	event->sample_rate = audio_state.freq;
	event->ring_frames = (uint32_t)audio_ring_total_frames();
	event->dma_pool_frames = dma_tx_pool_frames();
	event->state = usb_audio_get_state();
	if (detail_name == NULL)
		event->detail_name[0] = '\0';
	else
		snprintf(event->detail_name, sizeof(event->detail_name), "%s", detail_name);
	event_count++;
	spin_unlock(event_lock, owner);
}

void cdc_diagnostics_log_state_change(usb_audio_state_t previous_state,
									  usb_audio_state_t state, bool session_start)
{
	if (!COM_PORT_OUTPUT_ENABLE || !diagnostics_enabled || !session_active || event_lock == NULL)
		return;

	const uint32_t timestamp_us = time_us_32();
	const uint32_t sample_rate = audio_state.freq;
	const uint32_t ring_frames = (uint32_t)audio_ring_total_frames();
	const uint32_t dma_pool_frames = dma_tx_pool_frames();
	const uint32_t owner = spin_lock_blocking(event_lock);
	if (event_count == CDC_EVENT_QUEUE_SIZE)
	{
		event_drop_count++;
		spin_unlock(event_lock, owner);
		return;
	}

	const uint8_t index = (uint8_t)((event_head + event_count) % CDC_EVENT_QUEUE_SIZE);
	cdc_anomaly_t *event = &event_queue[index];
	event->timestamp_us = timestamp_us;
	event->detail_value = 0;
	event->sample_rate = sample_rate;
	event->ring_frames = ring_frames;
	event->dma_pool_frames = dma_pool_frames;
	event->code = 0;
	event->state = state;
	if (session_start)
		snprintf(event->detail_name, sizeof(event->detail_name), "%s", "START");
	else
		snprintf(event->detail_name, sizeof(event->detail_name), "%s",
				 audio_state_name(previous_state));
	event_count++;
	spin_unlock(event_lock, owner);
}

void cdc_diagnostics_task(void)
{
	if (!COM_PORT_OUTPUT_ENABLE)
		return;

	const uint32_t now = time_us_32();
	static uint32_t next_discard_us;
	if ((int32_t)(now - next_discard_us) >= 0)
	{
		next_discard_us = now + 50000u;
		const uint32_t input_available = tud_cdc_n_available(1);
		if (input_available != 0)
		{
			uint8_t discard[64];
			const uint32_t discard_length = input_available < sizeof(discard)
				? input_available : sizeof(discard);
			tud_cdc_n_read(1, discard, discard_length);
		}
	}

	// Suppress diagnostic output in GADGET MODE.
	if (!diagnostics_enabled)
	{
		if (session_active)
		{
			session_active = false;
			reset_event_queue();
		}
		return;
	}

	if (!tud_cdc_n_connected(1))
	{
		if (session_active)
		{
			session_active = false;
			reset_event_queue();
		}
		return;
	}

	if (!session_active && !start_session(now))
		return;

	sample_ring_level();
	if ((int32_t)(now - next_sample_us) >= 0)
	{
		if (!write_sample(now))
			row_drop_count++;
	}
	write_pending_anomalies(now);
}

#else

void cdc_diagnostics_init(void)
{
}

bool cdc_diagnostics_is_connected(void)
{
	return false;
}

void cdc_diagnostics_set_enabled(bool enabled)
{
	(void)enabled;
}

void cdc_diagnostics_log_anomaly(uint8_t code, const char *detail_name, uint32_t detail_value)
{
	(void)code;
	(void)detail_name;
	(void)detail_value;
}

void cdc_diagnostics_log_state_change(usb_audio_state_t previous_state,
									 usb_audio_state_t state, bool session_start)
{
	(void)previous_state;
	(void)state;
	(void)session_start;
}

void cdc_diagnostics_task(void)
{
}

#endif
