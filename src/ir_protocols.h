#ifndef IR_PROTOCOLS_H
#define IR_PROTOCOLS_H

#include <stdint.h>

typedef struct {
    uint8_t sizes[4];
    uint8_t indices[4];
    const uint16_t *timings;
} ir_protocol_t;

static const uint16_t IR_TIMINGS_SAMSUNG07[] = {14, 12, 14, 12, 14};
static const ir_protocol_t IR_PROT_SAMSUNG07 = {
    .sizes = {5, 0, 0, 0},
    .indices = {0, 0, 0, 0},
    .timings = IR_TIMINGS_SAMSUNG07,
};

static const uint16_t IR_TIMINGS_XPAND[] = {18, 20, 18, 20, 18, 18, 60, 18};
static const ir_protocol_t IR_PROT_XPAND = {
    .sizes = {5, 0, 3, 0},
    .indices = {0, 0, 5, 0},
    .timings = IR_TIMINGS_XPAND,
};

static const uint16_t IR_TIMINGS_3DVISION[] = {
    23, 46, 31,
    23, 21, 24,
    43,
    23, 78, 40
};
static const ir_protocol_t IR_PROT_3DVISION = {
    .sizes = {3, 3, 1, 3},
    .indices = {0, 3, 6, 7},
    .timings = IR_TIMINGS_3DVISION,
};

static const uint16_t IR_TIMINGS_SHARP[] = {
    20, 20, 20, 20, 20, 80, 20, 140, 20, 20, 20, 80, 20, 20, 20,
    20, 20, 20, 20, 20, 60, 20, 60, 20, 20, 20, 80, 20, 20, 20
};
static const ir_protocol_t IR_PROT_SHARP = {
    .sizes = {15, 0, 15, 0},
    .indices = {0, 0, 15, 0},
    .timings = IR_TIMINGS_SHARP,
};

static const uint16_t IR_TIMINGS_SONY[] = {
    20, 20, 20, 20, 20, 300, 20, 20, 20,
    20, 20, 20, 20, 20, 220, 20, 20, 20,
    20, 20, 20, 20, 20, 140, 20, 20, 20,
    20, 20, 20, 20, 20, 380, 20, 20, 20
};
static const ir_protocol_t IR_PROT_SONY = {
    .sizes = {9, 9, 9, 9},
    .indices = {27, 0, 9, 18},
    .timings = IR_TIMINGS_SONY,
};

static const uint16_t IR_TIMINGS_PANASONIC[] = {
    20, 20, 20, 100, 20, 20, 20,
    20, 60, 20, 20, 20, 60, 20,
    20, 60, 20, 60, 20, 20, 20,
    20, 20, 20, 60, 20, 60, 20
};
static const ir_protocol_t IR_PROT_PANASONIC = {
    .sizes = {7, 7, 7, 7},
    .indices = {0, 7, 14, 21},
    .timings = IR_TIMINGS_PANASONIC,
};

#endif
