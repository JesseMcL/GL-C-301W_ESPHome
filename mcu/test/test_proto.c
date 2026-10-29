/* Host test: drives proto.c the way ESPHome's tuya component does and checks
 * every reply frame (header, version, checksum, payload). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/proto.h"

static uint8_t tx[1024];
static int     txn;
static uint16_t chans[NUM_CH];
static uint16_t last_fade;
static uint8_t led_mode;
static int fails;

void hal_uart_put(uint8_t b) { tx[txn++] = b; }
void app_set_channels(const uint16_t *v, uint16_t f) { memcpy(chans, v, sizeof chans); last_fade = f; }
uint16_t app_get_target(uint8_t ch) { return chans[ch]; }
void app_set_led_mode(uint8_t m) { led_mode = m; }
uint8_t app_get_led_mode(void) { return led_mode; }

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void send(uint8_t cmd, const uint8_t *p, int n)
{
    uint8_t f[300]; int i, k = 0; uint8_t s = 0;
    f[k++] = 0x55; f[k++] = 0xAA; f[k++] = 0x00; f[k++] = cmd;
    f[k++] = n >> 8; f[k++] = n & 0xFF;
    for (i = 0; i < n; i++) f[k++] = p[i];
    for (i = 0; i < k; i++) s += f[i];
    f[k++] = s;
    txn = 0;
    for (i = 0; i < k; i++) proto_rx(f[i], 100);
}

/* Parse exactly the frames in tx[]; return count, store first frame */
static int frames(uint8_t *cmd0, uint8_t *pl0, int *len0)
{
    int pos = 0, count = 0;
    while (pos < txn) {
        int i, len; uint8_t s = 0;
        CHECK(pos + 7 <= txn, "short frame");
        if (pos + 7 > txn) return count;
        CHECK(tx[pos] == 0x55 && tx[pos + 1] == 0xAA, "bad header");
        CHECK(tx[pos + 2] == 0x03, "version %d", tx[pos + 2]);
        len = (tx[pos + 4] << 8) | tx[pos + 5];
        for (i = 0; i < 6 + len; i++) s += tx[pos + i];
        CHECK(s == tx[pos + 6 + len], "checksum");
        if (count == 0) { *cmd0 = tx[pos + 3]; memcpy(pl0, &tx[pos + 6], len); *len0 = len; }
        pos += 7 + len; count++;
    }
    return count;
}

int main(void)
{
    uint8_t cmd, pl[300]; int len, n;
    proto_init();

    /* 1. heartbeat: first reply 0x00 (MCU restarted), then 0x01 */
    send(0x00, NULL, 0); n = frames(&cmd, pl, &len);
    CHECK(n == 1 && cmd == 0x00 && len == 1 && pl[0] == 0x00, "hb1");
    send(0x00, NULL, 0); n = frames(&cmd, pl, &len);
    CHECK(n == 1 && pl[0] == 0x01, "hb2");

    /* 2. product query: printable JSON */
    send(0x01, NULL, 0); n = frames(&cmd, pl, &len);
    pl[len] = 0;
    CHECK(n == 1 && cmd == 0x01 && pl[0] == '{', "product");
    printf("product: %s\n", (char *)pl);

    /* 3. conf query: empty -> ESPHome goes to WIFI_STATE mode */
    send(0x02, NULL, 0); n = frames(&cmd, pl, &len);
    CHECK(n == 1 && cmd == 0x02 && len == 0, "conf");

    /* 4. wifi state 0x03: must be answered with 0x03 */
    { uint8_t w = 0x03; send(0x03, &w, 1); }
    n = frames(&cmd, pl, &len);
    CHECK(n == 1 && cmd == 0x03 && len == 0 && proto_wifi_state == 3, "wifi");

    /* 5. dp query: one 0x07 with 101,102,103 */
    send(0x08, NULL, 0); n = frames(&cmd, pl, &len);
    CHECK(n == 1 && cmd == 0x07 && len == 14 + 5 + 5, "query len %d", len);
    CHECK(pl[0] == 101 && pl[1] == 0 && pl[3] == 10, "dp101 hdr");
    CHECK(pl[14] == 102 && pl[15] == 4 && pl[18] == 0, "dp102");
    CHECK(pl[19] == 103 && pl[20] == 4, "dp103");

    /* 6. deliver dp101 with fade */
    {
        uint8_t d[] = {101, 0, 0, 12, 0x11,0x22, 0x33,0x44, 0x55,0x66, 0x77,0x88, 0xFF,0xFF, 0x00,0x32};
        send(0x06, d, sizeof d); n = frames(&cmd, pl, &len);
        CHECK(n == 1 && cmd == 0x07 && len == 14, "deliver reply");
        CHECK(chans[0] == 0x1122 && chans[4] == 0xFFFF && last_fade == 50, "values %04x %04x %u", chans[0], chans[4], last_fade);
        CHECK(memcmp(pl + 4, d + 4, 10) == 0, "echo");
    }
    /* deliver without fade */
    {
        uint8_t d[] = {101, 0, 0, 10, 0,1, 0,2, 0,3, 0,4, 0,5};
        send(0x06, d, sizeof d); frames(&cmd, pl, &len);
        CHECK(chans[4] == 5 && last_fade == 0, "nofade");
    }
    /* led mode */
    {
        uint8_t d[] = {103, 4, 0, 1, 2};
        send(0x06, d, sizeof d); n = frames(&cmd, pl, &len);
        CHECK(n == 1 && cmd == 0x07 && len == 5 && pl[4] == 2 && led_mode == 2, "led");
    }
    /* unknown dp still gets a 0x07 */
    {
        uint8_t d[] = {20, 1, 0, 1, 1};
        send(0x06, d, sizeof d); n = frames(&cmd, pl, &len);
        CHECK(n == 1 && cmd == 0x07, "unknown dp answered");
    }
    /* bad checksum ignored */
    {
        uint8_t f[] = {0x55,0xAA,0,0,0,0,0x00};
        int i; txn = 0;
        for (i = 0; i < 7; i++) proto_rx(f[i], 200);
        CHECK(txn == 0, "bad cksum ignored");
    }
    /* garbage + resync on 55 55 AA */
    {
        uint8_t f[] = {0x12,0x55,0x55,0xAA,0x00,0x00,0x00,0x00,0xFF};
        int i; txn = 0;
        for (i = 0; i < 9; i++) proto_rx(f[i], 300);
        n = frames(&cmd, pl, &len);
        CHECK(n == 1 && cmd == 0x00, "resync");
    }
    /* oversize frame skipped without crash, next frame ok */
    {
        uint8_t big[200]; memset(big, 0xAB, sizeof big);
        send(0x06, big, sizeof big);
        CHECK(txn == 0, "oversize ignored");
        send(0x00, NULL, 0); n = frames(&cmd, pl, &len);
        CHECK(n == 1, "after oversize");
    }
    /* button report */
    txn = 0; proto_report_button(BTN_EV_POWER_SHORT); n = frames(&cmd, pl, &len);
    CHECK(n == 1 && cmd == 0x07 && pl[0] == 102 && pl[4] == 1, "button");

    /* partial frame dropped after gap */
    {
        txn = 0;
        proto_rx(0x55, 1000); proto_rx(0xAA, 1000); proto_rx(0x00, 1000);
        proto_poll(1100);
        send(0x00, NULL, 0); n = frames(&cmd, pl, &len);
        CHECK(n == 1, "gap reset");
    }

    printf(fails ? "%d FAILURES\n" : "all protocol tests passed\n", fails);
    return fails != 0;
}
