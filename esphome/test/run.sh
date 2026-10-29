#!/usr/bin/env bash
# End-to-end test on a PC: ESPHome (host platform) <-> pty <-> simulated MCU.
# The ESPHome config is generated from ../gl-c-301w.yaml (make_host_yaml.py).
#   1. builds MCU firmware 1.0.0 (installed in the simulator) and 1.1.0 (bundled)
#   2. the host ESPHome turns the light on, then installs the MCU update
#   3. expects "MCU update to 1.1.0 verified", then a power click (off) and a
#      power hold (dims up from off)
# Needs: platformio, esphome, socat, a C compiler.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT

(cd "$ROOT/mcu" && rm -rf dist && FW_VERSION=1.0.0 pio run -e app -s && cp dist/*1.0.0.icp.bin "$WORK/old.icp.bin" \
  && rm -rf dist && FW_VERSION=1.1.0 pio run -s)
cc -O1 -DPROTO_VERSION_HOOK -I"$ROOT/mcu/include" -I"$ROOT/mcu/src/boot" -o "$WORK/mcu_sim" \
  "$ROOT/mcu/sim/mcu_sim.c" "$ROOT/mcu/src/app/proto.c" "$ROOT/mcu/src/boot/boot_core.c"

socat pty,raw,echo=0,link=/tmp/esptty pty,raw,echo=0,link=/tmp/mcutty &
sleep 1
python3 make_host_yaml.py ../gl-c-301w.yaml host-integration.yaml
esphome compile host-integration.yaml
"$WORK/mcu_sim" /tmp/mcutty "$WORK/old.icp.bin" --press-power-at 36000 --hold-power-at 38000 > "$WORK/sim.log" &
sleep 0.3
timeout 44 .esphome/build/gl-c-301w-host-test/.pioenvs/gl-c-301w-host-test/program > "$WORK/esp.log" 2>&1 || true
grep -E "tuya_mcu|MCU|TEST|\]\[light" "$WORK/esp.log" | sed 's/\x1b\[[0-9;]*m//g' | grep -v "\[C\]"
echo "--- simulator ---"; grep -v "APP ch" "$WORK/sim.log"
grep -q "MCU update to 1.1.0 verified" "$WORK/esp.log" && echo "PASS" || { echo "FAIL"; exit 1; }
