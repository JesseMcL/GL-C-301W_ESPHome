/* Bootloader tests: boot_core.c against a simulated NOR flash. */
#include <unity.h>
#include <stdlib.h>
#include "boot_core.h"
#include "tuya_dp.h"
#include "proto.h"
#include "../common/tuya_test_util.h"

/* logic.c is linked into the native build too */
uint8_t lhal_flash_read(uint16_t a) { (void)a; return 0xFF; }
void lhal_flash_erase(uint16_t a) { (void)a; }
void lhal_flash_write(uint16_t a, uint8_t b) { (void)a; (void)b; }

static uint8_t flash[FLASH_SIZE];
static int jumped, erases, writes;
static int fail_write_at = -1;

void bhal_putc(uint8_t b) { t_tx[t_txn++] = b; }
uint8_t bhal_flash_read(uint16_t a) { return flash[a]; }
void bhal_flash_erase(uint16_t a)
{
    TEST_ASSERT_TRUE_MESSAGE(a < APROM_SIZE, "erase outside APROM");
    memset(&flash[a & ~(FLASH_PAGE_SIZE - 1)], 0xFF, FLASH_PAGE_SIZE);
    erases++;
}
void bhal_flash_write(uint16_t a, uint8_t b)
{
    TEST_ASSERT_TRUE_MESSAGE(a < APROM_SIZE, "write outside APROM");
    flash[a] &= (a == fail_write_at) ? (uint8_t)(b ^ 1) : b;   /* NOR: 1 -> 0 only */
    writes++;
}
void bhal_jump_app(void) { jumped++; }
void bhal_leds(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }

/* proto.c is linked into the same native build; stub its hooks */
void hal_uart_put(uint8_t b) { (void)b; }
void app_set_channel(uint8_t c, uint16_t v) { (void)c; (void)v; }
uint16_t app_get_channel(uint8_t c) { (void)c; return 0; }
void app_set_fade(uint16_t ms) { (void)ms; }
uint16_t app_get_fade(void) { return 0; }
uint8_t app_get_button(uint8_t b) { (void)b; return 0; }
void app_set_led_mode(uint8_t m) { (void)m; }
uint8_t app_get_led_mode(void) { return 0; }
void app_set_led(uint8_t l, uint8_t on) { (void)l; (void)on; }
uint8_t app_get_led(uint8_t l) { (void)l; return 0; }
uint8_t app_ota_request(uint32_t s) { (void)s; return 0; }
uint8_t app_get_cfg(uint8_t dp) { (void)dp; return 0; }
void app_set_cfg(uint8_t dp, uint8_t v) { (void)dp; (void)v; }

static t_frame_t fr[8];
static uint8_t image[OTA_MAX_SIZE];
static int image_len;

/* Build an OTA image (app + footer) like scripts/fwimage.py */
static void make_image(int app_len, uint8_t seed)
{
    uint16_t crc = 0xFFFF;
    int i;
    uint8_t *f;
    srand(seed);
    for (i = 0; i < app_len; i++) {
        image[i] = (uint8_t)rand();
        crc = crc16_update(crc, image[i]);
    }
    f = &image[app_len];
    memset(f, 0xFF, FOOTER_SIZE);
    f[0] = 'G'; f[1] = 'L'; f[2] = 'C'; f[3] = 'W';
    f[FOOTER_OFS_LEN] = (uint8_t)app_len; f[FOOTER_OFS_LEN + 1] = (uint8_t)(app_len >> 8);
    f[FOOTER_OFS_CRC] = (uint8_t)crc; f[FOOTER_OFS_CRC + 1] = (uint8_t)(crc >> 8);
    f[FOOTER_OFS_FORMAT] = FOOTER_FORMAT;
    memset(f + FOOTER_OFS_VERSION, 0, 16);
    memcpy(f + FOOTER_OFS_VERSION, "9.9.9", 5);
    image_len = app_len + FOOTER_SIZE;
}

/* Install the current image as if programmed by ICP (app + record) */
static void icp_install(void)
{
    memset(flash, 0xFF, sizeof flash);
    memcpy(flash, image, image_len);
    memcpy(&flash[APP_RECORD_ADDR], &image[image_len - FOOTER_SIZE], RECORD_SIZE);
}

static int send(uint8_t cmd, const uint8_t *p, int n)
{
    uint8_t buf[300];
    int k = t_build(buf, cmd, p, n), i;
    t_txn = 0;
    for (i = 0; i < k; i++)
        boot_rx(buf[i]);
    return t_frames(fr, 8);
}

static int send_start(uint32_t size)
{
    uint8_t d[4] = {(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    return send(TCMD_OTA_START, d, 4);
}

static int send_data(uint32_t off, const uint8_t *p, int n)
{
    uint8_t d[4 + 256];
    d[0] = (uint8_t)(off >> 24); d[1] = (uint8_t)(off >> 16); d[2] = (uint8_t)(off >> 8); d[3] = (uint8_t)off;
    if (n) memcpy(d + 4, p, n);
    return send(TCMD_OTA_DATA, d, 4 + n);
}

/* full transfer; returns number of acked data packets */
static int transfer(void)
{
    int off, acks = 0;
    for (off = 0; off < image_len; off += OTA_PKT_SIZE) {
        int n = image_len - off < (int)OTA_PKT_SIZE ? image_len - off : (int)OTA_PKT_SIZE;
        if (send_data(off, image + off, n) == 1 && fr[0].cmd == TCMD_OTA_DATA)
            acks++;
    }
    send_data(image_len, NULL, 0);
    return acks;
}

void setUp(void)
{
    memset(flash, 0xFF, sizeof flash);
    jumped = erases = writes = 0;
    fail_write_at = -1;
    t_txn = 0;
}
void tearDown(void) {}

static void test_crc_matches_python_check_value(void)
{
    const char *s = "123456789";
    uint16_t c = 0xFFFF;
    while (*s) c = crc16_update(c, (uint8_t)*s++);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, c);
}

static void test_valid_app_detected(void)
{
    make_image(5000, 1);
    TEST_ASSERT_FALSE(boot_app_valid());   /* blank */
    icp_install();
    TEST_ASSERT_TRUE(boot_app_valid());
    flash[1234] ^= 0x40;                   /* corrupt */
    TEST_ASSERT_FALSE(boot_app_valid());
}

static void test_rescue_mode_answers_tuya_init(void)
{
    const uint8_t *v; uint8_t type; uint16_t len;
    boot_init(0, 0);
    TEST_ASSERT_EQUAL(BOOT_ST_RESCUE, boot_state);
    TEST_ASSERT_EQUAL(1, send(TCMD_HEARTBEAT, NULL, 0));
    TEST_ASSERT_EQUAL(0, fr[0].data[0]);
    TEST_ASSERT_EQUAL(1, send(TCMD_PRODUCT, NULL, 0));
    TEST_ASSERT_TRUE(fr[0].ok);
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_QUERY, NULL, 0));
    v = t_find_dp(&fr[0], DP_FW_VERSION, &type, &len);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(DPT_STRING, type);
    TEST_ASSERT_EQUAL_MEMORY("0.0.0", v, 5);
}

static void test_full_update_from_rescue(void)
{
    make_image(7000, 2);
    boot_init(0, 0);
    TEST_ASSERT_EQUAL(1, send_start(image_len));
    TEST_ASSERT_EQUAL(TCMD_OTA_START, fr[0].cmd);
    TEST_ASSERT_EQUAL(OTA_PKT_SEL, fr[0].data[0]);
    TEST_ASSERT_EQUAL((image_len + 255) / 256, transfer());
    TEST_ASSERT_EQUAL(1, jumped);
    TEST_ASSERT_EQUAL_MEMORY(image, flash, image_len);
    TEST_ASSERT_TRUE(boot_app_valid());
}

static void test_update_requested_by_app_with_handoff(void)
{
    make_image(3000, 3);
    icp_install();
    make_image(4000, 4);                    /* new version */
    t_txn = 0;
    boot_init(1, (uint16_t)image_len);      /* replies to the pending 0x0A */
    TEST_ASSERT_EQUAL(1, t_frames(fr, 8));
    TEST_ASSERT_EQUAL(TCMD_OTA_START, fr[0].cmd);
    TEST_ASSERT_EQUAL(BOOT_ST_READY, boot_state);
    transfer();
    TEST_ASSERT_EQUAL(1, jumped);
    TEST_ASSERT_EQUAL_MEMORY(image, flash, image_len);
    TEST_ASSERT_TRUE(boot_app_valid());
}

static void test_retransmitted_packet_is_acked_not_rewritten(void)
{
    int w;
    make_image(1000, 5);
    boot_init(0, 0);
    send_start(image_len);
    send_data(0, image, 256);
    w = writes;
    TEST_ASSERT_EQUAL(1, send_data(0, image, 256));     /* ack lost -> resend */
    TEST_ASSERT_EQUAL(TCMD_OTA_DATA, fr[0].cmd);
    TEST_ASSERT_EQUAL(w, writes);
    TEST_ASSERT_EQUAL(0, send_data(512, image + 512, 256));  /* gap: no ack */
}

static void test_corrupt_transfer_is_rejected(void)
{
    make_image(2000, 6);
    icp_install();
    boot_init(1, 0);
    TEST_ASSERT_EQUAL(BOOT_ST_WAIT, boot_state);
    send_start(image_len);
    image[100] ^= 0xFF;                     /* corrupted in transit */
    transfer();
    TEST_ASSERT_EQUAL(0, jumped);
    TEST_ASSERT_EQUAL(BOOT_ST_FAIL, boot_state);
    TEST_ASSERT_FALSE(boot_app_valid());    /* old app was invalidated */
    /* a fresh, correct attempt still works */
    image[100] ^= 0xFF;
    send_start(image_len);
    transfer();
    TEST_ASSERT_EQUAL(1, jumped);
    TEST_ASSERT_TRUE(boot_app_valid());
}

static void test_flash_verify_failure(void)
{
    make_image(2000, 7);
    boot_init(0, 0);
    send_start(image_len);
    fail_write_at = 300;
    transfer();
    TEST_ASSERT_EQUAL(0, jumped);
    TEST_ASSERT_EQUAL(BOOT_ST_FAIL, boot_state);
}

static void test_oversize_and_tiny_rejected(void)
{
    boot_init(0, 0);
    TEST_ASSERT_EQUAL(0, send_start(OTA_MAX_SIZE + 1));
    TEST_ASSERT_EQUAL(0, send_start(0x00010000));
    TEST_ASSERT_EQUAL(0, send_start(FOOTER_SIZE));
    TEST_ASSERT_EQUAL(1, send_start(OTA_MAX_SIZE));
}

static void test_wait_timeout_returns_to_app(void)
{
    int i;
    make_image(1500, 8);
    icp_install();
    boot_init(1, 0);
    for (i = 0; i < (int)BOOT_WAIT_TIMEOUT_MS - 10; i++)
        boot_tick();
    TEST_ASSERT_EQUAL(0, jumped);
    for (i = 0; i < 20; i++)
        boot_tick();
    TEST_ASSERT_TRUE(jumped >= 1);   /* on target the jump does not return */
}

static void test_rescue_never_times_out(void)
{
    int i;
    boot_init(0, 0);
    for (i = 0; i < 40000; i++)
        boot_tick();
    TEST_ASSERT_EQUAL(0, jumped);
}

static void test_record_and_settings_pages_survive_largest_image(void)
{
    static const uint8_t marker[8] = {0x5A, 1, 2, 3, 4, 5, 6, 7};
    make_image(OTA_MAX_SIZE - FOOTER_SIZE, 9);   /* largest possible app */
    memcpy(&flash[SETTINGS_ADDR], marker, sizeof marker);
    boot_init(0, 0);
    TEST_ASSERT_EQUAL(1, send_start(image_len));
    transfer();
    TEST_ASSERT_EQUAL(1, jumped);
    TEST_ASSERT_TRUE(boot_app_valid());
    /* the app's settings page survives the update */
    TEST_ASSERT_EQUAL_MEMORY(marker, &flash[SETTINGS_ADDR], sizeof marker);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_python_check_value);
    RUN_TEST(test_valid_app_detected);
    RUN_TEST(test_rescue_mode_answers_tuya_init);
    RUN_TEST(test_full_update_from_rescue);
    RUN_TEST(test_update_requested_by_app_with_handoff);
    RUN_TEST(test_retransmitted_packet_is_acked_not_rewritten);
    RUN_TEST(test_corrupt_transfer_is_rejected);
    RUN_TEST(test_flash_verify_failure);
    RUN_TEST(test_oversize_and_tiny_rejected);
    RUN_TEST(test_wait_timeout_returns_to_app);
    RUN_TEST(test_rescue_never_times_out);
    RUN_TEST(test_record_and_settings_pages_survive_largest_image);
    return UNITY_END();
}
