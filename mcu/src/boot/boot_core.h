/*
 * GL-C-301W LDROM bootloader - protocol / update logic.
 * Hardware independent so the same code runs in the host tests and the
 * simulator; boot_main.c provides the MS51 HAL.
 *
 * Speaks just enough TuyaMCU for ESPHome's tuya component to initialise
 * (heartbeat, product, conf, Wi-Fi state, DP query -> DP 120 = "0.0.0")
 * plus the standard Tuya MCU upgrade commands:
 *   0x0A  start   (module -> MCU: uint32 size BE)   reply: 0x0A [0x00 = 256 B packets]
 *   0x0B  data    (module -> MCU: uint32 offset BE + payload) reply: 0x0B []
 *         last packet: offset >= size, no payload
 */
#ifndef BOOT_CORE_H
#define BOOT_CORE_H

#include <stdint.h>
#include "layout.h"

#ifndef __SDCC
#define __xdata
#define __code
#endif

/* ---- HAL ------------------------------------------------------------- */
void    bhal_putc(uint8_t b);
uint8_t bhal_flash_read(uint16_t addr);
void    bhal_flash_erase(uint16_t addr);           /* page containing addr */
void    bhal_flash_write(uint16_t addr, uint8_t b);
void    bhal_jump_app(void);                       /* no return on target */
void    bhal_leds(uint8_t r, uint8_t g, uint8_t b);

/* ---- core ------------------------------------------------------------ */
#define BOOT_ST_RESCUE    0   /* no valid app: wait for an update forever */
#define BOOT_ST_WAIT      1   /* app asked for an update; app still valid */
#define BOOT_ST_READY     2   /* 0x0A accepted, waiting for data */
#define BOOT_ST_RECV      3   /* receiving data (app invalidated) */
#define BOOT_ST_FAIL      4   /* write / CRC error */

#define BOOT_WAIT_TIMEOUT_MS  15000U

extern uint8_t boot_state;

uint16_t crc16_update(uint16_t crc, uint8_t b);
uint8_t  boot_app_valid(void);
/* requested: entered via the app's software reset.
 * pending_size: size from the app's hand-off of the 0x0A it received
 *               (0 if unknown -> wait for the module to retry 0x0A). */
void     boot_init(uint8_t requested, uint16_t pending_size);
void     boot_rx(uint8_t b);
void     boot_tick(void);   /* every 1 ms */

#endif
