/*
 * GL-C-301W replacement firmware for the Nuvoton MS51FC0AE
 * Build-time configuration for the application. Everything that depends on
 * guesses about the board lives here.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* Injected by scripts/version.py from `git describe` (max 15 chars). */
#ifndef FW_VERSION
#define FW_VERSION            "0.0.0-dev"
#endif
#define PRODUCT_ID            "gl_c_301w"

/* ---- Clock / UART ---------------------------------------------------- */
#define FSYS_HZ               24000000UL
/* Must match `baud_rate:` of the uart: block in the ESPHome YAML. */
#ifndef UART_BAUD
#define UART_BAUD             115200UL
#endif

/* ---- PWM ------------------------------------------------------------- */
/* PWM0 clocked at Fsys/1. The frequency is a setting (DP 113, Set x2),
 * 600 Hz .. 8 kHz as in the stock firmware, default 1 kHz (24000 counts,
 * ~14.5 bit). See logic.c. */
/* Bits: 0=R 1=G 2=B 3=C 4=W. Set a bit if that driver is active-low. */
#ifndef PWM_INVERT_MASK
#define PWM_INVERT_MASK       0x00
#endif

/* ---- Indicator LEDs (one RGB indicator) ------------------------------ */
/* red   = MCU pin 8  P1.6 (shared with ICP DAT)
 * green = MCU pin 10 P1.5
 * blue  = MCU pin 11 P1.4                                               */
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW        1
#endif

/* ---- Buttons --------------------------------------------------------- */
/* PB-Set   -> P0.2 (pin 18, shared with ICP CLK), active-low.
 * PB-Power -> P2.0 (pin 4, nRESET), active-low. Works as a button only
 * once CONFIG0.RPD = 0 (reset pin disabled).                            */
#define BTN_DEBOUNCE_MS       20
/* Power button held this long = dimming (local control only; with ESPHome
 * connected the YAML handles the button). Set-button timing: logic.h. */
#define BTN_LONG_MS           800

/* ---- Fading ---------------------------------------------------------- */
/* Every channel change is spread over this many ms (DP 106 overrides it at
 * runtime). Smooths the ~20 Hz updates ESPHome sends during transitions. */
#define DEFAULT_FADE_MS       60

/* ---- Link supervision / local fallback ------------------------------- */
#define LINK_TIMEOUT_MS       40000UL
/* "On" levels when there is nothing to restore (local control, blinks) */
#define LOCAL_DEFAULT_R       0
#define LOCAL_DEFAULT_G       0
#define LOCAL_DEFAULT_B       0
#define LOCAL_DEFAULT_C       32768U
#define LOCAL_DEFAULT_W       32768U
#define LOCAL_FADE_MS         300
/* Local dimming: every DIM_STEP_MS the brightest channel moves by 1/32
 * (at least DIM_MIN_STEP), between DIM_MIN_LEVEL and full. */
#define DIM_STEP_MS           25
#define DIM_MIN_STEP          64U
#define DIM_MIN_LEVEL         640U

/* ---- Strip / indicator confirmation blinks ---------------------------- */
#define BLINK_HALF_MS         250   /* strip blink on / off time           */
#define FLASH_HALF_MS         200   /* indicator frequency flashes         */
#define FLASH_PAUSE_MS        2000  /* dark after the flashes (manual)     */
/* Settings changes are written to flash this long after the last change
 * (and only while the UART is idle: IAP stalls the CPU). */
#define SAVE_DELAY_MS         2000
#define SAVE_UART_IDLE_MS     30

/* Level at power-up, before ESPHome takes over (0..65535). */
#define BOOT_LEVEL_R          0
#define BOOT_LEVEL_G          0
#define BOOT_LEVEL_B          0
#define BOOT_LEVEL_C          0
#define BOOT_LEVEL_W          0

/* ---- Watchdog -------------------------------------------------------- */
/* Resets only if CONFIG4.WDTEN enables it (scripts/config.json). */
#define WDT_KICK_MS           100

#endif
