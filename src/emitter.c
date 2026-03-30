#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"

#include "emitter.h"
#include "ir_emitter.h"

static uint8_t command = 0;
static uint8_t offset = 0;
static uint8_t amount = 0;

static uint8_t ramx22[2];
static uint8_t ramx18[3];
static uint8_t reg1b = 0;

static uint32_t g_last_packet_ms = 0;
static uint64_t g_last_packet_us = 0;
static bool g_driver_enabled = false;
static bool g_holdover_active = false;
static uint64_t g_next_holdover_us = 0;
static uint8_t g_holdover_eye = 0;
static uint32_t g_holdover_period_us = 0;

#define DRIVER_EXIT_HOLD_MS        250u
#define HOLDOVER_START_MS          10u
#define HOLDOVER_DEFAULT_PERIOD_US 8333u
#define HOLDOVER_MIN_PERIOD_US     7600u
#define HOLDOVER_MAX_PERIOD_US     9000u
#define HOLDOVER_SMOOTH_SHIFT      2u    /* alpha = 1/4 */
#define FORCE_HOLDOVER_TEST_MODE   0u
#define FORCE_HOLDOVER_START_EYE   1u

static bool control_in_pending = false;
static uint8_t response_offset = 0;
static uint8_t response_amount = 0;
static uint8_t response_data[EMITTER_EPSIZE] = {0};

static inline uint32_t holdover_filter_period(uint32_t current_us, uint32_t new_us) {
    if ((new_us < HOLDOVER_MIN_PERIOD_US) || (new_us > HOLDOVER_MAX_PERIOD_US)) {
        return current_us;
    }

    if ((current_us < HOLDOVER_MIN_PERIOD_US) || (current_us > HOLDOVER_MAX_PERIOD_US)) {
        return new_us;
    }

    int32_t error = (int32_t)new_us - (int32_t)current_us;
    return (uint32_t)((int32_t)current_us + (error >> HOLDOVER_SMOOTH_SHIFT));
}

void emitter_init(void) {
    gpio_init(STBY_LED_PIN);
    gpio_set_dir(STBY_LED_PIN, GPIO_OUT);
    gpio_put(STBY_LED_PIN, 1);

    memset(ramx22, 0, sizeof(ramx22));
    memset(ramx18, 0, sizeof(ramx18));
    reg1b = 0;
    g_driver_enabled = false;
    g_last_packet_ms = 0;
    g_last_packet_us = 0;
    g_holdover_active = false;
    g_next_holdover_us = 0;
    g_holdover_eye = 0;
    g_holdover_period_us = HOLDOVER_DEFAULT_PERIOD_US;

#if FORCE_HOLDOVER_TEST_MODE
    /* Test mode: start continuous 120Hz holdover immediately at boot. */
    g_driver_enabled = true;
    g_holdover_active = true;
    /* Toggle-first scheduler: seed opposite so first frame is START_EYE. */
    g_holdover_eye = (uint8_t)(FORCE_HOLDOVER_START_EYE ^ 1u);
    g_next_holdover_us = time_us_64() + g_holdover_period_us;
#endif
}

void emitter_task(uint32_t cur_time_ms) {
#if FORCE_HOLDOVER_TEST_MODE
    uint64_t now_us = time_us_64();
    if (now_us >= g_next_holdover_us) {
        g_holdover_eye ^= 1u;
        ir_emitter_set_eye(g_holdover_eye);
        ir_emitter_start_frame();

        do {
            g_next_holdover_us += g_holdover_period_us;
        } while (g_next_holdover_us <= now_us);
    }

    ir_emitter_update(cur_time_ms);
    return;
#endif

    if (g_driver_enabled && (g_last_packet_ms != 0u)) {
        uint32_t since_last_packet_ms = cur_time_ms - g_last_packet_ms;

        if (since_last_packet_ms > DRIVER_EXIT_HOLD_MS) {
            g_last_packet_ms = 0;
            g_last_packet_us = 0;
            g_holdover_active = false;
            g_next_holdover_us = 0;
            ir_emitter_force_idle();
        } else if (since_last_packet_ms >= HOLDOVER_START_MS) {
            uint64_t now_us = time_us_64();

            if (!g_holdover_active) {
                g_holdover_active = true;
                /* Holdover must continue from the expected stream phase,
                 * not from "now", otherwise the first bridged frame is late. */
                g_next_holdover_us = g_last_packet_us + g_holdover_period_us;
            }

            if (now_us >= g_next_holdover_us) {
                /* Keep regular stream pattern: alternate eye each frame,
                 * producing the same 4-token L/R cycle as normal flow. */
                g_holdover_eye ^= 1u;
                ir_emitter_set_eye(g_holdover_eye);
                ir_emitter_start_frame();

                /* Advance phase to the first future slot to avoid backlog bursts. */
                do {
                    g_next_holdover_us += g_holdover_period_us;
                } while (g_next_holdover_us <= now_us);

                if (g_next_holdover_us <= now_us) {
                    g_next_holdover_us = now_us + g_holdover_period_us;
                }
            }
        }
    }

    ir_emitter_update(cur_time_ms);
}

void emitter_handle_control_out(const uint8_t *data, uint16_t len) {
    if (len < 3) {
        return;
    }

    command = data[0];
    offset = data[1];
    amount = data[2];

    if (command & 0x01u) {
        if (offset == 0x22u) {
            memcpy(ramx22, data + 4, amount);
        } else if (offset == 0x18u) {
            memcpy(ramx18, data + 4, amount);
        } else if ((offset == 0x1Bu) && (amount >= 1u) && (len >= 5u)) {
#if FORCE_HOLDOVER_TEST_MODE
            /* Keep test streamer active regardless of host driver mode writes. */
            reg1b = (uint8_t)(data[4] | 0x04u);
            g_driver_enabled = true;
            g_holdover_active = true;
            if (g_next_holdover_us == 0u) {
                g_next_holdover_us = time_us_64() + g_holdover_period_us;
            }
#else
            bool was_enabled = g_driver_enabled;
            reg1b = data[4];
            g_driver_enabled = (reg1b & 0x04u) != 0u;
            if (!g_driver_enabled) {
                g_last_packet_ms = 0;
                g_last_packet_us = 0;
                g_holdover_active = false;
                g_next_holdover_us = 0;
                ir_emitter_force_idle();
            } else if (!was_enabled) {
                g_last_packet_ms = 0;
                g_last_packet_us = 0;
                g_holdover_active = false;
                g_next_holdover_us = 0;
            }
#endif
        }
    } else if (command & 0x02u) {
        response_offset = offset;
        response_amount = amount;
        memset(response_data, 0, sizeof(response_data));
        if (offset == 0x22u) {
            memcpy(response_data, ramx22, amount);
        } else if (offset == 0x18u) {
            memcpy(response_data, ramx18, amount);
        } else if ((offset == 0x1Bu) && (amount >= 1u)) {
            response_data[0] = reg1b;
        }
        control_in_pending = true;
    }

    if (command & 0x40u) {
        if (offset == 0x22u) {
            memset(ramx22, 0, amount);
        } else if (offset == 0x18u) {
            memset(ramx18, 0, amount);
        }
    }
}

void emitter_handle_swap_out(const uint8_t *data, uint16_t len) {
#if FORCE_HOLDOVER_TEST_MODE
    (void)data;
    (void)len;
    return;
#else
    if (len != 8u) {
        return;
    }

    if (!g_driver_enabled) {
        return;
    }

    if ((data[0] != 0xAAu) || ((data[1] & 0xFEu) != 0xFEu)) {
        return;
    }

    g_last_packet_ms = emitter_millis();
    g_last_packet_us = time_us_64();
    {
        uint32_t pll_period_us = ir_emitter_get_last_valid_period_us();
        if (pll_period_us == 0u) {
            pll_period_us = HOLDOVER_DEFAULT_PERIOD_US;
        }
        g_holdover_period_us = holdover_filter_period(g_holdover_period_us, pll_period_us);
    }
    g_holdover_active = false;
    g_next_holdover_us = g_last_packet_us + g_holdover_period_us;
    g_holdover_eye = data[1] & 0x01u;

    ir_emitter_set_eye(data[1] & 0x01u);
    ir_emitter_start_frame();
#endif
}

bool emitter_control_in_pending(void) {
    return control_in_pending;
}

bool emitter_is_active(void) {
    return ir_emitter_is_active();
}

bool emitter_is_holdover_active(void) {
    return g_holdover_active;
}

uint16_t emitter_build_control_in(uint8_t *out, uint16_t max_len) {
    uint16_t payload_len = (uint16_t)(4u + response_amount);
    if (payload_len > max_len) {
        payload_len = max_len;
    }

    memset(out, 0, payload_len);

    if (payload_len >= 4) {
        out[0] = response_offset;
        out[1] = response_amount;
        out[2] = 0x00;
        out[3] = 0x04;

        memcpy(out + 4, response_data, response_amount);
    }

    control_in_pending = false;
    return payload_len;
}
