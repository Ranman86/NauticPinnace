# extra_script.py
#
# 1. Wraps the compiler with ccache if available (faster incremental builds).
#    Install:  scoop/choco/winget install ccache
#
# 2. Routes esp_restart_noos() through lib/NMEA2000_esp32, which takes the CAN
#    controller off the bus before any reset.
#
# 3. Prints the PSRAM budget of the 7B and 5B images after every link, and
#    writes FW_VERSION into IDF's app description (tools/app_desc.py).
#
# 4. Adds a custom "deploy" target that uploads LittleFS first, then firmware.
#    Usage:
#      pio run -t deploy
#      – or – PlatformIO IDE → Environment toolbar → "Custom" → deploy

from shutil import which
import sys, os, gzip, zlib
Import("env")  # SCons environment injected by PlatformIO

# ---- Windows console encoding -----------------------------------------------
# esptool 5.x draws its progress bar with block characters (U+2588 "█",
# U+2591 "░"). On Windows, Python encodes a redirected stdout as cp1252 by
# default, which has no code point for either - so esptool dies mid-transfer
# with
#     UnicodeEncodeError: 'charmap' codec can't encode characters ...
# and the upload looks like a hang: the chip is detected, the stub loads, the
# flash size is read, and then nothing. Measured here: eight minutes with no
# progress, against 49 seconds once this is set.
#
# It is set for the whole process, not just the build, because the failure is
# in the UPLOAD step - and it is set unconditionally: a UTF-8 stdout is correct
# everywhere, and on Linux and macOS it is already the default, so this changes
# nothing there.
os.environ.setdefault("PYTHONIOENCODING", "utf-8")

# ---- ccache -----------------------------------------------------------------
def _find_ccache():
    """which() first; then the winget install location, because a winget
    install only lands in NEW shells' PATH - long-running parents (IDE,
    agent harnesses) keep spawning children with the stale PATH and which()
    keeps missing it there."""
    hit = which("ccache")
    if hit:
        return hit
    import glob
    local = os.environ.get("LOCALAPPDATA", "")
    for pat in (os.path.join(local, "Microsoft", "WinGet", "Links", "ccache.exe"),
                os.path.join(local, "Microsoft", "WinGet", "Packages",
                             "Ccache.Ccache_*", "ccache-*", "ccache.exe")):
        m = glob.glob(pat)
        if m:
            return m[0]
    return None

ccache = _find_ccache()
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

# The prefix maps are SKIPPED for envs that use pioarduino's hybrid compile
# (custom_sdkconfig): that pass re-executes compile commands through its own
# machinery, where an argument containing our space-in-path project dir gets
# split and every file dies with "invalid argument ... to '-ffile-prefix-map'"
# - the exact failure IDF's own -fmacro-prefix-map produced there before it
# was disabled via CONFIG_COMPILER_HIDE_PATHS_MACROS=n. The 7B and 5B images
# are published too (web flasher, since v1.1.0), so for them the build paths
# have to be kept out by other means: tools/gen_web_flasher.py runs their
# release builds through neutral_build_env(), and tools/release_checks.py
# refuses any image that still contains the account name.
_is_hybrid = bool(env.GetProjectConfig().get(
    "env:" + env["PIOENV"], "custom_sdkconfig", ""))
_maps = [] if _is_hybrid else _prefix_maps(env)
if _maps:
    env.Append(CCFLAGS=_maps, CXXFLAGS=_maps, ASFLAGS=_maps)
    print("  [build] path privacy: %d -ffile-prefix-map rule(s)" % len(_maps))
elif _is_hybrid:
    print("  [build] path privacy: skipped (hybrid compile env, space-in-path)")


# ---- CAN controller off the bus on every restart ------------------------------
# lib/NMEA2000_esp32 parks the CAN TX pin before a reset, because the CPU reset
# leaves the TWAI controller running at half the bit rate, and on every board
# replaces IDF's CPU reset, which hung there (see there). esp_restart(), a
# panic and a watchdog all end in esp_restart_noos(), so every call to it is
# routed through the library's __wrap_esp_restart_noos. The bring-up envs do
# not link the library - the flag would leave the wrapper undefined there.
if not env["PIOENV"].startswith("bringup"):
    env.Append(LINKFLAGS=["-Wl,--wrap=esp_restart_noos"])


# ---- PSRAM budget of the 7B and 5B images -------------------------------------
# On those boards code runs from PSRAM in 64 KB pages, and one page too many
# used to leave the panel dark. After every link, tools/psram_budget.py says
# how many pages are left before the canvas arena has to shrink; the release
# gate refuses an image where it would. Images without the budget symbol
# (the 4-inch board) print nothing.
#
# The same images - and only they, the budget symbol is the board test - must
# carry the panic wrap (src/LoopWatchdog.cpp, -Wl,--wrap in platformio.ini):
# --wrap is silent when it matches nothing, and the unused wrapper is then
# garbage-collected, so a missing symbol fails the build here.
def _psram_budget(target, source, env):
    sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))
    import psram_budget
    psram_budget.report(str(target[0]))
    # Its own read of the ELF: a budget that cannot be read only prints
    # "budget check failed" above, and must not let a missing wrap through.
    why = psram_budget.panic_wrap_problem(str(target[0]))
    if why:
        print("[psram] FAILED: %s" % why)
        return 1

if env.get("PIOPLATFORM") != "native" and not env["PIOENV"].startswith("bringup"):
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf",
                      env.VerboseAction(_psram_budget, "Checking the PSRAM budget"))


# ---- version in IDF's app description ------------------------------------------
# esp_app_desc_t (image offset 0x20) is compiled into the framework's IDF libs,
# so it carried their strings, not ours: "esp-idf: v4.4.7 38eeba213a" on the
# 4-inch, `git describe` of the tree at libs-build time ("v1.2.0-dirty" on
# 1.3.0) on the 7B and 5B - which is what esptool image_info shows.
# tools/app_desc.py writes FW_VERSION and "NauticPinnace" into
# the ELF right after the link, before firmware.bin is made and hashed.
def _app_desc(target, source, env):
    project = env.subst("$PROJECT_DIR")
    sys.path.insert(0, os.path.join(project, "tools"))
    import app_desc
    import release_checks   # the one reader of FW_VERSION in src/Version.h
    try:
        version = release_checks.fw_version(project)
        old = app_desc.patch(str(target[0]), version)
    except (OSError, ValueError) as e:
        print("[app_desc] FAILED: %s" % e)
        return 1
    print('[app_desc] version "%s" (was "%s"), project "%s" (was "%s")'
          % (version, old[0], app_desc.PROJECT_NAME, old[1]))

if env.get("PIOPLATFORM") != "native":
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf",
                      env.VerboseAction(_app_desc, "Writing the version into the app description"))


# ---- pre-compress the web UI -------------------------------------------------
# index.html is ~127 KB of HTML + CSS + JS and used to go over the air
# UNCOMPRESSED: measured 4.4 s to load on the 7B and 13.4 s on the 5B (weaker
# signal, same page). gzip cuts it to roughly a quarter, which matters a lot on
# a boat where the phone is three bulkheads away from the display.
#
# Checked on every pio run, so the .gz can never go stale against the source -
# that staleness is exactly why this is a build step and not a checked-in
# artefact. Both files land in the LittleFS image; the plain index.html stays
# as the fallback WebConfig serves when the .gz is missing (and as what
# serveStatic hands out for direct file access). The device serves the .gz
# first, so a stale one ships old text that no grep of index.html can see.
#
# Decided by CONTENT, not by modification time: a checkout, a stash pop or a
# restored file can leave an outdated .gz with the newer timestamp, and the
# old rule ("skip if the .gz is newer") then kept it. The .gz is rewritten
# only when its decompressed bytes differ from index.html - which also leaves
# it alone when a different zlib (newer Pythons ship zlib-ng) would compress
# the same input to different bytes.
def _gzip_webui():
    src = os.path.join(env.subst("$PROJECT_DIR"), "data", "index.html")
    dst = src + ".gz"
    if not os.path.isfile(src):
        return
    with open(src, "rb") as fh:
        raw = fh.read()
    if os.path.isfile(dst):
        try:
            with gzip.open(dst, "rb") as fh:
                if fh.read() == raw:
                    return
        except (OSError, EOFError, zlib.error):
            pass            # unreadable or not gzip at all: regenerate it
    # mtime=0 keeps the output byte-identical for identical input, so the file
    # does not churn in git on every build.
    with gzip.GzipFile(dst, "wb", compresslevel=9, mtime=0) as fh:
        fh.write(raw)
    print("[webui] gzip index.html: %d -> %d bytes (%.0f%% of original)"
          % (len(raw), os.path.getsize(dst), 100.0 * os.path.getsize(dst) / len(raw)))

_gzip_webui()

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
