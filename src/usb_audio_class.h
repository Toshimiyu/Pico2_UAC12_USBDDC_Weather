#ifndef USB_AUDIO_CLASS_H_
#define USB_AUDIO_CLASS_H_

#include <stdbool.h>
#include <stdint.h>

#define USB_AUDIO_MAX_SAMPLE_RATES 6

typedef struct
{
	uint8_t bytes_per_sample;
	uint8_t bit_depth;
	uint32_t max_rate_hz;
} usb_audio_format_t;

const uint32_t *usb_audio_class_sample_rates(uint8_t *count);
bool usb_audio_class_get_format(uint8_t alt, usb_audio_format_t *format);

#endif
