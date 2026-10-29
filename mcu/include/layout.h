/*
 * Flash / RAM layout shared by the application (APROM), the bootloader
 * (LDROM), the post-build script and the host tests.
 *
 *  MS51FC0AE: 32 KB flash = APROM + LDROM, 128-byte pages.
 *
 *  0x0000 ......................... application code + footer (OTA image)
 *  ...                              (0xFF)
 *  APP_RECORD_ADDR (last APROM page) validity record, written by the
 *                                   bootloader after a CRC-checked update
 *                                   (or by the ICP image)
 *  APROM_SIZE .. 0x7FFF             LDROM (bootloader, linked at 0x0000 in
 *                                   its own address space)
 *
 * scripts/fwimage.py parses this file: keep the #defines simple expressions.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#define FLASH_SIZE          32768U
#ifndef LDROM_SIZE
#define LDROM_SIZE          3072U
#endif
#define APROM_SIZE          (FLASH_SIZE - LDROM_SIZE)
#define FLASH_PAGE_SIZE     128U
#define APP_RECORD_ADDR     (APROM_SIZE - FLASH_PAGE_SIZE)

/* Footer appended to the application binary by scripts/fwimage.py.
 * All multi-byte fields little-endian. */
#define FOOTER_SIZE         32U
#define FOOTER_MAGIC0       'G'
#define FOOTER_MAGIC1       'L'
#define FOOTER_MAGIC2       'C'
#define FOOTER_MAGIC3       'W'
#define FOOTER_OFS_LEN      4U      /* uint16 app length (bytes before footer) */
#define FOOTER_OFS_CRC      6U      /* uint16 CRC-16/CCITT-FALSE of app bytes */
#define FOOTER_OFS_FORMAT   8U      /* uint8 = 1 */
#define FOOTER_OFS_VERSION  16U     /* char[16], NUL padded */
#define FOOTER_FORMAT       1U
#define RECORD_SIZE         8U      /* first 8 bytes of the footer */

/* Largest OTA image (app + footer) that fits below the record page */
#define OTA_MAX_SIZE        APP_RECORD_ADDR
#define APP_MAX_CODE        (OTA_MAX_SIZE - FOOTER_SIZE)

/* XRAM hand-off from app to bootloader across the software reset.
 * Neither image's linker may use this area (xram-size is reduced). */
#define XRAM_SIZE_TOTAL     2048U
#define HANDOFF_XADDR       0x07F0U
#define XRAM_USABLE         HANDOFF_XADDR
#define HANDOFF_MAGIC       0xB007U

/* Tuya MCU-upgrade packet size we ask for (0x00 = 256 bytes) */
#define OTA_PKT_SEL         0x00
#define OTA_PKT_SIZE        256U

#endif
