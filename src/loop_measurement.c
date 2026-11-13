#include "loop_measurement.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "pico/stdlib.h"
#include "tusb.h"
#include "common.h"
#include "display_rta.h"
#include "usb_device_control.h"
#include "transmit_to_dac.h"

#define LOOP_MEASUREMENT_INTERVAL_US 1000000u
#define LOOP_MEASUREMENT_LINE_SIZE 128u
#define WEATHER_MEASUREMENT_WINDOW_US 2000000u
#define WEATHER_MEASUREMENT_CAPACITY 32u
#define WEATHER_CLOCK_MAX_AGE_US 300000000ull

typedef struct
{
	uint32_t start_us, duration_us, rate, bytes, core0_us, core1_us;
	uint32_t overflow_frames, fifo_frames, underflows, output_stops, state_changes;
	uint32_t timestamp_utc;
	display_page_t page;
	usb_audio_state_t initial_state;
	bool core1_valid;
	bool timestamp_valid;
} weather_measurement_t;

static weather_measurement_t weather_records[WEATHER_MEASUREMENT_CAPACITY];
static uint8_t weather_record_count;
static uint8_t weather_dump_index;
static bool weather_dumping;
static bool weather_dump_header;
static uint32_t weather_lost;
static uint32_t weather_dump_failures;
static bool weather_active;
static uint32_t weather_generation;
static atomic_uint_least32_t weather_active_generation;
static atomic_uint_least32_t weather_core1_ack;
static atomic_uint_least32_t weather_core1_max;
static uint32_t weather_last_rx_us;
static uint32_t weather_start_us;
static uint32_t weather_rate;
static display_page_t weather_page;
static usb_audio_state_t weather_initial_state;
static uint32_t weather_bytes;
static uint32_t weather_core0_max;
static uint32_t weather_start_overflow, weather_start_fifo, weather_start_underflow, weather_start_stop;
static uint32_t weather_state_changes;
static uint64_t weather_start_us64;
static uint64_t weather_clock_us;
static uint64_t weather_clock_unix_ms;
static uint32_t weather_timestamp_utc;
static bool weather_clock_valid;
static bool weather_timestamp_valid;

static void weather_set_start_timestamp(void)
{
	if (!weather_clock_valid || weather_timestamp_valid)
		return;
	int64_t delta = (int64_t)weather_start_us64 - (int64_t)weather_clock_us;
	if (delta > (int64_t)WEATHER_CLOCK_MAX_AGE_US || delta < -2000000)
		return;
	int64_t unix_us = (int64_t)(weather_clock_unix_ms * 1000u) + delta;
	if (unix_us < 0 || (uint64_t)(unix_us / 1000000) > UINT32_MAX)
		return;
	weather_timestamp_utc = (uint32_t)(unix_us / 1000000);
	weather_timestamp_valid = true;
}

void loop_measurement_weather_set_time(uint64_t unix_ms)
{
	weather_clock_us = time_us_64();
	weather_clock_unix_ms = unix_ms;
	weather_clock_valid = true;
	if (weather_active)
		weather_set_start_timestamp();
}

static void weather_sample_core1(uint32_t now_us)
{
	static uint32_t generation_seen, previous_us;
	const uint32_t generation = atomic_load_explicit(&weather_active_generation, memory_order_acquire);
	if (generation == 0u)
		return;
	if (generation != generation_seen)
	{
		generation_seen = generation;
		previous_us = now_us;
		atomic_store_explicit(&weather_core1_max, 0u, memory_order_relaxed);
		atomic_store_explicit(&weather_core1_ack, generation, memory_order_release);
		return;
	}
	const uint32_t gap = now_us - previous_us;
	previous_us = now_us;
	uint_least32_t observed = atomic_load_explicit(&weather_core1_max, memory_order_relaxed);
	while (gap > observed &&
		!atomic_compare_exchange_weak_explicit(&weather_core1_max, &observed, gap,
			memory_order_relaxed, memory_order_relaxed))
	{
	}
}

static atomic_uint_least32_t active_generation;
static atomic_uint_least32_t core1_ack_generation;
static atomic_uint_least32_t core1_max_us;
static uint32_t core0_max_us;

typedef struct
{
	uint32_t generation;
	uint32_t previous_us;
} loop_sample_state_t;

static loop_sample_state_t core0_sample;
static loop_sample_state_t core1_sample;

void loop_measurement_invalidate(void)
{
	atomic_store_explicit(&active_generation, 0u, memory_order_release);
	if (weather_active)
		weather_state_changes++;
}

void loop_measurement_sample_core0(uint32_t now_us)
{
	static uint32_t previous_us;
	static bool previous_valid;
	if (weather_active && previous_valid)
	{
		const uint32_t gap = now_us - previous_us;
		if (gap > weather_core0_max)
			weather_core0_max = gap;
	}
	previous_us = now_us;
	previous_valid = true;
	const uint32_t generation = atomic_load_explicit(&active_generation, memory_order_acquire);
	if (generation == 0u)
		return;
	if (core0_sample.generation == generation)
	{
		const uint32_t gap = now_us - core0_sample.previous_us;
		if (gap > core0_max_us)
			core0_max_us = gap;
	}
	core0_sample.generation = generation;
	core0_sample.previous_us = now_us;
}

void loop_measurement_sample_core1(uint32_t now_us)
{
	weather_sample_core1(now_us);
	const uint32_t generation = atomic_load_explicit(&active_generation, memory_order_acquire);
	if (generation == 0u)
		return;
	if (core1_sample.generation != generation)
	{
		core1_sample.generation = generation;
		core1_sample.previous_us = now_us;
		atomic_store_explicit(&core1_max_us, 0u, memory_order_relaxed);
		atomic_store_explicit(&core1_ack_generation, generation, memory_order_release);
		return;
	}

	const uint32_t gap = now_us - core1_sample.previous_us;
	core1_sample.previous_us = now_us;
	uint_least32_t observed = atomic_load_explicit(&core1_max_us, memory_order_relaxed);
	while (gap > observed &&
		!atomic_compare_exchange_weak_explicit(&core1_max_us, &observed, gap,
			memory_order_relaxed, memory_order_relaxed))
	{
	}
}

void loop_measurement_weather_received(uint32_t bytes)
{
	const uint32_t now = time_us_32();
	if (!weather_active)
	{
		const usb_audio_stats_t *stats = usb_audio_get_stats();
		weather_start_us = now;
		weather_start_us64 = time_us_64();
		weather_timestamp_valid = false;
		weather_set_start_timestamp();
		weather_rate = audio_state.freq;
		weather_page = display_rta_get_page();
		weather_initial_state = usb_audio_get_state();
		weather_bytes = 0u;
		weather_core0_max = 0u;
		weather_start_overflow = stats->drop_ovf_frames;
		weather_start_fifo = stats->drop_fifo_frames;
		weather_start_underflow = stats->underflow_count;
		weather_start_stop = dma_diag.output_stop_count;
		weather_state_changes = 0u;
		if (++weather_generation == 0u)
			++weather_generation;
		weather_active = true;
		atomic_store_explicit(&weather_active_generation, weather_generation, memory_order_release);
	}
	weather_bytes += bytes;
	weather_last_rx_us = now;
}

void loop_measurement_weather_dump(void)
{
	weather_dump_index = 0u;
	weather_dump_header = false;
	weather_dumping = true;
}

void loop_measurement_weather_clear(void)
{
	weather_record_count = 0u;
	weather_lost = 0u;
	weather_dump_failures = 0u;
	weather_dumping = false;
}

static void weather_measurement_task(void)
{
	const uint32_t now = time_us_32();
	if (weather_active && (uint32_t)(now - weather_last_rx_us) >= WEATHER_MEASUREMENT_WINDOW_US)
	{
		const usb_audio_stats_t *stats = usb_audio_get_stats();
		atomic_store_explicit(&weather_active_generation, 0u, memory_order_release);
		if (weather_record_count < WEATHER_MEASUREMENT_CAPACITY)
		{
			weather_measurement_t *record = &weather_records[weather_record_count++];
			const bool core1_valid =
				atomic_load_explicit(&weather_core1_ack, memory_order_acquire) == weather_generation;
			*record = (weather_measurement_t){
				.start_us = weather_start_us, .duration_us = now - weather_start_us,
				.rate = weather_rate, .bytes = weather_bytes, .core0_us = weather_core0_max,
				.page = weather_page, .initial_state = weather_initial_state,
				.core1_us = atomic_load_explicit(&weather_core1_max, memory_order_relaxed),
				.core1_valid = core1_valid,
				.timestamp_utc = weather_timestamp_utc,
				.timestamp_valid = weather_timestamp_valid,
				.overflow_frames = stats->drop_ovf_frames - weather_start_overflow,
				.fifo_frames = stats->drop_fifo_frames - weather_start_fifo,
				.underflows = stats->underflow_count - weather_start_underflow,
				.output_stops = dma_diag.output_stop_count - weather_start_stop,
				.state_changes = weather_state_changes
			};
		}
		else
			weather_lost++;
		weather_active = false;
	}

	if (!weather_dumping || !tud_cdc_n_connected(1) || display_rta_is_diagnostic_mode())
		return;
	if (tud_cdc_n_write_available(1) < 200u)
		return;
	char line[200];
	int length;
	if (!weather_dump_header)
	{
		length = snprintf(line, sizeof(line),
			"#WX,START_US,DURATION_US,RATE_HZ,PAGE,STATE,BYTES,A_US,B_US,B_VALID,O_FRAMES,F_FRAMES,UNDERFLOWS,OUTPUT_STOPS,STATE_CHANGES,LOST,START_JST\r\n");
	}
	else if (weather_dump_index < weather_record_count)
	{
		const weather_measurement_t *r = &weather_records[weather_dump_index];
		char timestamp[32] = "UNSYNCED";
		if (r->timestamp_valid)
		{
			const time_t jst = (time_t)r->timestamp_utc + 9 * 60 * 60;
			struct tm calendar;
			if (gmtime_r(&jst, &calendar) == NULL ||
				strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S+09:00", &calendar) == 0u)
				panic("Weather measurement timestamp conversion failed");
		}
		length = snprintf(line, sizeof(line), "WX,%lu,%lu,%lu,%u,%u,%lu,%lu,%lu,%u,%lu,%lu,%lu,%lu,%lu,%lu,%s\r\n",
			(unsigned long)r->start_us, (unsigned long)r->duration_us, (unsigned long)r->rate,
			(unsigned)r->page, (unsigned)r->initial_state,
			(unsigned long)r->bytes, (unsigned long)r->core0_us, (unsigned long)r->core1_us,
			r->core1_valid ? 1u : 0u, (unsigned long)r->overflow_frames,
			(unsigned long)r->fifo_frames, (unsigned long)r->underflows,
			(unsigned long)r->output_stops, (unsigned long)r->state_changes, (unsigned long)weather_lost,
			timestamp);
	}
	else
	{
		length = snprintf(line, sizeof(line), "WX_END,%u,%lu,%lu\r\n",
			(unsigned)weather_record_count, (unsigned long)weather_lost,
			(unsigned long)weather_dump_failures);
	}
	if (length < 0 || (size_t)length >= sizeof(line))
		panic("Weather measurement line overflow");
	if (tud_cdc_n_write(1, line, (uint32_t)length) != (uint32_t)length)
	{
		weather_dump_failures++;
		weather_dumping = false;
		return;
	}
	if (!weather_dump_header)
		weather_dump_header = true;
	else if (weather_dump_index >= weather_record_count)
		weather_dumping = false;
	else
		weather_dump_index++;
	tud_cdc_n_write_flush(1);
}

void loop_measurement_task(void)
{
	weather_measurement_task();
	static const char header[] =
		"#LOOP,UPTIME_MS,PAGE,RATE_HZ,WINDOW_US,CORE0_MAX_US,CORE1_MAX_US,DROPPED\r\n";
	static bool header_sent;
	static bool measuring;
	static bool window_started;
	static uint32_t generation;
	static uint32_t start_us;
	static uint32_t rate;
	static display_page_t page;
	static uint32_t dropped;

	const bool connected = tud_cdc_n_connected(1);
	const display_page_t current_page = display_rta_get_page();
	const bool eligible = !display_rta_is_diagnostic_mode() && display_rta_page_ready() &&
		(current_page == DISPLAY_PAGE_WEATHER || current_page == DISPLAY_PAGE_PLAYBACK) &&
		usb_audio_get_state() == USB_AUDIO_STREAMING;

	if (!connected || display_rta_is_diagnostic_mode())
		header_sent = false;
	if (measuring && (!eligible ||
		(page != current_page || rate != audio_state.freq ||
		 atomic_load_explicit(&active_generation, memory_order_acquire) != generation)))
	{
		loop_measurement_invalidate();
		measuring = false;
		window_started = false;
		display_rta_set_loop_measurement(false, 0u, 0u);
	}
	if (!eligible)
		return;

	if (connected && !header_sent)
	{
		if (tud_cdc_n_write_available(1) >= sizeof(header) - 1u)
		{
			if (tud_cdc_n_write(1, header, sizeof(header) - 1u) != sizeof(header) - 1u)
				dropped++;
			else
				header_sent = true;
			tud_cdc_n_write_flush(1);
		}
	}

	if (!measuring)
	{
		page = current_page;
		rate = audio_state.freq;
		if (++generation == 0u)
			++generation;
		atomic_store_explicit(&active_generation, generation, memory_order_release);
		measuring = true;
	}

	if (!window_started)
	{
		if (atomic_load_explicit(&core1_ack_generation, memory_order_acquire) != generation)
			return;
		core0_max_us = 0u;
		atomic_exchange_explicit(&core1_max_us, 0u, memory_order_relaxed);
		start_us = time_us_32();
		window_started = true;
		return;
	}

	const uint32_t now = time_us_32();
	const uint32_t elapsed = now - start_us;
	if (elapsed < LOOP_MEASUREMENT_INTERVAL_US)
		return;

	const uint32_t max0 = core0_max_us;
	core0_max_us = 0u;
	const uint32_t max1 = atomic_exchange_explicit(&core1_max_us, 0u, memory_order_relaxed);
	start_us = now;
	display_rta_set_loop_measurement(true, max0, max1);
	if (!connected)
		return;
	if (!header_sent || tud_cdc_n_write_available(1) < LOOP_MEASUREMENT_LINE_SIZE)
	{
		dropped++;
		return;
	}
	char line[LOOP_MEASUREMENT_LINE_SIZE];
	const int length = snprintf(line, sizeof(line), "LOOP,%lu,%s,%lu,%lu,%lu,%lu,%lu\r\n",
		(unsigned long)(now / 1000u),
		page == DISPLAY_PAGE_WEATHER ? "WEATHER" : "RTA",
		(unsigned long)rate, (unsigned long)elapsed,
		(unsigned long)max0, (unsigned long)max1, (unsigned long)dropped);
	if (length < 0 || (size_t)length >= sizeof(line))
		panic("Loop measurement line overflow");
	if (tud_cdc_n_write(1, line, (uint32_t)length) != (uint32_t)length)
		dropped++;
	else
		dropped = 0u;
	tud_cdc_n_write_flush(1);
}
