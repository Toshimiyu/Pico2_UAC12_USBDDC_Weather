#include "weather_rx.h"

#include <stdlib.h>
#include <string.h>

#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_audio_config.h"

#define WEATHER_LINE_CAPACITY 128u
#define WEATHER_USB_TIMEOUT_US (300000000u)
#define WEATHER_TEMPERATURE_PERIOD_US (1000000u)

static weather_snapshot_t weather_snapshot = {
	.usb_status = WEATHER_USB_WAIT
};
static char input_line[WEATHER_LINE_CAPACITY];
static size_t input_line_length;
static bool discard_until_newline;
static bool receiver_enabled;
static bool have_valid_rx;
static uint32_t last_valid_rx_us;
static uint32_t next_temperature_us;

static bool parse_unsigned(const char *text, int minimum, int maximum, int *value)
{
	if (text == NULL || *text == '\0')
		return false;

	int parsed = 0;
	for (const char *cursor = text; *cursor != '\0'; ++cursor)
	{
		if (*cursor < '0' || *cursor > '9')
			return false;
		parsed = parsed * 10 + (*cursor - '0');
		if (parsed > maximum)
			return false;
	}

	if (parsed < minimum)
		return false;
	*value = parsed;
	return true;
}

static bool parse_signed_temperature(const char *text, int *value)
{
	if (text == NULL || *text == '\0')
		return false;

	const char *digits = text;
	bool negative = false;
	if (*digits == '-')
	{
		negative = true;
		++digits;
	}
	if (*digits == '\0')
		return false;

	int magnitude = 0;
	for (const char *cursor = digits; *cursor != '\0'; ++cursor)
	{
		if (*cursor < '0' || *cursor > '9')
			return false;
		magnitude = magnitude * 10 + (*cursor - '0');
		if (magnitude > 50)
			return false;
	}

	const int parsed = negative ? -magnitude : magnitude;
	if (parsed < -30 || parsed > 50)
		return false;
	*value = parsed;
	return true;
}

static bool is_leap_year(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static bool valid_date(int year, int month, int day)
{
	static const uint8_t month_days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	if (month < 1 || month > 12)
		return false;

	int maximum_day = month_days[month - 1];
	if (month == 2 && is_leap_year(year))
		maximum_day++;
	return day >= 1 && day <= maximum_day;
}

static bool split_fields(char *line, char **fields, size_t expected_count)
{
	size_t count = 1;
	fields[0] = line;
	for (char *cursor = line; *cursor != '\0'; ++cursor)
	{
		if (*cursor == ',')
		{
			*cursor = '\0';
			if (count >= expected_count)
				return false;
			fields[count++] = cursor + 1;
		}
	}
	return count == expected_count;
}

static bool parse_condition(const char *text, weather_condition_t *condition)
{
	if (strcmp(text, "") == 0)
	{
		*condition = WEATHER_CONDITION_NONE;
		return true;
	}
	if (strcmp(text, "晴れ") == 0)
		*condition = WEATHER_CONDITION_SUNNY;
	else if (strcmp(text, "曇り") == 0)
		*condition = WEATHER_CONDITION_CLOUDY;
	else if (strcmp(text, "雨") == 0)
		*condition = WEATHER_CONDITION_RAIN;
	else if (strcmp(text, "雪") == 0)
		*condition = WEATHER_CONDITION_SNOW;
	else
		return false;
	return true;
}

static bool parse_connector(const char *text, weather_connector_t *connector)
{
	if (strcmp(text, "") == 0)
		*connector = WEATHER_CONNECTOR_NONE;
	else if (strcmp(text, "時々") == 0)
		*connector = WEATHER_CONNECTOR_INTERMITTENT;
	else if (strcmp(text, "のち") == 0)
		*connector = WEATHER_CONNECTOR_THEN;
	else
		return false;
	return true;
}

static bool parse_day(char **fields, weather_day_t *day)
{
	weather_day_t parsed = {0};
	int high = 0;
	int low = 0;
	int precipitation;
	// Empty temperature fields mean JMA no longer publishes them (e.g. evening).
	const bool no_temperature = fields[4][0] == '\0' && fields[5][0] == '\0';
	if (!parse_condition(fields[1], &parsed.first) ||
		!parse_connector(fields[2], &parsed.connector) ||
		!parse_condition(fields[3], &parsed.second) ||
		(!no_temperature &&
		 (!parse_signed_temperature(fields[4], &high) ||
		  !parse_signed_temperature(fields[5], &low) || (high < low))) ||
		!parse_unsigned(fields[6], 0, 100, &precipitation))
		return false;
	parsed.has_temperature = !no_temperature;

	if ((parsed.connector == WEATHER_CONNECTOR_NONE) !=
		(parsed.second == WEATHER_CONDITION_NONE))
		return false;

	parsed.high_c = (int8_t)high;
	parsed.low_c = (int8_t)low;
	parsed.precipitation_percent = (uint8_t)precipitation;
	parsed.valid = true;
	*day = parsed;
	return true;
}

static bool parse_time(char **fields, weather_snapshot_t *updated)
{
	int year;
	int month;
	int day;
	int hour;
	int minute;
	if (!parse_unsigned(fields[1], 2000, 2099, &year) ||
		!parse_unsigned(fields[2], 1, 12, &month) ||
		!parse_unsigned(fields[3], 1, 31, &day) ||
		!parse_unsigned(fields[4], 0, 23, &hour) ||
		!parse_unsigned(fields[5], 0, 59, &minute) ||
		!valid_date(year, month, day))
		return false;

	updated->year = (uint16_t)year;
	updated->month = (uint8_t)month;
	updated->day = (uint8_t)day;
	updated->hour = (uint8_t)hour;
	updated->minute = (uint8_t)minute;
	updated->has_time = true;
	return true;
}

static bool parse_weather_line(char *line)
{
	char *fields[8];
	weather_snapshot_t updated = weather_snapshot;
	bool valid = false;

	if (strncmp(line, "TIME,", 5) == 0)
	{
		valid = split_fields(line, fields, 6) &&
			strcmp(fields[0], "TIME") == 0 &&
			parse_time(fields, &updated);
	}
	else if (strncmp(line, "TODAY,", 6) == 0)
	{
		valid = split_fields(line, fields, 7) &&
			strcmp(fields[0], "TODAY") == 0 &&
			parse_day(fields, &updated.today);
	}
	else if (strncmp(line, "TOMORROW,", 9) == 0)
	{
		valid = split_fields(line, fields, 7) &&
			strcmp(fields[0], "TOMORROW") == 0 &&
			parse_day(fields, &updated.tomorrow);
	}

	if (!valid)
		return false;

	weather_snapshot = updated;
	last_valid_rx_us = time_us_32();
	have_valid_rx = true;
	weather_snapshot.usb_status = WEATHER_USB_OK;
	return true;
}

static void receive_byte(uint8_t byte)
{
	if (byte == '\r')
		return;

	if (byte == '\n')
	{
		if (!discard_until_newline && input_line_length != 0)
		{
			input_line[input_line_length] = '\0';
			(void)parse_weather_line(input_line);
		}
		input_line_length = 0;
		discard_until_newline = false;
		return;
	}

	if (discard_until_newline)
		return;
	if (input_line_length + 1 >= sizeof(input_line))
	{
		input_line_length = 0;
		discard_until_newline = true;
		return;
	}

	input_line[input_line_length++] = (char)byte;
}

static void sample_temperature(uint32_t now)
{
	if ((int32_t)(now - next_temperature_us) < 0)
		return;
	next_temperature_us = now + WEATHER_TEMPERATURE_PERIOD_US;

	const uint16_t raw = adc_read();
	const float voltage = (float)raw * (3.3f / 4095.0f);
	const float temperature_c = 27.0f - (voltage - 0.706f) / 0.001721f;
	weather_snapshot.pico_temperature_tenths_c =
		(int16_t)(temperature_c * 10.0f + (temperature_c >= 0.0f ? 0.5f : -0.5f));
	weather_snapshot.has_pico_temperature = true;
}

void weather_rx_init(void)
{
	adc_init();
	adc_set_temp_sensor_enabled(true);
	adc_select_input(4);
	sleep_us(20);
	next_temperature_us = time_us_32();
}

void weather_rx_set_enabled(bool enabled)
{
	receiver_enabled = enabled && USB_CDC_ENABLED;
}

void weather_rx_task(void)
{
	const uint32_t now = time_us_32();
	sample_temperature(now);

	if (have_valid_rx &&
		(uint32_t)(now - last_valid_rx_us) >= WEATHER_USB_TIMEOUT_US)
		weather_snapshot.usb_status = WEATHER_USB_LOST;

#if USB_CDC_ENABLED
	// Poll the CDC FIFO only every 50 ms (RX FIFO is 256 bytes) to keep the
	// main loop's USB/CDC work from disturbing audio.
	static uint32_t next_poll_us;
	if (!receiver_enabled || (int32_t)(now - next_poll_us) < 0)
		return;
	next_poll_us = now + 50000u;
	if (!tud_cdc_connected())
		return;

	uint8_t bytes[64];
	for (uint32_t pass = 0; pass < 4; ++pass)
	{
		const uint32_t available = tud_cdc_available();
		if (available == 0)
			break;
		const uint32_t count = available < sizeof(bytes) ? available : sizeof(bytes);
		const uint32_t received = tud_cdc_read(bytes, count);
		for (uint32_t index = 0; index < received; ++index)
			receive_byte(bytes[index]);
	}
#endif
}

void weather_rx_get_snapshot(weather_snapshot_t *snapshot)
{
	if (snapshot != NULL)
		*snapshot = weather_snapshot;
}
