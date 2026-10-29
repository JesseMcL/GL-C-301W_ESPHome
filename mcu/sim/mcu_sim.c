/*
 * GL-C-301W MCU simulator for integration tests against a real ESPHome host
 * build. Runs the actual app protocol (src/app/proto.c) and bootloader core
 * (src/boot/boot_core.c) on a serial device / pty, with a simulated flash
 * initialised from an ICP image.
 *
 *   cc -DPROTO_VERSION_HOOK -Iinclude -Isrc/boot -o mcu_sim sim/mcu_sim.c \
 *      src/app/proto.c src/boot/boot_core.c
 *   ./mcu_sim /dev/pts/N app.icp.bin [--press-power-at MS] [--hold-power-at MS]
 *
 * Prints what ESPHome does (channel values, LED, buttons, mode switches).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "boot_core.h"
#include "layout.h"
#include "proto.h"
#include "tuya_dp.h"

static int fd;
static uint8_t flash[FLASH_SIZE];
static enum { MODE_BOOT, MODE_APP } mode;
static int jump_pending;
static char version[17];

static uint16_t ch[NUM_CH], fade = 60;
static uint8_t btn[2], led_mode, leds[NUM_LED];

static void out(uint8_t b)
{
    if (write(fd, &b, 1) != 1)
        perror("write");
}

/* ---- app hooks ---- */
void hal_uart_put(uint8_t b) { out(b); }
void app_set_channel(uint8_t c, uint16_t v)
{
    static const char n[] = "RGBCW";
    ch[c] = v;
    printf("APP ch %c = %5u\n", n[c], v);
    fflush(stdout);
}
uint16_t app_get_channel(uint8_t c) { return ch[c]; }
void app_set_fade(uint16_t ms) { fade = ms; printf("APP fade %u\n", ms); fflush(stdout); }
uint16_t app_get_fade(void) { return fade; }
uint8_t app_get_button(uint8_t b) { return btn[b]; }
void app_set_led_mode(uint8_t m) { led_mode = m; printf("APP led mode %u\n", m); fflush(stdout); }
uint8_t app_get_led_mode(void) { return led_mode; }
void app_set_led(uint8_t l, uint8_t on) { leds[l] = on; printf("APP led %u = %u\n", l, on); fflush(stdout); }
uint8_t app_get_led(uint8_t l) { return leds[l]; }
static uint8_t cfgv[3];                 /* strip, freq, memory */
uint8_t app_get_cfg(uint8_t dp) { return dp < DP_RESET_SETTINGS ? cfgv[dp - DP_STRIP_TYPE] : 0; }
void app_set_cfg(uint8_t dp, uint8_t v)
{
    printf("APP cfg dp %u = %u\n", dp, v);
    fflush(stdout);
    if (dp < DP_RESET_SETTINGS)
        cfgv[dp - DP_STRIP_TYPE] = v;
    else if (v)
        memset(cfgv, 0, sizeof cfgv);
}
const char *proto_fw_version(void) { return version; }

uint8_t app_ota_request(uint32_t size)
{
    printf("APP 0x0A size %u -> software reset into LDROM\n", (unsigned)size);
    fflush(stdout);
    mode = MODE_BOOT;
    boot_init(1, (uint16_t)size);
    return 1;
}

/* ---- bootloader HAL ---- */
void bhal_putc(uint8_t b) { out(b); }
uint8_t bhal_flash_read(uint16_t a) { return flash[a]; }
void bhal_flash_erase(uint16_t a) { memset(&flash[a & ~(FLASH_PAGE_SIZE - 1)], 0xFF, FLASH_PAGE_SIZE); }
void bhal_flash_write(uint16_t a, uint8_t b) { flash[a] &= b; }
void bhal_jump_app(void) { jump_pending = 1; }
void bhal_leds(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }

static void load_version(void)
{
    uint16_t len = flash[APP_RECORD_ADDR + FOOTER_OFS_LEN] | (flash[APP_RECORD_ADDR + FOOTER_OFS_LEN + 1] << 8);
    memcpy(version, &flash[len + FOOTER_OFS_VERSION], 16);
    version[16] = 0;
}

static void start_app(void)
{
    load_version();
    printf("BOOT -> APP, version %s\n", version);
    fflush(stdout);
    mode = MODE_APP;
    proto_init();
}

static uint16_t ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint16_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    struct termios tio;
    uint8_t b;
    uint16_t last = ms();
    long press_at = -1, hold_at = -1, elapsed = 0;
    int a;
    FILE *f;

    if (argc < 3) {
        fprintf(stderr, "usage: %s TTY ICP_IMAGE [--press-power-at MS]\n", argv[0]);
        return 2;
    }
    for (a = 3; a + 1 < argc; a += 2) {
        if (!strcmp(argv[a], "--press-power-at"))
            press_at = atol(argv[a + 1]);
        else if (!strcmp(argv[a], "--hold-power-at"))
            hold_at = atol(argv[a + 1]);
    }

    memset(flash, 0xFF, sizeof flash);
    f = fopen(argv[2], "rb");
    if (!f || fread(flash, 1, APROM_SIZE, f) == 0) {
        perror(argv[2]);
        return 1;
    }
    fclose(f);

    fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    tcsetattr(fd, TCSANOW, &tio);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    /* power-on: bootloader validates and jumps */
    if (boot_app_valid()) {
        start_app();
    } else {
        printf("BOOT rescue mode (no valid app)\n");
        mode = MODE_BOOT;
        boot_init(0, 0);
    }

    for (;;) {
        uint16_t now = ms();
        while (read(fd, &b, 1) == 1) {
            if (getenv("SIM_TRACE"))
                printf("<%02X", b), fflush(stdout);
            if (mode == MODE_APP)
                proto_rx(b, now);
            else
                boot_rx(b);
            if (jump_pending) {
                jump_pending = 0;
                start_app();
            }
        }
        while ((uint16_t)(now - last) > 0) {
            last++;
            elapsed++;
            if (mode == MODE_BOOT) {
                boot_tick();
                if (jump_pending) {
                    jump_pending = 0;
                    start_app();
                }
            }
            if (press_at >= 0 && elapsed == press_at && mode == MODE_APP) {
                printf("SIM press power\n");
                fflush(stdout);
                btn[BTN_POWER] = 1;
                proto_report_button(BTN_POWER);
            }
            if (press_at >= 0 && elapsed == press_at + 150 && mode == MODE_APP) {
                btn[BTN_POWER] = 0;
                proto_report_button(BTN_POWER);
            }
            if (hold_at >= 0 && elapsed == hold_at && mode == MODE_APP) {
                printf("SIM hold power\n");
                fflush(stdout);
                btn[BTN_POWER] = 1;
                proto_report_button(BTN_POWER);
            }
            if (hold_at >= 0 && elapsed == hold_at + 1500 && mode == MODE_APP) {
                printf("SIM release power\n");
                fflush(stdout);
                btn[BTN_POWER] = 0;
                proto_report_button(BTN_POWER);
            }
        }
        if (mode == MODE_APP)
            proto_poll(now);
        usleep(300);
    }
}
