/*
 * Minimal TuyaMCU (serial protocol v3, MCU side) for the GL-C-301W.
 * Hardware independent: compiles with SDCC for the MS51 and with gcc for the
 * host unit tests.
 *
 * Datapoints
 *   101 raw   channels  RRRR GGGG BBBB CCCC WWWW [FFFF]
 *                       5 x uint16 big-endian, 0..65535 linear duty,
 *                       optional uint16 fade time in ms.
 *   102 enum  button    0 none, 1 power short, 2 power long,
 *                       3 set short, 4 set long   (MCU -> module only)
 *   103 enum  led mode  0 auto (shows Wi-Fi state), 1 off, 2 on
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

#define DP_CHANNELS       101
#define DP_BUTTON         102
#define DP_LED_MODE       103

#define BTN_EV_NONE       0
#define BTN_EV_POWER_SHORT 1
#define BTN_EV_POWER_LONG 2
#define BTN_EV_SET_SHORT  3
#define BTN_EV_SET_LONG   4

#define LED_MODE_AUTO     0
#define LED_MODE_OFF      1
#define LED_MODE_ON       2

#define WIFI_UNKNOWN      0xFF

/* ---- provided by the application (main.c or the host test) ---------- */
void     hal_uart_put(uint8_t b);
void     app_set_channels(const uint16_t __xdata *v, uint16_t fade_ms);
uint16_t app_get_target(uint8_t ch);
void     app_set_led_mode(uint8_t mode);
uint8_t  app_get_led_mode(void);

/* ---- protocol API ---------------------------------------------------- */
void    proto_init(void);
/* Feed one received byte. now_ms is a free-running 16-bit ms counter. */
void    proto_rx(uint8_t b, uint16_t now_ms);
/* Call regularly; drops half-received frames after a gap. */
void    proto_poll(uint16_t now_ms);
/* Send an unsolicited button event report. */
void    proto_report_button(uint8_t ev);

/* Last WIFI_STATE received from the module (WIFI_UNKNOWN if none). */
extern uint8_t proto_wifi_state;
/* Set to 1 every time a valid frame arrives; the app clears it. */
extern volatile uint8_t proto_link_seen;

#endif
