"""ICP programming through a Raspberry Pi running nuvo51icpy.

As a PlatformIO extra script it wires up:
    pio run -e app  -t upload       APROM only (app.icp.bin, keeps LDROM/CONFIG)
    pio run -e boot -t upload       LDROM + CONFIG (config/dev.json)
    pio run -e app  -t provision    mass erase + LDROM + APROM + CONFIG (dev)
                                    (build the boot env first: `pio run`)
    pio run -e app  -t icp_status   show device info / CONFIG
    pio run -e app  -t icp_release  write config/release.json (reset pin -> button, WDT on)

Standalone:  python scripts/icp.py provision --aprom X --ldrom Y --config Z

Environment:
    ICP_HOST    ssh host of the Pi             (default: raspberrypi)
    ICP_USER    ssh user on the Pi             (default: pi; set it empty to
                                                use ~/.ssh/config / your login)
    ICP_PYTHON  python on the Pi with nuvoprogpy (default: python3)
    ICP_DIR     staging directory on the Pi    (default: gl-c-301w-icp)
"""
import io
import os
import shlex
import subprocess
import sys
import tarfile

USER = os.environ.get("ICP_USER", "pi")
HOST = os.environ.get("ICP_HOST", "raspberrypi")
TARGET = "%s@%s" % (USER, HOST) if USER else HOST   # ssh destination
PY = os.environ.get("ICP_PYTHON", "python3")
RDIR = os.environ.get("ICP_DIR", "gl-c-301w-icp")
HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else None


def run_remote(action, scripts_dir, aprom=None, ldrom=None, config=None, mass_erase=False):
    """One ssh connection (one password prompt): the files travel as a tar
    stream on stdin, are unpacked into ICP_DIR, then pi_flash.py runs.
    pi_flash.py never reads stdin, so no tty is needed; `python -u` keeps its
    progress / "power-cycle now" output unbuffered."""
    files = [os.path.join(scripts_dir, "pi_flash.py")]
    args = [action]
    for opt, path in (("--aprom", aprom), ("--ldrom", ldrom), ("--config", config)):
        if path:
            if not os.path.isfile(path):
                raise SystemExit("missing %s (build it first)" % path)
            files.append(path)
            args += [opt, RDIR + "/" + os.path.basename(path)]
    if mass_erase:
        args.append("--mass-erase")

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for f in files:
            tar.add(f, arcname=os.path.basename(f))

    d = shlex.quote(RDIR)
    flash = " ".join(shlex.quote(x) for x in [PY, "-u", RDIR + "/pi_flash.py", *args])
    remote = "mkdir -p %s && tar -xf - -C %s && %s" % (d, d, flash)
    return subprocess.run(["ssh", TARGET, remote], input=buf.getvalue()).returncode


if __name__ == "SCons.Script":
    Import("env")  # noqa: F821
    proj = env.subst("$PROJECT_DIR")  # noqa: F821
    scripts = os.path.join(proj, "scripts")
    cfg = lambda n: os.path.join(proj, "config", n)  # noqa: E731
    app_icp = os.path.join(proj, ".pio", "build", "app", "app.icp.bin")
    boot_bin = os.path.join(proj, ".pio", "build", "boot", "boot.bin")

    def _do(action, **kw):
        def _act(*_a, **_k):
            rc = run_remote(action, scripts, **kw)
            if rc:
                raise SystemExit(rc)
        return _act

    if env.subst("$PIOENV") == "boot":  # noqa: F821
        env.Replace(UPLOADCMD=_do("boot", ldrom=boot_bin, config=cfg("dev.json")))  # noqa: F821
    else:
        env.Replace(UPLOADCMD=_do("app", aprom=app_icp))  # noqa: F821
        env.AddCustomTarget(  # noqa: F821
            "provision", "$BUILD_DIR/${PROGNAME}.hex",
            _do("provision", aprom=app_icp, ldrom=boot_bin, config=cfg("dev.json"), mass_erase=True),
            title="Provision (ICP)", description="Mass erase + LDROM + APROM + dev CONFIG")
        env.AddCustomTarget(  # noqa: F821
            "icp_status", None, _do("status"), title="ICP status",
            description="Show device info and CONFIG")
        env.AddCustomTarget(  # noqa: F821
            "icp_release", None, _do("config", config=cfg("release.json")),
            title="ICP release CONFIG",
            description="Reset pin -> power button, WDT on (ICP needs power-on entry afterwards)")

elif __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["status", "app", "boot", "provision", "config"])
    ap.add_argument("--aprom")
    ap.add_argument("--ldrom")
    ap.add_argument("--config")
    ap.add_argument("--mass-erase", action="store_true")
    a = ap.parse_args()
    sys.exit(run_remote(a.action, HERE, a.aprom, a.ldrom, a.config, a.mass_erase))
