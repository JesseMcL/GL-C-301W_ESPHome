/*
 * TuyaMCU protocol (MCU side, serial protocol v3) for the GL-C-301W app.
 * Hardware independent: compiles with SDCC for the MS51 and with gcc for the
 * host tests / simulator. Datapoints: see tuya_dp.h.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#ifndef __SDCC
#define __xdata
#define __code
#endif

#define NUM_CH            5
#define CH_R              0
#define CH_G              1
#define CH_B              2
#define CH_C              3
#define CH_W              4

#define NUM_LED           3
#define LED_R             0
#define LED_G             1
#define LED_B             2

#define BTN_POWER         0
#define BTN_SET           1

#define WIFI_UNKNOWN      0xFF

/* ---- provided by the application (main.c, host test, simulator) ----- */
void     hal_uart_put(uint8_t b);
void     app_set_channel(uint8_t ch, uint16_t v);
uint16_t app_get_channel(uint8_t ch);        /* target value */
void     app_set_fade(uint16_t ms);
uint16_t app_get_fade(void);
uint8_t  app_get_button(uint8_t btn);
void     app_set_led_mode(uint8_t mode);
uint8_t  app_get_led_mode(void);
void     app_set_led(uint8_t led, uint8_t on);
uint8_t  app_get_led(uint8_t led);
/* 1-byte settings DPs 113..115 (PWM frequency, power memory, reset).
 * The app ignores out-of-range values. */
uint8_t  app_get_cfg(uint8_t dp);
void     app_set_cfg(uint8_t dp, uint8_t v);
/* Start an MCU firmware update of `size` bytes. On success this resets
 * into the bootloader and does not return; returns 0 if not possible. */
uint8_t  app_ota_request(uint32_t size);

/* ---- protocol API ---------------------------------------------------- */
void    proto_init(void);
void    proto_rx(uint8_t b, uint16_t now_ms);
void    proto_poll(uint16_t now_ms);
/* Report one datapoint (e.g. after a local change). */
void    proto_report(uint8_t dp);
/* Report a button state change (DP 107 / 108). */
void    proto_report_button(uint8_t btn);

extern uint8_t proto_wifi_state;
extern volatile uint8_t proto_link_seen;

#endif
