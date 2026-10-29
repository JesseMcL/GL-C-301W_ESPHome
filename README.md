# Gledopto GL-C-301W: ESPHome + custom MS51 firmware

Custom firmware for the Gledopto GL-C-301W 5-in-1 Wi-Fi LED controller:

- **Wi-Fi module:** a Tuya T1-U-HL (BK7238) running ESPHome. LibreTiny board `t1-u`.
- **LED MCU:** a Nuvoton MS51FC0AE (8051 core, 32 KB flash), replaced with a small open firmware that makes it a 5-channel PWM slave.

## Stock architecture

These findings come from the T1 flash dumps and `storagedata.json`.

- Stock firmware is a Tuya "gfw" TuyaMCU setup: product key `pfwga9a37gkewrbj`, firmware 3.0.22, built 2025-03-05.
- The T1 is only a Wi-Fi bridge. The MS51 drives the LEDs and talks to the T1 using the Tuya serial protocol. The T1 also supports updating the MCU firmware over UART.
- Stock data points:

  | DP | Function |
  |----|----------|
  | 20 | Power |
  | 21 | Mode (white, colour, scene, music) |
  | 22 | Brightness, 10–1000 |
  | 23 | Colour temperature, 0–1000 |
  | 24 | Colour as an HSV hex string, `HHHHSSSSVVVV` |
  | 25 | Scene |
  | 26 | Countdown |
  | 27 | Music |
  | 31 | Sleep |
  | 32 | Wake-up |
  | 33 | Power-on memory |
  | 34 | Do not disturb |

- If you only want ESPHome with the stock MCU, use the `tuya:` component with `light: platform: tuya` on DPs 20/22/23/24. Nothing on the MCU needs to change. The custom firmware below is for full control: ESPHome handles gamma, transitions and colour, there's local fallback, and nothing depends on Tuya.

## New architecture

```
ESPHome (T1-U, BK7238)  --UART1 P11/P10, 115200, TuyaMCU frames-->  MS51FC0AE
  light (cwww / rgbww)                                                PWM0 CH0..4 @ 5.86 kHz, 12-bit
  template outputs -> DP 101 every 50 ms                              linear fade between frames
  buttons <- DP 102                                                   buttons, status LED, link watchdog
```

| DP  | Type | Direction | Meaning |
|-----|------|-----------|---------|
| 101 | raw  | both      | R,G,B,C,W as uint16 big-endian (0..65535 linear duty) + optional uint16 fade time in ms |
| 102 | enum | MCU → T1  | button: 1 power short, 2 power long, 3 set short, 4 set long (0 = none) |
| 103 | enum | both      | status LED: 0 auto (Wi-Fi state), 1 off, 2 on |

The framing is standard TuyaMCU, so the stock ESPHome `tuya:` component talks to it unchanged. The MCU:

- answers heartbeat, product, configuration, Wi-Fi state, data point delivery and data point query
- always replies 0x07 to a 0x06, because ESPHome waits for it
- keeps the outputs where they are if the Wi-Fi module goes quiet
- after 40 s with no heartbeat, makes a short press of the power button toggle the light locally

## Pin map

The pin map comes from your reverse engineering.

| MCU pin | Port | Function | Goes to |
|---|---|---|---|
| 13 | P1.2 | PWM0_CH0 | W driver |
| 14 | P1.1 | PWM0_CH1 | C driver |
| 15 | P1.0 | PWM0_CH2 | B driver |
| 16 | P0.0 | PWM0_CH3 | G driver |
| 17 | P0.1 | PWM0_CH4 | R driver |
| 2 | P0.6 | UART0 TXD | T1 pin 16 (P10/RX1) |
| 3 | P0.7 | UART0 RXD | T1 pin 15 (P11/TX1) |
| 18 | P0.2 | ICP CLK | PB-Set, TP-SCL |
| 8 | P1.6 | ICP DAT | TP-SDA, an LED (left untouched) |
| 4 | P2.0 | nRESET | PB-Power/Dimmer, TP-RST |
| 10 | P1.5 | — | status LED |
| 11 | P1.4 | — | aux LED (held off) |
| 1,5,6,12,19,20 | — | — | 2.4 GHz RF IC (left as inputs) |

## Repository layout

```
mcu/src/main.c          MS51 hardware: clock, PWM, UART ISR, fades, buttons, LED, WDT
mcu/src/proto.c/.h      TuyaMCU protocol (hardware-independent)
mcu/src/config.h        polarities, PWM frequency, fallback levels  <- edit this
mcu/test/test_proto.c   host unit test (make test)
mcu/test/mcu_sim.c      simulated MCU for testing against a real ESPHome host build
mcu/build/*.hex|.bin    prebuilt image (default config)
esphome/bathroom-mirror-1.yaml
```

## 1. Build the MCU firmware

```sh
cd mcu
./get_bsp.sh            # Nuvoton MS51 BSP (Apache-2.0), register headers only
make                    # needs SDCC (4.x): build/gl_c_301w_ms51.hex / .bin  (~4.3 KB)
make test               # protocol unit tests
```

On Windows, use WSL (`sudo apt install sdcc make git`) or SDCC for Windows with MSYS2.

## 2. Program the MS51 (ICP)

> **Point of no return:** the stock MCU firmware is almost certainly read-locked. Once you erase the chip, you can't go back to the stock Tuya behaviour unless the read in step 3 succeeds. The T1 dumps you already have don't contain the MCU firmware.

**Hardware.** Use a Nuvoton Nu-Link2-Me, which you can snap off any NuTiny/NuMaker MS51 board, or another Nu-Link. Connect it to the test pads:

| Nu-Link | Board pad | MCU pin |
|---|---|---|
| ICE_DAT | TP-SDA | 8 (P1.6) |
| ICE_CLK | TP-SCL | 18 (P0.2) |
| nRESET  | TP-RST | 4 (P2.0) |
| GND     | GND | 7 |
| VDD     | 3V3 | 9 |

**Power.** Power the board from its normal 12/24 V input with the LED strip disconnected, and set the Nu-Link not to supply power. Powering from the Nu-Link 3.3 V would also have to feed the T1-U and the RF chip, which usually fails ICP entry.

**Software.** Use the NuMicro ICP Programming Tool (Windows), chip series "8051 1T", target MS51FC0AE.

1. Connect, then **read and save the CONFIG bytes**. They show how Gledopto configured the chip: reset pin, LDROM (bootloader) size and watchdog.
2. Try **Read APROM** (and LDROM). If it returns all `FF` or an error, the chip is locked, which is expected.
3. If the read succeeds, save it. That's your only stock MCU backup.
4. Set CONFIG for the first flash:
   - Boot from APROM (CBS)
   - LDROM size: 0 KB
   - Security lock: **unlocked**
   - Reset pin (P2.0): **enabled** for now
   - WDT: disabled for now
   - BOD: default
5. Erase the whole chip, then program `build/gl_c_301w_ms51.hex` to APROM. If you use the `.bin`, program it at address 0x0000.

**After bring-up works**, change two CONFIG settings:

- **Disable the reset pin (P2.0 as GPIO)** so the power/dimmer button works. While reset is enabled, pressing it just resets the MCU, which is harmless. With reset disabled, ICP entry may need you to power-cycle the board when the tool asks, so keep the reset pin enabled until the firmware is final.
- **Enable WDT reset in CONFIG4.** The firmware already sets a prescaler of about 1.6 s and resets the watchdog every 100 ms. Nuvoton recommends enabling WDT reset through ICP rather than from code.

## 3. ESPHome on the T1-U

Use `esphome/bathroom-mirror-1.yaml` with your own `secrets.yaml`. It needs these keys: `wifi_ssid`, `wifi_password`, `fallback_ap_password`, `api_encryption_key`. OTA reuses the API encryption key.

- **UART:** `tx_pin: P11`, `rx_pin: P10`, 115200 baud. This is UART1, which you also use for flashing, so logging is set to `baud_rate: 0`.
- **Light:** the YAML assumes a CCT (warm and cool white) mirror strip and uses a `cwww` light on C/W. For an RGB+CCT strip, swap in the commented `rgbww` block.
- **Buttons:**
  - Power short press toggles the light.
  - Power long press switches between 100 % and 15 %.
  - Set short press cycles warm, neutral and cool.
  - Every press also fires the `Button` event entity, so you can build automations in Home Assistant.
- **Status LED:** auto mode blinks at about 1 Hz when there's no Wi-Fi, blinks at about 4 Hz in AP/pairing mode, and stays off once connected. A short blip every 2 s means the link to the T1 is dead.

Flash it the way you did before (ltchiptool UART with the `.uf2`, or OTA).

## 4. Bring-up checklist

1. Temporarily set `logs: tuya: DEBUG` and watch the log over Wi-Fi (`esphome logs` or the Home Assistant log viewer). Serial logging isn't available because UART1 belongs to the MCU.
2. You should see:
   - `MCU restarted`
   - `Product: '{"p":"gl_c_301w_pwm","v":"1.0.0","m":0}'`
   - data points 101, 102 and 103 in the dump
3. Turn on each channel in turn with a test strip. If a colour lands on the wrong channel, swap the outputs in the YAML.
4. If a channel is inverted (on when it should be off), set its bit in `PWM_INVERT_MASK` in `config.h` and rebuild.
5. If the status LED shows the opposite of what it should, flip `STATUS_LED_ACTIVE_LOW`.
6. If a button doesn't register, check whether it's active-high; the firmware assumes active-low.

## Verification done

- The MCU firmware compiles cleanly with SDCC 4.2: 4.3 KB code, 350 B XRAM, 150 B of stack free.
- The protocol layer passes the host unit tests. They cover the init sequence, data point delivery with and without fade, LED mode, unknown data points, bad checksums, resync, oversized frames, gap timeout and button reports.
- **End-to-end test against real ESPHome 2026.10.0.** The test used a host build of this YAML with the identical `tuya:` component and lambdas, connected over a pty to `mcu_sim`, which runs the same `proto.c`. Results:
  - Init completes.
  - The LED mode is pushed on init.
  - A 1 s transition to 2700 K arrives as about 20 data point 101 frames ramping W from 0 to 65535 with gamma applied.
  - Instant off works.
  - The set button cycles to 4000 K, giving C+W of 36432+29103, which is constant brightness.
  - The power button toggles the light back on.
- **Not tested on hardware:** the MS51-specific register code (clock trim, PWM, UART/timer ISRs, GPIO). It follows Nuvoton's BSP samples for this chip, but it hasn't run on silicon. If anything misbehaves, ICP can always reflash.

## Open items

- **The 2.4 GHz RF IC isn't supported**, so the Gledopto RF remote won't work. It has a 12 MHz crystal and six signal lines to the MCU, which looks like an LT8900/PL1167-class transceiver on bit-banged SPI plus reset and packet flag. Supporting it means sniffing the remote's protocol and adding a driver. The firmware has plenty of room for it.
- Polarities of the indicator LEDs and the P1.6 LED are unconfirmed.
- Nothing is persisted in MCU flash. Power-on state comes from ESPHome `restore_mode`, and the outputs stay dark for the roughly 1–2 s the T1 takes to boot.
