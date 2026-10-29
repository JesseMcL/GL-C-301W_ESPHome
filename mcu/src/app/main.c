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
#include "logic.h"

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
/* Settings flash (logic.c keeps the log, this is the IAP access)     */
/* ------------------------------------------------------------------ */
#define IAP_PROG_AP       0x21
#define IAP_ERASE_AP      0x22

static __bit iap_ea;

static void iap_begin(void)
{
    iap_ea = EA;
    EA = 0;
    SFRS = 0;
    TA = 0xAA; TA = 0x55; CHPCON |= 0x01;       /* IAPEN */
    TA = 0xAA; TA = 0x55; IAPUEN |= 0x01;       /* APUEN */
}

static void iap_end(void)
{
    TA = 0xAA; TA = 0x55; IAPUEN &= ~0x01;
    TA = 0xAA; TA = 0x55; CHPCON &= ~0x01;
    EA = iap_ea;
}

static void iap_go(uint8_t cmd, uint16_t addr, uint8_t data)
{
    iap_begin();
    IAPCN = cmd;
    IAPAH = (uint8_t)(addr >> 8);
    IAPAL = (uint8_t)addr;
    IAPFD = data;
    TA = 0xAA; TA = 0x55; IAPTRG |= 0x01;       /* CPU halts until done */
    iap_end();
}

uint8_t lhal_flash_read(uint16_t addr)  { return ((__code uint8_t *)0)[addr]; }
void lhal_flash_erase(uint16_t addr)     { iap_go(IAP_ERASE_AP, addr, 0xFF); }
void lhal_flash_write(uint16_t addr, uint8_t b) { iap_go(IAP_PROG_AP, addr, b); }

/* Deferred save: written SAVE_DELAY_MS after the last change, while the
 * UART is idle (an erase stalls the CPU ~5 ms = ~60 bytes at 115200). */
static __xdata uint16_t save_age = 0xFFFF;      /* 0xFFFF = nothing pending */
static __xdata uint8_t  rx_idle;                /* ms since the last received byte */

static void save_request(void) { save_age = 0; }

static void save_tick(void)
{
    if (save_age == 0xFFFF)
        return;
    if (save_age < SAVE_DELAY_MS) {
        save_age++;
        return;
    }
    if (rx_idle >= SAVE_UART_IDLE_MS) {
        settings_save();
        save_age = 0xFFFF;
    }
}

/* ------------------------------------------------------------------ */
/* PWM + fading                                                       */
/* ------------------------------------------------------------------ */
/* cur/tgt are the logical channels (R,G,B,C,W as ESPHome sets them);
 * strip_map() turns them into the five outputs for the strip type. */
static __xdata uint16_t tgt[NUM_CH];
static __xdata uint16_t cur[NUM_CH];
static __xdata uint16_t phys[NUM_CH];
static __xdata uint16_t tmp[NUM_CH];
static __xdata int32_t  cur_fp[NUM_CH];      /* value << 8 */
static __xdata int32_t  step_fp[NUM_CH];
static __xdata uint16_t remain[NUM_CH];
static __xdata uint16_t saved[NUM_CH];
static __xdata uint16_t pwm_period;
static __bit pwm_dirty;
static __bit period_dirty;

/* Strip confirmation blinks: blink_left half periods to go; on even
 * values the output is inverted (off if lit, default level if dark). */
static __xdata uint8_t  blink_left;
static __xdata uint16_t blink_t;

static void strip_blink(uint8_t n)
{
    blink_left = n * 2;
    blink_t = 0;
    pwm_dirty = 1;
}

static uint16_t duty_of(uint16_t v)
{
    if (v == 0xFFFF)
        return pwm_period + 1;           /* > period = 100 % */
    return (uint16_t)(((uint32_t)v * (uint32_t)(pwm_period + 1)) >> 16);
}

/* Caller makes sure no LOAD is pending (PWM0CON0.LOAD clear). */
static void pwm_apply(void)
{
    uint8_t i, any = 0;
    uint16_t d0, d1, d2, d3, d4;

    strip_map(cfg.strip, cur, phys);
    if (blink_left && !(blink_left & 1)) {
        for (i = 0; i < NUM_CH; i++)
            if (phys[i]) any = 1;
        if (any) {
            for (i = 0; i < NUM_CH; i++)
                phys[i] = 0;
        } else {
            strip_default_on(cfg.strip, tmp);
            strip_map(cfg.strip, tmp, phys);
        }
    }

    d0 = duty_of(phys[CH_W]);
    d1 = duty_of(phys[CH_C]);
    d2 = duty_of(phys[CH_B]);
    d3 = duty_of(phys[CH_G]);
    d4 = duty_of(phys[CH_R]);

    SFRS = 0;
    if (period_dirty) {
        period_dirty = 0;
        PWM0PH = (uint8_t)(pwm_period >> 8);
        PWM0PL = (uint8_t)pwm_period;
    }
    PWM0C0H = d0 >> 8; PWM0C0L = (uint8_t)d0;
    PWM0C1H = d1 >> 8; PWM0C1L = (uint8_t)d1;
    PWM0C2H = d2 >> 8; PWM0C2L = (uint8_t)d2;
    PWM0C3H = d3 >> 8; PWM0C3L = (uint8_t)d3;
    SFRS = 1;
    PWM0C4H = d4 >> 8; PWM0C4L = (uint8_t)d4;
    SFRS = 0;
    PWM0CON0 |= 0x40;                    /* LOAD (period + duties) at end of period */
}

static void pwm_set_freq(uint8_t f)
{
    cfg.freq = f;
    pwm_period = pwm_period_of(f);
    period_dirty = 1;
    pwm_dirty = 1;
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

    pwm_set_freq(cfg.freq);
    pwm_apply();                         /* period + duties, LOAD pending */
    ENABLE_PWM0_CH0_P12_OUTPUT;
    ENABLE_PWM0_CH1_P11_OUTPUT;
    ENABLE_PWM0_CH2_P10_OUTPUT;
    ENABLE_PWM0_CH3_P00_OUTPUT;
    ENABLE_PWM0_CH4_P01_OUTPUT;
    SFRS = 0;
    PWM0CON0 |= 0xC0;                    /* RUN + LOAD */
    pwm_dirty = 0;
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

void app_set_channel(uint8_t ch, uint16_t v) { set_channel_fade(ch, v, cfg.fade_ms); }
uint16_t app_get_channel(uint8_t ch)          { return tgt[ch]; }
uint16_t app_get_fade(void)                   { return cfg.fade_ms; }

void app_set_fade(uint16_t ms)
{
    if (ms != cfg.fade_ms) {
        cfg.fade_ms = ms;
        save_request();
    }
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
    if (blink_left && ++blink_t >= BLINK_HALF_MS) {
        blink_t = 0;
        blink_left--;
        pwm_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Indicator LEDs (R = P1.6, G = P1.5, B = P1.4)                      */
/* ------------------------------------------------------------------ */
static __xdata uint8_t led_manual[NUM_LED];
/* Frequency-change sequence: purple flashes, then dark (manual) */
static __xdata uint16_t ind_seq;                 /* ms into the sequence, 0 = idle */
static __xdata uint16_t ind_seq_len;
static __xdata uint8_t  ind_flashes;

void app_set_led(uint8_t led, uint8_t on) { led_manual[led] = on; }
uint8_t app_get_led(uint8_t led)          { return led_manual[led]; }
uint8_t app_get_led_mode(void)            { return cfg.led_mode; }

void app_set_led_mode(uint8_t m)
{
    if (m < LED_MODE_COUNT && m != cfg.led_mode) {
        cfg.led_mode = m;
        save_request();
    }
}

static void ind_flash(uint8_t n)
{
    ind_flashes = n;
    ind_seq_len = (uint16_t)n * 2 * FLASH_HALF_MS + FLASH_PAUSE_MS;
    ind_seq = 1;
}

#if LED_ACTIVE_LOW
#define LED_LEVEL(on) (!(on))
#else
#define LED_LEVEL(on) (on)
#endif

static void leds_write(uint8_t col)
{
    P16 = LED_LEVEL(col & COLOUR_R);
    P15 = LED_LEVEL(col & COLOUR_G);
    P14 = LED_LEVEL(col & COLOUR_B);
}

static void leds_init(void)
{
    leds_write(0);
    P16_PUSHPULL_MODE;
    P15_PUSHPULL_MODE;
    P14_PUSHPULL_MODE;
}

/* ------------------------------------------------------------------ */
/* Settings actions (DPs 113..116 and the Set button)                 */
/* ------------------------------------------------------------------ */
static void set_strip(uint8_t s)
{
    if (s != cfg.strip) {
        cfg.strip = s;
        pwm_dirty = 1;
        save_request();
    }
}

static void set_freq(uint8_t f)
{
    if (f != cfg.freq) {
        pwm_set_freq(f);
        save_request();
    }
}

static void factory_reset(void)
{
    settings_defaults();
    pwm_set_freq(cfg.freq);
    save_request();
    strip_blink(1);
}

static void report_settings(void)
{
    proto_report(DP_STRIP_TYPE);
    proto_report(DP_PWM_FREQ);
    proto_report(DP_POWER_MEMORY);
    proto_report(DP_LED_MODE);
    proto_report(DP_FADE_MS);
}

uint8_t app_get_cfg(uint8_t dp)
{
    switch (dp) {
    case DP_STRIP_TYPE:   return cfg.strip;
    case DP_PWM_FREQ:     return cfg.freq;
    case DP_POWER_MEMORY: return cfg.memory;
    default:              return 0;          /* DP_RESET_SETTINGS reads 0 */
    }
}

void app_set_cfg(uint8_t dp, uint8_t v)
{
    switch (dp) {
    case DP_STRIP_TYPE:
        if (v < STRIP_COUNT)
            set_strip(v);
        break;
    case DP_PWM_FREQ:
        if (v < PWM_FREQ_COUNT)
            set_freq(v);
        break;
    case DP_POWER_MEMORY:
        v = v != 0;
        if (v != cfg.memory) {
            cfg.memory = v;
            save_request();
        }
        break;
    case DP_RESET_SETTINGS:
        if (v) {
            factory_reset();
            report_settings();
        }
        break;
    default:
        break;
    }
}

static void set_gesture(uint8_t g)
{
    switch (g) {
    case GEST_CLICK1:                    /* next strip type */
        set_strip(cfg.strip + 1 >= STRIP_COUNT ? 0 : cfg.strip + 1);
        proto_report(DP_STRIP_TYPE);
        break;
    case GEST_CLICK2:                    /* next PWM frequency */
        set_freq(pwm_freq_next(cfg.freq));
        ind_flash(pwm_freq_flashes(cfg.freq));
        proto_report(DP_PWM_FREQ);
        break;
    case GEST_CLICK3:                    /* power-on memory */
        cfg.memory = !cfg.memory;
        save_request();
        strip_blink(3);
        proto_report(DP_POWER_MEMORY);
        break;
    case GEST_LONG:
        factory_reset();
        report_settings();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  stable;        /* 1 = pressed */
    uint8_t  cnt;
    uint16_t held;
} btn_t;

static __xdata btn_t btns[2];
static __xdata uint16_t link_age = 0xFFFF;     /* ms since last valid frame */
static __bit dim_up;
static __xdata uint8_t dim_t;

uint8_t app_get_button(uint8_t b) { return btns[b].stable; }

#define LOCAL_CONTROL() (link_age > LINK_TIMEOUT_MS)

static uint8_t lit(void)
{
    uint8_t i;
    for (i = 0; i < NUM_CH; i++)
        if (tgt[i]) return 1;
    return 0;
}

static void local_power_toggle(void)
{
    uint8_t i, any = 0;

    if (lit()) {
        for (i = 0; i < NUM_CH; i++) {
            saved[i] = tgt[i];
            set_channel_fade(i, 0, LOCAL_FADE_MS);
        }
        return;
    }
    for (i = 0; i < NUM_CH; i++)
        if (saved[i]) any = 1;
    if (!any)
        strip_default_on(cfg.strip, saved);
    for (i = 0; i < NUM_CH; i++)
        set_channel_fade(i, saved[i], LOCAL_FADE_MS);
}

/* One dimming step: scale all channels so the brightest moves by ~3 %. */
static void local_dim_step(void)
{
    uint8_t i;
    uint16_t m = 0, nm, step;

    for (i = 0; i < NUM_CH; i++)
        if (tgt[i] > m) m = tgt[i];
    if (m == 0) {                        /* off: start from the bottom */
        strip_default_on(cfg.strip, tmp);
        for (i = 0; i < NUM_CH; i++)
            set_channel_fade(i, (uint16_t)((uint32_t)tmp[i] * DIM_MIN_LEVEL / LOCAL_DEFAULT_LEVEL),
                             DIM_STEP_MS);
        return;
    }
    step = m >> 5;
    if (step < DIM_MIN_STEP)
        step = DIM_MIN_STEP;
    if (dim_up)
        nm = (m > 0xFFFF - step) ? 0xFFFF : m + step;
    else
        nm = (m < DIM_MIN_LEVEL + step) ? (m < DIM_MIN_LEVEL ? m : DIM_MIN_LEVEL) : m - step;
    if (nm == m)
        return;
    for (i = 0; i < NUM_CH; i++)
        set_channel_fade(i, (uint16_t)((uint32_t)tgt[i] * nm / m), DIM_STEP_MS);
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
            } else if (b->held < BTN_LONG_MS && idx == BTN_POWER && LOCAL_CONTROL()) {
                local_power_toggle();          /* ESPHome unreachable */
            }
        }
    } else {
        b->cnt = 0;
    }
    if (b->stable && b->held < 0xFFFF)
        b->held++;

    if (idx == BTN_SET) {
        set_gesture(gesture_tick(b->stable));
    } else if (b->stable && b->held >= BTN_LONG_MS && LOCAL_CONTROL()) {
        if (b->held == BTN_LONG_MS) {      /* long press: alternate direction */
            dim_up = lit() ? !dim_up : 1;
            dim_t = 0;
        }
        if (++dim_t >= DIM_STEP_MS) {
            dim_t = 0;
            local_dim_step();
        }
    }
}

static void buttons_init(void)
{
    P02 = 1;
    P02_QUASI_MODE;                      /* weak pull-up */
    /* P2.0 is input-only; external pull-up on the board */
    gesture_reset();
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
static uint8_t indicator(uint16_t phase)
{
    uint16_t p;

    if (ind_seq) {
        p = ind_seq - 1;
        if (p < (uint16_t)ind_flashes * 2 * FLASH_HALF_MS)
            return ((p / FLASH_HALF_MS) & 1) ? 0 : COLOUR_PURPLE;
        return 0;
    }
    if (cfg.led_mode == LED_MODE_MANUAL)
        return (led_manual[LED_R] ? COLOUR_R : 0) | (led_manual[LED_G] ? COLOUR_G : 0) |
               (led_manual[LED_B] ? COLOUR_B : 0);
    if (LOCAL_CONTROL())
        return (phase & 0x7FF) < 100 ? COLOUR_R : 0;         /* red blip / 2 s */
    if (proto_wifi_state <= 0x01)
        return (phase & 0x80) ? COLOUR_B : 0;                /* blue ~4 Hz: pairing/AP */
    if (proto_wifi_state == 0x02 || proto_wifi_state == WIFI_UNKNOWN)
        return (phase & 0x200) ? COLOUR_G : 0;               /* green ~1 Hz: no Wi-Fi */
    return cfg.led_mode == LED_MODE_QUIET ? 0 : strip_colour(cfg.strip);
}

void main(void)
{
    uint16_t now, led_phase = 0;
    uint8_t  wdt_cnt = 0, b;

    clr_AUXR1_SWRF;                      /* see bootloader: SWRF = OTA request */
    handoff_magic = 0;

    hirc_24mhz();
    wdt_init();
    settings_load();

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
            rx_idle = 0;
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
            if (rx_idle < 0xFF)
                rx_idle++;
            save_tick();

            btn_tick(BTN_SET, !P02);
            btn_tick(BTN_POWER, !P20);

            led_phase++;
            if (ind_seq && ++ind_seq > ind_seq_len)
                ind_seq = 0;
            leds_write(indicator(led_phase));

            if (++wdt_cnt >= WDT_KICK_MS) {
                wdt_cnt = 0;
                set_WDCON_WDCLR;
            }
        }

        /* New values are latched at the end of a PWM period (up to 1.7 ms
         * at 600 Hz); while a LOAD is pending, try again next loop. */
        SFRS = 0;
        if (pwm_dirty && !(PWM0CON0 & 0x40)) {
            pwm_dirty = 0;
            pwm_apply();
        }
    }
}
