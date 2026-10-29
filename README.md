# GL-C-301W_ESPHome

Open firmware for the **Gledopto GL-C-301W** Wi-Fi 5-in-1 LED controller:

- **ESPHome** on the Tuya **T1-U-HL** Wi-Fi module (Beken BK7238, LibreTiny board `t1-u`).
- **Custom firmware** for the **Nuvoton MS51FC0AE** LED-driver MCU (8051, 32 KB flash). It has a TuyaMCU-compatible LDROM **bootloader**, so the MCU can be updated over the air from ESPHome.

```
 Home Assistant ──API── ESPHome on T1-U (BK7238)
                          │ tuya: + tuya_mcu (this repo) + tuya_mcu_dfu
                          │ UART1 P11/P10, 115200, TuyaMCU frames
                          ▼
                       MS51FC0AE
                          ├─ APROM: app - 5 × PWM 600 Hz–8 kHz, fades,
                          │         buttons, RGB status LED, DP per channel,
                          │         settings saved in flash
                          └─ LDROM: bootloader - Tuya MCU upgrade (0x0A/0x0B),
                                    CRC-checked app, rescue mode
```

> Status: ESPHome and the MS51 firmware run on the device, and MCU updates
> from ESPHome through the LDROM bootloader work on hardware, using
> [esphome_tuya_mcu_dfu](https://github.com/JesseMcL/esphome_tuya_mcu_dfu).
> Not done: replacing the **stock** MCU firmware over the air (§7), and the
> 2.4 GHz RF remote. See [Verification](#verification).

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

### 2.4 GHz RF IC (unmarked, SSOP16)

The RF IC has a 12 MHz crystal (pins 6/7), the antenna on pin 10, and VDD_PA-style decoupling on pins 8/11. Six signal lines go to the MCU (1, 2, 4, 13, 14, 16). This looks like an LT8900-class transceiver (SPI + RESET + PKT), as used by the Gledopto RF remotes. **The remote is not supported yet.**

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

- The stock MCU talks TuyaMCU at **9600 baud**. The app shows MCU v1.0.0 and offers no MCU update, so there's no official MCU image to obtain.
- The MS51 ships **security-locked** with the **reset pin disabled** (CONFIG0.RPD = 0). Its firmware can't be read, and ICP entry only works by catching the chip at power-on.
- The dumps contain the Tuya local key and Wi-Fi credentials, so they are **not** part of this repo.

### Stock functions (from the Gledopto manual)

The [GL-C-301W manual](https://manuals.plus/gledopto/gl-c-301w-ultra-mini-wi-fi-5-inch-1-tuya-led-smart-controller-manual) describes these controller-side functions. This firmware implements them the same way, except the strip type, and every setting is also a datapoint:

| Function | Stock | Here |
|---|---|---|
| Set, 1 click | next "device function" (strip type): RGBCCT → RGBW → RGB → CCT → Dimmer. The status LED shows it: white, yellow, blue, green, red | **not implemented**: the strip type is the light in the ESPHome YAML (an ESPHome light's capabilities are fixed at compile time). The click is free for automations (DP 108) |
| Set, 2 clicks | next PWM frequency: 1000 → 2000 → 4000 → 8000 → 600 → 800 Hz (default 1000). The LED flashes purple once per step (600 Hz = 1 … 8 kHz = 6), then stays off for 2 s | same, DP 113. The manual leaves the 600 Hz count blank; 1 is assumed |
| Set, 3 clicks | power-on memory on/off (default off): restore on/off after a power cut. The strip blinks 3 times | same, DP 114. The restore itself is done by ESPHome (see §6) |
| Set, hold 3 s | reset to defaults (RGBCCT); the strip flashes once | same (without the strip type), DP 115. Wi-Fi is not touched |
| Power, click | on/off | ESPHome; the MCU itself when the T1 is unreachable |
| Power, hold | dim; each hold reverses the direction | same |
| 2.4 GHz RF remote | pairing at power-on | **not supported** |

Stock wiring per strip type (terminals, top to bottom: W C B G R). Here every terminal is simply its own channel, so wire the strip to the channels the ESPHome light uses:

| Type | W | C | B | G | R |
|---|---|---|---|---|---|
| RGBCCT | W | C | B | G | R |
| RGBW | W | unused | B | G | R |
| RGB | unused | unused | B | G | R |
| CCT | W | C | (stock: 2nd strip W) | (stock: 2nd strip C) | unused |
| Dimmer | all five strips, driven together in stock |

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
  src/app/                application: main.c (hardware), proto.c (TuyaMCU),
                          logic.c (settings log, PWM frequencies, Set gestures)
  src/boot/               bootloader: boot_main.c (hardware), boot_core.c (protocol/update)
  scripts/                bsp.py (fetch Nuvoton headers), version.py, fwimage.py (images,
                          manifest), icp.py (PlatformIO ICP targets), pi_flash.py (runs on the Pi)
  config/                 CONFIG byte profiles for ICP (dev / release)
  test/                   Unity host tests (pio test -e native)
  sim/                    MCU simulator (real protocol + bootloader core on a pty)
esphome/
  gl-c-301w.yaml          the device
  stock-mcu-update.yaml   experiment: update a stock MCU through its Tuya OTA
  components/tuya_mcu/    external component: paced DP outputs
  test/                   host integration test (ESPHome host build ↔ simulator)
.github/workflows/        CI + tagged releases
```

## 5. MCU firmware

### Build and test

```sh
cd mcu
pio run                     # → dist/: *.ota.bin, *.icp.bin, *.app.hex, boot *.bin/.hex, manifest.json
pio test -e native          # protocol, logic and bootloader tests on the host
```

`scripts/bsp.py` sparse-checks-out Nuvoton's [MS51 BSP](https://github.com/OpenNuvoton/MS51_BSP) headers (Apache-2.0) at a pinned commit into `mcu/.bsp/`. They are not vendored.

The version comes from git: tag `v1.2.3` gives `1.2.3`, otherwise `dev-<sha>`. The `FW_VERSION` environment variable overrides it (15 characters max).

### Flash map (3 KB LDROM)

| Range | Content |
|---|---|
| 0x0000 | app code, then a 32-byte footer: `GLCW`, length, CRC-16/CCITT, version |
| 0x7300–0x737F | settings log: 8-byte records (PWM frequency, power-on memory, LED mode, fade), appended, newest valid wins; one erase per 16 saves. Never touched by updates |
| 0x7380–0x73FF | validity record: copy of the footer header. Written by the bootloader only after the CRC checks out, or present in the ICP image |
| 0x7400–0x7FFF | LDROM bootloader (2.4 KB used) |
| XRAM 0x7F0 | app → bootloader hand-off (update size) across the software reset |

### Datapoints

| DP | Type | Dir | Meaning |
|---|---|---|---|
| 101–105 | value | rw | R, G, B, C, W duty, linear 0–65535 |
| 106 | value | rw | fade time per change in ms (default 60) · saved |
| 107 | bool | ro | power button pressed |
| 108 | bool | ro | set button pressed |
| 109 | enum | rw | status LED mode: 0 auto, 1 manual · saved |
| 110 / 111 / 112 | bool | rw | red / green / blue status LED (manual mode) |
| 113 | enum | rw | PWM frequency: 0 600, 1 800, 2 1000, 3 2000, 4 4000, 5 8000 Hz · saved |
| 114 | bool | rw | power-on memory · saved |
| 115 | bool | w | 1 = reset the settings above to defaults (always reads 0) |
| 120 | string | ro | firmware version. The bootloader reports `0.0.0` |

The version is also in the product information (`0x01` reply, `"v"`), which is what the DFU component checks first after an update. The bootloader answers with `0.0.0-dev`.

"Saved" settings are written to the settings page 2 s after the last change, while the UART is idle (a flash erase stalls the CPU for a few ms).

**PWM frequency.** 600 Hz–8 kHz as in the stock firmware, default 1 kHz (24000 counts, about 14.5 bits). At low duty the MOSFET gate drive needs a few µs to switch on. Lower frequencies make each pulse longer, so dim levels become visible. Higher frequencies can quieten whining power supplies, but with a bigger dead zone at the bottom.

**Status LED:**

| LED pattern | Meaning |
|---|---|
| Dark (auto mode) | connected |
| Purple flashes, then 2 s dark | PWM frequency changed (flash count = step) |
| Green blink, about 1 Hz | no Wi-Fi |
| Blue blink, about 4 Hz | AP/pairing mode |
| Red blip every 2 s | no contact with the T1 for 40 s |

When the T1 is unreachable, the MCU handles the power button itself: click toggles, hold dims (alternating direction).

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
5. The end packet triggers a check of the footer and CRC. The bootloader then writes the record and resets into the new app.
6. ESPHome asks for product information (`0x01`) until the new app answers with the new version. It falls back to DP 120 once `tuya:` has re-initialised.
7. If the update is interrupted or bad, the chip stays in the bootloader (rescue mode). Rescue mode still answers TuyaMCU init and reports `0.0.0`, so ESPHome offers the update again. If the app asks for an update but nothing arrives within 15 s, the bootloader goes back to the old app.

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

Set `ICP_HOST` (default `raspberrypi`), `ICP_USER` (default `pi`; set it empty to use your `~/.ssh/config` or login name), `ICP_PYTHON` (Python on the Pi with nuvoprogpy installed), `ICP_DIR` and `ICP_LIBRARY` (`pigpio` or `gpiod`; only passed to nuvo51icpy when set) as needed. `scripts/pi_flash.py` is copied to the Pi and does the work there.

## 6. ESPHome

`esphome/gl-c-301w.yaml` is the whole device. The light is an RGBCCT (`rgbww`) light; for other strips change its platform and keep `id: rgbcw_light` (the comment above the light lists the options). The `min_power` substitution maps 0 % to the level where the strip just starts to glow. The YAML uses ESPHome actions only, no lambdas.

Both external components come from GitHub. When working on `tuya_mcu` locally, switch to the commented `type: local` source in the YAML.

Two external components:

- **`output: platform: tuya_mcu`** gives one float output per channel DP, so a normal `cwww`/`rgbww` light can use them. Writes are coalesced and paced (`min_interval`, oldest first), so a 5-channel transition can't flood the request/response UART. The MCU fades between updates using DP 106. Writes are held while `tuya:` is suspended for an MCU update.
- **`update: platform: tuya_mcu_dfu`** from [esphome_tuya_mcu_dfu](https://github.com/JesseMcL/esphome_tuya_mcu_dfu) is the **MCU firmware** update entity in Home Assistant. It lives in its own repo and works with any TuyaMCU device.
  - It shows the MCU's version from DP 120 and installs the bundled release with the standard Tuya MCU upgrade protocol, showing progress.
  - After the MCU reboots it checks the new version in the product information, then on DP 120.
  - The YAML pulls it from GitHub (`@main`); pin a tag or commit there for reproducible builds.
- Buttons are `binary_sensor: tuya` entities. Power: click → `light.toggle`; hold → a script repeats `light.dim_relative` (±3 % every 50 ms) while the button is down, reversing direction on each hold. The Set button is handled in the MCU and is only reported.
- Settings: **PWM frequency** and **Status LED mode** are `select: tuya`; **Power-on memory** is a `switch: tuya`; **Reset controller settings** is a template button that writes DP 115. They follow changes made with the Set button.
- **Power-on memory.** The light uses `restore_mode: RESTORE_DEFAULT_OFF`, which restores the state from before a reboot. With memory off (the stock default), the MCU reports DP 114 = off during the start-up handshake. That fires `on_turn_off` inside a 30 s boot window, which turns the light on. Note that an ESPHome OTA reboot counts as a power-up here too.

**Firmware source.** `tuya_mcu_dfu` can download at runtime with `http_request` (`fetch: runtime`), but `http_request` isn't available on BK72xx. This YAML therefore uses `fetch: build`: the image is fetched **when ESPHome compiles**, from `mcu_firmware`.

- The default is `https://github.com/JesseMcL/GL-C-301W_ESPHome/releases/latest/download/manifest.json`. ESPHome checks the GitHub release page at build time, and the device offers the update when the MCU's version differs.
- For a local MCU build, use `../mcu/dist/manifest.json` (commented in the YAML), or `esphome -s mcu_firmware ../mcu/dist/manifest.json run …`. If the source can't be fetched, the build continues without bundled firmware and logs a warning.
- To roll out a new MCU release: tag it, then rebuild and OTA ESPHome, then press Install.

The ESPHome version of `tuya:` only resends WIFI_STATE when the Wi-Fi status changes. After an MCU restart its re-init would stall at that step. The MCU firmware sends an unsolicited WIFI_STATE ack and queries `0x2B` to work around this, and the update component pokes a heartbeat after flashing so the re-init happens immediately.

## 7. Experiment: update a stock MCU over the air (untested)

Catching the locked MS51 at power-on is a pain. The alternative is to let the stock Gledopto firmware's own Tuya MCU OTA install this firmware: flash `esphome/stock-mcu-update.yaml` to the T1 and follow the steps at the top of that file. It drives the light with the stock DPs at 9600 baud, logs at VERBOSE, and offers **MCU firmware**. The stock MCU has no version DP, so the version shows as unknown and the update counts as available.

What's sent is `gl-c-301w-mcu-<ver>.ota.bin`: the raw APROM application plus a 32-byte footer. Whether the stock updater accepts it, where it writes it and what it checks is unknown. Tuya defines only the transport, not the image (see the disclaimer in the [DFU component's README](https://github.com/JesseMcL/esphome_tuya_mcu_dfu)). If the update fails, the MCU may need ICP recovery (`provision`). On success the new app talks at 115200, so the experiment config loses it and reports a failed verification; switch to `gl-c-301w.yaml`.

**What a stock update can and can't change.** The stock updater most likely writes only APROM. The stock LDROM, its size and the CONFIG bytes (locked, reset pin disabled) stay as they are, which matters for this firmware:

- The app is laid out for an LDROM of **3 KB or less**: its settings page is at 0x7300 and the validity record at 0x7380. With a 4 KB stock LDROM (APROM ends at 0x6FFF), those addresses fall inside the LDROM. The app would run, but settings and the version record wouldn't be stored.
- The app's own update path (`0x0A`) resets into whatever is in LDROM. Under a stock LDROM that's the stock bootloader, with unknown behaviour.
- The power button stays a reset button only if CONFIG0.RPD changes. Under stock CONFIG (RPD = 0) it's a GPIO, as the app expects.

**A possible path (not implemented).** The MS51 can rewrite its own LDROM and CONFIG bytes by IAP from APROM: `IAPUEN.LDUEN` / `CFUEN`, all-CONFIG erase plus byte program, and CONFIG0.LOCK doesn't block IAP. LDROM size is CONFIG1.LDSIZE and takes effect after a reset. So a stage-1 "installer" app sent through the stock updater could:

1. Write this repo's bootloader into LDROM.
2. Set CONFIG.
3. Reset into the normal update path.

The risky step is CONFIG: it's erase-all, then program. A power cut in between leaves factory defaults: boot from APROM, no LDROM, unlocked, reset pin enabled. That's still recoverable by ICP, but this needs care and a device you can recover.

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

- **Hardware:**
  - ESPHome on the BK7238 and the MS51 app and bootloader run on the GL-C-301W.
  - MCU updates from ESPHome through the bootloader work.
- **SDCC 4.2 via PlatformIO:** app 8.1 KB of 29.4 KB, bootloader 2.4 KB of 3 KB.
- **Host tests (Unity), 36 cases:** TuyaMCU init, every DP including the settings DPs, multi-DP frames, clamping, framing errors, and the OTA hand-off. Logic: settings log (round trip, wrap and erase, torn and out-of-range records), PWM periods and the manual's frequency order, and Set gestures (1/2/3 clicks, hold, click then hold). For the bootloader against simulated NOR flash: full update, update from rescue, retransmits, corrupted image, verify failure, size limits, wait timeout, rescue never times out, and the largest possible image (the settings page survives it).
- **End-to-end** (`esphome/test/run.sh`):
  - ESPHome 2026.10 (host build of this YAML with the real `tuya:`, `tuya_mcu` and `tuya_mcu_dfu`; `run.sh` uses a sibling `esphome_tuya_mcu_dfu` checkout, or `DFU_DIR`, if present) talks over a pty to `mcu/sim`, which runs the real `proto.c` and `boot_core.c`.
  - The light transitions arrive as paced DP writes. The MCU update goes 1.0.0 → 1.1.0 and is verified in about 1 s. A corrupted app (rescue mode) is recovered. A power click turns the light off, and a power hold dims it up from off.
- **ESPHome build for BK7238** (board `t1-u`) with both components compiles.
- **`esphome_tuya_mcu_dfu`** has its own host tests against a fake TuyaMCU: bundled and downloaded images, verification with and without a version DP, and MD5 rejection.
- **PlatformIO ICP targets** produce the expected single ssh session (files streamed as tar, then `pi_flash.py`; tested with a dummy ssh).

Not done yet:

- The stock-MCU DFU experiment (§7).
- RF transceiver.

## 10. Open items

- RF remote (2.4 GHz IC driver and protocol).
- Other board revisions: LED polarity (`LED_ACTIVE_LOW`) and PWM polarity (`PWM_INVERT_MASK`) are build options in `config.h`, in case they differ.
- Stock behaviour not in the manual is guessed: the click gap (400 ms), the strip blink timing.

## License

[MIT](LICENSE), except as noted below.

- The MS51 clock-trim and IAP sequences in `mcu/src/` follow Nuvoton's [MS51 BSP](https://github.com/OpenNuvoton/MS51_BSP) (Apache-2.0) and Nikita Lita's [NuMicro-8051-prog](https://github.com/nikitalita/NuMicro-8051-prog) (MIT). The BSP headers are fetched at build time and keep their own license.
- `esphome/components/tuya_mcu` is built against ESPHome, whose C++ core is GPL-3.0, so compiled ESPHome firmware is distributed under GPL-3.0.
