# GL-C-301W_ESPHome

Open firmware for the **Gledopto GL-C-301W** Wi-Fi 5-in-1 LED controller:

- **ESPHome** on the Tuya **T1-U-HL** Wi-Fi module (Beken BK7238, LibreTiny board `t1-u`).
- **Custom firmware** for the **Nuvoton MS51FC0AE** LED-driver MCU (8051, 32 KB flash). It has a TuyaMCU-compatible LDROM **bootloader**, so the MCU can be updated over the air from ESPHome.

```
 Home Assistant ──API── ESPHome on T1-U (BK7238)
                          │ tuya: + tuya_mcu (this repo)
                          │ UART1 P11/P10, 115200, TuyaMCU frames
                          ▼
                       MS51FC0AE
                          ├─ APROM: app - 5 × PWM @ 5.86 kHz/12 bit, fades,
                          │         buttons, RGB status LED, DP per channel
                          └─ LDROM: bootloader - Tuya MCU upgrade (0x0A/0x0B),
                                    CRC-checked app, rescue mode
```

> Status: the firmware builds, the host tests pass, and an end-to-end
> ESPHome ↔ MCU update was tested against a simulator (see [Verification](#verification)).
> The MS51 register-level code has **not run on hardware yet**.

---

## 1. Hardware (reverse engineered)

### T1-U-HL module

| Pin | Signal | Use |
|---|---|---|
| 1 | P14/SCK | mechanical only? |
| 13 | GND | |
| 14 | 3V3 | |
| 15 | P11 / TX1 | → MCU pin 3 (UART0 RXD) |
| 16 | P10 / RX1 | ← MCU pin 2 (UART0 TXD) |
| 21 | P15/SCL2 | mechanical only? |
| 2–7, 17–20 | NC | |

UART1 (P10/P11) is used both for flashing the T1 and for the MCU link.

### MS51FC0AE (TSSOP20)

| Pin | Port / function | Connected to |
|---|---|---|
| 1 | P0.5 | RF IC pin 16 |
| 2 | P0.6 UART0_TXD | T1-U pin 16 (RX) |
| 3 | P0.7 UART0_RXD | T1-U pin 15 (TX) |
| 4 | P2.0 / nRESET | PB-On/Off/Dimmer, TP-RST |
| 5 | P3.0 | RF IC pin 4 |
| 6 | P1.7 | RF IC pin 14 |
| 7 | VSS | |
| 8 | P1.6 ICE_DAT / I2C0_SDA / UART1_TXD | TP-SDA, **red** status LED |
| 9 | VDD | |
| 10 | P1.5 PWM0_CH5 | **green** status LED |
| 11 | P1.4 PWM0_CH1 (alt) | **blue** status LED |
| 12 | P1.3 | RF IC pin 13 |
| 13 | P1.2 PWM0_CH0 | Driver **W** |
| 14 | P1.1 PWM0_CH1 | Driver **C** |
| 15 | P1.0 PWM0_CH2 | Driver **B** |
| 16 | P0.0 PWM0_CH3 | Driver **G** |
| 17 | P0.1 PWM0_CH4 | Driver **R** |
| 18 | P0.2 ICE_CLK / I2C0_SCL / UART1_RXD | PB-Set, TP-SCL |
| 19 | P0.3 | RF IC pin 2 (3V3 pull-up) |
| 20 | P0.4 | RF IC pin 1 |

All five drivers sit on independent PWM0 channels CH0–CH4.

### 2.4 GHz RF IC (unmarked, 16 pins)

The RF IC has a 12 MHz crystal (pins 6/7), the antenna on pin 10, and VDD_PA-style decoupling on pins 8/11. Six signal lines go to the MCU (1, 2, 4, 13, 14, 16). This looks like an LT8900/PL1167-class transceiver (SPI + RESET + PKT), as used by the Gledopto RF remotes. **The remote is not supported yet.**

### Test pads

| Pad | MCU pin | Signal |
|---|---|---|
| TP-SDA | 8 | ICP DAT |
| TP-SCL | 18 | ICP CLK |
| TP-RST | 4 | nRESET |

## 2. Stock firmware findings

These come from the T1 flash dumps taken before and after pairing (ltchiptool), the T1 key-value storage, and the device's Tuya cloud schema.

- The stock T1 firmware is a Tuya "gfw" TuyaMCU bridge (SDK 3.0.22, built 2025-03), product key `pfwga9a37gkewrbj`. It supports MCU OTA (`_gfw_mcu_ota.c`).
- Stock datapoints:

  | DP | Function |
  |---|---|
  | 20 | `switch_led` |
  | 21 | `work_mode` (white/colour/scene/music) |
  | 22 | `bright_value` (10–1000) |
  | 23 | `temp_value` (0–1000) |
  | 24 | `colour_data` (HSV hex) |
  | 25 | `scene_data` |
  | 26 | `countdown` |
  | 27 | `music_data` |
  | 31 | `sleep_mode` |
  | 32 | `wakeup_mode` |
  | 33 | `power_memory` |
  | 34 | `do_not_disturb` |

- The MS51 ships **security-locked** with the **reset pin disabled** (CONFIG0.RPD = 0). Its firmware can't be read, and ICP entry only works by catching the chip at power-on.
- The dumps contain the Tuya local key and Wi-Fi credentials, so they are **not** part of this repo.

## 3. Process

### T1-U → ESPHome

1. Wire a USB-UART to T1 pins 15/16 (P11/P10), GND and 3V3. Hold CEN low or power-cycle to enter download mode.
2. Back up the flash with `ltchiptool` (full 2 MB, plus the efuse and ROM reads).
3. Flash the ESPHome `.uf2` with ltchiptool. After that, use OTA.

### MS51 → this firmware (ICP from a Raspberry Pi)

ICP uses a Raspberry Pi Zero (`pizero`) with Nikita Lita's [NuMicro-8051-prog](https://github.com/nikitalita/NuMicro-8051-prog) (`nuvo51icpy`):

| Pi GPIO | Board pad |
|---|---|
| 26 | TP-SCL (CLK) |
| 20 | TP-SDA (DAT) |
| 21 | TP-RST |
| GND | GND |

Power the board from its own supply. The Pi can't switch the board's power, so **power-cycle it when the tool says so**: `nuvo51icpy` keeps retrying entry at power-on.

First time, from `mcu/`:

```sh
pio run                       # builds app + bootloader
pio run -e app -t provision   # mass erase (unlocks, re-enables nRESET),
                              # LDROM bootloader + app + dev CONFIG
```

After provisioning, **update over the air from ESPHome**. ICP is only needed for recovery or for bootloader changes.

## 4. Repository layout

```
mcu/                      PlatformIO project (platform intel_mcs51, board MS51FC0AE)
  include/                config.h (board guesses), layout.h (flash map), tuya_dp.h (DPs)
  src/app/                application: main.c (hardware), proto.c (TuyaMCU)
  src/boot/               bootloader: boot_main.c (hardware), boot_core.c (protocol/update)
  scripts/                bsp.py (fetch Nuvoton headers), version.py, fwimage.py (images,
                          manifest), icp.py (PlatformIO ICP targets), pi_flash.py (runs on the Pi)
  config/                 CONFIG byte profiles for ICP (dev / release)
  test/                   Unity host tests (pio test -e native)
  sim/                    MCU simulator (real protocol + bootloader core on a pty)
esphome/
  bathroom-mirror-1.yaml  the device
  stock-mcu-update.yaml   experiment: update a stock MCU through its Tuya OTA
  components/tuya_mcu/    external component: paced DP outputs + MCU update entity
  test/                   host integration test (ESPHome host build ↔ simulator)
.github/workflows/        CI + tagged releases
```

## 5. MCU firmware

### Build and test

```sh
cd mcu
pio run                     # → dist/: *.ota.bin, *.icp.bin, *.app.hex, boot *.bin/.hex, manifest.json
pio test -e native          # protocol + bootloader tests on the host
```

`scripts/bsp.py` sparse-checks-out Nuvoton's [MS51 BSP](https://github.com/OpenNuvoton/MS51_BSP) headers (Apache-2.0) at a pinned commit into `mcu/.bsp/`. They are not vendored.

The version comes from git: tag `v1.2.3` gives `1.2.3`, otherwise `dev-<sha>`. The `FW_VERSION` environment variable overrides it (15 characters max).

### Flash map (3 KB LDROM)

| Range | Content |
|---|---|
| 0x0000 | app code, then a 32-byte footer: `GLCW`, length, CRC-16/CCITT, version |
| 0x7380–0x73FF | validity record: copy of the footer header. Written by the bootloader only after the CRC checks out, or present in the ICP image |
| 0x7400–0x7FFF | LDROM bootloader (2.4 KB used) |
| XRAM 0x7F0 | app → bootloader hand-off (update size) across the software reset |

### Datapoints

| DP | Type | Dir | Meaning |
|---|---|---|---|
| 101–105 | value | rw | R, G, B, C, W duty, linear 0–65535 |
| 106 | value | rw | fade time per change in ms (default 60) |
| 107 | bool | ro | power button pressed |
| 108 | bool | ro | set button pressed |
| 109 | enum | rw | status LED mode: 0 auto, 1 manual |
| 110 / 111 / 112 | bool | rw | red / green / blue status LED (manual mode) |
| 120 | string | ro | firmware version. The bootloader reports `0.0.0` |

**Status LED in auto mode:**

| LED pattern | Meaning |
|---|---|
| Green blink, about 1 Hz | no Wi-Fi |
| Blue blink, about 4 Hz | AP/pairing mode |
| Red blip every 2 s | no contact with the T1 for 40 s |
| Dark | connected |

When the T1 is unreachable, a short press of the power button toggles the light locally.

**Bootloader LEDs:**

| LED pattern | Meaning |
|---|---|
| Blue | waiting for update data |
| Fast blue | receiving |
| Red blink | rescue (no valid app) |
| Solid red | write or CRC error |

### Update flow

1. ESPHome sends `0x0A [size]`.
2. The app stores the size in XRAM and software-resets into LDROM.
3. The bootloader sees `AUXR1.SWRF` and replies `0x0A [0x00 = 256-byte packets]`.
4. Each `0x0B [offset][256 B]` packet is written and verified, then acknowledged. The first packet erases the validity record, so the old app counts as invalid from that point.
5. The end packet triggers a check of the footer and CRC. The bootloader then writes the record and resets into the new app. The app reports its version on DP 120.
6. If the update is interrupted or bad, the chip stays in the bootloader (rescue mode). Rescue mode still answers TuyaMCU init and reports `0.0.0`, so ESPHome offers the update again. If the app asks for an update but nothing arrives within 15 s, the bootloader goes back to the old app.

### CONFIG profiles (`mcu/config/`)

| Profile | CONFIG0–4 | Meaning |
|---|---|---|
| `dev.json` | 7F FC FF FF FF | boot from LDROM (3 KB), reset pin enabled, unlocked, WDT off |
| `release.json` | 7B FC FF FF 5F | as dev, plus P2.0 as GPIO (power button works) and WDT reset on |

`pio run -e app -t icp_release` applies the release profile. After that, ICP needs power-on entry again, which is fine because updates go over the air.

### ICP targets (via `ssh pizero`)

| Command | Does |
|---|---|
| `pio run -e app -t provision` | mass erase, LDROM, APROM, dev CONFIG |
| `pio run -e app -t upload` | APROM only |
| `pio run -e boot -t upload` | LDROM + dev CONFIG |
| `pio run -e app -t icp_status` | device info and CONFIG |
| `pio run -e app -t icp_release` | release CONFIG |

Set `ICP_HOST` (default `raspberrypi`), `ICP_USER` (default `pi`; set it empty to use your `~/.ssh/config` or login name), `ICP_PYTHON` (Python on the Pi with nuvoprogpy installed) and `ICP_DIR` as needed. `scripts/pi_flash.py` is copied to the Pi and does the work there.

## 6. ESPHome

`esphome/bathroom-mirror-1.yaml` uses the external component `tuya_mcu`:

- **`output: platform: tuya_mcu`** gives one float output per channel DP, so a normal `cwww`/`rgbww` light can use them. Writes are coalesced and paced (`min_interval`, oldest first), so a 5-channel transition can't flood the request/response UART. The MCU fades between updates using DP 106.
- **`update: platform: tuya_mcu`** is an **MCU firmware** update entity in Home Assistant. It compares DP 120 with the bundled release and installs it with the standard Tuya MCU upgrade protocol, showing progress. The new version is verified after the MCU reboots.
- Buttons are `binary_sensor: tuya` entities (single click toggles, a long press switches between 100 % and 15 %, Set cycles colour temperature). The status LED uses `select`/`switch: tuya`, and the MCU version is a `text_sensor`.

**Firmware source.** `http_request` isn't available on BK72xx, so the update entity can't download at runtime. Instead, the image is fetched **when ESPHome compiles**, from `mcu_firmware`:

- The default is `https://github.com/JesseMcL/GL-C-301W_ESPHome/releases/latest/download/manifest.json`. ESPHome checks the GitHub release page at build time, and the device offers the update when the MCU's version differs.
- Before the first release exists, use `../mcu/dist/manifest.json`, or `esphome -s mcu_firmware ../mcu/dist/manifest.json run …`. If the URL can't be fetched, the build continues without bundled firmware and logs a warning.
- To roll out a new MCU release: tag it, then rebuild and OTA ESPHome, then press Install.

The ESPHome version of `tuya:` only resends WIFI_STATE when the Wi-Fi status changes. After an MCU restart its re-init would stall at that step. The MCU firmware sends an unsolicited WIFI_STATE ack and queries `0x2B` to work around this, and the update component pokes a heartbeat after flashing so the re-init happens immediately.

## 7. Experiment: update a stock MCU over the air

Catching the locked MS51 at power-on is a pain. The alternative is to let the stock Gledopto firmware's own Tuya MCU OTA install this firmware: flash `esphome/stock-mcu-update.yaml` to the T1 and follow the steps at the top of that file.

- It drives the light with the stock DPs, runs the UART at 9600 (the likely stock rate), logs at VERBOSE, and offers **MCU firmware**. The stock MCU reports no DP 120, so the version shows as unknown and the update counts as available.
- What's sent is `gl-c-301w-mcu-<ver>.ota.bin`: the raw APROM application plus a 32-byte footer. The app doesn't depend on this repo's bootloader. Under a stock LDROM bootloader, a later `0x0A` resets into *that* bootloader, which may handle updates itself.
- Unknowns: whether the stock updater accepts the image, where it writes it, and whether it checks anything. If it fails, the MCU may need ICP recovery (`provision`). On success the new app talks at 115200, so switch to `bathroom-mirror-1.yaml`.

## 8. Releases and CI

| Workflow | Runs on | Does |
|---|---|---|
| `mcu.yml` | every push/PR | host tests and build, with artifacts |
| `mcu.yml` | tags `v*` | GitHub release with `*.ota.bin`, `*.icp.bin`, hex files, bootloader, `manifest.json` (with release URL), CONFIG profiles and `pi_flash.py` |
| `esphome.yml` | every push/PR | builds the MCU firmware, then validates and compiles the BK7238 config with that firmware bundled |

To release:

```sh
git tag v1.0.0 && git push origin v1.0.0
```

## 9. Verification

Done:

- **SDCC 4.2 via PlatformIO:** app 5.1 KB of 29.5 KB, bootloader 2.4 KB of 3 KB.
- **Host tests (Unity), 21 cases:** TuyaMCU init, every DP, multi-DP frames, clamping, framing errors, and the OTA hand-off. For the bootloader against simulated NOR flash: full update, update from rescue, retransmits, corrupted image, verify failure, size limits, wait timeout, rescue never times out, and the largest possible image.
- **End-to-end** (`esphome/test/run.sh`):
  - ESPHome 2026.10 (host build of this YAML with the real `tuya:` and `tuya_mcu`) talks over a pty to `mcu/sim`, which runs the real `proto.c` and `boot_core.c`.
  - The light transitions arrive as paced DP writes. The MCU update goes 1.0.0 → 1.1.0 and is verified in about 1 s. A corrupted app (rescue mode) is recovered. A button press toggles the light.
- **ESPHome build for BK7238** (board `t1-u`) with this component compiles.
- **PlatformIO ICP targets** produce the expected ssh/scp/`pi_flash.py` calls (tested with a dummy ssh).

Not done yet (needs hardware):

- MS51 clock trim, PWM, UART/timer, IAP and CONFIG on silicon.
- The real BK7238 UART.
- The stock-MCU experiment.

## 10. Open items

- RF remote (2.4 GHz IC driver and protocol).
- Confirm LED polarity (`LED_ACTIVE_LOW`), button polarity, and PWM polarity (`PWM_INVERT_MASK`).
- Optional: persist the last state in MCU data flash, so the light comes back without waiting for ESPHome.
