#ifndef CDC_DIAGNOSTICS_H
#define CDC_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>
#include "usb_device_control.h"

void cdc_diagnostics_init(void);
void cdc_diagnostics_set_enabled(bool enabled);
bool cdc_diagnostics_is_connected(void);
void cdc_diagnostics_log_anomaly(uint8_t code, const char *detail_name, uint32_t detail_value);
void cdc_diagnostics_log_state_change(usb_audio_state_t previous_state, usb_audio_state_t state, bool session_start);
void cdc_diagnostics_task(void);

#endif
