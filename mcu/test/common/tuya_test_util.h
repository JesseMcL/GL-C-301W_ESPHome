/* Shared helpers for the host unit tests (header-only). */
#ifndef TUYA_TEST_UTIL_H
#define TUYA_TEST_UTIL_H

#include <stdint.h>
#include <string.h>

/* Captured MCU -> module bytes */
static uint8_t  t_tx[8192];
static int      t_txn;

typedef struct {
    uint8_t cmd;
    uint8_t ver;
    uint16_t len;
    const uint8_t *data;
    int ok;            /* checksum ok */
} t_frame_t;

/* Parse the n-th frame from t_tx (0-based). Returns number of frames. */
static int t_frames(t_frame_t *out, int max)
{
    int pos = 0, n = 0;
    while (pos + 7 <= t_txn && n < max) {
        uint16_t len = (uint16_t)((t_tx[pos + 4] << 8) | t_tx[pos + 5]);
        uint8_t s = 0;
        int i;
        if (t_tx[pos] != 0x55 || t_tx[pos + 1] != 0xAA || pos + 7 + len > t_txn)
            break;
        for (i = 0; i < 6 + len; i++)
            s += t_tx[pos + i];
        out[n].ver = t_tx[pos + 2];
        out[n].cmd = t_tx[pos + 3];
        out[n].len = len;
        out[n].data = &t_tx[pos + 6];
        out[n].ok = s == t_tx[pos + 6 + len];
        n++;
        pos += 7 + len;
    }
    return n;
}

/* Build a module -> MCU frame (version 0) into buf; returns length. */
static int t_build(uint8_t *buf, uint8_t cmd, const uint8_t *p, int n)
{
    int i, k = 0;
    uint8_t s = 0;
    buf[k++] = 0x55; buf[k++] = 0xAA; buf[k++] = 0x00; buf[k++] = cmd;
    buf[k++] = (uint8_t)(n >> 8); buf[k++] = (uint8_t)n;
    for (i = 0; i < n; i++) buf[k++] = p[i];
    for (i = 0; i < k; i++) s += buf[i];
    buf[k++] = s;
    return k;
}

/* Find datapoint `id` inside a 0x07 payload. Returns pointer to its value
 * (and sets *len), or NULL. */
static const uint8_t *t_find_dp(const t_frame_t *f, uint8_t id, uint8_t *type, uint16_t *len)
{
    int pos = 0;
    while (pos + 4 <= f->len) {
        uint16_t l = (uint16_t)((f->data[pos + 2] << 8) | f->data[pos + 3]);
        if (f->data[pos] == id) {
            if (type) *type = f->data[pos + 1];
            if (len) *len = l;
            return &f->data[pos + 4];
        }
        pos += 4 + l;
    }
    return NULL;
}

#endif
