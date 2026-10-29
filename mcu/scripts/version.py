"""PlatformIO pre-script: derive the firmware version from git.

  tag v1.2.3 on HEAD -> "1.2.3"
  otherwise          -> "dev-<sha7>", plus "+<3 hex>" when mcu/ has local
                        changes (hash of the changes, so each dirty build
                        gets its own version)
The FW_VERSION environment variable overrides (used by the release workflow).
The string is limited to 15 characters (footer / DP 120 field).
"""
import hashlib
import os
import subprocess

Import("env")  # noqa: F821


def dirty_suffix(root):
    """"" for a clean tree, else "+xxx": 3 hex chars hashed from the local
    changes under mcu/ (diff + untracked files), so every distinct dirty
    build gets a distinct version and the ESPHome update entity notices."""
    diff = subprocess.check_output(["git", "-C", root, "diff", "HEAD", "--binary", "--", "."],
                                   stderr=subprocess.DEVNULL)
    untracked = subprocess.check_output(
        ["git", "-C", root, "ls-files", "--others", "--exclude-standard", "-z", "--", "."],
        stderr=subprocess.DEVNULL).split(b"\0")
    if not diff and not any(untracked):
        return ""
    h = hashlib.sha1(diff)
    for name in sorted(u for u in untracked if u):
        h.update(name)
        try:
            with open(os.path.join(root, name.decode()), "rb") as f:
                h.update(f.read())
        except OSError:
            pass
    return "+" + h.hexdigest()[:3]


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
        return "dev-" + sha + dirty_suffix(root)
    except Exception:
        return "0.0.0-dev"


version = describe()[:15]
env["FW_VERSION"] = version  # noqa: F821
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
print("Firmware version: %s" % version)
