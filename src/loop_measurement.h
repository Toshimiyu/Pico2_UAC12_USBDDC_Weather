#ifndef LOOP_MEASUREMENT_H
#define LOOP_MEASUREMENT_H

#include <stdint.h>

void loop_measurement_sample_core0(uint32_t now_us);
void loop_measurement_sample_core1(uint32_t now_us);
void loop_measurement_invalidate(void);
void loop_measurement_task(void);
void loop_measurement_weather_received(uint32_t bytes);
void loop_measurement_weather_set_time(uint64_t unix_ms);
void loop_measurement_weather_dump(void);
void loop_measurement_weather_clear(void);

#endif
