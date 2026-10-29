#include "boot_core.h"
#include "tuya_dp.h"

#ifndef BOOT_VERSION
#ifdef FW_VERSION
#define BOOT_VERSION FW_VERSION
#else
#define BOOT_VERSION "0.0.0-dev"
#endif
#endif

#define RX_MAX   (4 + OTA_PKT_SIZE)

uint8_t boot_state;

static __xdata uint8_t rx_buf[RX_MAX];
static uint8_t  rx_st;
static uint8_t  rx_cmd;
static uint16_t rx_len;
static uint16_t rx_idx;
static uint8_t  rx_sum;
static uint8_t  rx_gap;

static uint8_t  hb_sent;
static uint16_t ota_size;
static uint16_t expected;
static uint16_t idle_ms;
static uint16_t phase;

static const char __code product_json[] =
    "{\"p\":\"gl_c_301w_boot\",\"v\":\"" BOOT_VERSION "\",\"m\":0}";
/* DP 120 reports "0.0.0" so the ESPHome update entity offers an install */
static const uint8_t __code dp_query_reply[] = {
    DP_FW_VERSION, DPT_STRING, 0, 5, '0', '.', '0', '.', '0'
};

uint16_t crc16_update(uint16_t crc, uint8_t b)
{
    /* CRC-16/CCITT-FALSE (poly 0x1021), table-less, byte-wise so SDCC
     * produces compact code */
    uint8_t hi, lo, x;
    hi = (uint8_t)(crc >> 8);
    lo = (uint8_t)crc;
    x = hi ^ b;
    x ^= x >> 4;
    hi = lo ^ (uint8_t)(x << 4) ^ (x >> 3);
    lo = (uint8_t)(x << 5) ^ x;
    return ((uint16_t)hi << 8) | lo;
}

static uint16_t rd16(uint16_t a)
{
    return bhal_flash_read(a) | ((uint16_t)bhal_flash_read(a + 1) << 8);
}

static const char __code magic[4] = {FOOTER_MAGIC0, FOOTER_MAGIC1, FOOTER_MAGIC2, FOOTER_MAGIC3};

/* Footer/record at `a`: magic, len, crc. CRC must match flash 0..len-1. */
static uint8_t image_ok(uint16_t a)
{
    uint16_t len, crc = 0xFFFF, i;
    for (i = 0; i < 4; i++)
        if (bhal_flash_read(a + i) != (uint8_t)magic[i])
            return 0;
    len = rd16(a + FOOTER_OFS_LEN);
    if (len == 0 || len > APP_MAX_CODE)
        return 0;
    for (i = 0; i < len; i++)
        crc = crc16_update(crc, bhal_flash_read(i));
    return crc == rd16(a + FOOTER_OFS_CRC);
}

uint8_t boot_app_valid(void)
{
    return image_ok(APP_RECORD_ADDR);
}

/* ---------------- TX ---------------- */
static uint8_t tx_sum;

static void tx(uint8_t b)
{
    tx_sum += b;
    bhal_putc(b);
}

static void tx_begin(uint8_t cmd, uint8_t len)
{
    tx_sum = 0;
    tx(0x55);
    tx(0xAA);
    tx(TUYA_MCU_VER);
    tx(cmd);
    tx(0);
    tx(len);
}

static void tx_end(void)
{
    bhal_putc(tx_sum);
}

static void tx_code(uint8_t cmd, const uint8_t __code *p, uint8_t len)
{
    tx_begin(cmd, len);
    while (len--)
        tx(*p++);
    tx_end();
}

static void ack(uint8_t cmd)
{
    tx_begin(cmd, 0);
    tx_end();
}

/* 32-bit big-endian at rx_buf[0..3]; returns 0xFFFF if it doesn't fit 16 bits */
static uint16_t be32_16(void)
{
    if (rx_buf[0] | rx_buf[1])
        return 0xFFFF;
    return ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
}

static void ota_accept(uint16_t size)
{
    if (size <= FOOTER_SIZE || size > OTA_MAX_SIZE)
        return;                      /* no reply -> module gives up */
    ota_size = size;
    expected = 0;
    if (boot_state != BOOT_ST_RECV)
        boot_state = BOOT_ST_READY;
    tx_begin(TCMD_OTA_START, 1);
    tx(OTA_PKT_SEL);
    tx_end();
}

static void ota_finish(void)
{
    uint16_t fa = ota_size - FOOTER_SIZE;
    uint8_t i;

    if (rd16(fa + FOOTER_OFS_LEN) != fa || !image_ok(fa)) {
        boot_state = BOOT_ST_FAIL;
        return;                      /* no ack: the module reports failure */
    }
    /* record page was erased when the transfer started */
    for (i = 0; i < RECORD_SIZE; i++)
        bhal_flash_write(APP_RECORD_ADDR + i, bhal_flash_read(fa + i));
    if (!boot_app_valid()) {
        boot_state = BOOT_ST_FAIL;
        return;
    }
    ack(TCMD_OTA_DATA);
    bhal_jump_app();
}

static void ota_data(void)
{
    uint16_t off, n, i, a;

    if (rx_len < 4 || (boot_state != BOOT_ST_READY && boot_state != BOOT_ST_RECV))
        return;
    off = be32_16();
    n = rx_len - 4;

    if (n == 0) {
        if (off >= ota_size && expected >= ota_size)
            ota_finish();
        return;
    }
    if (off < expected) {            /* retransmission of an acked packet */
        ack(TCMD_OTA_DATA);
        return;
    }
    if (off != expected || n > ota_size - off)
        return;

    if (boot_state != BOOT_ST_RECV) {
        /* first data: invalidate the current app before touching it */
        bhal_flash_erase(APP_RECORD_ADDR);
        boot_state = BOOT_ST_RECV;
    }
    a = off;
    for (i = 0; i < n; i++, a++) {
        if ((a & (FLASH_PAGE_SIZE - 1)) == 0)
            bhal_flash_erase(a);
        bhal_flash_write(a, rx_buf[4 + i]);
        if (bhal_flash_read(a) != rx_buf[4 + i]) {
            boot_state = BOOT_ST_FAIL;
            return;
        }
    }
    expected += n;
    ack(TCMD_OTA_DATA);
}

static void handle_frame(void)
{
    idle_ms = 0;
    switch (rx_cmd) {
    case TCMD_HEARTBEAT:
        tx_begin(TCMD_HEARTBEAT, 1);
        tx(hb_sent);
        tx_end();
        hb_sent = 1;
        break;
    case TCMD_PRODUCT:
        tx_code(TCMD_PRODUCT, (const uint8_t __code *)product_json, sizeof(product_json) - 1);
        break;
    case TCMD_CONF:
        ack(TCMD_CONF);
        ack(TCMD_WIFI_STATE);   /* see app proto.c: un-stalls ESPHome re-init */
        break;
    case TCMD_WIFI_STATE:
        ack(TCMD_WIFI_STATE);
        break;
    case TCMD_DP_DELIVER:
    case TCMD_DP_QUERY:
        tx_code(TCMD_DP_REPORT, dp_query_reply, sizeof(dp_query_reply));
        break;
    case TCMD_OTA_START:
        if (rx_len >= 4)
            ota_accept(be32_16());
        break;
    case TCMD_OTA_DATA:
        ota_data();
        break;
    default:
        break;
    }
}

void boot_init(uint8_t requested, uint16_t pending_size)
{
    rx_st = 0;
    hb_sent = 0;
    idle_ms = 0;
    boot_state = requested && boot_app_valid() ? BOOT_ST_WAIT : BOOT_ST_RESCUE;
    if (pending_size)
        ota_accept(pending_size);
}

void boot_rx(uint8_t b)
{
    rx_gap = 0;
    switch (rx_st) {
    case 0:
        if (b == 0x55) { rx_sum = b; rx_st = 1; }
        break;
    case 1:
        if (b == 0xAA) { rx_sum += b; rx_st = 2; }
        else if (b != 0x55) rx_st = 0;
        break;
    case 2:
        rx_sum += b; rx_st = 3;
        break;
    case 3:
        rx_cmd = b; rx_sum += b; rx_st = 4;
        break;
    case 4:
        rx_len = (uint16_t)b << 8; rx_sum += b; rx_st = 5;
        break;
    case 5:
        rx_len |= b; rx_sum += b; rx_idx = 0;
        rx_st = rx_len ? 6 : 7;
        break;
    case 6:
        if (rx_idx < RX_MAX)
            rx_buf[rx_idx] = b;
        rx_sum += b;
        if (++rx_idx >= rx_len)
            rx_st = 7;
        break;
    default:
        rx_st = 0;
        if (b == rx_sum && rx_len <= RX_MAX)
            handle_frame();
        break;
    }
}

void boot_tick(void)
{
    uint8_t on;

    if (rx_st && ++rx_gap > 50)
        rx_st = 0;

    if (idle_ms < 0xFFFF)
        idle_ms++;
    /* asked to update but nothing came: go back to the (still valid) app */
    if ((boot_state == BOOT_ST_WAIT || boot_state == BOOT_ST_READY) &&
        idle_ms > BOOT_WAIT_TIMEOUT_MS && boot_app_valid())
        bhal_jump_app();

    /* LEDs: rescue = red blink, receiving = blue fast blink,
     * fail = red, waiting/ready = blue */
    phase++;
    on = (phase & 0x100) != 0;
    if (boot_state == BOOT_ST_RECV)
        bhal_leds(0, 0, (phase & 0x40) != 0);
    else if (boot_state == BOOT_ST_RESCUE || boot_state == BOOT_ST_FAIL)
        bhal_leds(boot_state == BOOT_ST_FAIL || on, 0, 0);
    else
        bhal_leds(0, 0, 1);
}
