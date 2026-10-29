/*
 * TuyaMCU datapoints exposed by the GL-C-301W replacement firmware.
 * Keep the table in README.md and the ESPHome YAML in sync.
 */
#ifndef TUYA_DP_H
#define TUYA_DP_H

/* Tuya frame commands */
#define TCMD_HEARTBEAT      0x00
#define TCMD_PRODUCT        0x01
#define TCMD_CONF           0x02
#define TCMD_WIFI_STATE     0x03
#define TCMD_DP_DELIVER     0x06
#define TCMD_DP_REPORT      0x07
#define TCMD_DP_QUERY       0x08
#define TCMD_OTA_START      0x0A
#define TCMD_OTA_DATA       0x0B
#define TCMD_NET_STATUS     0x2B   /* MCU -> module: query network status */

#define TUYA_MCU_VER        0x03

/* Datapoint types */
#define DPT_RAW             0x00
#define DPT_BOOL            0x01
#define DPT_VALUE           0x02
#define DPT_STRING          0x03
#define DPT_ENUM            0x04

/* Datapoints */
#define DP_CH_R             101   /* value 0..65535, linear duty            rw */
#define DP_CH_G             102   /* value                                  rw */
#define DP_CH_B             103   /* value                                  rw */
#define DP_CH_C             104   /* value                                  rw */
#define DP_CH_W             105   /* value                                  rw */
#define DP_FADE_MS          106   /* value 0..10000, fade applied per change rw */
#define DP_BTN_POWER        107   /* bool, pressed                          ro */
#define DP_BTN_SET          108   /* bool, pressed                          ro */
#define DP_LED_MODE         109   /* enum, LED_MODE_*                       rw */
#define DP_LED_RED          110   /* bool (P1.6)                            rw */
#define DP_LED_GREEN        111   /* bool (P1.5)                            rw */
#define DP_LED_BLUE         112   /* bool (P1.4)                            rw */
#define DP_PWM_FREQ         113   /* enum, PWM_FREQ_* index                 rw */
#define DP_POWER_MEMORY     114   /* bool, restore on/off after power loss  rw */
#define DP_RESET_SETTINGS   115   /* bool, write 1 = factory defaults (reads 0) */
#define DP_FW_VERSION       120   /* string, e.g. "1.1.0"                   ro */

#define FADE_MS_MAX         10000U
/* Status LED: AUTO = link / Wi-Fi state (dark when all is well),
 * MANUAL = DPs 110..112. */
#define LED_MODE_AUTO       0
#define LED_MODE_MANUAL     1
#define LED_MODE_COUNT      2

/* PWM frequency, as in the manual: 600, 800, 1000, 2000, 4000, 8000 Hz */
#define PWM_FREQ_600        0
#define PWM_FREQ_800        1
#define PWM_FREQ_1000       2
#define PWM_FREQ_2000       3
#define PWM_FREQ_4000       4
#define PWM_FREQ_8000       5
#define PWM_FREQ_COUNT      6

#endif
