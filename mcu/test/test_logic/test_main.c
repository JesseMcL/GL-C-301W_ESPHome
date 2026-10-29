/* Application logic tests: settings log, strip mapping, PWM frequency
 * table and Set-button gestures (src/app/logic.c). */
#include <unity.h>
#include <string.h>
#include "logic.h"
#include "config.h"
#include "layout.h"
#include "tuya_dp.h"

/* ---- simulated NOR flash for the settings page ---- */
static uint8_t flash[FLASH_SIZE];
static int erases, writes;

uint8_t lhal_flash_read(uint16_t a) { return flash[a]; }
void lhal_flash_erase(uint16_t a)
{
    TEST_ASSERT_EQUAL_HEX16(SETTINGS_ADDR, a & ~(FLASH_PAGE_SIZE - 1));
    memset(&flash[a & ~(FLASH_PAGE_SIZE - 1)], 0xFF, FLASH_PAGE_SIZE);
    erases++;
}
void lhal_flash_write(uint16_t a, uint8_t b)
{
    TEST_ASSERT_TRUE(a >= SETTINGS_ADDR && a < SETTINGS_ADDR + FLASH_PAGE_SIZE);
    flash[a] &= b;
    writes++;
}

/* ---- proto.c / boot_core.c are linked too; stub their hooks ---- */
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
void bhal_putc(uint8_t b) { (void)b; }
uint8_t bhal_flash_read(uint16_t a) { (void)a; return 0xFF; }
void bhal_flash_erase(uint16_t a) { (void)a; }
void bhal_flash_write(uint16_t a, uint8_t b) { (void)a; (void)b; }
void bhal_jump_app(void) {}
void bhal_leds(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }

void setUp(void)
{
    memset(flash, 0xFF, sizeof flash);
    erases = writes = 0;
    settings_defaults();
    gesture_reset();
}
void tearDown(void) {}

/* ---- settings ---- */
static void test_defaults_on_blank_flash(void)
{
    cfg.strip = STRIP_RGB;
    TEST_ASSERT_EQUAL(0, settings_load());
    TEST_ASSERT_EQUAL(STRIP_RGBCCT, cfg.strip);
    TEST_ASSERT_EQUAL(PWM_FREQ_1000, cfg.freq);
    TEST_ASSERT_EQUAL(0, cfg.memory);
    TEST_ASSERT_EQUAL(LED_MODE_AUTO, cfg.led_mode);
    TEST_ASSERT_EQUAL(DEFAULT_FADE_MS, cfg.fade_ms);
}

static void test_save_load_roundtrip(void)
{
    cfg.strip = STRIP_CCT;
    cfg.freq = PWM_FREQ_600;
    cfg.memory = 1;
    cfg.led_mode = LED_MODE_QUIET;
    cfg.fade_ms = 1234;
    settings_save();
    settings_defaults();
    TEST_ASSERT_EQUAL(1, settings_load());
    TEST_ASSERT_EQUAL(STRIP_CCT, cfg.strip);
    TEST_ASSERT_EQUAL(PWM_FREQ_600, cfg.freq);
    TEST_ASSERT_EQUAL(1, cfg.memory);
    TEST_ASSERT_EQUAL(LED_MODE_QUIET, cfg.led_mode);
    TEST_ASSERT_EQUAL(1234, cfg.fade_ms);
}

static void test_unchanged_save_writes_nothing(void)
{
    cfg.strip = STRIP_RGBW;
    settings_save();
    writes = 0;
    settings_save();
    TEST_ASSERT_EQUAL(0, writes);
}

static void test_log_wraps_with_one_erase_per_page(void)
{
    int i, slots = FLASH_PAGE_SIZE / SETTINGS_RECORD;
    for (i = 0; i < slots; i++) {
        cfg.fade_ms = (uint16_t)(100 + i);
        settings_save();
    }
    TEST_ASSERT_EQUAL(0, erases);
    cfg.fade_ms = 999;
    settings_save();                    /* page full -> erase, slot 0 */
    TEST_ASSERT_EQUAL(1, erases);
    settings_defaults();
    settings_load();
    TEST_ASSERT_EQUAL(999, cfg.fade_ms);
}

static void test_torn_record_is_skipped(void)
{
    cfg.strip = STRIP_RGB;
    settings_save();
    cfg.strip = STRIP_DIMMER;
    settings_save();
    flash[SETTINGS_ADDR + SETTINGS_RECORD + 7] = 0x00;   /* break 2nd check byte */
    settings_load();
    TEST_ASSERT_EQUAL(STRIP_RGB, cfg.strip);
    /* the next save goes after the torn slot, and wins */
    cfg.strip = STRIP_CCT;
    settings_save();
    settings_defaults();
    settings_load();
    TEST_ASSERT_EQUAL(STRIP_CCT, cfg.strip);
}

static void test_out_of_range_record_rejected(void)
{
    cfg.strip = STRIP_RGB;
    settings_save();
    /* hand-made record with strip = 9 but a correct check byte */
    {
        uint8_t r[8] = {SETTINGS_TAG, 9, 0, 0, 0, 0, 0, 0};
        uint8_t i, sum = 0;
        for (i = 0; i < 7; i++) sum += r[i];
        r[7] = (uint8_t)~sum;
        memcpy(&flash[SETTINGS_ADDR + SETTINGS_RECORD], r, 8);
    }
    settings_load();
    TEST_ASSERT_EQUAL(STRIP_RGB, cfg.strip);
}

/* ---- PWM frequency ---- */
static void test_pwm_periods(void)
{
    TEST_ASSERT_EQUAL(39999, pwm_period_of(PWM_FREQ_600));
    TEST_ASSERT_EQUAL(29999, pwm_period_of(PWM_FREQ_800));
    TEST_ASSERT_EQUAL(23999, pwm_period_of(PWM_FREQ_1000));
    TEST_ASSERT_EQUAL(11999, pwm_period_of(PWM_FREQ_2000));
    TEST_ASSERT_EQUAL(5999, pwm_period_of(PWM_FREQ_4000));
    TEST_ASSERT_EQUAL(2999, pwm_period_of(PWM_FREQ_8000));
}

static void test_pwm_freq_order_matches_manual(void)
{
    /* 1000 -> 2000 -> 4000 -> 8000 -> 600 -> 800 -> 1000 */
    static const uint8_t seq[] = {PWM_FREQ_2000, PWM_FREQ_4000, PWM_FREQ_8000,
                                  PWM_FREQ_600, PWM_FREQ_800, PWM_FREQ_1000};
    uint8_t f = PWM_FREQ_1000, i;
    for (i = 0; i < sizeof seq; i++) {
        f = pwm_freq_next(f);
        TEST_ASSERT_EQUAL(seq[i], f);
    }
    TEST_ASSERT_EQUAL(2, pwm_freq_flashes(PWM_FREQ_800));
    TEST_ASSERT_EQUAL(6, pwm_freq_flashes(PWM_FREQ_8000));
}

/* ---- strip mapping ---- */
static void test_strip_map(void)
{
    const uint16_t l[NUM_CH] = {1, 2, 3, 4, 5};     /* R G B C W */
    uint16_t p[NUM_CH];

    strip_map(STRIP_RGBCCT, l, p);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(l, p, NUM_CH);

    strip_map(STRIP_RGBW, l, p);
    { const uint16_t e[] = {1, 2, 3, 0, 5}; TEST_ASSERT_EQUAL_UINT16_ARRAY(e, p, NUM_CH); }

    strip_map(STRIP_RGB, l, p);
    { const uint16_t e[] = {1, 2, 3, 0, 0}; TEST_ASSERT_EQUAL_UINT16_ARRAY(e, p, NUM_CH); }

    strip_map(STRIP_CCT, l, p);   /* 2nd strip: W on B, C on G, R unused */
    { const uint16_t e[] = {0, 4, 5, 4, 5}; TEST_ASSERT_EQUAL_UINT16_ARRAY(e, p, NUM_CH); }

    strip_map(STRIP_DIMMER, l, p);
    { const uint16_t e[] = {5, 5, 5, 5, 5}; TEST_ASSERT_EQUAL_UINT16_ARRAY(e, p, NUM_CH); }
}

static void test_default_on_lights_something_for_every_type(void)
{
    uint16_t l[NUM_CH], p[NUM_CH];
    uint8_t s, i, any;
    for (s = 0; s < STRIP_COUNT; s++) {
        strip_default_on(s, l);
        strip_map(s, l, p);
        any = 0;
        for (i = 0; i < NUM_CH; i++)
            if (p[i]) any = 1;
        TEST_ASSERT_TRUE_MESSAGE(any, "strip type would stay dark");
    }
}

static void test_indicator_colours(void)
{
    TEST_ASSERT_EQUAL(COLOUR_R | COLOUR_G | COLOUR_B, strip_colour(STRIP_RGBCCT)); /* white */
    TEST_ASSERT_EQUAL(COLOUR_R | COLOUR_G, strip_colour(STRIP_RGBW));             /* yellow */
    TEST_ASSERT_EQUAL(COLOUR_B, strip_colour(STRIP_RGB));
    TEST_ASSERT_EQUAL(COLOUR_G, strip_colour(STRIP_CCT));
    TEST_ASSERT_EQUAL(COLOUR_R, strip_colour(STRIP_DIMMER));
}

/* ---- Set button gestures ---- */
static uint8_t run(uint8_t pressed, int ms)
{
    uint8_t ev = GEST_NONE, e;
    while (ms-- > 0)
        if ((e = gesture_tick(pressed)) != GEST_NONE) {
            TEST_ASSERT_EQUAL_MESSAGE(GEST_NONE, ev, "two events");
            ev = e;
        }
    return ev;
}

static uint8_t clicks(int n)
{
    uint8_t ev = GEST_NONE, e;
    int i;
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(GEST_NONE, run(1, 120));
        e = run(0, i == n - 1 ? 1000 : 150);
        if (e) ev = e;
    }
    return ev;
}

static void test_click_counts(void)
{
    TEST_ASSERT_EQUAL(GEST_CLICK1, clicks(1));
    TEST_ASSERT_EQUAL(GEST_CLICK2, clicks(2));
    TEST_ASSERT_EQUAL(GEST_CLICK3, clicks(3));
    TEST_ASSERT_EQUAL(GEST_NONE, clicks(4));
}

static void test_event_after_gap_only(void)
{
    run(1, 100);
    TEST_ASSERT_EQUAL(GEST_NONE, run(0, GEST_GAP_MS));       /* release tick + gap */
    TEST_ASSERT_EQUAL(GEST_CLICK1, run(0, 1));
}

static void test_long_press(void)
{
    TEST_ASSERT_EQUAL(GEST_NONE, run(1, GEST_LONG_MS - 1));
    TEST_ASSERT_EQUAL(GEST_LONG, run(1, 1));
    TEST_ASSERT_EQUAL(GEST_NONE, run(1, 2000));      /* fires once */
    TEST_ASSERT_EQUAL(GEST_NONE, run(0, 1000));      /* release is not a click */
    TEST_ASSERT_EQUAL(GEST_CLICK1, clicks(1));
}

static void test_click_then_long_is_long(void)
{
    run(1, 100);
    run(0, 100);
    TEST_ASSERT_EQUAL(GEST_LONG, run(1, GEST_LONG_MS));
    TEST_ASSERT_EQUAL(GEST_NONE, run(0, 1000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_on_blank_flash);
    RUN_TEST(test_save_load_roundtrip);
    RUN_TEST(test_unchanged_save_writes_nothing);
    RUN_TEST(test_log_wraps_with_one_erase_per_page);
    RUN_TEST(test_torn_record_is_skipped);
    RUN_TEST(test_out_of_range_record_rejected);
    RUN_TEST(test_pwm_periods);
    RUN_TEST(test_pwm_freq_order_matches_manual);
    RUN_TEST(test_strip_map);
    RUN_TEST(test_default_on_lights_something_for_every_type);
    RUN_TEST(test_indicator_colours);
    RUN_TEST(test_click_counts);
    RUN_TEST(test_event_after_gap_only);
    RUN_TEST(test_long_press);
    RUN_TEST(test_click_then_long_is_long);
    return UNITY_END();
}
