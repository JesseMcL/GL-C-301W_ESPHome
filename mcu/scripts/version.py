"""PlatformIO pre-script: derive the firmware version from git.

  tag v1.2.3 on HEAD -> "1.2.3"
  otherwise          -> "dev-<sha7>" (+"*" when the tree is dirty)
The FW_VERSION environment variable overrides (used by the release workflow).
The string is limited to 15 characters (footer / DP 120 field).
"""
import os
import subprocess

Import("env")  # noqa: F821


def describe():
    if os.environ.get("FW_VERSION"):
        return os.environ["FW_VERSION"]
    root = env.subst("$PROJECT_DIR")  # noqa: F821
    try:
        tag = subprocess.check_output(
            ["git", "-C", root, "describe", "--tags", "--exact-match", "--match", "v*"],
            stderr=subprocess.DEVNULL, text=True).strip()
        return tag[1:]
    except Exception:
        pass
    try:
        sha = subprocess.check_output(["git", "-C", root, "rev-parse", "--short=7", "HEAD"],
                                      stderr=subprocess.DEVNULL, text=True).strip()
        dirty = subprocess.call(["git", "-C", root, "diff", "--quiet", "HEAD", "--", "."],
                                stderr=subprocess.DEVNULL) != 0
        return "dev-" + sha + ("*" if dirty else "")
    except Exception:
        return "0.0.0-dev"


version = describe()[:15]
env["FW_VERSION"] = version  # noqa: F821
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
print("Firmware version: %s" % version)
