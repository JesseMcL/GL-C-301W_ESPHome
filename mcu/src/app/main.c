/*
 * GL-C-301W replacement firmware for the Nuvoton MS51FC0AE (TSSOP20)
 * Application (APROM). The MCU is a 5-channel PWM slave behind the T1-U
 * (BK7238) running ESPHome; framing is TuyaMCU (see proto.c, tuya_dp.h).
 *
 * Pin map (reverse engineered from the board):
 *   pin 13 P1.2  PWM0_CH0  -> W driver
 *   pin 14 P1.1  PWM0_CH1  -> C driver
 *   pin 15 P1.0  PWM0_CH2  -> B driver
 *   pin 16 P0.0  PWM0_CH3  -> G driver
 *   pin 17 P0.1  PWM0_CH4  -> R driver
 *   pin  2 P0.6  UART0 TXD -> T1-U P10 (RX1)
 *   pin  3 P0.7  UART0 RXD <- T1-U P11 (TX1)
 *   pin 18 P0.2  PB-Set (also ICP CLK)
 *   pin  4 P2.0  PB-Power/Dimmer (also nRESET)
 *   pin  8 P1.6  red indicator LED (also ICP DAT)
 *   pin 10 P1.5  green indicator LED
 *   pin 11 P1.4  blue indicator LED
 *   pins 1,5,6,12,19,20 -> 2.4 GHz RF IC, left as inputs
 *
 * Clock-trim code follows Nuvoton's MS51 BSP (Apache-2.0).
 */
#include "numicro_8051.h"
#include "config.h"
#include "layout.h"
#include "tuya_dp.h"
#include "proto.h"

/* Globals the BSP's macros expect */
BIT BIT_TMP;
__data uint8_t TA_REG_TMP, BYTE_TMP, SFRS_TMP;

/* App -> bootloader hand-off across the software reset (see layout.h) */
__xdata __at(HANDOFF_XADDR)     volatile uint16_t handoff_magic;
__xdata __at(HANDOFF_XADDR + 2) volatile uint32_t handoff_size;

#define IAP_READ_CONFIG   0xC0

/* ------------------------------------------------------------------ */
/* Time base                                                          */
/* ------------------------------------------------------------------ */
#define T0_RELOAD (65536UL - (FSYS_HZ / 12UL / 1000UL))   /* 1 ms at Fsys/12 */

static volatile uint8_t  tick_pending;
static volatile uint16_t ms_now;

void Timer0_ISR(void) __interrupt(1)
{
    PUSH_SFRS;
    SFRS = 0;
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)T0_RELOAD;
    ms_now++;
    tick_pending++;
    POP_SFRS;
}

static uint16_t millis16(void)
{
    uint16_t t;
    ET0 = 0;
    t = ms_now;
    ET0 = 1;
    return t;
}

/* ------------------------------------------------------------------ */
/* UART0 with ring buffers                                            */
/* ------------------------------------------------------------------ */
#define RXQ_SIZE 64
#define TXQ_SIZE 128

static __xdata uint8_t rxq[RXQ_SIZE];
static __xdata uint8_t txq[TXQ_SIZE];
static volatile uint8_t rxq_head, rxq_tail;
static volatile uint8_t txq_head, txq_tail;
static volatile __bit   tx_busy;

void UART0_ISR(void) __interrupt(4)
{
    uint8_t n;
    PUSH_SFRS;
    SFRS = 0;
    if (RI) {
        RI = 0;
        n = (rxq_head + 1) & (RXQ_SIZE - 1);
        if (n != rxq_tail) {
            rxq[rxq_head] = SBUF;
            rxq_head = n;
        }
    }
    if (TI) {
        TI = 0;
        if (txq_tail != txq_head) {
            SBUF = txq[txq_tail];
            txq_tail = (txq_tail + 1) & (TXQ_SIZE - 1);
        } else {
            tx_busy = 0;
        }
    }
    POP_SFRS;
}

void hal_uart_put(uint8_t b)
{
    uint8_t n = (txq_head + 1) & (TXQ_SIZE - 1);
    while (n == txq_tail)
        ;
    ES = 0;
    if (!tx_busy) {
        tx_busy = 1;
        SFRS = 0;
        SBUF = b;
    } else {
        txq[txq_head] = b;
        txq_head = n;
    }
    ES = 1;
}

static void uart0_init(void)
{
    P06_PUSHPULL_MODE;
    P06 = 1;
    P07_INPUT_MODE;

    SFRS = 0;
    SCON = 0x50;
    set_PCON_SMOD;
    T3CON &= 0xF8;
    set_T3CON_BRCK;
    RH3 = (uint8_t)((65536UL - (FSYS_HZ / 16UL / UART_BAUD)) >> 8);
    RL3 = (uint8_t)(65536UL - (FSYS_HZ / 16UL / UART_BAUD));
    set_T3CON_TR3;
    TI = 0;
    RI = 0;
    tx_busy = 0;
    ES = 1;
}

/* ------------------------------------------------------------------ */
/* Clock: trim HIRC to 24 MHz (as Nuvoton MODIFY_HIRC(HIRC_24))       */
/* ------------------------------------------------------------------ */
static void hirc_24mhz(void)
{
    uint8_t t0, t1;
    SFRS = 0;
    set_CHPCON_IAPEN;
    IAPAL = 0x38;
    IAPAH = 0x00;
    IAPCN = READ_UID;
    set_IAPTRG_IAPGO;
    t0 = IAPFD;
    IAPAL++;
    set_IAPTRG_IAPGO;
    t1 = IAPFD;
    TA = 0xAA; TA = 0x55; RCTRIM0 = t0;
    TA = 0xAA; TA = 0x55; RCTRIM1 = t1;
    clr_CHPCON_IAPEN;
    PCON &= CLR_BIT4;
}

static uint8_t read_config_byte(uint8_t idx)
{
    uint8_t v;
    SFRS = 0;
    set_CHPCON_IAPEN;
    IAPAH = 0;
    IAPAL = idx;
    IAPCN = IAP_READ_CONFIG;
    set_IAPTRG_IAPGO;
    v = IAPFD;
    clr_CHPCON_IAPEN;
    return v;
}

/* ------------------------------------------------------------------ */
/* Firmware update hand-off                                           */
/* ------------------------------------------------------------------ */
uint8_t app_ota_request(uint32_t size)
{
    /* CONFIG1.LDS: 111 = no LDROM -> nothing to reset into */
    if ((read_config_byte(1) & 0x07) == 0x07)
        return 0;
    if (size == 0 || size > OTA_MAX_SIZE)
        return 0;

    handoff_size = size;
    handoff_magic = HANDOFF_MAGIC;

    EA = 0;
    SFRS = 0;
    TA = 0xAA; TA = 0x55; CHPCON |= 0x02;       /* BS = 1: boot LDROM */
    TA = 0xAA; TA = 0x55; CHPCON |= 0x80;       /* SWRST */
    for (;;)
        ;
}

/* ------------------------------------------------------------------ */
/* PWM + fading                                                       */
/* ------------------------------------------------------------------ */
static __xdata uint16_t tgt[NUM_CH];
static __xdata uint16_t cur[NUM_CH];
static __xdata int32_t  cur_fp[NUM_CH];      /* value << 8 */
static __xdata int32_t  step_fp[NUM_CH];
static __xdata uint16_t remain[NUM_CH];
static __xdata uint16_t saved[NUM_CH];
static uint16_t fade_ms = DEFAULT_FADE_MS;
static __bit pwm_dirty;

static void pwm_write_duties(uint16_t d0, uint16_t d1, uint16_t d2, uint16_t d3, uint16_t d4);

static uint16_t duty_of(uint16_t v)
{
    if (v == 0xFFFF)
        return PWM_PERIOD + 1;           /* > period = 100 % */
    return (uint16_t)(((uint32_t)v * (uint32_t)(PWM_PERIOD + 1)) >> 16);
}

static void pwm_apply(void)
{
    uint16_t d0, d1, d2, d3, d4;
    uint16_t guard = 0;

    d0 = duty_of(cur[CH_W]);
    d1 = duty_of(cur[CH_C]);
    d2 = duty_of(cur[CH_B]);
    d3 = duty_of(cur[CH_G]);
    d4 = duty_of(cur[CH_R]);

    SFRS = 0;
    while ((PWM0CON0 & 0x40) && ++guard < 2000)
        ;                                /* previous LOAD still pending */

    pwm_write_duties(d0, d1, d2, d3, d4);
    SFRS = 0;
    PWM0CON0 |= 0x40;                    /* LOAD at end of period */
}

static void pwm_write_duties(uint16_t d0, uint16_t d1, uint16_t d2, uint16_t d3, uint16_t d4)
{
    SFRS = 0;
    PWM0C0H = d0 >> 8; PWM0C0L = (uint8_t)d0;
    PWM0C1H = d1 >> 8; PWM0C1L = (uint8_t)d1;
    PWM0C2H = d2 >> 8; PWM0C2L = (uint8_t)d2;
    PWM0C3H = d3 >> 8; PWM0C3L = (uint8_t)d3;
    SFRS = 1;
    PWM0C4H = d4 >> 8; PWM0C4L = (uint8_t)d4;
    SFRS = 0;
}

static void pwm_init(void)
{
    P12 = 0; P11 = 0; P10 = 0; P00 = 0; P01 = 0;
    P12_PUSHPULL_MODE; P11_PUSHPULL_MODE; P10_PUSHPULL_MODE;
    P00_PUSHPULL_MODE; P01_PUSHPULL_MODE;

    SFRS = 0;
    clr_CKCON_PWMCKS;                    /* PWM clock = Fsys */
    PWM0CON1 = 0x00;                     /* independent, edge-aligned, /1 */

    /* PNP bit n inverts PWM0_CHn; map the RGBCW mask onto channels */
    PNP = ((PWM_INVERT_MASK >> CH_W) & 1) << 0 |
          ((PWM_INVERT_MASK >> CH_C) & 1) << 1 |
          ((PWM_INVERT_MASK >> CH_B) & 1) << 2 |
          ((PWM_INVERT_MASK >> CH_G) & 1) << 3 |
          ((PWM_INVERT_MASK >> CH_R) & 1) << 4;

    PWM0PH = (uint8_t)(PWM_PERIOD >> 8);
    PWM0PL = (uint8_t)PWM_PERIOD;

    pwm_write_duties(duty_of(cur[CH_W]), duty_of(cur[CH_C]), duty_of(cur[CH_B]),
                     duty_of(cur[CH_G]), duty_of(cur[CH_R]));
    ENABLE_PWM0_CH0_P12_OUTPUT;
    ENABLE_PWM0_CH1_P11_OUTPUT;
    ENABLE_PWM0_CH2_P10_OUTPUT;
    ENABLE_PWM0_CH3_P00_OUTPUT;
    ENABLE_PWM0_CH4_P01_OUTPUT;
    SFRS = 0;
    PWM0CON0 |= 0xC0;                    /* RUN + LOAD */
}

static void set_channel_fade(uint8_t ch, uint16_t v, uint16_t ms)
{
    tgt[ch] = v;
    if (ms == 0) {
        cur[ch] = v;
        cur_fp[ch] = (int32_t)v << 8;
        remain[ch] = 0;
        pwm_dirty = 1;
    } else {
        step_fp[ch] = (((int32_t)v << 8) - cur_fp[ch]) / (int32_t)ms;
        remain[ch] = ms;
    }
}

void app_set_channel(uint8_t ch, uint16_t v) { set_channel_fade(ch, v, fade_ms); }
uint16_t app_get_channel(uint8_t ch)          { return tgt[ch]; }
void app_set_fade(uint16_t ms)                { fade_ms = ms; }
uint16_t app_get_fade(void)                   { return fade_ms; }

static void fade_tick(void)
{
    uint8_t i;
    uint16_t nv;
    for (i = 0; i < NUM_CH; i++) {
        if (!remain[i])
            continue;
        if (--remain[i] == 0)
            cur_fp[i] = (int32_t)tgt[i] << 8;
        else
            cur_fp[i] += step_fp[i];
        nv = (uint16_t)(cur_fp[i] >> 8);
        if (nv != cur[i]) {
            cur[i] = nv;
            pwm_dirty = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Indicator LEDs (R = P1.6, G = P1.5, B = P1.4)                      */
/* ------------------------------------------------------------------ */
static uint8_t led_mode = LED_MODE_AUTO;
static uint8_t led_manual[NUM_LED];

void app_set_led_mode(uint8_t m)          { led_mode = m; }
uint8_t app_get_led_mode(void)            { return led_mode; }
void app_set_led(uint8_t led, uint8_t on) { led_manual[led] = on; }
uint8_t app_get_led(uint8_t led)          { return led_manual[led]; }

#if LED_ACTIVE_LOW
#define LED_LEVEL(on) (!(on))
#else
#define LED_LEVEL(on) (on)
#endif

static void leds_write(uint8_t r, uint8_t g, uint8_t b)
{
    P16 = LED_LEVEL(r);
    P15 = LED_LEVEL(g);
    P14 = LED_LEVEL(b);
}

static void leds_init(void)
{
    leds_write(0, 0, 0);
    P16_PUSHPULL_MODE;
    P15_PUSHPULL_MODE;
    P14_PUSHPULL_MODE;
}

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  stable;        /* 1 = pressed */
    uint8_t  cnt;
    uint16_t held;
} btn_t;

static btn_t btns[2];
static uint16_t link_age = 0xFFFF;     /* ms since last valid frame */

uint8_t app_get_button(uint8_t b) { return btns[b].stable; }

static void local_power_toggle(void)
{
    uint8_t i, any = 0;
    uint16_t v;

    for (i = 0; i < NUM_CH; i++)
        if (tgt[i]) any = 1;

    if (any) {
        for (i = 0; i < NUM_CH; i++) {
            saved[i] = tgt[i];
            set_channel_fade(i, 0, LOCAL_FADE_MS);
        }
        return;
    }
    for (i = 0; i < NUM_CH; i++)
        if (saved[i]) any = 1;
    for (i = 0; i < NUM_CH; i++) {
        if (any) {
            v = saved[i];
        } else {
            switch (i) {
            case CH_R: v = LOCAL_DEFAULT_R; break;
            case CH_G: v = LOCAL_DEFAULT_G; break;
            case CH_B: v = LOCAL_DEFAULT_B; break;
            case CH_C: v = LOCAL_DEFAULT_C; break;
            default:   v = LOCAL_DEFAULT_W; break;
            }
        }
        set_channel_fade(i, v, LOCAL_FADE_MS);
    }
}

/* Called every 1 ms with the raw (active-high) pin state. */
static void btn_tick(uint8_t idx, uint8_t raw)
{
    btn_t *b = &btns[idx];
    if (raw != b->stable) {
        if (++b->cnt >= BTN_DEBOUNCE_MS) {
            b->stable = raw;
            b->cnt = 0;
            proto_report_button(idx);
            if (raw) {
                b->held = 0;
            } else if (b->held < BTN_LONG_MS && idx == BTN_POWER &&
                       link_age > LINK_TIMEOUT_MS) {
                local_power_toggle();          /* ESPHome unreachable */
            }
        }
    } else {
        b->cnt = 0;
    }
    if (b->stable && b->held < 0xFFFF)
        b->held++;
}

static void buttons_init(void)
{
    P02 = 1;
    P02_QUASI_MODE;                      /* weak pull-up */
    /* P2.0 is input-only; external pull-up on the board */
}

/* ------------------------------------------------------------------ */
/* Watchdog                                                           */
/* ------------------------------------------------------------------ */
static void wdt_init(void)
{
    SFRS = 0;
    BIT_TMP = EA; EA = 0;
    TA = 0xAA; TA = 0x55; WDCON |= 0x07; /* /256 -> ~1.6 s */
    EA = BIT_TMP;
}

/* ------------------------------------------------------------------ */
void main(void)
{
    uint16_t now, led_phase = 0;
    uint8_t  wdt_cnt = 0, b, r, g, bl;

    clr_AUXR1_SWRF;                      /* see bootloader: SWRF = OTA request */
    handoff_magic = 0;

    hirc_24mhz();
    wdt_init();

    cur[CH_R] = BOOT_LEVEL_R; cur[CH_G] = BOOT_LEVEL_G; cur[CH_B] = BOOT_LEVEL_B;
    cur[CH_C] = BOOT_LEVEL_C; cur[CH_W] = BOOT_LEVEL_W;
    for (b = 0; b < NUM_CH; b++) {
        tgt[b] = cur[b];
        cur_fp[b] = (int32_t)cur[b] << 8;
    }
    pwm_init();
    leds_init();
    buttons_init();

    proto_init();
    uart0_init();

    SFRS = 0;
    TMOD = (TMOD & 0xF0) | 0x01;
    clr_CKCON_T0M;
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)T0_RELOAD;
    TR0 = 1;
    ET0 = 1;
    EA = 1;

    for (;;) {
        now = millis16();
        while (rxq_tail != rxq_head) {
            b = rxq[rxq_tail];
            rxq_tail = (rxq_tail + 1) & (RXQ_SIZE - 1);
            proto_rx(b, now);
        }
        proto_poll(now);

        if (proto_link_seen) {
            proto_link_seen = 0;
            link_age = 0;
        }

        while (tick_pending) {
            ET0 = 0;
            tick_pending--;
            ET0 = 1;

            fade_tick();

            if (link_age < 0xFFFF)
                link_age++;

            btn_tick(BTN_SET, !P02);
            btn_tick(BTN_POWER, !P20);

            /* Indicator */
            led_phase++;
            if (led_mode == LED_MODE_MANUAL) {
                r = led_manual[LED_R]; g = led_manual[LED_G]; bl = led_manual[LED_B];
            } else {
                r = g = bl = 0;
                if (link_age > LINK_TIMEOUT_MS)
                    r = (led_phase & 0x7FF) < 100;           /* red blip / 2 s */
                else if (proto_wifi_state <= 0x01)
                    bl = (led_phase & 0x80) != 0;            /* blue ~4 Hz: pairing/AP */
                else if (proto_wifi_state == 0x02 || proto_wifi_state == WIFI_UNKNOWN)
                    g = (led_phase & 0x200) != 0;            /* green ~1 Hz: no Wi-Fi */
                /* connected: dark */
            }
            leds_write(r, g, bl);

            if (++wdt_cnt >= WDT_KICK_MS) {
                wdt_cnt = 0;
                set_WDCON_WDCLR;
            }
        }

        if (pwm_dirty) {
            pwm_dirty = 0;
            pwm_apply();
        }
    }
}
