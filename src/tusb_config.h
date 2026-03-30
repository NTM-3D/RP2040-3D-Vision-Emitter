#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#ifdef __cplusplus
}
#endif

#endif
