/* Simulated MCU for integration testing against a real ESPHome (host build).
 * Runs proto.c on a serial device / pty and logs what ESPHome sends.
 *   mcu_sim /dev/pts/N [button_event_at_ms ev]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include "../src/proto.h"

static int fd;
static uint16_t tgt[NUM_CH];
static uint8_t led_mode;
static unsigned frames101;

void hal_uart_put(uint8_t b) { if (write(fd, &b, 1) != 1) perror("write"); }
void app_set_channels(const uint16_t *v, uint16_t fade)
{
    memcpy(tgt, v, sizeof tgt);
    frames101++;
    printf("DP101 #%u R=%5u G=%5u B=%5u C=%5u W=%5u fade=%u\n",
           frames101, v[0], v[1], v[2], v[3], v[4], fade);
    fflush(stdout);
}
uint16_t app_get_target(uint8_t ch) { return tgt[ch]; }
void app_set_led_mode(uint8_t m) { led_mode = m; printf("LED mode %u\n", m); fflush(stdout); }
uint8_t app_get_led_mode(void) { return led_mode; }

static uint16_t ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint16_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    struct termios tio;
    uint8_t b, last_wifi = 0xEE;
    long btn_at = argc > 3 ? atol(argv[2]) : -1;
    int btn_ev = argc > 3 ? atoi(argv[3]) : 0;
    struct timespec t0, t;

    fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return 1; }
    tcgetattr(fd, &tio); cfmakeraw(&tio); tcsetattr(fd, TCSANOW, &tio);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    proto_init();
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        while (read(fd, &b, 1) == 1)
            proto_rx(b, ms());
        proto_poll(ms());
        if (proto_wifi_state != last_wifi) {
            last_wifi = proto_wifi_state;
            printf("WIFI_STATE %u\n", last_wifi); fflush(stdout);
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (btn_at >= 0 && (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > btn_at) {
            printf("pressing button ev %d\n", btn_ev); fflush(stdout);
            proto_report_button((uint8_t)btn_ev);
            btn_at = -1;
        }
        usleep(500);
    }
}
