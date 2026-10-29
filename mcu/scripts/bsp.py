"""PlatformIO pre-script: fetch Nuvoton's MS51 32K BSP headers (Apache-2.0).

Only the register/macro headers are needed. They are sparse-checked-out at a
pinned commit into mcu/.bsp/ (git-ignored) so builds are reproducible.
"""
import os
import subprocess

Import("env")  # noqa: F821  (provided by SCons)

BSP_URL = "https://github.com/OpenNuvoton/MS51_BSP"
BSP_SHA = "3fc5eeea985bae889a38ab96e0eaf0b54f5e64e4"
CHIP_DIR = "MS51FC0AE_MS51XC0BE_MS51EB0AE_MS51EC0AE_MS51TC0AE_MS51PC0AE"
PATHS = [f"{CHIP_DIR}/Library/Device/Include", f"{CHIP_DIR}/Library/StdDriver/inc"]

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
bsp_dir = os.path.join(project_dir, ".bsp")
stamp = os.path.join(bsp_dir, ".pinned-" + BSP_SHA)


def git(*args):
    subprocess.check_call(["git", "-C", bsp_dir, *args])


if not os.path.exists(stamp):
    print("Fetching Nuvoton MS51 BSP headers @ %s" % BSP_SHA[:10])
    os.makedirs(bsp_dir, exist_ok=True)
    if not os.path.isdir(os.path.join(bsp_dir, ".git")):
        git("init", "-q")
        git("remote", "add", "origin", BSP_URL)
    git("sparse-checkout", "set", "--no-cone", *PATHS)
    git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", BSP_SHA)
    git("checkout", "-q", "FETCH_HEAD")
    open(stamp, "w").close()

env.Append(CPPPATH=[os.path.join(bsp_dir, p) for p in PATHS])  # noqa: F821
