"""Firmware image tools for the GL-C-301W MCU.

Used three ways:
  * PlatformIO post-script (extra_scripts = post:scripts/fwimage.py):
    turns firmware.hex (Intel HEX from SDCC) into the release files in dist/
  * importable module (tests, simulator, release workflow)
  * CLI:  python scripts/fwimage.py manifest --version 1.2.3 --ota FILE --release-url URL

Layout constants are read from include/layout.h so C and Python agree.

Files produced in mcu/dist/:
  gl-c-301w-mcu-<ver>.ota.bin   app + 32-byte footer  (Tuya MCU OTA / ESPHome)
  gl-c-301w-mcu-<ver>.icp.bin   full APROM image incl. validity record (ICP)
  gl-c-301w-mcu-<ver>.app.hex   raw linker output
  gl-c-301w-boot-<ver>.bin      LDROM image (ICP)
  gl-c-301w-boot-<ver>.hex
"""
import hashlib
import json
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else None


# --------------------------------------------------------------------------
# layout.h
# --------------------------------------------------------------------------
def load_layout(mcu_dir, ldrom_size=None):
    defs = {}
    with open(os.path.join(mcu_dir, "include", "layout.h")) as f:
        for line in f:
            m = re.match(r"\s*#define\s+(\w+)\s+(.+?)\s*(/\*.*)?$", line)
            if not m:
                continue
            name, expr = m.group(1), m.group(2)
            expr = re.sub(r"(\d+)U\b", r"\1", expr)
            expr = re.sub(r"'(.)'", lambda c: str(ord(c.group(1))), expr)
            defs[name] = expr
    if ldrom_size is not None:
        defs["LDROM_SIZE"] = str(ldrom_size)
    out = {}

    def ev(name):
        if name in out:
            return out[name]
        expr = defs[name]
        expr = re.sub(r"\b([A-Z_][A-Z0-9_]*)\b",
                      lambda m: str(ev(m.group(1))) if m.group(1) in defs else m.group(1), expr)
        out[name] = int(eval(expr, {}, {}))  # constants only
        return out[name]

    for k in defs:
        ev(k)
    return out


# --------------------------------------------------------------------------
# images
# --------------------------------------------------------------------------
def parse_ihx(path):
    mem = {}
    base = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            raw = bytes.fromhex(line[1:])
            n, addr, typ = raw[0], (raw[1] << 8) | raw[2], raw[3]
            data = raw[4:4 + n]
            if (sum(raw) & 0xFF) != 0:
                raise ValueError("bad checksum in %s" % path)
            if typ == 0:
                for i, b in enumerate(data):
                    mem[base + addr + i] = b
            elif typ == 4:
                base = ((data[0] << 8) | data[1]) << 16
            elif typ == 1:
                break
    if not mem:
        return b""
    out = bytearray(b"\xff" * (max(mem) + 1))
    for a, b in mem.items():
        out[a] = b
    return bytes(out)


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def make_footer(app, version, L):
    if len(app) > L["APP_MAX_CODE"]:
        raise ValueError("app is %d bytes, max %d" % (len(app), L["APP_MAX_CODE"]))
    ver = version.encode()[:15]
    f = bytearray(b"\xff" * L["FOOTER_SIZE"])
    f[0:4] = b"GLCW"
    struct.pack_into("<HH", f, L["FOOTER_OFS_LEN"], len(app), crc16(app))
    f[L["FOOTER_OFS_FORMAT"]] = L["FOOTER_FORMAT"]
    v = L["FOOTER_OFS_VERSION"]
    f[v:v + 16] = ver.ljust(16, b"\0")
    return bytes(f)


def ota_image(app, version, L):
    return app + make_footer(app, version, L)


def icp_image(ota, L):
    """Full APROM image: OTA image, 0xFF fill, validity record in the last page."""
    rec = L["APP_RECORD_ADDR"]
    img = bytearray(b"\xff" * L["APROM_SIZE"])
    img[0:len(ota)] = ota
    footer = ota[-L["FOOTER_SIZE"]:]
    img[rec:rec + L["RECORD_SIZE"]] = footer[:L["RECORD_SIZE"]]
    return bytes(img)


def parse_footer(ota, L):
    f = ota[-L["FOOTER_SIZE"]:]
    if f[0:4] != b"GLCW":
        raise ValueError("no GLCW footer")
    ln, crc = struct.unpack_from("<HH", f, L["FOOTER_OFS_LEN"])
    v = L["FOOTER_OFS_VERSION"]
    return {"len": ln, "crc": crc,
            "version": f[v:v + 16].split(b"\0")[0].decode(errors="replace"),
            "crc_ok": ln == len(ota) - L["FOOTER_SIZE"] and crc16(ota[:ln]) == crc}


def manifest(version, ota_path, release_url="", summary=""):
    with open(ota_path, "rb") as f:
        data = f.read()
    return {
        "name": "GL-C-301W MS51FC0AE firmware",
        "version": version,
        "builds": [{
            "chipFamily": "MS51FC0AE",
            "ota": {
                "path": os.path.basename(ota_path),
                "md5": hashlib.md5(data).hexdigest(),
                "size": len(data),
                "release_url": release_url,
                "summary": summary,
            },
        }],
    }


# --------------------------------------------------------------------------
# PlatformIO post-build hook
# --------------------------------------------------------------------------
def _pio_post(source, target, env):
    mcu_dir = env.subst("$PROJECT_DIR")
    ldrom = int(env.GetProjectOption("custom_ldrom_size", "2048"))
    L = load_layout(mcu_dir, ldrom)
    version = env.get("FW_VERSION", "0.0.0-dev")
    safe = version.replace("*", "-dirty")
    hexfile = str(target[0])
    build = env.subst("$BUILD_DIR")
    dist = os.path.join(mcu_dir, "dist")
    os.makedirs(dist, exist_ok=True)
    img = parse_ihx(hexfile)
    kind = env.subst("$PIOENV")

    if kind == "boot":
        if len(img) > ldrom:
            raise SystemExit("bootloader is %d bytes, LDROM is %d" % (len(img), ldrom))
        name = "gl-c-301w-boot-%s" % safe
        _write(os.path.join(build, "boot.bin"), img)
        _write(os.path.join(dist, name + ".bin"), img)
        shutil.copyfile(hexfile, os.path.join(dist, name + ".hex"))
        print("LDROM bootloader: %d / %d bytes" % (len(img), ldrom))
    else:
        ota = ota_image(img, version, L)
        icp = icp_image(ota, L)
        name = "gl-c-301w-mcu-%s" % safe
        _write(os.path.join(build, "app.ota.bin"), ota)
        _write(os.path.join(build, "app.icp.bin"), icp)
        _write(os.path.join(dist, name + ".ota.bin"), ota)
        _write(os.path.join(dist, name + ".icp.bin"), icp)
        shutil.copyfile(hexfile, os.path.join(dist, name + ".app.hex"))
        m = manifest(version, os.path.join(dist, name + ".ota.bin"))
        _write(os.path.join(dist, "manifest.json"), json.dumps(m, indent=2).encode())
        print("App: %d / %d bytes (OTA image %d bytes, CRC %04X)" %
              (len(img), L["APP_MAX_CODE"], len(ota), crc16(img)))


def _write(path, data):
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "SCons.Script":  # running as a PlatformIO extra_script
    Import("env")  # noqa: F821
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _pio_post)  # noqa: F821

elif __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("manifest", help="write manifest.json for an OTA image")
    m.add_argument("--version", required=True)
    m.add_argument("--ota", required=True)
    m.add_argument("--release-url", default="")
    m.add_argument("--summary", default="")
    m.add_argument("-o", "--output", default="-")
    i = sub.add_parser("info", help="show footer of an OTA image")
    i.add_argument("ota")
    a = ap.parse_args()
    mcu = os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[0])))
    if a.cmd == "manifest":
        s = json.dumps(manifest(a.version, a.ota, a.release_url, a.summary), indent=2)
        if a.output == "-":
            print(s)
        else:
            _write(a.output, s.encode())
    else:
        with open(a.ota, "rb") as f:
            print(parse_footer(f.read(), load_layout(mcu)))
