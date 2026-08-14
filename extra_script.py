# extra_script.py
#
# 1. Wraps the compiler with ccache if available (faster incremental builds).
#    Install:  scoop/choco/winget install ccache
#
# 2. Adds a custom "deploy" target that uploads LittleFS first, then firmware.
#    Usage:
#      pio run -t deploy
#      – or – PlatformIO IDE → Environment toolbar → "Custom" → deploy

from shutil import which
import sys, os
Import("env")  # SCons environment injected by PlatformIO

# ---- ccache -----------------------------------------------------------------
ccache = which("ccache")
if ccache:
    env["CC"]  = ccache + " " + env["CC"]
    env["CXX"] = ccache + " " + env["CXX"]
    print(f"  [build] ccache enabled ({ccache})")
else:
    print("  [build] ccache not found – install it for faster incremental builds")

# ---- strip build-machine paths from the binary -------------------------------
# The Arduino core's logging macros embed __FILE__, so absolute source paths end
# up as strings inside firmware.bin — including the developer's home directory
# and Windows account name (23 occurrences before this was added). The published
# images in docs/flash/ and the release ZIP would carry them.
#
# Done here rather than in platformio.ini because these paths contain spaces and
# backslashes; the ini parser mangles both. SCons passes the list form through
# without a shell, so a space inside one argument is safe. The compiler sees
# forward slashes, so the mapping has to use them too.
def _prefix_maps(env):
    def norm(p):
        return os.path.abspath(p).replace("\\", "/").rstrip("/")
    seen, flags = set(), []
    for real, fake in ((env.subst("$PROJECT_PACKAGES_DIR"), "/pkg"),
                       (env.subst("$PROJECT_WORKSPACE_DIR"), "/build"),
                       (env.subst("$PROJECT_DIR"), "/src")):
        if not real:
            continue
        n = norm(real)
        if n and n not in seen:
            seen.add(n)
            flags.append("-ffile-prefix-map=%s=%s" % (n, fake))
    return flags

_maps = _prefix_maps(env)
if _maps:
    env.Append(CCFLAGS=_maps, CXXFLAGS=_maps, ASFLAGS=_maps)
    print("  [build] path privacy: %d -ffile-prefix-map rule(s)" % len(_maps))


# ---- deploy target ----------------------------------------------------------
def run_deploy(source, target, env):
    """Upload LittleFS filesystem then firmware in one step."""
    pio_exe = [sys.executable, "-m", "platformio"]
    env_name = env["PIOENV"]

    print("\n=== deploy: step 1 – uploadfs ===")
    rc = env.Execute(
        env.VerboseAction(
            " ".join(pio_exe + ["run", "-t", "uploadfs", "-e", env_name]),
            "Uploading LittleFS filesystem..."
        )
    )
    if rc:
        print("[deploy] ERROR: uploadfs failed – aborting")
        return rc

    print("\n=== deploy: step 2 – upload firmware ===")
    return env.Execute(
        env.VerboseAction(
            " ".join(pio_exe + ["run", "-t", "upload", "-e", env_name]),
            "Uploading firmware..."
        )
    )

env.AddCustomTarget(
    name="deploy",
    dependencies=None,
    actions=run_deploy,
    title="Full Deploy",
    description="Upload LittleFS filesystem then firmware (single command)"
)
