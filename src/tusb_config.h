/*
 * TinyUSB Audio configuration for the selected UAC1/UAC2 speaker implementation.
 *
 * The MIT License (MIT)
 * Copyright (c) 2020 Ha Thach (tinyusb.org)
 * Copyright (c) 2020 Jerzy Kasenberg
 */



#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "usb_audio_config.h"
#if USB_AUDIO_CLASS_VERSION == 1
#include "usb_audio_uac1.h"
#else
#include "usb_audio_uac2.h"
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

// RP2350 USB controller is Full-Speed only
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------
#define CFG_TUSB_MCU OPT_MCU_RP2040
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_PICO
#endif

// Debug output costs time on the audio stream
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_AUDIO             1
#define CFG_TUD_HID               0
#define CFG_TUD_CDC               (USB_CDC_ENABLED ? 2 : 0)
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX              2

#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX      USB_AUDIO_CLASS_SUBSLOT_MAX
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX              USB_AUDIO_CLASS_RESOLUTION_MAX

// Largest OUT packet over all alternate settings
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX              USB_AUDIO_CLASS_EP_OUT_SIZE_MAX

// Software FIFO between the USB ISR and the main loop: 16 ms at the largest packet size.
// It is drained on every main-loop pass; the depth absorbs short scheduling delays.
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ           (16 * USB_AUDIO_CLASS_EP_OUT_SIZE_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT                     1

// Required by the TinyUSB bundled with pico-sdk 2.x (ignored by newer TinyUSB).
// Numeric on purpose: this header is parsed before TinyUSB's own descriptor macros exist.
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                   USB_AUDIO_CLASS_DESCRIPTOR_LENGTH
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT                   USB_AUDIO_CLASS_N_AS_INTERFACES
#define CFG_TUD_AUDIO_CTRL_BUF_SZ                       USB_AUDIO_CLASS_CTRL_BUF_SIZE

// Retain the per-function spelling for compatibility with older TinyUSB trees.
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ                CFG_TUD_AUDIO_CTRL_BUF_SZ

// Explicit feedback EP. The value is set by usb_device_control.c (tud_audio_fb_set),
// the built-in FIFO-count method is NOT used.
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                1

#define CFG_TUD_CDC_RX_BUFSIZE                          256
#define CFG_TUD_CDC_TX_BUFSIZE                          512
#define CFG_TUD_CDC_RX_EPSIZE                           64
#define CFG_TUD_CDC_TX_EPSIZE                           64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
