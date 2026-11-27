# build4.ps1 - build (or upload) the 4" env with its PRIVATE package dir.
#
# Why: the 4" platform (espressif32@6.12.0) and the 7B's pioarduino platform
# resolve packages BY NAME in the one shared ~/.platformio/packages dir.
# tool-esptoolpy exists there in two incompatible flavours (registry 2.x vs
# pioarduino's 5.3.0 zip) and every board switch evicted the other side's
# copy - the first 7B build after a 4" build then failed fast with the
# "Path/None" TypeError. Version pins and symlink:// specs were both tried
# and measured: pioarduino manages the tool outside the spec mechanics, the
# fight always came back. A private PLATFORMIO_PACKAGES_DIR for this
# rarely-built legacy env is the clean cut.
#
# The private dir only holds DOWNLOADED TOOLCHAINS (~1.5 GB, filled on the
# first run) - never any project source code.
#
# Usage:
#   tools\build4.ps1                 # plain build
#   tools\build4.ps1 -t upload       # any extra pio args are passed through
param([Parameter(ValueFromRemainingArguments = $true)] $Rest)

# Everything below is derived from PlatformIO's own directory rather than
# written out: an absolute path with an author's home directory in it is a name
# in a public repository, and it is a path nobody else can build into.
$core = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR }
        else { Join-Path $HOME ".platformio" }

# The two variables below are put back the way they were at the end. A script
# called with "&" shares the caller's environment: without the restore, every
# pio run typed into the same terminal afterwards - a 7B or 5B build - silently
# used this private package dir and build dir. It installed the pioarduino
# packages into packages-4inch (esptool 5.3.0 included, the very eviction this
# dir exists to stop), recompiled the IDF libs there for an hour, and left its
# firmware.bin in build\nauticpinnace-4inch\<env>, where nobody looked for it
# (seen 2026-10-13).
$savedPkgs  = $env:PLATFORMIO_PACKAGES_DIR
$savedBuild = $env:PLATFORMIO_BUILD_DIR
$env:PLATFORMIO_PACKAGES_DIR = Join-Path $core "packages-4inch"

# ...and its own BUILD dir, for the same reason one level up. Changing the
# package directory makes PlatformIO see a different project configuration, so
# it wipes the shared build_dir from platformio.ini - taking the 5B and 7B
# firmware.bin with it. That is silent: the build reports success and the
# binaries are simply gone at flash time. Measured twice on 2026-08-22, once
# after a simulator build and once after this script.
# Environment beats the ini here, so this needs no change to platformio.ini
# and leaves the other environments alone. Costs one full rebuild the first
# time, then nothing.
$env:PLATFORMIO_BUILD_DIR = Join-Path $core "build\nauticpinnace-4inch"

# PlatformIO's bundled interpreter first (that is where a PlatformIO IDE
# install puts it), a pio on PATH second.
$code = 1
try {
    $pio = Join-Path $core "penv\Scripts\pio.exe"
    if (-not (Test-Path $pio)) {
        $onPath = Get-Command pio -ErrorAction SilentlyContinue
        if ($onPath) { $pio = $onPath.Source }
        else { Write-Error "pio not found - looked in $pio and on PATH"; exit 1 }
    }
    $proj = Split-Path $PSScriptRoot -Parent
    & $pio run -d $proj -e waveshare_esp32s3_4 @Rest
    $code = $LASTEXITCODE
} finally {
    # Back to the caller's values; an unset variable stays unset.
    if ($null -eq $savedPkgs)  { Remove-Item Env:PLATFORMIO_PACKAGES_DIR -ErrorAction SilentlyContinue }
    else { $env:PLATFORMIO_PACKAGES_DIR = $savedPkgs }
    if ($null -eq $savedBuild) { Remove-Item Env:PLATFORMIO_BUILD_DIR -ErrorAction SilentlyContinue }
    else { $env:PLATFORMIO_BUILD_DIR = $savedBuild }
}
exit $code
