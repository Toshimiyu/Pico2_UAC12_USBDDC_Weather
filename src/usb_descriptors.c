/*
 * USB descriptors : UAC2 speaker (async + explicit feedback), Full-Speed
 * Based on TinyUSB examples/device/uac2_speaker_fb (HiFiPhile)
 *
 * The MIT License (MIT)
 * Copyright (c) 2023 HiFiPhile
 */

#include "tusb.h"
#include "pico/unique_id.h"
#include "usb_audio_config.h"
#include "usb_descriptors.h"
#include "usb_device_control.h"
#include "common_types.h"
#include "common.h" // MFG_NAME

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    // Use Interface Association Descriptor (IAD) for Audio
    // As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0300,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
uint8_t const * tud_descriptor_device_cb(void) {
  usb_audio_record_event('D', 0, 0, 0, true, false);
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_AUDIO_INTERFACE,
  STRID_CDC_INTERFACE = USB_CDC_STRING_INDEX,
};

static char const *string_desc_arr[] =
{
  (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
  MFG_NAME,                       // 1: Manufacturer
  USB_AUDIO_PRODUCT_NAME,         // 2: Product
  NULL,                           // 3: Serial = RP2350 unique board ID
  USB_AUDIO_PRODUCT_NAME,         // 4: Audio Interface
#if USB_CDC_ENABLED
  "Weather/Diagnostic CDC",       // 5: CDC Interface
#endif
};

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  usb_audio_record_event('T', index, 0, 0, true, false);
  size_t chr_count;

  switch ( index ) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    case STRID_SERIAL: {
      char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
      pico_get_unique_board_id_string(serial, sizeof(serial));
      chr_count = strlen(serial);
      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = serial[i];
      }
      break;
    }

    default: {
      if ( !(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) ) return NULL;

      const char *str = string_desc_arr[index];

      // Cap at max char
      chr_count = strlen(str);
      size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1; // -1 for string type
      if ( chr_count > max_count ) chr_count = max_count;

      // Convert ASCII string into UTF-16
      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = str[i];
      }
      break;
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));

  return _desc_str;
}
