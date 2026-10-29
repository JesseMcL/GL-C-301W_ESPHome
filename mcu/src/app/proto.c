#include "proto.h"
#include "config.h"
#include "tuya_dp.h"

#define RX_MAX            48     /* largest frame we act on: a few DPs */
#define RX_GAP_MS         50

uint8_t proto_wifi_state;
volatile uint8_t proto_link_seen;

static __xdata uint8_t rx_buf[RX_MAX];
static uint8_t  rx_state;
static uint8_t  rx_cmd;
static uint16_t rx_len;
static uint16_t rx_idx;
static uint8_t  rx_sum;
static uint16_t rx_last_ms;
static uint8_t  hb_sent;

static const char __code product_json[] =
    "{\"p\":\"" PRODUCT_ID "\",\"v\":\"" FW_VERSION "\",\"m\":0}";
#ifdef PROTO_VERSION_HOOK
/* host simulator: version comes from the image "in flash" */
const char *proto_fw_version(void);
#define FWV_STR   proto_fw_version()
#define FWV_LEN   ((uint8_t)strlen(proto_fw_version()))
#include <string.h>
#else
static const char __code fw_version[] = FW_VERSION;
#define FWV_STR   fw_version
#define FWV_LEN   (sizeof(fw_version) - 1)
#endif

static const uint8_t __code all_dps[] = {
    DP_CH_R, DP_CH_G, DP_CH_B, DP_CH_C, DP_CH_W, DP_FADE_MS,
    DP_BTN_POWER, DP_BTN_SET, DP_LED_MODE, DP_LED_RED, DP_LED_GREEN,
    DP_LED_BLUE, DP_PWM_FREQ, DP_POWER_MEMORY, DP_RESET_SETTINGS,
    DP_FW_VERSION
};

/* ---------------- TX ---------------- */
static uint8_t tx_sum;

static void tx_byte(uint8_t b)
{
    tx_sum += b;
    hal_uart_put(b);
}

static void tx_begin(uint8_t cmd, uint16_t len)
{
    tx_sum = 0;
    tx_byte(0x55);
    tx_byte(0xAA);
    tx_byte(TUYA_MCU_VER);
    tx_byte(cmd);
    tx_byte((uint8_t)(len >> 8));
    tx_byte((uint8_t)len);
}

static void tx_end(void)
{
    hal_uart_put(tx_sum);
}

static void send_simple(uint8_t cmd)
{
    tx_begin(cmd, 0);
    tx_end();
}

/* Emit (or just size, when emit == 0) one datapoint record. Returns the
 * record size, 0 for unknown ids. */
static uint8_t tx_dp(uint8_t id, uint8_t emit)
{
    uint8_t type, len, i;
    uint16_t v = 0;

    if (id >= DP_CH_R && id <= DP_FADE_MS) {
        type = DPT_VALUE; len = 4;
        v = (id == DP_FADE_MS) ? app_get_fade() : app_get_channel(id - DP_CH_R);
    } else if (id == DP_BTN_POWER || id == DP_BTN_SET) {
        type = DPT_BOOL; len = 1;
        v = app_get_button(id == DP_BTN_POWER ? BTN_POWER : BTN_SET);
    } else if (id == DP_LED_MODE) {
        type = DPT_ENUM; len = 1;
        v = app_get_led_mode();
    } else if (id >= DP_LED_RED && id <= DP_LED_BLUE) {
        type = DPT_BOOL; len = 1;
        v = app_get_led(id - DP_LED_RED);
    } else if (id >= DP_PWM_FREQ && id <= DP_RESET_SETTINGS) {
        type = (id == DP_PWM_FREQ) ? DPT_ENUM : DPT_BOOL; len = 1;
        v = app_get_cfg(id);
    } else if (id == DP_FW_VERSION) {
        type = DPT_STRING; len = FWV_LEN;
    } else {
        return 0;
    }

    if (emit) {
        tx_byte(id);
        tx_byte(type);
        tx_byte(0);
        tx_byte(len);
        if (type == DPT_STRING) {
            for (i = 0; i < len; i++)
                tx_byte((uint8_t)FWV_STR[i]);
        } else if (len == 4) {
            tx_byte(0);
            tx_byte(0);
            tx_byte((uint8_t)(v >> 8));
            tx_byte((uint8_t)v);
        } else {
            tx_byte((uint8_t)v);
        }
    }
    return 4 + len;
}

static void report_list(const uint8_t __xdata *ids, uint8_t n)
{
    uint8_t i;
    uint16_t len = 0;
    for (i = 0; i < n; i++)
        len += tx_dp(ids[i], 0);
    tx_begin(TCMD_DP_REPORT, len);
    for (i = 0; i < n; i++)
        tx_dp(ids[i], 1);
    tx_end();
}

static __xdata uint8_t id_list[sizeof(all_dps)];

static void report_all(void)
{
    uint8_t i;
    for (i = 0; i < sizeof(all_dps); i++)
        id_list[i] = all_dps[i];
    report_list(id_list, sizeof(all_dps));
}

/* Not via id_list: may be called while handle_dp_deliver() fills it */
void proto_report(uint8_t dp)
{
    tx_begin(TCMD_DP_REPORT, tx_dp(dp, 0));
    tx_dp(dp, 1);
    tx_end();
}

void proto_report_button(uint8_t btn)
{
    proto_report((btn == BTN_POWER) ? DP_BTN_POWER : DP_BTN_SET);
}

/* ---------------- RX handling ---------------- */
static uint16_t be16(const uint8_t __xdata *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void handle_dp_deliver(void)
{
    uint16_t pos = 0, dlen, v16;
    uint8_t id, n = 0;
    const uint8_t __xdata *d;

    while (pos + 4 <= rx_len) {
        id   = rx_buf[pos];
        dlen = be16(&rx_buf[pos + 2]);
        d    = &rx_buf[pos + 4];
        pos += 4;
        if (pos + dlen > rx_len)
            break;
        pos += dlen;

        if (dlen == 4) {
            /* value DPs: ignore the top 16 bits unless they're non-zero */
            v16 = (d[0] || d[1]) ? 0xFFFF : be16(d + 2);
            if (id >= DP_CH_R && id <= DP_CH_W) {
                app_set_channel(id - DP_CH_R, v16);
            } else if (id == DP_FADE_MS) {
                app_set_fade(v16 > FADE_MS_MAX ? FADE_MS_MAX : v16);
            } else {
                continue;
            }
        } else if (dlen == 1) {
            if (id == DP_LED_MODE) {
                if (d[0] < LED_MODE_COUNT)
                    app_set_led_mode(d[0]);
            } else if (id >= DP_LED_RED && id <= DP_LED_BLUE) {
                app_set_led(id - DP_LED_RED, d[0] != 0);
            } else if (id >= DP_PWM_FREQ && id <= DP_RESET_SETTINGS) {
                app_set_cfg(id, d[0]);          /* validated by the app */
            } else {
                continue;
            }
        } else {
            continue;
        }
        if (n < sizeof(id_list))
            id_list[n++] = id;
    }

    /* The module waits for a 0x07 after every 0x06, so always answer. */
    if (n)
        report_list(id_list, n);
    else
        report_all();
}

static void handle_frame(void)
{
    uint8_t i;
    proto_link_seen = 1;

    switch (rx_cmd) {
    case TCMD_HEARTBEAT:
        tx_begin(TCMD_HEARTBEAT, 1);
        tx_byte(hb_sent ? 0x01 : 0x00);
        tx_end();
        hb_sent = 1;
        break;

    case TCMD_PRODUCT:
        tx_begin(TCMD_PRODUCT, sizeof(product_json) - 1);
        for (i = 0; i < sizeof(product_json) - 1; i++)
            tx_byte((uint8_t)product_json[i]);
        tx_end();
        break;

    case TCMD_CONF:
        /* Empty = MCU handles network status itself -> WIFI_STATE updates.
         * ESPHome's tuya: only sends WIFI_STATE when the status changes, so
         * after an MCU restart (e.g. a firmware update) its re-init would
         * stall waiting for it. An unsolicited WIFI_STATE ack moves it on,
         * and 0x2B asks for the current network status instead. */
        send_simple(TCMD_CONF);
        send_simple(TCMD_WIFI_STATE);
        send_simple(TCMD_NET_STATUS);
        break;

    case TCMD_WIFI_STATE:
        if (rx_len >= 1)
            proto_wifi_state = rx_buf[0];
        send_simple(TCMD_WIFI_STATE);
        break;

    case TCMD_NET_STATUS:
        if (rx_len >= 1)
            proto_wifi_state = rx_buf[0];
        break;

    case TCMD_DP_DELIVER:
        handle_dp_deliver();
        break;

    case TCMD_DP_QUERY:
        report_all();
        break;

    case TCMD_OTA_START:
        /* 4-byte (legacy) or 8-byte form; size is the first 4 bytes BE.
         * If accepted we reset into the bootloader, which sends the reply. */
        if (rx_len >= 4)
            app_ota_request(((uint32_t)rx_buf[0] << 24) | ((uint32_t)rx_buf[1] << 16) |
                            ((uint32_t)rx_buf[2] << 8) | rx_buf[3]);
        break;

    default:
        /* time sync, acks, RSSI, stray OTA data etc.: nothing to do */
        break;
    }
}

void proto_init(void)
{
    rx_state = 0;
    hb_sent = 0;
    proto_wifi_state = WIFI_UNKNOWN;
    proto_link_seen = 0;
}

void proto_poll(uint16_t now_ms)
{
    if (rx_state != 0 && (uint16_t)(now_ms - rx_last_ms) > RX_GAP_MS)
        rx_state = 0;
}

void proto_rx(uint8_t b, uint16_t now_ms)
{
    rx_last_ms = now_ms;

    switch (rx_state) {
    case 0:
        if (b == 0x55) { rx_sum = b; rx_state = 1; }
        break;
    case 1:
        if (b == 0xAA) { rx_sum += b; rx_state = 2; }
        else if (b != 0x55) rx_state = 0;
        break;
    case 2:                                 /* version (ignored) */
        rx_sum += b; rx_state = 3;
        break;
    case 3:
        rx_cmd = b; rx_sum += b; rx_state = 4;
        break;
    case 4:
        rx_len = (uint16_t)b << 8; rx_sum += b; rx_state = 5;
        break;
    case 5:
        rx_len |= b; rx_sum += b; rx_idx = 0;
        rx_state = rx_len ? 6 : 7;
        break;
    case 6:
        if (rx_idx < RX_MAX)
            rx_buf[rx_idx] = b;
        rx_sum += b;
        if (++rx_idx >= rx_len)
            rx_state = 7;
        break;
    case 7:
        rx_state = 0;
        if (b == rx_sum && rx_len <= RX_MAX)
            handle_frame();
        break;
    default:
        rx_state = 0;
        break;
    }
}
