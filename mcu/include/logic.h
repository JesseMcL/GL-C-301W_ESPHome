/*
 * Hardware-independent application logic: persistent settings, PWM
 * frequency table and Set-button gestures.
 * Compiles with SDCC for the MS51 and with gcc for the host tests.
 */
#ifndef LOGIC_H
#define LOGIC_H

#include <stdint.h>
#include "proto.h"

/* ---- settings (persisted in the SETTINGS_ADDR flash page) ------------ */
typedef struct {
    uint8_t  freq;          /* PWM_FREQ_*         DP 113 */
    uint8_t  memory;        /* 0/1                DP 114 */
    uint8_t  led_mode;      /* LED_MODE_*         DP 109 */
    uint16_t fade_ms;       /* 0..FADE_MS_MAX     DP 106 */
} settings_t;

extern __xdata settings_t cfg;

void    settings_defaults(void);
/* Load the newest valid record; 0 (and defaults) if there is none. */
uint8_t settings_load(void);
/* Append cfg to the log (no-op if unchanged); erases the page when full. */
void    settings_save(void);

#define SETTINGS_RECORD     8U
#define SETTINGS_TAG        0x5BU

/* flash access, provided by main.c / tests (addresses are APROM offsets) */
uint8_t lhal_flash_read(uint16_t addr);
void    lhal_flash_erase(uint16_t addr);
void    lhal_flash_write(uint16_t addr, uint8_t b);

/* ---- PWM frequency --------------------------------------------------- */
uint16_t pwm_period_of(uint8_t freq);   /* PWM0P value (counts - 1) at Fsys */
uint8_t  pwm_freq_next(uint8_t freq);   /* Set x2: 1000->2000->4000->8000->600->800 */
uint8_t  pwm_freq_flashes(uint8_t freq);/* indicator flashes, 600 Hz = 1 .. 8 kHz = 6 */

/* ---- misc ------------------------------------------------------------ */
/* channel levels (R,G,B,C,W) for "on" when there is nothing to restore */
void    default_on(uint16_t *levels);

/* indicator colour: bit2 = red, bit1 = green, bit0 = blue */
#define COLOUR_R            4U
#define COLOUR_G            2U
#define COLOUR_B            1U
#define COLOUR_PURPLE       (COLOUR_R | COLOUR_B)

/* ---- Set button gestures --------------------------------------------- */
#define GEST_NONE           0
#define GEST_CLICK1         1     /* not used by the MCU (stock: strip type) */
#define GEST_CLICK2         2     /* switch PWM frequency   */
#define GEST_CLICK3         3     /* toggle power memory    */
#define GEST_LONG           4     /* factory defaults       */

#define GEST_GAP_MS         400U  /* release gap that ends a click series */
#define GEST_LONG_MS        3000U

void    gesture_reset(void);
/* Call every 1 ms with the debounced state (1 = pressed). */
uint8_t gesture_tick(uint8_t pressed);

#endif
