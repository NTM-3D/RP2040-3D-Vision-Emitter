#include "status_led.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

// RP2040-Zero built-in WS2812B data pin.
#define STATUS_LED_WS2812_PIN 16u
#define STATUS_LED_WS2812_FREQ 800000u

// Compiled form of the standard 4-instruction ws2812 PIO program.
static const uint16_t ws2812_program_instructions[] = {
    0x6221,
    0x1123,
    0x1400,
    0xA442,
};

static const struct pio_program ws2812_program = {
    .instructions = ws2812_program_instructions,
    .length = 4,
    .origin = -1,
};

enum {
    WS2812_WRAP_TARGET = 0,
    WS2812_WRAP = 3,
    WS2812_T1 = 3,
    WS2812_T2 = 3,
    WS2812_T3 = 4,
};

static PIO g_status_led_pio = pio0;
static uint g_status_led_sm = 0;
static uint g_status_led_offset = 0;
static bool g_status_led_ready = false;
static uint32_t g_status_led_last_color = 0xffffffffu;

#define STATUS_LED_BRIGHTNESS_PERCENT 1u

static inline uint8_t scale_brightness(uint8_t value) {
    return (uint8_t)(((uint16_t)value * STATUS_LED_BRIGHTNESS_PERCENT) / 100u);
}

static inline pio_sm_config ws2812_program_get_default_config(uint offset) {
    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset + WS2812_WRAP_TARGET, offset + WS2812_WRAP);
    sm_config_set_sideset(&config, 1, false, false);
    return config;
}

static void ws2812_program_init(PIO pio, uint sm, uint offset, uint pin, float freq) {
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

    pio_sm_config config = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&config, pin);
    sm_config_set_out_shift(&config, false, true, 24);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

    int cycles_per_bit = WS2812_T1 + WS2812_T2 + WS2812_T3;
    float divider = (float)clock_get_hz(clk_sys) / (freq * (float)cycles_per_bit);
    sm_config_set_clkdiv(&config, divider);

    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_enabled(pio, sm, true);
}

static inline uint32_t ws2812_color_from_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return ((uint32_t)green << 16) | ((uint32_t)red << 8) | (uint32_t)blue;
}

void status_led_init(void) {
    g_status_led_offset = pio_add_program(g_status_led_pio, &ws2812_program);
    ws2812_program_init(g_status_led_pio, g_status_led_sm, g_status_led_offset, STATUS_LED_WS2812_PIN, (float)STATUS_LED_WS2812_FREQ);
    g_status_led_ready = true;
    g_status_led_last_color = 0xffffffffu;
    status_led_set_rgb(0u, 0u, 0u);
}

void status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    if (!g_status_led_ready) {
        return;
    }

    red = scale_brightness(red);
    green = scale_brightness(green);
    blue = scale_brightness(blue);

    uint32_t color = ws2812_color_from_rgb(red, green, blue);
    if (color == g_status_led_last_color) {
        return;
    }

    pio_sm_put_blocking(g_status_led_pio, g_status_led_sm, color << 8u);
    sleep_us(60);
    g_status_led_last_color = color;
}

void status_led_update(bool usb_connected, bool emitter_active) {
    if (!usb_connected) {
		// Red when USB is not connected
        status_led_set_rgb(0xffu, 0x00u, 0x00u);
        return;
    }

    if (emitter_active) {
		// Green when 3D is active
        status_led_set_rgb(0x00u, 0xffu, 0x00u);
        return;
    }

	// Blue when idle
    status_led_set_rgb(0x00u, 0x00u, 0xffu);
}