/*
 * GL-C-301W LDROM bootloader - MS51FC0AE hardware layer.
 *
 * Runs after every reset (CONFIG0.CBS = 0, boot from LDROM):
 *   - software reset from the app (AUXR1.SWRF set) = update requested;
 *     the app leaves the 0x0A size in XRAM so we can reply immediately
 *   - otherwise: if the APROM record + CRC are valid, jump to the app
 *   - else stay here and wait for an update (rescue mode, red blink)
 * Polled UART0 @ 115200 (Timer3), no interrupts.
 *
 * Clock-trim and IAP sequences follow Nuvoton's MS51 BSP (Apache-2.0) and
 * Nikita Lita's NuMicro-8051-prog bootloader (MIT).
 */
#include "numicro_8051.h"
#include "config.h"
#include "boot_core.h"

BIT BIT_TMP;
__data uint8_t TA_REG_TMP, BYTE_TMP, SFRS_TMP;

__xdata __at(HANDOFF_XADDR)     volatile uint16_t handoff_magic;
__xdata __at(HANDOFF_XADDR + 2) volatile uint32_t handoff_size;

#define IAP_READ_AP     0x00
#define IAP_PROG_AP     0x21
#define IAP_ERASE_AP    0x22

#define TA_UNLOCK       TA = 0xAA; TA = 0x55
#define IAP_GO          TA_UNLOCK; IAPTRG |= 0x01

#define T0_RELOAD (65536UL - (FSYS_HZ / 12UL / 1000UL))

static void iap_addr(uint8_t cmd, uint16_t addr)
{
    IAPCN = cmd;
    IAPAH = (uint8_t)(addr >> 8);
    IAPAL = (uint8_t)addr;
}

uint8_t bhal_flash_read(uint16_t addr)
{
    iap_addr(IAP_READ_AP, addr);
    IAP_GO;
    return IAPFD;
}

void bhal_flash_erase(uint16_t addr)
{
    iap_addr(IAP_ERASE_AP, addr);
    IAPFD = 0xFF;
    IAP_GO;
}

void bhal_flash_write(uint16_t addr, uint8_t b)
{
    iap_addr(IAP_PROG_AP, addr);
    IAPFD = b;
    IAP_GO;
}

void bhal_putc(uint8_t b)
{
    SBUF = b;
    while (!TI)
        ;
    TI = 0;
}

#if LED_ACTIVE_LOW
#define LVL(x) (!(x))
#else
#define LVL(x) (x)
#endif

void bhal_leds(uint8_t r, uint8_t g, uint8_t b)
{
    P16 = LVL(r);
    P15 = LVL(g);
    P14 = LVL(b);
}

void bhal_jump_app(void)
{
    bhal_leds(0, 0, 0);
    TA_UNLOCK;
    IAPUEN = 0;
    TA_UNLOCK;
    CHPCON = 0x80;          /* SWRST, BS = 0 -> boot APROM; IAPEN cleared */
    for (;;)
        ;
}

void main(void)
{
    uint8_t requested, t0, t1, wdt = 0;
    uint16_t pending = 0;

    EA = 0;
    SFRS = 0;
    TA_UNLOCK;
    CHPCON |= 0x01;         /* IAPEN */
    TA_UNLOCK;
    IAPUEN = 0x01;          /* APUEN: allow APROM update */

    /* HIRC -> 24 MHz */
    iap_addr(READ_UID, 0x38);
    IAP_GO;
    t0 = IAPFD;
    IAPAL = 0x39;
    IAP_GO;
    t1 = IAPFD;
    TA_UNLOCK; RCTRIM0 = t0;
    TA_UNLOCK; RCTRIM1 = t1;

    TA_UNLOCK;
    WDCON |= 0x07;          /* WDT /256 (~1.6 s) if CONFIG4 enables it */

    /* LEDs */
    bhal_leds(0, 0, 0);
    P1M1 &= ~0x70;          /* P1.4..P1.6 push-pull */
    P1M2 |= 0x70;

    requested = (AUXR1 & 0x80) != 0;
    AUXR1 &= 0x7F;
    if (requested && handoff_magic == HANDOFF_MAGIC && handoff_size <= OTA_MAX_SIZE)
        pending = (uint16_t)handoff_size;
    handoff_magic = 0;

    if (!requested && boot_app_valid())
        bhal_jump_app();

    /* UART0 115200 from Timer3, polled */
    P06 = 1;
    P0M1 = (P0M1 & ~0x40) | 0x80;   /* P0.6 push-pull, P0.7 input */
    P0M2 = (P0M2 | 0x40) & ~0x80;
    SCON = 0x50;
    PCON |= 0x80;           /* SMOD */
    T3CON = 0x20;           /* BRCK = Timer3, prescale 1 */
    RH3 = (uint8_t)((65536UL - (FSYS_HZ / 16UL / UART_BAUD)) >> 8);
    RL3 = (uint8_t)(65536UL - (FSYS_HZ / 16UL / UART_BAUD));
    T3CON |= 0x08;          /* TR3 */

    /* Timer0 1 ms, polled */
    TMOD = (TMOD & 0xF0) | 0x01;
    CKCON &= 0xF7;          /* T0 = Fsys/12 */
    TH0 = (uint8_t)(T0_RELOAD >> 8);
    TL0 = (uint8_t)T0_RELOAD;
    TR0 = 1;

    boot_init(requested, pending);

    for (;;) {
        if (RI) {
            RI = 0;
            boot_rx(SBUF);
        }
        if (TF0) {
            TF0 = 0;
            TH0 = (uint8_t)(T0_RELOAD >> 8);
            TL0 = (uint8_t)T0_RELOAD;
            boot_tick();
            if (++wdt >= 100) {
                wdt = 0;
                TA_UNLOCK;
                WDCON |= 0x40;  /* WDCLR */
            }
        }
    }
}
