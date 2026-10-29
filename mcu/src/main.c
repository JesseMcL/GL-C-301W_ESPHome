/*
 * GL-C-301W replacement firmware for the Nuvoton MS51FC0AE (TSSOP20)
 *
 * The MCU becomes a 5-channel PWM slave behind the T1-U (BK7238) module,
 * which runs ESPHome. Framing is TuyaMCU so ESPHome's stock `tuya:` component
 * can talk to it; see proto.h for the datapoints.
 *
 * Pin map (from the board):
 *   pin 13 P1.2  PWM0_CH0  -> W driver
 *   pin 14 P1.1  PWM0_CH1  -> C driver
 *   pin 15 P1.0  PWM0_CH2  -> B driver
 *   pin 16 P0.0  PWM0_CH3  -> G driver
 *   pin 17 P0.1  PWM0_CH4  -> R driver
 *   pin  2 P0.6  UART0 TXD -> T1-U P10 (RX1)
 *   pin  3 P0.7  UART0 RXD <- T1-U P11 (TX1)
 *   pin 18 P0.2  PB-Set (also ICP CLK)
 *   pin  4 P2.0  PB-Power/Dimmer (also nRESET)
 *   pin 10 P1.5  status LED
 *   pin 11 P1.4  aux LED
 *   pin  8 P1.6  ICP DAT + LED     -> left as input (not touched)
 *   pins 1,5,6,12,19,20            -> 2.4 GHz RF IC, left as inputs
 *
 * Small parts of the clock-trim code follow Nuvoton's MS51 BSP (Apache-2.0).
 */
#include "numicro_8051.h"
#include "config.h"
#include "proto.h"

/* Globals the BSP's macros expect */
BIT BIT_TMP;
__data uint8_t TA_REG_TMP, BYTE_TMP, SFRS_TMP;

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
        if (n != rxq_tail) {             /* drop on overflow */
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
        ;                                /* wait for room (ISR drains) */
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
    SCON = 0x50;                         /* mode 1, REN */
    set_PCON_SMOD;                       /* double rate */
    T3CON &= 0xF8;                       /* T3 prescale 1 */
    set_T3CON_BRCK;                      /* UART0 baud from Timer3 */
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

/* ------------------------------------------------------------------ */
/* PWM + fading                                                       */
/* ------------------------------------------------------------------ */
static __xdata uint16_t tgt[NUM_CH];
static __xdata uint16_t cur[NUM_CH];
static __xdata int32_t  cur_fp[NUM_CH];      /* value << 8 */
static __xdata int32_t  step_fp[NUM_CH];
static __xdata uint16_t remain[NUM_CH];
static __xdata uint16_t saved[NUM_CH];
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

static void pwm_write_regs(void)
{
    pwm_write_duties(duty_of(cur[CH_W]), duty_of(cur[CH_C]), duty_of(cur[CH_B]),
                     duty_of(cur[CH_G]), duty_of(cur[CH_R]));
}

static void pwm_init(void)
{
    /* Drive low first so nothing flashes while PWM is configured */
    P12 = 0; P11 = 0; P10 = 0; P00 = 0; P01 = 0;
    P12_PUSHPULL_MODE; P11_PUSHPULL_MODE; P10_PUSHPULL_MODE;
    P00_PUSHPULL_MODE; P01_PUSHPULL_MODE;

    SFRS = 0;
    clr_CKCON_PWMCKS;                    /* PWM clock = Fsys */
    PWM0CON1 = 0x00;                     /* independent, edge-aligned, /1 */

    /* Polarity: PNP bit n inverts PWM0_CHn.  Map RGBCW mask to CH bits. */
    PNP = ((PWM_INVERT_MASK >> CH_W) & 1) << 0 |
          ((PWM_INVERT_MASK >> CH_C) & 1) << 1 |
          ((PWM_INVERT_MASK >> CH_B) & 1) << 2 |
          ((PWM_INVERT_MASK >> CH_G) & 1) << 3 |
          ((PWM_INVERT_MASK >> CH_R) & 1) << 4;

    PWM0PH = (uint8_t)(PWM_PERIOD >> 8);
    PWM0PL = (uint8_t)PWM_PERIOD;

    pwm_write_regs();
    ENABLE_PWM0_CH0_P12_OUTPUT;
    ENABLE_PWM0_CH1_P11_OUTPUT;
    ENABLE_PWM0_CH2_P10_OUTPUT;
    ENABLE_PWM0_CH3_P00_OUTPUT;
    ENABLE_PWM0_CH4_P01_OUTPUT;
    SFRS = 0;
    PWM0CON0 |= 0xC0;                    /* RUN + LOAD */
}

void app_set_channels(const uint16_t __xdata *v, uint16_t fade_ms)
{
    uint8_t i;
    for (i = 0; i < NUM_CH; i++) {
        tgt[i] = v[i];
        if (fade_ms == 0) {
            cur[i] = v[i];
            cur_fp[i] = (int32_t)v[i] << 8;
            remain[i] = 0;
        } else {
            step_fp[i] = (((int32_t)v[i] << 8) - cur_fp[i]) / (int32_t)fade_ms;
            remain[i] = fade_ms;
        }
    }
    if (fade_ms == 0)
        pwm_dirty = 1;
}

uint16_t app_get_target(uint8_t ch)
{
    return tgt[ch];
}

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
/* Indicator LEDs                                                     */
/* ------------------------------------------------------------------ */
static uint8_t led_mode = LED_MODE_AUTO;

void app_set_led_mode(uint8_t m) { led_mode = m; }
uint8_t app_get_led_mode(void)   { return led_mode; }

static void led_write(uint8_t on)
{
#if STATUS_LED_ACTIVE_LOW
    P15 = !on;
#else
    P15 = on;
#endif
}

static void leds_init(void)
{
#if AUX_LED_ACTIVE_LOW
    P14 = 1;
#else
    P14 = 0;
#endif
    P14_PUSHPULL_MODE;
    led_write(0);
    P15_PUSHPULL_MODE;
}

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  stable;        /* 1 = pressed */
    uint8_t  cnt;
    uint16_t held;
    uint8_t  long_sent;
} btn_t;

static btn_t btn_pwr, btn_set;
static uint16_t link_age = 0xFFFF;     /* ms since last valid frame */

static void local_power_toggle(void)
{
    uint8_t i, any = 0;
    static __xdata uint16_t v[NUM_CH];

    for (i = 0; i < NUM_CH; i++)
        if (tgt[i]) any = 1;

    if (any) {
        for (i = 0; i < NUM_CH; i++) { saved[i] = tgt[i]; v[i] = 0; }
    } else {
        any = 0;
        for (i = 0; i < NUM_CH; i++) { v[i] = saved[i]; if (v[i]) any = 1; }
        if (!any) {
            v[CH_R] = LOCAL_DEFAULT_R; v[CH_G] = LOCAL_DEFAULT_G;
            v[CH_B] = LOCAL_DEFAULT_B; v[CH_C] = LOCAL_DEFAULT_C;
            v[CH_W] = LOCAL_DEFAULT_W;
        }
    }
    app_set_channels(v, LOCAL_FADE_MS);
}

static void button_event(uint8_t ev)
{
    proto_report_button(ev);
    if (link_age > LINK_TIMEOUT_MS && ev == BTN_EV_POWER_SHORT)
        local_power_toggle();
}

/* Called every 1 ms with the raw (active-high) pin state. */
static void btn_tick(btn_t *b, uint8_t raw, uint8_t ev_short, uint8_t ev_long)
{
    if (raw != b->stable) {
        if (++b->cnt >= BTN_DEBOUNCE_MS) {
            b->stable = raw;
            b->cnt = 0;
            if (raw) {
                b->held = 0;
                b->long_sent = 0;
            } else if (!b->long_sent) {
                button_event(ev_short);
            }
        }
    } else {
        b->cnt = 0;
    }
    if (b->stable && !b->long_sent) {
        if (++b->held >= BTN_LONG_MS) {
            b->long_sent = 1;
            button_event(ev_long);
        }
    }
}

static void buttons_init(void)
{
    P02 = 1;
    P02_QUASI_MODE;                      /* weak pull-up */
    /* P2.0 is input-only; external pull-up on the board */
    btn_pwr.stable = 0;
    btn_set.stable = 0;
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

static void wdt_kick(void)
{
    set_WDCON_WDCLR;
}

/* ------------------------------------------------------------------ */
void main(void)
{
    uint16_t now, led_phase = 0;
    uint8_t  wdt_cnt = 0, b, on;
    static __xdata uint16_t boot[NUM_CH];

    hirc_24mhz();
    wdt_init();
    pwm_init();
    leds_init();
    buttons_init();

    boot[CH_R] = BOOT_LEVEL_R; boot[CH_G] = BOOT_LEVEL_G;
    boot[CH_B] = BOOT_LEVEL_B; boot[CH_C] = BOOT_LEVEL_C;
    boot[CH_W] = BOOT_LEVEL_W;
    app_set_channels(boot, 0);

    proto_init();
    uart0_init();

    /* Timer0: mode 1, Fsys/12, 1 ms */
    SFRS = 0;
    TMOD = (TMOD & 0xF0) | 0x01;
    clr_CKCON_T0M;
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)T0_RELOAD;
    TR0 = 1;
    ET0 = 1;
    EA = 1;

    for (;;) {
        /* Drain received bytes into the protocol parser */
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

            btn_tick(&btn_set, !P02, BTN_EV_SET_SHORT, BTN_EV_SET_LONG);
            btn_tick(&btn_pwr, !P20, BTN_EV_POWER_SHORT, BTN_EV_POWER_LONG);

            /* Status LED */
            led_phase++;
            if (led_mode == LED_MODE_ON) {
                on = 1;
            } else if (led_mode == LED_MODE_OFF) {
                on = 0;
            } else if (link_age > LINK_TIMEOUT_MS) {
                on = (led_phase & 0x7FF) < 100;          /* short blip / 2 s */
            } else if (proto_wifi_state <= 0x01) {
                on = (led_phase & 0x80) != 0;            /* ~4 Hz: pairing */
            } else if (proto_wifi_state == 0x02 || proto_wifi_state == WIFI_UNKNOWN) {
                on = (led_phase & 0x200) != 0;           /* ~1 Hz: no Wi-Fi */
            } else {
                on = 0;                                  /* connected */
            }
            led_write(on);

            if (++wdt_cnt >= WDT_KICK_MS) {
                wdt_cnt = 0;
                wdt_kick();
            }
        }

        if (pwm_dirty) {
            pwm_dirty = 0;
            pwm_apply();
        }
    }
}
