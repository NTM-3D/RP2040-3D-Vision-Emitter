#ifndef IR_EMITTER_H
#define IR_EMITTER_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_DURATION_HUS (2u * 400u)
#define FRAME_PAN_HUS (2u * 0u)
#define FRAME_ALARM_DELAY_US 3000u

#define IR_PIN 2
#define EYE_LED_PIN 3
#define ACTIVE_LED_PIN 4
#define STBY_LED_PIN 5

void ir_emitter_init(void);
void ir_emitter_update(uint32_t cur_time_ms);
void ir_emitter_swap_eyes(uint8_t swap);
void ir_emitter_set_eye(uint8_t eye);
void ir_emitter_start_frame(void);
void ir_emitter_force_idle(void);
bool ir_emitter_is_active(void);
bool ir_emitter_is_busy(void);
uint32_t ir_emitter_get_last_valid_period_us(void);

#endif
