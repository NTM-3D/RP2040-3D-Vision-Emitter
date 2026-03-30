#include <stdbool.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "emitter.h"
#include "ir_emitter.h"
#include "ir_protocols.h"

/* ------------------------------------------------------------------
 *  PIO program - 4 instructions
 *
 *  .wrap_target
 *  0: pull block         ; Wait for FIFO data (stalls if empty)
 *  1: out  pins, 1       ; Shift MSB (bit 31) to output pin
 *  2: out  x, 31         ; Shift remaining 31 bits into X
 *  3: jmp  x--, 3        ; Count-down delay loop
 *  .wrap
 *
 *  Each FIFO word: bit[31] = pin level, bits[30:0] = delay ticks.
 *  PIO clock = 1 MHz -> 1 tick = 1 us.  Protocol timing values
 *  are used directly as microseconds (matching the original
 *  interrupt-based code).  Edge-to-edge overhead is 4 ticks, so
 *  the delay field is set to (desired_ticks - 4).
 * ------------------------------------------------------------------ */
static const uint16_t ir_wave_program_instructions[] = {
    0x80a0,  /* pull block   */
    0x6001,  /* out pins, 1  */
    0x603f,  /* out x, 31    */
    0x0043,  /* jmp x--, 3   */
};

static const struct pio_program ir_wave_program = {
    .instructions = ir_wave_program_instructions,
    .length       = 4,
    .origin       = -1,
};

#define IR_PIO_WRAP_TARGET 0
#define IR_PIO_WRAP        3
#define IR_PIO_FREQ_HZ     1000000u   /* 1 MHz -> 1 us per tick */
#define PIO_EDGE_OVERHEAD  4u
#define IR_MAX_SEGMENTS    32u

static const ir_protocol_t *g_protocol = &IR_PROT_3DVISION;

static volatile bool     g_emitter_active      = false;
static bool              g_emitter_active_last  = false;
static volatile uint32_t g_last_frame_ms        = 0;

static uint8_t           g_swap_eyes = 0;
static volatile uint8_t  g_cur_eye   = 0;
static uint8_t           g_next_eye  = 0;



/* PIO / DMA resources */
static PIO  g_ir_pio;
static uint g_ir_sm;
static uint g_ir_offset;
static int  g_ir_dma_chan = -1;

/* Waveform buffer: filled by CPU, consumed by DMA -> PIO */
static uint32_t g_waveform[IR_MAX_SEGMENTS];
static uint     g_waveform_len;

/* Deferred frame alarm */
static alarm_id_t g_frame_alarm   = 0;
static uint8_t    g_pending_token = 0;

/* Software PLL: smooth the fire time to absorb USB packet jitter.
 * g_phase_us  = estimated absolute time (µs) for the next waveform fire
 * g_pll_locked = true once we have at least two packets to work with
 * We use the 64-bit microsecond timer (time_us_64) for phase tracking. */
#define PLL_NOMINAL_PERIOD_US  8333u  /* 120Hz */
#define PLL_GAIN_SHIFT         3u     /* alpha = 1/8: steady-state jitter filter */
#define PLL_FAST_GAIN_SHIFT    1u     /* alpha = 1/2: fast convergence at startup */
#define PLL_FAST_FRAMES        4u     /* use fast gain for this many frames */
#define PLL_RESYNC_THRESHOLD   2000   /* hard re-sync if error exceeds ±2ms */
#define PLL_VALID_MIN_PERIOD_US 7600u
#define PLL_VALID_MAX_PERIOD_US 9000u
static uint64_t g_phase_us    = 0;
static bool     g_pll_locked  = false;
static uint8_t  g_pll_frame_count = 0;
static uint32_t g_last_valid_pll_period_us = PLL_NOMINAL_PERIOD_US;

/* Keep abort wait bounded to avoid lockups if DMA gets wedged. */
#define DMA_ABORT_WAIT_US 1000u

/* ------------------------------------------------------------------ */

static inline uint32_t ir_pio_word(uint pin_level, uint32_t delay_us) {
    uint32_t ticks = (delay_us > PIO_EDGE_OVERHEAD)
                   ? (delay_us - PIO_EDGE_OVERHEAD)
                   : 0u;
    return ((uint32_t)(pin_level & 1u) << 31) | (ticks & 0x7FFFFFFFu);
}

/*
 * Build the complete waveform for a token pair starting at start_token.
 * Returns the number of 32-bit words written to g_waveform[].
 */
static uint build_waveform(uint8_t start_token) {
    uint n = 0;

    /* Leading LOW wait: FRAME_PAN (convert half-us to us) */
    g_waveform[n++] = ir_pio_word(0, FRAME_PAN_HUS / 2u);

    uint8_t token       = start_token;
    bool    first_token = true;

    for (;;) {
        uint8_t size = g_protocol->sizes[token];
        if (size == 0u) break;

        if (!first_token) {
            /* Inter-token gap (LOW, convert half-us to us) */
            g_waveform[n++] = ir_pio_word(0, FRAME_DURATION_HUS / 2u);
        }
        first_token = false;

        uint8_t base = g_protocol->indices[token];
        for (uint8_t i = 0; i < size; i++) {
            uint pin = (i & 1u) ? 0u : 1u;   /* even index = HIGH, odd = LOW */
            g_waveform[n++] = ir_pio_word(pin, g_protocol->timings[base + i]);
            if (n >= IR_MAX_SEGMENTS - 1u) goto done;
        }

        /* Tokens are emitted in pairs: 0+1, 2+3 */
        if ((token & 1u) == 0u && g_protocol->sizes[token + 1u] > 0u) {
            token++;
            continue;
        }
        break;
    }

    /* Repeat single-pulse tokens (e.g. Token 3 "Open Left Eye") to give
     * the glasses a second chance to detect the weakest signal in the
     * protocol.  Only fires for size-1 odd tokens at the end of a pair. */
    {
        uint8_t last_size = g_protocol->sizes[token];
        if (last_size == 1u && (token & 1u) && n < IR_MAX_SEGMENTS - 3u) {
            uint8_t base = g_protocol->indices[token];
            g_waveform[n++] = ir_pio_word(0, FRAME_DURATION_HUS / 2u);
            for (uint8_t i = 0; i < last_size; i++) {
                uint pin = (i & 1u) ? 0u : 1u;
                g_waveform[n++] = ir_pio_word(pin, g_protocol->timings[base + i]);
            }
        }
    }

done:
    /* Trailing LOW so pin ends low when PIO stalls on next pull */
    g_waveform[n++] = ir_pio_word(0, PIO_EDGE_OVERHEAD);
    return n;
}

/* ------------------------------------------------------------------ */

static void ir_pio_init(void) {
    g_ir_pio    = pio1;
    g_ir_sm     = pio_claim_unused_sm(g_ir_pio, true);
    g_ir_offset = (uint)pio_add_program(g_ir_pio, &ir_wave_program);

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg,
                       g_ir_offset + IR_PIO_WRAP_TARGET,
                       g_ir_offset + IR_PIO_WRAP);
    sm_config_set_out_pins(&cfg, IR_PIN, 1);
    sm_config_set_set_pins(&cfg, IR_PIN, 1);
    sm_config_set_out_shift(&cfg, false, false, 32); /* left shift, manual pull */
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);

    float div = (float)clock_get_hz(clk_sys) / (float)IR_PIO_FREQ_HZ;
    sm_config_set_clkdiv(&cfg, div);

    pio_gpio_init(g_ir_pio, IR_PIN);
    pio_sm_set_consecutive_pindirs(g_ir_pio, g_ir_sm, IR_PIN, 1, true);

    pio_sm_init(g_ir_pio, g_ir_sm, g_ir_offset, &cfg);
    /* SM starts disabled; enabled per-frame */

    g_ir_dma_chan = dma_claim_unused_channel(true);
}

static void ir_start_dma(const uint32_t *buf, uint count) {
    dma_channel_config c = dma_channel_get_default_config((uint)g_ir_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_ir_pio, g_ir_sm, true));

    dma_channel_configure(
        (uint)g_ir_dma_chan,
        &c,
        &g_ir_pio->txf[g_ir_sm],   /* write: PIO TX FIFO   */
        buf,                        /* read:  waveform array */
        count,                      /* transfer count        */
        true                        /* start immediately     */
    );
}

static bool ir_dma_is_busy(void) {
    if (g_ir_dma_chan < 0) {
        return false;
    }
    return dma_channel_is_busy((uint)g_ir_dma_chan);
}

static bool ir_dma_stop_sync(void) {
    if (g_ir_dma_chan < 0) {
        return true;
    }

    dma_channel_abort((uint)g_ir_dma_chan);

    absolute_time_t deadline = make_timeout_time_us(DMA_ABORT_WAIT_US);
    while (dma_channel_is_busy((uint)g_ir_dma_chan)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

/* ================================================================== */

void ir_emitter_init(void) {
    gpio_init(IR_PIN);
    gpio_set_dir(IR_PIN, GPIO_OUT);
    gpio_put(IR_PIN, 0);

    gpio_init(EYE_LED_PIN);
    gpio_set_dir(EYE_LED_PIN, GPIO_OUT);
    gpio_put(EYE_LED_PIN, 0);

    gpio_init(ACTIVE_LED_PIN);
    gpio_set_dir(ACTIVE_LED_PIN, GPIO_OUT);
    gpio_put(ACTIVE_LED_PIN, 0);

    ir_pio_init();
}

void ir_emitter_update(uint32_t cur_time_ms) {
    if (g_emitter_active) {
        if (!g_emitter_active_last) {
            gpio_put(ACTIVE_LED_PIN, 1);
            g_emitter_active_last = true;
        } else if ((cur_time_ms - g_last_frame_ms) >= 200u) {
            gpio_put(ACTIVE_LED_PIN, 0);
            gpio_put(EYE_LED_PIN, 1);
            g_emitter_active = false;
            g_emitter_active_last = false;
        }
    }
}

void ir_emitter_swap_eyes(uint8_t swap) {
    g_swap_eyes = (swap != 0u) ? 1u : 0u;
}

void ir_emitter_set_eye(uint8_t eye) {
    g_next_eye = (eye ^ g_swap_eyes) & 1u;
}

/* ---------- Deferred waveform fire (runs from timer IRQ) ---------- */

static void do_fire_waveform(void) {
    uint8_t token = g_pending_token;
    if (g_protocol->sizes[token] == 0u) return;

    /* Abort any in-progress waveform safely */
    pio_sm_exec(g_ir_pio, g_ir_sm, pio_encode_set(pio_pins, 0));
    pio_sm_set_enabled(g_ir_pio, g_ir_sm, false);
    if (!ir_dma_stop_sync()) {
        pio_sm_exec(g_ir_pio, g_ir_sm, pio_encode_set(pio_pins, 0));
        pio_sm_clear_fifos(g_ir_pio, g_ir_sm);
        return;
    }
    pio_sm_clear_fifos(g_ir_pio, g_ir_sm);
    pio_sm_restart(g_ir_pio, g_ir_sm);
    pio_sm_exec(g_ir_pio, g_ir_sm, pio_encode_jmp(g_ir_offset));

    g_waveform_len = build_waveform(token);

    g_emitter_active = true;
    g_cur_eye        = g_next_eye;
    g_last_frame_ms  = emitter_millis();

    gpio_put(EYE_LED_PIN, 1);

    ir_start_dma(g_waveform, g_waveform_len);
    pio_sm_set_enabled(g_ir_pio, g_ir_sm, true);
}

static int64_t frame_alarm_cb(alarm_id_t id, void *user_data) {
    (void)id; (void)user_data;
    g_frame_alarm = 0;
    do_fire_waveform();
    return 0;  /* don't reschedule */
}

void ir_emitter_start_frame(void) {
    uint8_t token = (uint8_t)(g_next_eye * 2u);
    if (g_protocol->sizes[token] == 0u) return;

    uint64_t now = time_us_64();

    /* --- Software PLL: always track phase, even during startup gate.
     * This way the PLL is already locked when we start emitting. --- */
    uint64_t target;

    if (!g_pll_locked) {
        /* First packet: seed the phase */
        target = now + FRAME_ALARM_DELAY_US;
        g_phase_us   = target;
        g_pll_locked = true;
        g_pll_frame_count = 0;
    } else {
        uint64_t prev_phase = g_phase_us;

        /* Predict next fire time from phase + period */
        target = g_phase_us + PLL_NOMINAL_PERIOD_US;

        /* Compute error: how far off is the predicted time from the
         * "ideal" time derived from this USB packet? */
        int64_t ideal = (int64_t)(now + FRAME_ALARM_DELAY_US);
        int64_t error = ideal - (int64_t)target;

        if (error > PLL_RESYNC_THRESHOLD || error < -PLL_RESYNC_THRESHOLD) {
            /* Way off — hard re-sync (e.g. after pause or mode change) */
            target = (uint64_t)ideal;
            g_pll_frame_count = 0;
        } else {
            /* Use fast gain at startup for quick convergence,
             * then switch to slow gain for jitter filtering */
            uint shift = (g_pll_frame_count < PLL_FAST_FRAMES)
                       ? PLL_FAST_GAIN_SHIFT
                       : PLL_GAIN_SHIFT;
            target += error >> shift;

            uint64_t period = target - prev_phase;
            if ((period >= PLL_VALID_MIN_PERIOD_US) &&
                (period <= PLL_VALID_MAX_PERIOD_US)) {
                g_last_valid_pll_period_us = (uint32_t)period;
            }
        }
        g_phase_us = target;
        if (g_pll_frame_count < PLL_FAST_FRAMES)
            g_pll_frame_count++;
    }

    /* Compute alarm delay from now; clamp to a small minimum */
    int64_t delay = (int64_t)target - (int64_t)now;
    if (delay < 50) delay = 50;

    uint32_t save = save_and_disable_interrupts();

    /* Cancel any pending alarm */
    if (g_frame_alarm > 0) {
        cancel_alarm(g_frame_alarm);
        g_frame_alarm = 0;
    }

    g_pending_token = token;

    g_frame_alarm = add_alarm_in_us(
        (uint64_t)delay, frame_alarm_cb, NULL, true);

    restore_interrupts(save);
}

void ir_emitter_force_idle(void) {
    uint32_t save = save_and_disable_interrupts();
    if (g_frame_alarm > 0) {
        cancel_alarm(g_frame_alarm);
        g_frame_alarm = 0;
    }
    g_pll_locked = false;
    restore_interrupts(save);

    /* Stop immediately — pin LOW, PIO off, DMA off.
     * The glasses will time out and open both lenses on their own. */
    pio_sm_exec(g_ir_pio, g_ir_sm, pio_encode_set(pio_pins, 0));
    pio_sm_set_enabled(g_ir_pio, g_ir_sm, false);
    (void)ir_dma_stop_sync();
    pio_sm_clear_fifos(g_ir_pio, g_ir_sm);

    g_emitter_active      = false;
    g_emitter_active_last = false;

    gpio_put(EYE_LED_PIN, 1);
    gpio_put(ACTIVE_LED_PIN, 0);
}

bool ir_emitter_is_active(void) {
    return g_emitter_active;
}

bool ir_emitter_is_busy(void) {
    return (g_frame_alarm > 0) || ir_dma_is_busy();
}

uint32_t ir_emitter_get_last_valid_period_us(void) {
    return g_last_valid_pll_period_us;
}
