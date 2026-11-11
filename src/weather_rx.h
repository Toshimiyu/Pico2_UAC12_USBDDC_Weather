#ifndef WEATHER_RX_H
#define WEATHER_RX_H

#include <stdbool.h>
#include <stdint.h>

#define WEATHER_AREA_LABEL_MAX_LENGTH 22u

typedef enum
{
	WEATHER_CONDITION_NONE = 0,
	WEATHER_CONDITION_SUNNY,
	WEATHER_CONDITION_CLOUDY,
	WEATHER_CONDITION_RAIN,
	WEATHER_CONDITION_SNOW
} weather_condition_t;

typedef enum
{
	WEATHER_CONNECTOR_NONE = 0,
	WEATHER_CONNECTOR_INTERMITTENT,
	WEATHER_CONNECTOR_THEN
} weather_connector_t;

typedef enum
{
	WEATHER_USB_WAIT = 0,
	WEATHER_USB_OK,
	WEATHER_USB_LOST
} weather_usb_status_t;

typedef struct
{
	weather_condition_t first;
	weather_connector_t connector;
	weather_condition_t second;
	int8_t high_c;
	int8_t low_c;
	uint8_t precipitation_percent;
	bool has_temperature;
	bool valid;
} weather_day_t;

typedef struct
{
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	bool has_time;
	char area_label[WEATHER_AREA_LABEL_MAX_LENGTH + 1u];
	weather_day_t today;
	weather_day_t tomorrow;
	int16_t pico_temperature_tenths_c;
	bool has_pico_temperature;
	weather_usb_status_t usb_status;
} weather_snapshot_t;

void weather_rx_init(void);
void weather_rx_set_enabled(bool enabled);
void weather_rx_task(void);
void weather_rx_get_snapshot(weather_snapshot_t *snapshot);
uint32_t weather_rx_generation(void);

#endif
