#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "tusb.h"

#include "emitter.h"
#include "ir_emitter.h"
#include "status_led.h"

static volatile uint32_t g_millis_passed = 0;
static repeating_timer_t g_ms_timer;

static bool ms_tick_cb(repeating_timer_t *timer) {
    (void) timer;
    g_millis_passed++;
    return true;
}

uint32_t emitter_millis(void) {
    return g_millis_passed;
}

int main(void) {
    stdio_init_all();

    add_repeating_timer_ms(1, ms_tick_cb, NULL, &g_ms_timer);

    status_led_init();
    ir_emitter_init();
    emitter_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    tusb_init(0, &dev_init);

    while (true) {
        tud_task();
        uint32_t now_ms = emitter_millis();
        emitter_task(now_ms);
        status_led_update(tud_mounted(), emitter_is_active());
    }
}
