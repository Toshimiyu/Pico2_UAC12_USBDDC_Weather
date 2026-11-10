/*
 * TinyUSB UAC2 class callbacks (control requests, alternate-setting changes, feedback setup).
 * Control-request handling follows examples/device/uac2_speaker_fb; the DDC-specific work
 * (clock tree, buffers, volume) is delegated to usb_device_control.c / common.h.
 *
 * Audio data is NOT received here: it is pulled from the TinyUSB software FIFO by
 * usb_audio_task() in the Core0 main loop (see usb_device_control.c).
 */

#include "tusb.h"
#include "usb_audio_config.h"
#include "uac2_compat.h"
#include "usb_audio_uac2.h"
#include "common_types.h"
#include "common.h"
#include "usb_device_control.h"

static const uint32_t uac2_sample_rates[USB_AUDIO_MAX_SAMPLE_RATES] = {
    44100, 48000, 88200, 96000, 176400, 192000
};

const uint32_t *usb_audio_class_sample_rates(uint8_t *count)
{
    *count = USB_AUDIO_MAX_SAMPLE_RATES;
    return uac2_sample_rates;
}

bool usb_audio_class_get_format(uint8_t alt, usb_audio_format_t *format)
{
    switch (alt)
    {
    case UAC2_ALT_16BIT:
        *format = (usb_audio_format_t){2, 16, UAC2_MAX_RATE_16BIT};
        return true;
    case UAC2_ALT_24BIT:
        *format = (usb_audio_format_t){3, 24, UAC2_MAX_RATE_24BIT};
        return true;
    case UAC2_ALT_32BIT:
        *format = (usb_audio_format_t){4, 32, UAC2_MAX_RATE_32BIT};
        return true;
    default:
        return false;
    }
}

//--------------------------------------------------------------------+
// UAC2 configuration descriptor
//--------------------------------------------------------------------+
#define UAC2_CDC_DESC_LEN (USB_CDC_ENABLED ? TUD_CDC_DESC_LEN : 0)
#define UAC2_CONFIG_INTERFACE_COUNT (USB_CDC_ENABLED ? ITF_NUM_TOTAL : ITF_NUM_CDC)
#define CONFIG_UAC2_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO20_SPEAKER_STEREO_FB_MULTIALT_DESC_LEN + UAC2_CDC_DESC_LEN)

static const uint8_t desc_uac2_configuration[] =
{
    TUD_CONFIG_DESCRIPTOR(1, UAC2_CONFIG_INTERFACE_COUNT, 0, CONFIG_UAC2_TOTAL_LEN, 0x00, 100),
    TUD_AUDIO20_SPEAKER_STEREO_FB_MULTIALT_DESCRIPTOR(
        ITF_NUM_AUDIO_CONTROL, 0, UAC2_EPNUM_AUDIO_OUT,
        UAC2_EPNUM_AUDIO_FB | 0x80, UAC2_FB_EP_SIZE),
#if USB_CDC_ENABLED
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, USB_CDC_STRING_INDEX, USB_CDC_EP_NOTIF, 8,
        USB_CDC_EP_OUT, USB_CDC_EP_IN, 64),
#endif
};

TU_VERIFY_STATIC(sizeof(desc_uac2_configuration) == CONFIG_UAC2_TOTAL_LEN, "Incorrect UAC2 descriptor size");
TU_VERIFY_STATIC(sizeof(desc_uac2_configuration) == TUD_CONFIG_DESC_LEN
                 + UAC2_FUNC_DESC_LEN_NUM + UAC2_CDC_DESC_LEN,
                 "UAC2_FUNC_DESC_LEN_NUM mismatch");
TU_VERIFY_STATIC(UAC2_EP_SZ_MAX <= 1023, "Full-Speed isochronous packet size limit exceeded");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    usb_audio_record_event('C', index, 0, 0, true, false);
    return desc_uac2_configuration;
}

//--------------------------------------------------------------------+
// Clock source (sample rate)
//--------------------------------------------------------------------+
static bool clock_get_request(uint8_t rhport, tusb_control_request_t const *request)
{
  uint8_t const ctrl_sel = TU_U16_HIGH(request->wValue);

  if (ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_4_t curf = { (int32_t) tu_htole32(usb_audio_get_current_rate()) };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &curf, sizeof(curf));
    } else if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
    uint8_t rate_count;
    const uint32_t *rates = usb_audio_class_sample_rates(&rate_count);
    audio20_control_range_4_n_t(USB_AUDIO_MAX_SAMPLE_RATES) rangef = {
      .wNumSubRanges = tu_htole16(rate_count)
    };
    for (uint8_t i = 0; i < rate_count; i++) {
      rangef.subrange[i].bMin = (int32_t) rates[i];
      rangef.subrange[i].bMax = (int32_t) rates[i];
      rangef.subrange[i].bRes = 0;
    }
    const uint16_t range_size = (uint16_t)(sizeof(rangef.wNumSubRanges)
        + rate_count * sizeof(rangef.subrange[0]));
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &rangef, range_size);
    } else if (request->bRequest == 4) { // UAC2 CS_REQ_INFO = 4
      uint8_t info = 3; // GET and SET supported
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &info, sizeof(info));
    }
  } else if (ctrl_sel == AUDIO20_CS_CTRL_CLK_VALID) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_1_t cur_valid = { .bCur = 1 };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur_valid, sizeof(cur_valid));
    } else if (request->bRequest == 4) {
      uint8_t info = 1; // Read-only (GET supported)
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &info, sizeof(info));
    }
  }
  return false;
}

static bool clock_set_request(tusb_control_request_t const *request, uint8_t const *buf)
{
  uint8_t const ctrl_sel = TU_U16_HIGH(request->wValue);

  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));
    uint32_t const new_freq = (uint32_t) ((audio20_control_cur_4_t const *) buf)->bCur;
    TU_VERIFY(usb_audio_rate_supported(new_freq));

    usb_audio_apply_sample_rate(new_freq);
    return true;
  }
  return false;
}

//--------------------------------------------------------------------+
// Feature unit (master mute / volume only)
//--------------------------------------------------------------------+
static bool feature_unit_get_request(uint8_t rhport, tusb_control_request_t const *request)
{
  uint8_t const ctrl_sel    = TU_U16_HIGH(request->wValue);
  uint8_t const channel_num = TU_U16_LOW(request->wValue);

  // Only the master channel (0) is exposed in the descriptor
  TU_VERIFY(channel_num == 0);

  if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_1_t mute1 = { .bCur = audio_state.mute ? 1 : 0 };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &mute1, sizeof(mute1));
    } else if (request->bRequest == 4) {
      uint8_t info = 3; // GET/SET supported
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &info, sizeof(info));
    }
  } else if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
    if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_2_n_t(1) range_vol = {
        .wNumSubRanges = tu_htole16(1),
        .subrange[0] = { .bMin = tu_htole16(MIN_VOLUME), tu_htole16(MAX_VOLUME), tu_htole16(VOLUME_RESOLUTION) }
      };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range_vol, sizeof(range_vol));
    } else if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_2_t cur_vol = { .bCur = tu_htole16(audio_state.acq_volume) };
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cur_vol, sizeof(cur_vol));
    } else if (request->bRequest == 4) {
      uint8_t info = 3; // GET/SET supported
      return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &info, sizeof(info));
    }
  }
  return false;
}

static bool feature_unit_set_request(tusb_control_request_t const *request, uint8_t const *buf)
{
  uint8_t const ctrl_sel    = TU_U16_HIGH(request->wValue);
  uint8_t const channel_num = TU_U16_LOW(request->wValue);

  TU_VERIFY(channel_num == 0);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_1_t));
    audio_state.mute = (((audio20_control_cur_1_t const *) buf)->bCur != 0);
    volume_control();
    return true;
  } else if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_2_t));
    int16_t vol = (int16_t) ((audio20_control_cur_2_t const *) buf)->bCur;
    if (vol > MAX_VOLUME) vol = MAX_VOLUME;
    if (vol < MIN_VOLUME) vol = MIN_VOLUME;
    audio_state.acq_volume = vol;
    volume_control();
    return true;
  }
  return false;
}

//--------------------------------------------------------------------+
// TinyUSB audio class callbacks
//--------------------------------------------------------------------+

bool tud_audio_get_req_entity_cb(
    uint8_t rhport,
    tusb_control_request_t const *p_request)
{
    uint8_t const entity_id =
        TU_U16_HIGH(p_request->wIndex);

    bool ok = false;

    if (entity_id == UAC2_ENTITY_CLOCK)
        ok = clock_get_request(rhport, p_request);
    else if (entity_id == UAC2_ENTITY_FEATURE_UNIT)
        ok = feature_unit_get_request(rhport, p_request);

    usb_audio_record_event(
        'G',
        entity_id,
        TU_U16_HIGH(p_request->wValue),
        p_request->bRequest,
        ok,
        true);

    return ok;
}

bool tud_audio_set_req_entity_cb(
    uint8_t rhport,
    tusb_control_request_t const *p_request,
    uint8_t *buf)
{
    (void)rhport;

    uint8_t const entity_id =
        TU_U16_HIGH(p_request->wIndex);

    bool ok = false;

    if (entity_id == UAC2_ENTITY_FEATURE_UNIT)
        ok = feature_unit_set_request(p_request, buf);
    else if (entity_id == UAC2_ENTITY_CLOCK)
        ok = clock_set_request(p_request, buf);

    usb_audio_record_event(
        'S',
        entity_id,
        TU_U16_HIGH(p_request->wValue),
        p_request->bRequest,
        ok,
        true);

    return ok;
}

bool tud_audio_set_itf_cb(
    uint8_t rhport,
    tusb_control_request_t const *p_request)
{
    (void)rhport;

    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);

    usb_audio_record_event(
        alt == 0 ? 'A' : 'I',
        itf,
        alt,
        0,
        true,
        false);

    if (itf != ITF_NUM_AUDIO_STREAMING)
        return true;

    if (alt == 0)
        usb_audio_stream_close();
    else
        return usb_audio_stream_open(alt);

    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
  (void) rhport;
  uint8_t const itf = tu_u16_low(p_request->wIndex);
  uint8_t const alt = tu_u16_low(p_request->wValue);

  if (itf == ITF_NUM_AUDIO_STREAMING && alt == 0)
    usb_audio_stream_close();
  return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
{
  (void) func_id;
  (void) alt_itf;
  feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
  feedback_param->sample_freq = audio_state.freq;
}

void tud_umount_cb(void)
{
    usb_audio_log_anomaly(4, 0xFF, "EVENT_ID", 0xFF);
    usb_audio_record_event(
        'U',
        0,
        0,
        0,
        true,
        false);

    usb_audio_stream_close();
}

void tud_mount_cb(void)
{
    usb_audio_record_event('M', 0, 0, 0, true, false);
}