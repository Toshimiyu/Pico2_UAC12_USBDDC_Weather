#include "tusb.h"
#include "usb_audio_config.h"
#include "usb_audio_class.h"
#include "usb_audio_uac1.h"
#include "usb_device_control.h"
#include "common_types.h"
#include "common.h"

static const uint32_t uac1_sample_rates[] = {44100, 48000, 88200, 96000};
static uint16_t uac1_volume_resolution = VOLUME_RESOLUTION;

const uint32_t *usb_audio_class_sample_rates(uint8_t *count)
{
    *count = (uint8_t)(sizeof(uac1_sample_rates) / sizeof(uac1_sample_rates[0]));
    return uac1_sample_rates;
}

bool usb_audio_class_get_format(uint8_t alt, usb_audio_format_t *format)
{
    switch (alt)
    {
    case UAC1_ALT_16BIT:
        *format = (usb_audio_format_t){2, 16, 96000};
        return true;
    case UAC1_ALT_24BIT:
        *format = (usb_audio_format_t){3, 24, 96000};
        return true;
    default:
        return false;
    }
}

#define UAC1_AS_FORMAT(_alt, _subslot, _bits, _max_packet) \
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, (_alt), 2, 0), \
    TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_INPUT_TERMINAL, 0, AUDIO10_FORMAT_TYPE_I), \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(2, (_subslot), (_bits), 44100, 48000, 88200, 96000), \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(UAC1_EPNUM_AUDIO_OUT, \
        (TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS), (_max_packet), 1, UAC1_EPNUM_AUDIO_FB), \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, \
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0), \
    9, TUSB_DESC_ENDPOINT, UAC1_EPNUM_AUDIO_FB, \
        (TUSB_XFER_ISOCHRONOUS | (1u << 4)), U16_TO_U8S_LE(UAC1_FB_EP_SIZE), 1, 1, 0

#define UAC1_AS_ALT_LEN (TUD_AUDIO10_DESC_STD_AS_LEN \
    + TUD_AUDIO10_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(4) \
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN \
    + TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN)
#define UAC1_CDC_DESC_LEN (USB_CDC_ENABLED ? 2 * TUD_CDC_DESC_LEN : 0)
#define UAC1_CONFIG_INTERFACE_COUNT (USB_CDC_ENABLED ? ITF_NUM_TOTAL : ITF_NUM_CDC_WEATHER)
#define UAC1_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC1_FUNC_DESC_LEN_NUM + UAC1_CDC_DESC_LEN)

static const uint8_t desc_uac1_configuration[] =
{
    TUD_CONFIG_DESCRIPTOR(1, UAC1_CONFIG_INTERFACE_COUNT, 0, UAC1_CONFIG_TOTAL_LEN, 0x00, 100),
    8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_AUDIO_CONTROL, 2,
        TUSB_CLASS_AUDIO, 0, 0, 0,
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0, 0),
    TUD_AUDIO10_DESC_CS_AC(0x0100, TUD_AUDIO10_DESC_INPUT_TERM_LEN
        + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN,
        ITF_NUM_AUDIO_STREAMING),
    TUD_AUDIO10_DESC_INPUT_TERM(UAC1_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING,
        0, 2, AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0, 0),
    TUD_AUDIO10_DESC_FEATURE_UNIT(UAC1_ENTITY_FEATURE_UNIT, UAC1_ENTITY_INPUT_TERMINAL, 0,
        0x0003, 0x0000, 0x0000),
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC1_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_DESKTOP_SPEAKER,
        0, UAC1_ENTITY_FEATURE_UNIT, 0),
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0, 0, 0),
    UAC1_AS_FORMAT(UAC1_ALT_16BIT, 2, 16, UAC1_EP_SZ_16BIT),
    UAC1_AS_FORMAT(UAC1_ALT_24BIT, 3, 24, UAC1_EP_SZ_24BIT),
#if USB_CDC_ENABLED
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_WEATHER, USB_CDC_WEATHER_STRING_INDEX,
        USB_CDC_WEATHER_EP_NOTIF, 8, USB_CDC_WEATHER_EP_OUT, USB_CDC_WEATHER_EP_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_DIAGNOSTICS, USB_CDC_DIAGNOSTICS_STRING_INDEX,
        USB_CDC_DIAGNOSTICS_EP_NOTIF, 8, USB_CDC_DIAGNOSTICS_EP_OUT,
        USB_CDC_DIAGNOSTICS_EP_IN, 64),
#endif
};

TU_VERIFY_STATIC(sizeof(desc_uac1_configuration) == UAC1_CONFIG_TOTAL_LEN,
                 "Incorrect UAC1 descriptor size");
TU_VERIFY_STATIC(UAC1_FUNC_DESC_LEN_NUM == 8 + TUD_AUDIO10_DESC_STD_AC_LEN
                 + TUD_AUDIO10_DESC_CS_AC_LEN(1) + TUD_AUDIO10_DESC_INPUT_TERM_LEN
                 + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN
                 + TUD_AUDIO10_DESC_STD_AS_LEN + 2 * UAC1_AS_ALT_LEN,
                 "UAC1_FUNC_DESC_LEN_NUM mismatch");
TU_VERIFY_STATIC(UAC1_EP_SZ_MAX <= 1023, "Full-Speed isochronous packet size limit exceeded");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    usb_audio_record_event('C', index, 0, 0, true, false);
    return desc_uac1_configuration;
}

static bool send_uac1_value(uint8_t rhport, tusb_control_request_t const *request,
                            void *value, uint16_t length)
{
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, value, length);
}

static bool feature_unit_get_request(uint8_t rhport, tusb_control_request_t const *request)
{
    const uint8_t selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel = TU_U16_LOW(request->wValue);

    if (channel != 0)
        return false;

    if (selector == AUDIO10_FU_CTRL_MUTE && request->bRequest == AUDIO10_CS_REQ_GET_CUR)
    {
        uint8_t mute = audio_state.mute ? 1 : 0;
        return send_uac1_value(rhport, request, &mute, sizeof(mute));
    }

    if (selector != AUDIO10_FU_CTRL_VOLUME)
        return false;

    int16_t value;
    switch (request->bRequest)
    {
    case AUDIO10_CS_REQ_GET_CUR:
        value = audio_state.acq_volume;
        break;
    case AUDIO10_CS_REQ_GET_MIN:
        value = MIN_VOLUME;
        break;
    case AUDIO10_CS_REQ_GET_MAX:
        value = MAX_VOLUME;
        break;
    case AUDIO10_CS_REQ_GET_RES:
        value = (int16_t)uac1_volume_resolution;
        break;
    default:
        return false;
    }

    uint16_t little_endian = tu_htole16((uint16_t)value);
    return send_uac1_value(rhport, request, &little_endian, sizeof(little_endian));
}

static bool feature_unit_set_request(tusb_control_request_t const *request, const uint8_t *buffer)
{
    const uint8_t selector = TU_U16_HIGH(request->wValue);
    const uint8_t channel = TU_U16_LOW(request->wValue);

    if (channel != 0)
        return false;

    if (selector == AUDIO10_FU_CTRL_VOLUME
        && request->bRequest == AUDIO10_CS_REQ_SET_RES
        && request->wLength == 2)
    {
        const uint16_t resolution =
            (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        if (resolution == 0u || (resolution & 0x8000u) != 0u)
            return false;

        uac1_volume_resolution = resolution;
        return true;
    }

    if (request->bRequest != AUDIO10_CS_REQ_SET_CUR)
        return false;

    if (selector == AUDIO10_FU_CTRL_MUTE && request->wLength == 1)
    {
        audio_state.mute = buffer[0] != 0;
        volume_control();
        return true;
    }

    if (selector == AUDIO10_FU_CTRL_VOLUME && request->wLength == 2)
    {
        int16_t volume = (int16_t)((uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8));
        if (volume > MAX_VOLUME)
            volume = MAX_VOLUME;
        if (volume < MIN_VOLUME)
            volume = MIN_VOLUME;
        audio_state.acq_volume = volume;
        volume_control();
        return true;
    }

    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);
    const bool ok = entity_id == UAC1_ENTITY_FEATURE_UNIT
        && feature_unit_get_request(rhport, request);
    usb_audio_record_event('G', entity_id, TU_U16_HIGH(request->wValue),
                           request->bRequest, ok, true);
    return ok;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    (void)rhport;
    const uint8_t entity_id = TU_U16_HIGH(request->wIndex);
    const bool ok = entity_id == UAC1_ENTITY_FEATURE_UNIT
        && feature_unit_set_request(request, buffer);
    usb_audio_record_event('S', entity_id, TU_U16_HIGH(request->wValue),
                           request->bRequest, ok, true);
    return ok;
}

static uint32_t decode_sample_rate(const uint8_t buffer[3])
{
    return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) | ((uint32_t)buffer[2] << 16);
}

static bool sample_rate_request_matches(tusb_control_request_t const *request)
{
    return TU_U16_LOW(request->wIndex) == UAC1_EPNUM_AUDIO_OUT
        && TU_U16_HIGH(request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    if (!sample_rate_request_matches(request))
        return false;

    uint32_t rate;
    switch (request->bRequest)
    {
    case AUDIO10_CS_REQ_GET_CUR:
        rate = usb_audio_get_current_rate();
        break;
    case AUDIO10_CS_REQ_GET_MIN:
        rate = uac1_sample_rates[0];
        break;
    case AUDIO10_CS_REQ_GET_MAX:
        rate = uac1_sample_rates[sizeof(uac1_sample_rates) / sizeof(uac1_sample_rates[0]) - 1];
        break;
    case AUDIO10_CS_REQ_GET_RES:
        rate = 0;
        break;
    default:
        return false;
    }

    uint8_t response[3] = {
        (uint8_t)rate,
        (uint8_t)(rate >> 8),
        (uint8_t)(rate >> 16),
    };
    return send_uac1_value(rhport, request, response, sizeof(response));
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buffer)
{
    (void)rhport;
    if (!sample_rate_request_matches(request)
        || request->bRequest != AUDIO10_CS_REQ_SET_CUR
        || request->wLength != 3)
        return false;

    const uint32_t rate = decode_sample_rate(buffer);
    if (!usb_audio_rate_supported(rate))
        return false;

    usb_audio_apply_sample_rate(rate);
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    const uint8_t interface_number = TU_U16_LOW(request->wIndex);
    const uint8_t alt = TU_U16_LOW(request->wValue);

    usb_audio_record_event(alt == 0 ? 'A' : 'I', interface_number, alt, 0, true, false);
    if (interface_number != ITF_NUM_AUDIO_STREAMING)
        return true;
    if (alt == 0)
    {
        usb_audio_stream_close();
        return true;
    }
    return usb_audio_stream_open(alt);
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    if (TU_U16_LOW(request->wIndex) == ITF_NUM_AUDIO_STREAMING
        && TU_U16_LOW(request->wValue) == 0)
        usb_audio_stream_close();
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                  audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    feedback_param->sample_freq = audio_state.freq;
}

void tud_umount_cb(void)
{
    usb_audio_log_anomaly(4, 0xFF, "EVENT_ID", 0xFF);
    usb_audio_record_event('U', 0, 0, 0, true, false);
    usb_audio_stream_close();
}

void tud_mount_cb(void)
{
    usb_audio_record_event('M', 0, 0, 0, true, false);
}
