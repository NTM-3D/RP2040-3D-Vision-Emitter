#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

void status_led_init(void);
void status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
void status_led_update(bool usb_connected, bool emitter_active);

#endif