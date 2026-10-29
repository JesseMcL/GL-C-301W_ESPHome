/* Application TuyaMCU protocol tests: drives proto.c the way ESPHome's
 * tuya component does. */
#include <unity.h>
#include "proto.h"
#include "tuya_dp.h"
#include "boot_core.h"
#include "../common/tuya_test_util.h"

/* ---- app hooks ---- */
static uint16_t ch[NUM_CH], fade = 60;
static uint8_t btn[2], led_mode, leds[NUM_LED];
static uint32_t ota_req;
static int ota_calls;

void hal_uart_put(uint8_t b) { t_tx[t_txn++] = b; }
void app_set_channel(uint8_t c, uint16_t v) { ch[c] = v; }
uint16_t app_get_channel(uint8_t c) { return ch[c]; }
void app_set_fade(uint16_t ms) { fade = ms; }
uint16_t app_get_fade(void) { return fade; }
uint8_t app_get_button(uint8_t b) { return btn[b]; }
void app_set_led_mode(uint8_t m) { led_mode = m; }
uint8_t app_get_led_mode(void) { return led_mode; }
void app_set_led(uint8_t l, uint8_t on) { leds[l] = on; }
uint8_t app_get_led(uint8_t l) { return leds[l]; }
uint8_t app_ota_request(uint32_t size) { ota_req = size; ota_calls++; return 0; }
static uint8_t cfgv[3];                       /* DPs 113..115 */
static int resets;
uint8_t app_get_cfg(uint8_t dp) { return dp == DP_RESET_SETTINGS ? 0 : cfgv[dp - DP_PWM_FREQ]; }
void app_set_cfg(uint8_t dp, uint8_t v)
{
    if (dp == DP_RESET_SETTINGS) { if (v) resets++; return; }
    cfgv[dp - DP_PWM_FREQ] = v;
}

/* logic.c is linked into the native build; no flash here */
uint8_t lhal_flash_read(uint16_t a) { (void)a; return 0xFF; }
void lhal_flash_erase(uint16_t a) { (void)a; }
void lhal_flash_write(uint16_t a, uint8_t b) { (void)a; (void)b; }

/* boot_core is linked into the same native build; stub its HAL */
void bhal_putc(uint8_t b) { (void)b; }
uint8_t bhal_flash_read(uint16_t a) { (void)a; return 0xFF; }
void bhal_flash_erase(uint16_t a) { (void)a; }
void bhal_flash_write(uint16_t a, uint8_t b) { (void)a; (void)b; }
void bhal_jump_app(void) {}
void bhal_leds(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }

static t_frame_t fr[16];

static int send(uint8_t cmd, const uint8_t *p, int n)
{
    uint8_t buf[300];
    int k = t_build(buf, cmd, p, n), i;
    t_txn = 0;
    for (i = 0; i < k; i++)
        proto_rx(buf[i], 100);
    return t_frames(fr, 16);
}

void setUp(void)
{
    memset(ch, 0, sizeof ch);
    memset(leds, 0, sizeof leds);
    memset(cfgv, 0, sizeof cfgv);
    led_mode = 0; fade = 60; ota_calls = 0; resets = 0;
    proto_init();
}
void tearDown(void) {}

static void test_init_sequence(void)
{
    uint8_t w = 0x03;
    TEST_ASSERT_EQUAL(1, send(TCMD_HEARTBEAT, NULL, 0));
    TEST_ASSERT_TRUE(fr[0].ok);
    TEST_ASSERT_EQUAL_HEX8(0x03, fr[0].ver);
    TEST_ASSERT_EQUAL(0x00, fr[0].data[0]);           /* first: restarted */
    send(TCMD_HEARTBEAT, NULL, 0);
    TEST_ASSERT_EQUAL(0x01, fr[0].data[0]);

    TEST_ASSERT_EQUAL(1, send(TCMD_PRODUCT, NULL, 0));
    TEST_ASSERT_EQUAL(TCMD_PRODUCT, fr[0].cmd);
    TEST_ASSERT_EQUAL('{', fr[0].data[0]);

    TEST_ASSERT_EQUAL(3, send(TCMD_CONF, NULL, 0));
    TEST_ASSERT_EQUAL(TCMD_CONF, fr[0].cmd);
    TEST_ASSERT_EQUAL(0, fr[0].len);
    TEST_ASSERT_EQUAL(TCMD_WIFI_STATE, fr[1].cmd);     /* un-stall ESPHome re-init */
    TEST_ASSERT_EQUAL(TCMD_NET_STATUS, fr[2].cmd);
    { uint8_t st = 0x04; send(TCMD_NET_STATUS, &st, 1); }
    TEST_ASSERT_EQUAL(4, proto_wifi_state);

    send(TCMD_WIFI_STATE, &w, 1);
    TEST_ASSERT_EQUAL(TCMD_WIFI_STATE, fr[0].cmd);
    TEST_ASSERT_EQUAL(3, proto_wifi_state);
}

static void test_query_reports_all_dps(void)
{
    uint8_t type; uint16_t len;
    const uint8_t *v;
    int id;
    ch[CH_C] = 0x1234;
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_QUERY, NULL, 0));
    TEST_ASSERT_EQUAL(TCMD_DP_REPORT, fr[0].cmd);
    TEST_ASSERT_TRUE(fr[0].ok);
    for (id = DP_CH_R; id <= DP_RESET_SETTINGS; id++)
        TEST_ASSERT_NOT_NULL_MESSAGE(t_find_dp(&fr[0], (uint8_t)id, NULL, NULL), "dp missing");
    v = t_find_dp(&fr[0], DP_CH_C, &type, &len);
    TEST_ASSERT_EQUAL(DPT_VALUE, type);
    TEST_ASSERT_EQUAL(4, len);
    TEST_ASSERT_EQUAL_HEX8(0x12, v[2]);
    TEST_ASSERT_EQUAL_HEX8(0x34, v[3]);
    v = t_find_dp(&fr[0], DP_FW_VERSION, &type, &len);
    TEST_ASSERT_EQUAL(DPT_STRING, type);
    TEST_ASSERT_TRUE(len > 0);
    t_find_dp(&fr[0], DP_BTN_POWER, &type, &len);
    TEST_ASSERT_EQUAL(DPT_BOOL, type);
    t_find_dp(&fr[0], DP_LED_MODE, &type, &len);
    TEST_ASSERT_EQUAL(DPT_ENUM, type);
}

static void test_set_channel_value_dp(void)
{
    uint8_t d[] = {DP_CH_W, DPT_VALUE, 0, 4, 0, 0, 0xAB, 0xCD};
    const uint8_t *v;
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_DELIVER, d, sizeof d));
    TEST_ASSERT_EQUAL_HEX16(0xABCD, ch[CH_W]);
    TEST_ASSERT_EQUAL(TCMD_DP_REPORT, fr[0].cmd);
    TEST_ASSERT_EQUAL(8, fr[0].len);                  /* echo of that DP only */
    v = t_find_dp(&fr[0], DP_CH_W, NULL, NULL);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_HEX8(0xAB, v[2]);
}

static void test_value_clamps(void)
{
    uint8_t d[] = {DP_CH_R, DPT_VALUE, 0, 4, 0, 1, 0, 0};       /* 65536 */
    uint8_t f[] = {DP_FADE_MS, DPT_VALUE, 0, 4, 0, 0, 0xFF, 0xFF};
    send(TCMD_DP_DELIVER, d, sizeof d);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, ch[CH_R]);
    send(TCMD_DP_DELIVER, f, sizeof f);
    TEST_ASSERT_EQUAL(FADE_MS_MAX, fade);
}

static void test_multiple_dps_one_frame(void)
{
    uint8_t d[] = {DP_CH_R, DPT_VALUE, 0, 4, 0, 0, 0, 1,
                   DP_LED_MODE, DPT_ENUM, 0, 1, 1,
                   DP_LED_GREEN, DPT_BOOL, 0, 1, 1};
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_DELIVER, d, sizeof d));
    TEST_ASSERT_EQUAL(1, ch[CH_R]);
    TEST_ASSERT_EQUAL(LED_MODE_MANUAL, led_mode);
    TEST_ASSERT_EQUAL(1, leds[LED_G]);
    TEST_ASSERT_EQUAL(8 + 5 + 5, fr[0].len);
}

static void test_settings_dps(void)
{
    uint8_t type;
    const uint8_t *v;
    uint8_t d[] = {DP_PWM_FREQ, DPT_ENUM, 0, 1, PWM_FREQ_600,
                   DP_POWER_MEMORY, DPT_BOOL, 0, 1, 1};
    uint8_t r[] = {DP_RESET_SETTINGS, DPT_BOOL, 0, 1, 1};
    uint8_t m[] = {DP_LED_MODE, DPT_ENUM, 0, 1, 7};

    TEST_ASSERT_EQUAL(1, send(TCMD_DP_DELIVER, d, sizeof d));
    TEST_ASSERT_EQUAL(PWM_FREQ_600, cfgv[0]);
    TEST_ASSERT_EQUAL(1, cfgv[1]);
    v = t_find_dp(&fr[0], DP_PWM_FREQ, &type, NULL);
    TEST_ASSERT_EQUAL(DPT_ENUM, type);
    TEST_ASSERT_EQUAL(PWM_FREQ_600, v[0]);
    v = t_find_dp(&fr[0], DP_POWER_MEMORY, &type, NULL);
    TEST_ASSERT_EQUAL(DPT_BOOL, type);
    TEST_ASSERT_EQUAL(1, v[0]);

    /* reset is momentary: acted on, reported back as 0 */
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_DELIVER, r, sizeof r));
    TEST_ASSERT_EQUAL(1, resets);
    v = t_find_dp(&fr[0], DP_RESET_SETTINGS, &type, NULL);
    TEST_ASSERT_EQUAL(0, v[0]);

    /* out-of-range LED mode ignored */
    led_mode = LED_MODE_MANUAL;
    send(TCMD_DP_DELIVER, m, sizeof m);
    TEST_ASSERT_EQUAL(LED_MODE_MANUAL, led_mode);
}

static void test_report_single_dp(void)
{
    const uint8_t *v;
    cfgv[0] = PWM_FREQ_8000;
    t_txn = 0;
    proto_report(DP_PWM_FREQ);
    TEST_ASSERT_EQUAL(1, t_frames(fr, 16));
    TEST_ASSERT_EQUAL(5, fr[0].len);
    v = t_find_dp(&fr[0], DP_PWM_FREQ, NULL, NULL);
    TEST_ASSERT_EQUAL(PWM_FREQ_8000, v[0]);
}

static void test_unknown_dp_still_answered(void)
{
    uint8_t d[] = {20, DPT_BOOL, 0, 1, 1};
    TEST_ASSERT_EQUAL(1, send(TCMD_DP_DELIVER, d, sizeof d));
    TEST_ASSERT_EQUAL(TCMD_DP_REPORT, fr[0].cmd);
}

static void test_button_report(void)
{
    uint8_t type;
    const uint8_t *v;
    t_txn = 0;
    btn[BTN_SET] = 1;
    proto_report_button(BTN_SET);
    TEST_ASSERT_EQUAL(1, t_frames(fr, 16));
    v = t_find_dp(&fr[0], DP_BTN_SET, &type, NULL);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(DPT_BOOL, type);
    TEST_ASSERT_EQUAL(1, v[0]);
    btn[BTN_SET] = 0;
}

static void test_ota_start_forwarded(void)
{
    uint8_t d4[] = {0, 0, 0x14, 0x0E};
    uint8_t d8[] = {0, 0, 0x20, 0x00, 10, 0, 0, 0};
    TEST_ASSERT_EQUAL(0, send(TCMD_OTA_START, d4, sizeof d4));   /* no reply */
    TEST_ASSERT_EQUAL(1, ota_calls);
    TEST_ASSERT_EQUAL(0x140E, ota_req);
    send(TCMD_OTA_START, d8, sizeof d8);
    TEST_ASSERT_EQUAL(0x2000, ota_req);
}

static void test_framing_robustness(void)
{
    uint8_t bad[] = {0x55, 0xAA, 0, 0, 0, 0, 0x00};
    uint8_t g[] = {0x12, 0x55, 0x55, 0xAA, 0x00, 0x00, 0x00, 0x00, 0xFF};
    uint8_t big[200];
    int i;
    t_txn = 0;
    for (i = 0; i < 7; i++) proto_rx(bad[i], 0);
    TEST_ASSERT_EQUAL(0, t_txn);
    for (i = 0; i < 9; i++) proto_rx(g[i], 0);
    TEST_ASSERT_EQUAL(1, t_frames(fr, 16));
    memset(big, 0xAB, sizeof big);
    TEST_ASSERT_EQUAL(0, send(TCMD_DP_DELIVER, big, sizeof big));
    TEST_ASSERT_EQUAL(1, send(TCMD_HEARTBEAT, NULL, 0));
    /* half frame then gap */
    t_txn = 0;
    proto_rx(0x55, 1000); proto_rx(0xAA, 1000); proto_rx(0, 1000);
    proto_poll(1100);
    TEST_ASSERT_EQUAL(1, send(TCMD_HEARTBEAT, NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_sequence);
    RUN_TEST(test_query_reports_all_dps);
    RUN_TEST(test_set_channel_value_dp);
    RUN_TEST(test_value_clamps);
    RUN_TEST(test_multiple_dps_one_frame);
    RUN_TEST(test_settings_dps);
    RUN_TEST(test_report_single_dp);
    RUN_TEST(test_unknown_dp_still_answered);
    RUN_TEST(test_button_report);
    RUN_TEST(test_ota_start_forwarded);
    RUN_TEST(test_framing_robustness);
    return UNITY_END();
}
