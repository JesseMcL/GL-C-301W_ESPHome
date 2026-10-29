#!/usr/bin/env python3
"""Runs ON the Raspberry Pi: program the MS51FC0AE over ICP with nuvo51icpy.

Needs Nikita Lita's NuMicro-8051-prog installed on the Pi
(https://github.com/nikitalita/NuMicro-8051-prog, `pip install -e .`).

  pi_flash.py status
  pi_flash.py app   --aprom app.icp.bin
  pi_flash.py boot  --ldrom boot.bin --config dev.json
  pi_flash.py provision --aprom app.icp.bin --ldrom boot.bin --config dev.json [--mass-erase]
  pi_flash.py config --config release.json

The chip on the GL-C-301W ships security-locked with the reset pin disabled,
so ICP entry only works at power-on: nuvo51icpy keeps retrying entry - power
the board up (or cycle it) while it does. A locked chip has to be mass
erased first, which also re-enables the reset pin.
"""
import argparse
import json
import os
import sys

try:
    from nuvoprogpy.nuvo51icpy import Nuvo51ICP
    from nuvoprogpy.config import N8051ConfigFlags
except ImportError:
    sys.exit("nuvoprogpy not found - install NuMicro-8051-prog on the Pi")


def load_config(path):
    with open(path) as f:
        j = json.load(f)
    return N8051ConfigFlags(list(j["config_bytes"]))


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["status", "app", "boot", "provision", "config"])
    ap.add_argument("--aprom")
    ap.add_argument("--ldrom")
    ap.add_argument("--config")
    ap.add_argument("--mass-erase", action="store_true")
    ap.add_argument("--library", default=os.environ.get("ICP_LIBRARY"),
                    help="GPIO library for nuvo51icpy: pigpio or gpiod "
                         "(default: nuvo51icpy's own default)")
    a = ap.parse_args()

    # only pass library= when asked, so nuvo51icpy's default applies otherwise
    icp_kwargs = {"library": a.library} if a.library else {}

    print(">>> Power-cycle the GL-C-301W now if ICP entry does not succeed <<<", flush=True)
    with Nuvo51ICP(**icp_kwargs) as icp:
        info = icp.get_device_info()
        print(info)
        locked = info.is_unsupported or icp._needs_unlock()

        if a.action == "status":
            if locked:
                print("Chip is locked (or not detected): flash/config unreadable")
            else:
                icp.read_config().print_config()
            return 0

        if a.mass_erase or locked:
            print("Mass erase (unlocks the chip, restores the reset pin)...")
            if not icp.mass_erase():
                print("Mass erase failed")
                return 1

        cfg = load_config(a.config) if a.config else icp.read_config()
        aprom = read(a.aprom) if a.aprom and a.action in ("app", "provision") else b""
        ldrom = read(a.ldrom) if a.ldrom and a.action in ("boot", "provision") else b""

        if a.action == "config":
            ok = icp.program_config(cfg)
        else:
            ok = icp.program_all(aprom, ldrom, cfg)
        print("OK" if ok else "FAILED")
        if ok:
            icp.read_config().print_config()
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
