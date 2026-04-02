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
static uint8_t g_filter_last_eye = 0;
static uint32_t g_stream_period_us = 0;
static bool g_last_packet_had_feff = false;
static bool g_master_locked = false;
static uint64_t g_master_next_us = 0;
static uint8_t g_master_next_eye = 0;
static uint8_t g_master_good_swaps = 0;
static uint8_t g_master_bad_swaps = 0;
static uint64_t g_master_last_swap_us = 0;
static uint8_t g_master_last_swap_eye = 0;

#define DRIVER_EXIT_HOLD_MS        250u
#define STREAM_DEFAULT_PERIOD_US   8333u
#define STREAM_MIN_PERIOD_US       7600u
#define STREAM_MAX_PERIOD_US       9000u
#define STREAM_PERIOD_SMOOTH_SHIFT 2u    /* alpha = 1/4 */
#define STALE_REPEAT_NUM           7u
#define STALE_REPEAT_DEN           5u
#define FEFF_FOLLOWUP_WINDOW_US    5000u
#define MASTER_MIN_PERIOD_US       7600u
#define MASTER_MAX_PERIOD_US       9000u
#define MASTER_LOCK_MIN_SWAPS      12u
#define MASTER_START_DELAY_US      FRAME_ALARM_DELAY_US
#define MASTER_SCHEDULE_EARLY_US   350u
#define MASTER_PHASE_GAIN_SHIFT    3u
#define MASTER_PHASE_CLAMP_US      700u
#define MASTER_MAX_NO_PACKET_US    120000u
#define MASTER_MAX_LATE_US         20000u
#define MASTER_UNLOCK_BAD_SWAPS    3u

static bool control_in_pending = false;
static uint8_t response_offset = 0;
static uint8_t response_amount = 0;
static uint8_t response_data[EMITTER_EPSIZE] = {0};

static inline void master_reset(void) {
    g_master_locked = false;
    g_master_next_us = 0;
    g_master_next_eye = 0;
    g_master_good_swaps = 0;
    g_master_bad_swaps = 0;
    g_master_last_swap_us = 0;
    g_master_last_swap_eye = 0;
}

static inline bool master_period_valid(uint64_t dt_us) {
    return (dt_us >= MASTER_MIN_PERIOD_US) && (dt_us <= MASTER_MAX_PERIOD_US);
}

static inline uint32_t stream_filter_period(uint32_t current_us, uint32_t new_us) {
    if ((new_us < STREAM_MIN_PERIOD_US) || (new_us > STREAM_MAX_PERIOD_US)) {
        return current_us;
    }

    if ((current_us < STREAM_MIN_PERIOD_US) || (current_us > STREAM_MAX_PERIOD_US)) {
        return new_us;
    }

    int32_t error = (int32_t)new_us - (int32_t)current_us;
    return (uint32_t)((int32_t)current_us + (error >> STREAM_PERIOD_SMOOTH_SHIFT));
}

void emitter_init(void) {
    memset(ramx22, 0, sizeof(ramx22));
    memset(ramx18, 0, sizeof(ramx18));
    reg1b = 0;
    g_driver_enabled = false;
    g_last_packet_ms = 0;
    g_last_packet_us = 0;
    g_filter_last_eye = 0;
    g_stream_period_us = STREAM_DEFAULT_PERIOD_US;
    g_last_packet_had_feff = false;
    master_reset();
}

void emitter_task(uint32_t cur_time_ms) {
    if (g_driver_enabled && (g_last_packet_ms != 0u)) {
        uint32_t since_last_packet_ms = cur_time_ms - g_last_packet_ms;

        if (since_last_packet_ms > DRIVER_EXIT_HOLD_MS) {
            g_last_packet_ms = 0;
            g_last_packet_us = 0;
            master_reset();
            ir_emitter_force_idle();
        }
    }

    if (g_driver_enabled && g_master_locked) {
        uint64_t now_us = time_us_64();

        if ((g_last_packet_us != 0u) && ((now_us - g_last_packet_us) > MASTER_MAX_NO_PACKET_US)) {
            master_reset();
        } else if ((g_master_next_us != 0u) && (now_us > (g_master_next_us + MASTER_MAX_LATE_US))) {
            master_reset();
        } else if ((g_master_next_us != 0u) && ((now_us + MASTER_SCHEDULE_EARLY_US) >= g_master_next_us)) {
            if (!ir_emitter_is_busy()) {
                ir_emitter_set_eye(g_master_next_eye);
                if (ir_emitter_start_frame_at(g_master_next_us)) {
                    g_master_next_us += g_stream_period_us;
                    g_master_next_eye ^= 1u;
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
            bool was_enabled = g_driver_enabled;
            reg1b = data[4];
            g_driver_enabled = (reg1b & 0x04u) != 0u;
            if (!g_driver_enabled) {
                g_last_packet_ms = 0;
                g_last_packet_us = 0;
                g_last_packet_had_feff = false;
                master_reset();
                ir_emitter_force_idle();
            } else if (!was_enabled) {
                g_last_packet_ms = 0;
                g_last_packet_us = 0;
                g_last_packet_had_feff = false;
                master_reset();
            }
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
    if (len != 8u) {
        return;
    }

    if (!g_driver_enabled) {
        return;
    }

    if ((data[0] != 0xAAu) || ((data[1] & 0xFEu) != 0xFEu)) {
        return;
    }

    uint64_t now_us = time_us_64();
    uint8_t new_eye = data[1] & 0x01u;
    bool packet_has_feff = (data[6] == 0xFEu) && (data[7] == 0xFFu);
    if ((g_last_packet_us != 0u) &&
        (new_eye == g_filter_last_eye) &&
        ((now_us - g_last_packet_us) >
         (((uint64_t)g_stream_period_us * STALE_REPEAT_NUM) / STALE_REPEAT_DEN))) {
        return;
    }
    if ((g_last_packet_us != 0u) &&
        !packet_has_feff &&
        g_last_packet_had_feff &&
        (new_eye == g_filter_last_eye) &&
        ((now_us - g_last_packet_us) <= FEFF_FOLLOWUP_WINDOW_US)) {
        return;
    }

    g_last_packet_ms = emitter_millis();
    g_last_packet_us = now_us;
    {
        uint32_t pll_period_us = ir_emitter_get_last_valid_period_us();
        if (pll_period_us == 0u) {
            pll_period_us = STREAM_DEFAULT_PERIOD_US;
        }
        g_stream_period_us = stream_filter_period(g_stream_period_us, pll_period_us);
    }

    g_filter_last_eye = new_eye;
    g_last_packet_had_feff = packet_has_feff;

    bool swap_phase_valid = false;
    if (g_master_last_swap_us != 0u) {
        uint64_t dt_us = now_us - g_master_last_swap_us;
        if ((new_eye != g_master_last_swap_eye) && master_period_valid(dt_us)) {
            swap_phase_valid = true;
            g_master_bad_swaps = 0;
            if (g_master_good_swaps < 255u) {
                g_master_good_swaps++;
            }
        } else {
            if (g_master_locked) {
                if (g_master_bad_swaps < 255u) {
                    g_master_bad_swaps++;
                }
                if (g_master_bad_swaps >= MASTER_UNLOCK_BAD_SWAPS) {
                    g_master_locked = false;
                    g_master_good_swaps = 0;
                    g_master_bad_swaps = 0;
                }
            } else {
                g_master_good_swaps = 0;
            }
        }
    }
    g_master_last_swap_us = now_us;
    g_master_last_swap_eye = new_eye;

    if (!g_master_locked && (g_master_good_swaps >= MASTER_LOCK_MIN_SWAPS)) {
        g_master_locked = true;
        g_master_next_us = now_us + MASTER_START_DELAY_US;
        g_master_next_eye = new_eye;
    }

    if (g_master_locked) {
        if (swap_phase_valid) {
            uint64_t target_us = now_us + MASTER_START_DELAY_US;
            int64_t error_us = (int64_t)target_us - (int64_t)g_master_next_us;
            if (error_us > (int64_t)MASTER_PHASE_CLAMP_US) {
                error_us = (int64_t)MASTER_PHASE_CLAMP_US;
            } else if (error_us < -(int64_t)MASTER_PHASE_CLAMP_US) {
                error_us = -(int64_t)MASTER_PHASE_CLAMP_US;
            }

            {
                int64_t correction_us = (error_us >> MASTER_PHASE_GAIN_SHIFT);
                int64_t next_us = (int64_t)g_master_next_us + correction_us;
                if (next_us < 0) {
                    next_us = 0;
                }
                g_master_next_us = (uint64_t)next_us;
            }

            if (new_eye != g_master_next_eye) {
                g_master_next_eye = new_eye;
                g_master_next_us = target_us;
            }
        }
        return;
    }

    ir_emitter_set_eye(new_eye);
    (void)ir_emitter_start_frame();
}

bool emitter_control_in_pending(void) {
    return control_in_pending;
}

bool emitter_is_active(void) {
    return ir_emitter_is_active();
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
