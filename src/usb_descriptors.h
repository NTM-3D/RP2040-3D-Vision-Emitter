#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdint.h>

uint8_t const *tud_descriptor_device_cb(void);
uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
uint8_t const *tud_descriptor_device_qualifier_cb(void);
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid);

#endif
