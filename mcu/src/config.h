/*
 * GL-C-301W replacement firmware for the Nuvoton MS51FC0AE
 * Build-time configuration. Everything that depends on guesses about the
 * board (polarities, which LED is which, local fallback levels) lives here.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define FW_VERSION_STR        "1.0.0"
#define PRODUCT_JSON          "{\"p\":\"gl_c_301w_pwm\",\"v\":\"" FW_VERSION_STR "\",\"m\":0}"

/* ---- Clock / UART ---------------------------------------------------- */
#define FSYS_HZ               24000000UL
/* Must match `baud_rate:` of the uart: block in the ESPHome YAML. */
#define UART_BAUD             115200UL

/* ---- PWM ------------------------------------------------------------- */
/* PWM0 clocked at Fsys/1. Period register = PWM_PERIOD, so the frequency is
 * Fsys / (PWM_PERIOD + 1). 4095 -> 5.86 kHz with 12-bit resolution.
 * 7999 -> 3.0 kHz / ~13 bit, 11999 -> 2.0 kHz / ~13.5 bit (better low-end
 * dimming, but more likely to show up as camera banding). */
#define PWM_PERIOD            4095U

/* Set to 1 if a driver channel turns out to be active-low (strip lights
 * when it should be off). Bits: 0=R 1=G 2=B 3=C 4=W */
#define PWM_INVERT_MASK       0x00

/* ---- Indicator LED --------------------------------------------------- */
/* MCU pin 10 (P1.5) goes to an LED. Pin 11 (P1.4) also goes to an LED; it
 * is driven to its inactive level and otherwise left alone. */
#define STATUS_LED_ACTIVE_LOW 1
#define AUX_LED_ACTIVE_LOW    1

/* ---- Buttons --------------------------------------------------------- */
/* PB-Set  -> P0.2 (pin 18, shared with ICP CLK). Assumed active-low.
 * PB-Power-> P2.0 (pin 4, nRESET). Only usable as a button once CONFIG0.RPD
 * is cleared (reset pin disabled) when programming. While reset is enabled
 * a press simply resets the MCU, which is harmless. */
#define BTN_DEBOUNCE_MS       20
#define BTN_LONG_MS           800

/* ---- Link supervision / local fallback ------------------------------- */
/* ESPHome sends a heartbeat every 15 s. If nothing arrives for this long,
 * the buttons control the outputs directly (outputs are never changed just
 * because the link went quiet). */
#define LINK_TIMEOUT_MS       40000UL

/* Levels used by a local power toggle when nothing was set before
 * (0..65535, order R,G,B,C,W). Default: both whites at 50%. */
#define LOCAL_DEFAULT_R       0
#define LOCAL_DEFAULT_G       0
#define LOCAL_DEFAULT_B       0
#define LOCAL_DEFAULT_C       32768U
#define LOCAL_DEFAULT_W       32768U
#define LOCAL_FADE_MS         300

/* Level at power-up, before ESPHome takes over (0..65535). 0 = dark. */
#define BOOT_LEVEL_R          0
#define BOOT_LEVEL_G          0
#define BOOT_LEVEL_B          0
#define BOOT_LEVEL_C          0
#define BOOT_LEVEL_W          0

/* ---- Watchdog -------------------------------------------------------- */
/* The WDT only resets the chip if CONFIG4.WDTEN enables it (set in the ICP
 * tool). The firmware always sets the prescaler and kicks it. */
#define WDT_KICK_MS           100

#endif
