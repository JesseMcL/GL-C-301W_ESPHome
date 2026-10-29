#include "proto.h"
#include "config.h"

#define CMD_HEARTBEAT     0x00
#define CMD_PRODUCT       0x01
#define CMD_CONF          0x02
#define CMD_WIFI_STATE    0x03
#define CMD_DP_DELIVER    0x06
#define CMD_DP_REPORT     0x07
#define CMD_DP_QUERY      0x08

#define DPT_RAW           0x00
#define DPT_ENUM          0x04

#define MCU_PROTO_VER     0x03
#define RX_MAX            48     /* largest frame we act on is 23 bytes */
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

static __xdata uint16_t tmp_ch[NUM_CH];

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
    tx_byte(MCU_PROTO_VER);
    tx_byte(cmd);
    tx_byte((uint8_t)(len >> 8));
    tx_byte((uint8_t)len);
}

static void tx_end(void)
{
    hal_uart_put(tx_sum);
}

/* Datapoint record sizes (header 4 bytes + value) */
#define DP101_SIZE  (4 + 2 * NUM_CH)
#define DPENUM_SIZE (4 + 1)

static void tx_dp_channels(void)
{
    uint8_t i;
    uint16_t v;
    tx_byte(DP_CHANNELS);
    tx_byte(DPT_RAW);
    tx_byte(0);
    tx_byte(2 * NUM_CH);
    for (i = 0; i < NUM_CH; i++) {
        v = app_get_target(i);
        tx_byte((uint8_t)(v >> 8));
        tx_byte((uint8_t)v);
    }
}

static void tx_dp_enum(uint8_t id, uint8_t val)
{
    tx_byte(id);
    tx_byte(DPT_ENUM);
    tx_byte(0);
    tx_byte(1);
    tx_byte(val);
}

static void send_simple(uint8_t cmd)
{
    tx_begin(cmd, 0);
    tx_end();
}

static void report_all(void)
{
    tx_begin(CMD_DP_REPORT, DP101_SIZE + DPENUM_SIZE + DPENUM_SIZE);
    tx_dp_channels();
    tx_dp_enum(DP_BUTTON, BTN_EV_NONE);
    tx_dp_enum(DP_LED_MODE, app_get_led_mode());
    tx_end();
}

void proto_report_button(uint8_t ev)
{
    tx_begin(CMD_DP_REPORT, DPENUM_SIZE);
    tx_dp_enum(DP_BUTTON, ev);
    tx_end();
}

/* ---------------- RX handling ---------------- */
static uint16_t be16(const uint8_t __xdata *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static void handle_dp_deliver(void)
{
    uint16_t pos = 0;
    uint16_t dlen;
    uint8_t id, type, i;
    uint8_t handled_ch = 0, handled_led = 0;

    while (pos + 4 <= rx_len) {
        id   = rx_buf[pos];
        type = rx_buf[pos + 1];
        dlen = be16(&rx_buf[pos + 2]);
        pos += 4;
        if (pos + dlen > rx_len)
            break;

        if (id == DP_CHANNELS && type == DPT_RAW &&
            (dlen == 2 * NUM_CH || dlen == 2 * NUM_CH + 2)) {
            for (i = 0; i < NUM_CH; i++)
                tmp_ch[i] = be16(&rx_buf[pos + 2 * i]);
            app_set_channels(tmp_ch,
                             dlen == 2 * NUM_CH + 2 ? be16(&rx_buf[pos + 2 * NUM_CH]) : 0);
            handled_ch = 1;
        } else if (id == DP_LED_MODE && dlen == 1) {
            if (rx_buf[pos] <= LED_MODE_ON)
                app_set_led_mode(rx_buf[pos]);
            handled_led = 1;
        }
        pos += dlen;
    }

    /* The module waits for a 0x07 after every 0x06, so always answer. */
    if (handled_ch && !handled_led) {
        tx_begin(CMD_DP_REPORT, DP101_SIZE);
        tx_dp_channels();
        tx_end();
    } else if (handled_led && !handled_ch) {
        tx_begin(CMD_DP_REPORT, DPENUM_SIZE);
        tx_dp_enum(DP_LED_MODE, app_get_led_mode());
        tx_end();
    } else {
        report_all();
    }
}

static void handle_frame(void)
{
    proto_link_seen = 1;

    switch (rx_cmd) {
    case CMD_HEARTBEAT:
        tx_begin(CMD_HEARTBEAT, 1);
        tx_byte(hb_sent ? 0x01 : 0x00);
        tx_end();
        hb_sent = 1;
        break;

    case CMD_PRODUCT: {
        static const char __code prod[] = PRODUCT_JSON;
        uint8_t i;
        tx_begin(CMD_PRODUCT, sizeof(prod) - 1);
        for (i = 0; i < sizeof(prod) - 1; i++)
            tx_byte((uint8_t)prod[i]);
        tx_end();
        break;
    }

    case CMD_CONF:
        /* Empty reply = "MCU handles network status itself";
         * ESPHome then sends WIFI_STATE updates. */
        send_simple(CMD_CONF);
        break;

    case CMD_WIFI_STATE:
        if (rx_len >= 1)
            proto_wifi_state = rx_buf[0];
        send_simple(CMD_WIFI_STATE);
        break;

    case CMD_DP_DELIVER:
        handle_dp_deliver();
        break;

    case CMD_DP_QUERY:
        report_all();
        break;

    default:
        /* time sync, acks, RSSI etc.: nothing to do */
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
    case 0:                                 /* 0x55 */
        if (b == 0x55) { rx_sum = b; rx_state = 1; }
        break;
    case 1:                                 /* 0xAA */
        if (b == 0xAA) { rx_sum += b; rx_state = 2; }
        else if (b != 0x55) rx_state = 0;  /* 55 55 AA resyncs */
        break;
    case 2:                                 /* version (ignored) */
        rx_sum += b; rx_state = 3;
        break;
    case 3:                                 /* command */
        rx_cmd = b; rx_sum += b; rx_state = 4;
        break;
    case 4:
        rx_len = (uint16_t)b << 8; rx_sum += b; rx_state = 5;
        break;
    case 5:
        rx_len |= b; rx_sum += b; rx_idx = 0;
        rx_state = rx_len ? 6 : 7;
        break;
    case 6:                                 /* payload */
        if (rx_idx < RX_MAX)
            rx_buf[rx_idx] = b;
        rx_sum += b;
        if (++rx_idx >= rx_len)
            rx_state = 7;
        break;
    case 7:                                 /* checksum */
        rx_state = 0;
        if (b == rx_sum && rx_len <= RX_MAX)
            handle_frame();
        break;
    default:
        rx_state = 0;
        break;
    }
}
