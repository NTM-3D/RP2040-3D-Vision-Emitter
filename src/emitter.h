#ifndef EMITTER_H
#define EMITTER_H

#include <stdbool.h>
#include <stdint.h>

#define EMITTER_EP_SWAP_OUT 0x01
#define EMITTER_EP_CONTROL_OUT 0x02
#define EMITTER_EP_BUTTON_IN 0x82
#define EMITTER_EP_CONTROL_IN 0x84
#define EMITTER_EPSIZE 32

void emitter_init(void);
void emitter_task(uint32_t cur_time_ms);

void emitter_handle_control_out(const uint8_t *data, uint16_t len);
void emitter_handle_swap_out(const uint8_t *data, uint16_t len);

uint16_t emitter_build_control_in(uint8_t *out, uint16_t max_len);
bool emitter_control_in_pending(void);
bool emitter_is_active(void);
bool emitter_is_holdover_active(void);

uint32_t emitter_millis(void);

#endif
