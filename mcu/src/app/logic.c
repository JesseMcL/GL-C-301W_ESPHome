#include "logic.h"
#include "config.h"
#include "layout.h"
#include "tuya_dp.h"

__xdata settings_t cfg;

/* ------------------------------------------------------------------ */
/* Settings log                                                       */
/*                                                                    */
/* One flash page of 8-byte records, appended in order:               */
/*   tag 0x5B, freq, memory, led_mode, fade lo, fade hi, 0, check     */
/* check = ~(sum of the first 7 bytes). The newest valid record wins; */
/* a torn record fails the check and is skipped. When the page is     */
/* full it is erased and the log restarts at slot 0 (16 saves per     */
/* erase).                                                            */
/* ------------------------------------------------------------------ */
#define SLOTS (FLASH_PAGE_SIZE / SETTINGS_RECORD)

static __xdata uint8_t rec[SETTINGS_RECORD];

void settings_defaults(void)
{
    cfg.freq = PWM_FREQ_1000;
    cfg.memory = 0;
    cfg.led_mode = LED_MODE_AUTO;
    cfg.fade_ms = DEFAULT_FADE_MS;
}

static void encode(void)
{
    uint8_t i, sum = 0;
    rec[0] = SETTINGS_TAG;
    rec[1] = cfg.freq;
    rec[2] = cfg.memory;
    rec[3] = cfg.led_mode;
    rec[4] = (uint8_t)cfg.fade_ms;
    rec[5] = (uint8_t)(cfg.fade_ms >> 8);
    rec[6] = 0;
    for (i = 0; i < SETTINGS_RECORD - 1; i++)
        sum += rec[i];
    rec[SETTINGS_RECORD - 1] = (uint8_t)~sum;
}

static void read_slot(uint8_t slot)
{
    uint8_t i;
    uint16_t a = SETTINGS_ADDR + (uint16_t)slot * SETTINGS_RECORD;
    for (i = 0; i < SETTINGS_RECORD; i++)
        rec[i] = lhal_flash_read(a + i);
}

static uint8_t slot_valid(void)
{
    uint8_t i, sum = 0;
    for (i = 0; i < SETTINGS_RECORD - 1; i++)
        sum += rec[i];
    return rec[0] == SETTINGS_TAG && rec[SETTINGS_RECORD - 1] == (uint8_t)~sum &&
           rec[1] < PWM_FREQ_COUNT && rec[2] <= 1 && rec[3] < LED_MODE_COUNT &&
           (uint16_t)(rec[4] | ((uint16_t)rec[5] << 8)) <= (uint16_t)FADE_MS_MAX &&
           rec[6] == 0;
}

static uint8_t slot_empty(void)
{
    uint8_t i;
    for (i = 0; i < SETTINGS_RECORD; i++)
        if (rec[i] != 0xFF)
            return 0;
    return 1;
}

uint8_t settings_load(void)
{
    uint8_t s, found = 0;
    settings_defaults();
    for (s = 0; s < SLOTS; s++) {
        read_slot(s);
        if (slot_empty())
            break;
        if (slot_valid()) {
            cfg.freq = rec[1];
            cfg.memory = rec[2];
            cfg.led_mode = rec[3];
            cfg.fade_ms = (uint16_t)rec[4] | ((uint16_t)rec[5] << 8);
            found = 1;
        }
    }
    return found;
}

void settings_save(void)
{
    uint8_t s, i, last = 0xFF, same;
    uint16_t a;

    /* find the newest valid record and the first free slot */
    for (s = 0; s < SLOTS; s++) {
        read_slot(s);
        if (slot_empty())
            break;
        if (slot_valid())
            last = s;
    }
    encode();
    if (last != 0xFF) {
        a = SETTINGS_ADDR + (uint16_t)last * SETTINGS_RECORD;
        same = 1;
        for (i = 0; i < SETTINGS_RECORD; i++)
            if (lhal_flash_read(a + i) != rec[i])
                same = 0;
        if (same)
            return;
    }
    if (s >= SLOTS) {
        lhal_flash_erase(SETTINGS_ADDR);
        s = 0;
    }
    a = SETTINGS_ADDR + (uint16_t)s * SETTINGS_RECORD;
    for (i = 0; i < SETTINGS_RECORD; i++)
        lhal_flash_write(a + i, rec[i]);
}

/* ------------------------------------------------------------------ */
/* PWM frequency                                                      */
/* ------------------------------------------------------------------ */
static const uint16_t __code freq_hz[PWM_FREQ_COUNT] = { 600, 800, 1000, 2000, 4000, 8000 };

uint16_t pwm_period_of(uint8_t freq)
{
    if (freq >= PWM_FREQ_COUNT)
        freq = PWM_FREQ_1000;
    return (uint16_t)(FSYS_HZ / freq_hz[freq] - 1);
}

uint8_t pwm_freq_next(uint8_t freq)
{
    return (freq + 1 >= PWM_FREQ_COUNT) ? 0 : freq + 1;
}

uint8_t pwm_freq_flashes(uint8_t freq)
{
    return freq + 1;
}

void default_on(uint16_t *l)
{
    l[CH_R] = LOCAL_DEFAULT_R;
    l[CH_G] = LOCAL_DEFAULT_G;
    l[CH_B] = LOCAL_DEFAULT_B;
    l[CH_C] = LOCAL_DEFAULT_C;
    l[CH_W] = LOCAL_DEFAULT_W;
}

/* ------------------------------------------------------------------ */
/* Set button: 1/2/3 clicks, 3 s hold                                 */
/* ------------------------------------------------------------------ */
static __xdata uint8_t  g_prev, g_count, g_long;
static __xdata uint16_t g_held, g_gap;

void gesture_reset(void)
{
    g_prev = g_count = g_long = 0;
    g_held = g_gap = 0;
}

uint8_t gesture_tick(uint8_t pressed)
{
    uint8_t ev = GEST_NONE;

    if (pressed) {
        if (!g_prev)
            g_held = 0;
        if (g_held < 0xFFFF)
            g_held++;
        if (g_held == GEST_LONG_MS && !g_long) {
            g_long = 1;
            g_count = 0;
            ev = GEST_LONG;
        }
    } else if (g_prev) {                /* released */
        if (g_long)
            g_long = 0;                 /* the hold already fired */
        else if (g_count < 0xFF)
            g_count++;
        g_gap = 0;
    } else if (g_count) {
        if (++g_gap >= GEST_GAP_MS) {
            if (g_count <= 3)
                ev = g_count;           /* GEST_CLICK1..3 */
            g_count = 0;
        }
    }
    g_prev = pressed;
    return ev;
}
