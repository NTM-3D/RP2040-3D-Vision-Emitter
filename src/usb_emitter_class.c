#include <string.h>

#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

#include "emitter.h"

static uint8_t g_swap_out_buf[EMITTER_EPSIZE];
static uint8_t g_control_out_buf[EMITTER_EPSIZE];
static uint8_t g_control_in_buf[EMITTER_EPSIZE];

static void arm_out_endpoints(uint8_t rhport) {
    usbd_edpt_xfer(rhport, EMITTER_EP_SWAP_OUT, g_swap_out_buf, 8);
    usbd_edpt_xfer(rhport, EMITTER_EP_CONTROL_OUT, g_control_out_buf, EMITTER_EPSIZE);
}

static void maybe_send_control_in(uint8_t rhport) {
    if (!emitter_control_in_pending()) {
        return;
    }

    uint16_t len = emitter_build_control_in(g_control_in_buf, sizeof(g_control_in_buf));
    usbd_edpt_xfer(rhport, EMITTER_EP_CONTROL_IN, g_control_in_buf, len);
}

static void emitterd_init(void) {
    memset(g_swap_out_buf, 0, sizeof(g_swap_out_buf));
    memset(g_control_out_buf, 0, sizeof(g_control_out_buf));
    memset(g_control_in_buf, 0, sizeof(g_control_in_buf));
}

static void emitterd_reset(uint8_t rhport) {
    (void) rhport;
}

static uint16_t emitterd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    TU_VERIFY(itf_desc->bInterfaceClass == 0xFF, 0);
    TU_VERIFY(itf_desc->bNumEndpoints == 4, 0);

    uint16_t desc_len = sizeof(tusb_desc_interface_t) + itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t);
    TU_VERIFY(max_len >= desc_len, 0);

    const uint8_t *p_desc = (const uint8_t *)itf_desc + sizeof(tusb_desc_interface_t);
    for (uint8_t i = 0; i < itf_desc->bNumEndpoints; i++) {
        const tusb_desc_endpoint_t *ep_desc = (const tusb_desc_endpoint_t *)p_desc;
        TU_ASSERT(usbd_edpt_open(rhport, ep_desc));
        p_desc += sizeof(tusb_desc_endpoint_t);
    }

    arm_out_endpoints(rhport);
    return desc_len;
}

static bool emitterd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void) rhport;
    (void) stage;
    (void) request;
    return false;
}

static bool emitterd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    if (result != XFER_RESULT_SUCCESS) {
        return true;
    }

    if (ep_addr == EMITTER_EP_SWAP_OUT) {
        emitter_handle_swap_out(g_swap_out_buf, (uint16_t)xferred_bytes);
        usbd_edpt_xfer(rhport, EMITTER_EP_SWAP_OUT, g_swap_out_buf, 8);
        return true;
    }

    if (ep_addr == EMITTER_EP_CONTROL_OUT) {
        emitter_handle_control_out(g_control_out_buf, (uint16_t)xferred_bytes);
        maybe_send_control_in(rhport);
        usbd_edpt_xfer(rhport, EMITTER_EP_CONTROL_OUT, g_control_out_buf, EMITTER_EPSIZE);
        return true;
    }

    if (ep_addr == EMITTER_EP_CONTROL_IN) {
        maybe_send_control_in(rhport);
        return true;
    }

    return false;
}

static void emitterd_sof(uint8_t rhport, uint32_t frame_count) {
    (void) frame_count;
    maybe_send_control_in(rhport);
}

usbd_class_driver_t const _emitter_driver = {
    .init = emitterd_init,
    .reset = emitterd_reset,
    .open = emitterd_open,
    .control_xfer_cb = emitterd_control_xfer_cb,
    .xfer_cb = emitterd_xfer_cb,
    .sof = emitterd_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_emitter_driver;
}
