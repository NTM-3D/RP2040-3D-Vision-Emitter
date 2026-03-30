#include <string.h>
#include <stdlib.h>

#include "tusb.h"

#include "usb_descriptors.h"

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x0955,
    .idProduct = 0x0007,
    .bcdDevice = 0x0300,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

static uint8_t const desc_configuration[] = {
    0x09, TUSB_DESC_CONFIGURATION,
    0x2E, 0x00,
    0x01,
    0x01,
    0x00,
    0x80,
    0xC8,

    0x09, TUSB_DESC_INTERFACE,
    0x00,
    0x00,
    0x04,
    0xFF,
    0x00,
    0x00,
    0x00,

    0x07, TUSB_DESC_ENDPOINT,
    0x01,
    TUSB_XFER_BULK,
    0x20, 0x00,
    0x01,

    0x07, TUSB_DESC_ENDPOINT,
    0x82,
    TUSB_XFER_INTERRUPT,
    0x20, 0x00,
    0x01,

    0x07, TUSB_DESC_ENDPOINT,
    0x02,
    TUSB_XFER_BULK,
    0x20, 0x00,
    0x00,

    0x07, TUSB_DESC_ENDPOINT,
    0x84,
    TUSB_XFER_BULK,
    0x20, 0x00,
    0x01,
};

static_assert(sizeof(desc_configuration) == 46, "Configuration descriptor size must remain 46 bytes");

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Copyright (c) 2010 NVIDIA Corporation",
    "NVIDIA stereo controller",
    "NVIDIA professional stereo controller",
};

static uint8_t *g_ctrl_buf = NULL;
static uint32_t g_ctrl_buf_cap = 0;

static bool ensure_ctrl_buf(uint32_t len) {
    if (len <= g_ctrl_buf_cap) {
        return true;
    }

    uint8_t *new_buf = (uint8_t *)realloc(g_ctrl_buf, len);
    if (new_buf == NULL) {
        return false;
    }

    g_ctrl_buf = new_buf;
    g_ctrl_buf_cap = len;
    return true;
}

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void) {
    // Original AVR path ACKs 0x0600 with no payload; return a zero-length descriptor to avoid stall.
    static uint8_t const zlp_desc[] = {0x00};
    return zlp_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;

    static uint16_t desc_str[64];

    if (index == 0) {
        desc_str[1] = 0x0409;
        desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return desc_str;
    }

    if (index == 4) {
        desc_str[1] = 0x6fbc;
        desc_str[2] = 0xd628;
        desc_str[3] = 0x9043;
        desc_str[4] = 0x29d7;
        desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 8));
        return desc_str;
    }

    const char *str = NULL;
    switch (index) {
        case 1:
            str = string_desc_arr[1];
            break;
        case 2:
            str = string_desc_arr[2];
            break;
        case 3:
        default:
            str = string_desc_arr[3];
            break;
    }

    size_t chr_count = strlen(str);
    if (chr_count > 63) {
        chr_count = 63;
    }

    for (size_t i = 0; i < chr_count; i++) {
        desc_str[1 + i] = (uint8_t)str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == 0xA0) {
        if (request->bmRequestType == 0x40) {
            uint16_t xfer_len = request->wLength;
            if (xfer_len == 0) {
                return tud_control_status(rhport, request);
            }

            if (!ensure_ctrl_buf(xfer_len)) {
                return false;
            }

            return tud_control_xfer(rhport, request, g_ctrl_buf, xfer_len);
        }
        if (request->bmRequestType == 0xC0) {
            uint16_t xfer_len = request->wLength;
            if (xfer_len == 0) {
                return tud_control_status(rhport, request);
            }

            if (!ensure_ctrl_buf(xfer_len)) {
                return false;
            }

            memset(g_ctrl_buf, 0, xfer_len);
            return tud_control_xfer(rhport, request, g_ctrl_buf, xfer_len);
        }
    }

    if ((request->bRequest == 0x06) && (request->bmRequestType == 0x80) && (request->wValue == 0x0600)) {
        return tud_control_xfer(rhport, request, NULL, 0);
    }

    return false;
}
