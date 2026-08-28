# gen_docs_shots.ps1 - regenerate the 1024x600 documentation screenshots.
#
# Produces docs/img/hires/<screen>_<theme>_<orientation>.png for every screen
# plus the home launcher, the settings page and the licence page, in light,
# dark and night, landscape and portrait.
#
# WHY A SCRIPT: the set was first made by hand, and six days later it showed a
# user interface that no longer existed - stale documentation is worse than
# none, because nobody can tell it is stale by looking at it. This takes about
# a minute, so there is no excuse for not re-running it after UI work.
#
# Two things it has to get right, neither obvious:
#
#  * PORTRAIT IS NOT A FLAG. The simulator reads display.rotation out of the
#    configuration, exactly like the device does - that is the point, it shows
#    what a rotated panel really shows. So each portrait pass gets a throwaway
#    config with rotation 90.
#
#  * A PORTRAIT CAPTURE COMES OUT SIDEWAYS. The window keeps the panel's
#    1024x600 shape and the picture lies on its side inside it, again exactly
#    like the hardware. The frames are turned upright here, which is why this
#    needs Python with Pillow.
#
# Usage:  tools\gen_docs_shots.ps1            all six passes, in parallel
#         tools\gen_docs_shots.ps1 -Landscape only the three landscape passes
param(
    [switch]$Landscape,          # skip the portrait passes
    [switch]$KeepTemp            # leave the raw .bmp captures for inspection
)

$ErrorActionPreference = "Stop"
$PROJECT = Split-Path $PSScriptRoot -Parent
$OUT     = Join-Path $PROJECT "docs\img\hires"

$PIO_CORE = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR }
            else { Join-Path $HOME ".platformio" }
$PIO = Join-Path $PIO_CORE "penv\Scripts\pio.exe"
if (-not (Test-Path $PIO)) {
    $c = Get-Command pio -ErrorAction SilentlyContinue
    if ($c) { $PIO = $c.Source } else { throw "pio not found" }
}

# SDL2.dll has to be on PATH for the simulator to start at all. Without it the
# process dies with 0xC0000135 and prints nothing, which reads exactly like
# "the simulator produced no images".
foreach ($d in @("C:\SDL2\bin", (Join-Path $HOME "scoop\apps\sdl2\current\bin"))) {
    if ((Test-Path (Join-Path $d "SDL2.dll")) -and ($env:PATH -notlike "*$d*")) {
        $env:PATH = "$d;$env:PATH"
    }
}

$SIMBUILD = Join-Path $PIO_CORE "build\nauticpinnace-sim"
Write-Host "Building simulator_7b..." -ForegroundColor Cyan
$env:PLATFORMIO_BUILD_DIR = $SIMBUILD
& $PIO run -d $PROJECT -e simulator_7b | Out-Null
if ($LASTEXITCODE -ne 0) { throw "simulator build failed" }
$EXE = Join-Path $SIMBUILD "simulator_7b\program.exe"
if (-not (Test-Path $EXE)) { throw "simulator binary not found: $EXE" }

$passes = @()
foreach ($theme in @("light", "dark", "night")) {
    $passes += @{ Theme = $theme; Orient = "landscape"; Rot = 0 }
    if (-not $Landscape) { $passes += @{ Theme = $theme; Orient = "portrait"; Rot = 90 } }
}

$tmpRoot = Join-Path $env:TEMP "nauticpinnace-shots-$PID"
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
New-Item -ItemType Directory -Force -Path $OUT | Out-Null

# Six simulator instances at once. They only read the project and write into
# their own directory, so there is nothing for them to fight over.
$jobs = @()
foreach ($p in $passes) {
    $dir = Join-Path $tmpRoot "$($p.Theme)_$($p.Orient)"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    # A throwaway config, so the real data\config.json is never touched.
    $cfg = Join-Path $dir "config.json"
    (Get-Content (Join-Path $PROJECT "data\config.json") -Raw) `
        -replace '"rotation"\s*:\s*\d+', ('"rotation": ' + $p.Rot) | Set-Content $cfg -Encoding utf8

    $jobs += Start-Job -ScriptBlock {
        param($exe, $dir, $theme, $cfg, $path)
        $env:PATH = $path
        & $exe --shots $dir --cfg $cfg "--$theme" --en --settle 900 2>&1 | Out-Null
    } -ArgumentList $EXE, $dir, $p.Theme, $cfg, $env:PATH
}
Write-Host "Capturing $($passes.Count) passes in parallel..." -ForegroundColor Cyan
$jobs | Wait-Job | Out-Null
$jobs | Remove-Job

# Turn the captures into the documentation set: strip the scr_NN_/ovl_NN_
# prefixes, rotate portrait upright, write PNG.
$py = @'
import sys, os, re, glob
from PIL import Image
src, out, theme, orient, rot = sys.argv[1:6]
n = 0
for f in glob.glob(os.path.join(src, "*.bmp")):
    base = os.path.basename(f)
    m = re.match(r"(?:scr|ovl)_\d+_(.+)\.bmp$", base)
    if not m:
        continue
    name = m.group(1)
    im = Image.open(f)
    if int(rot):
        # ROTATE_270 undoes the panel's 90 degree mount, so the result reads
        # the way it does on a wall-mounted display.
        im = im.transpose(Image.ROTATE_270)
    im.save(os.path.join(out, "%s_%s_%s.png" % (name, theme, orient)))
    n += 1
print(n)
'@
$pyFile = Join-Path $tmpRoot "convert.py"
Set-Content -Path $pyFile -Value $py -Encoding utf8

$total = 0
foreach ($p in $passes) {
    $dir = Join-Path $tmpRoot "$($p.Theme)_$($p.Orient)"
    $n = & python $pyFile $dir $OUT $p.Theme $p.Orient $p.Rot
    Write-Host ("  {0,-6} {1,-10} {2} images" -f $p.Theme, $p.Orient, $n)
    $total += [int]$n
}

if (-not $KeepTemp) { Remove-Item -Recurse -Force $tmpRoot -ErrorAction SilentlyContinue }
Write-Host ""
Write-Host "$total screenshots written to docs\img\hires" -ForegroundColor Green
