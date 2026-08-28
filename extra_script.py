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
import sys, os, gzip
Import("env")  # SCons environment injected by PlatformIO

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
# was disabled via CONFIG_COMPILER_HIDE_PATHS_MACROS=n. Path privacy matters
# for the RELEASED 4" images; the 7B is not published yet, and once it is,
# releasing requires either a space-free project path or list-form handling
# in the hybrid pass.
_is_hybrid = bool(env.GetProjectConfig().get(
    "env:" + env["PIOENV"], "custom_sdkconfig", ""))
_maps = [] if _is_hybrid else _prefix_maps(env)
if _maps:
    env.Append(CCFLAGS=_maps, CXXFLAGS=_maps, ASFLAGS=_maps)
    print("  [build] path privacy: %d -ffile-prefix-map rule(s)" % len(_maps))
elif _is_hybrid:
    print("  [build] path privacy: skipped (hybrid compile env, space-in-path)")


# ---- pre-compress the web UI -------------------------------------------------
# index.html is ~127 KB of HTML + CSS + JS and used to go over the air
# UNCOMPRESSED: measured 4.4 s to load on the 7B and 13.4 s on the 5B (weaker
# signal, same page). gzip cuts it to roughly a quarter, which matters a lot on
# a boat where the phone is three bulkheads away from the display.
#
# Regenerated on every pio run whenever index.html is newer, so the .gz can
# never go stale against the source - that staleness is exactly why this is a
# build step and not a checked-in artefact. Both files land in the LittleFS
# image; the plain index.html stays as the fallback WebConfig serves when the
# .gz is missing (and as what serveStatic hands out for direct file access).
def _gzip_webui():
    src = os.path.join(env.subst("$PROJECT_DIR"), "data", "index.html")
    dst = src + ".gz"
    if not os.path.isfile(src):
        return
    if os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    with open(src, "rb") as fh:
        raw = fh.read()
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
