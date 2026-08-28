param(
    [string]$Action    = "",
    [string]$Board     = "",  # 4 | 7b | 5b | sim | sim7b   (empty = ask)
    [switch]$FullDeploy,
    [switch]$FsOnly,
    [switch]$Simulator,
    [switch]$Build,
    [switch]$Monitor,
    [switch]$Clean,
    [switch]$Help,
    [string]$Port      = ""   # serial port; empty = detect, and ask when unclear
)

$PROJECT = $PSScriptRoot

# PlatformIO's own directory. Everything else here derives from it: the pio
# executable, the build output, the 4-inch package dir. Respecting
# PLATFORMIO_CORE_DIR matters because a PlatformIO IDE install can put it
# elsewhere, and because a machine with the core on another drive would
# otherwise get three subtly different answers from three places.
$PIO_CORE = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR }
            else { Join-Path $HOME ".platformio" }

# The bundled interpreter first, a pio on PATH second (pipx / pip install
# platformio puts it there and never creates a penv).
$PIO = Join-Path $PIO_CORE "penv\Scripts\pio.exe"
if (-not (Test-Path $PIO)) {
    $onPath = Get-Command pio -ErrorAction SilentlyContinue
    if ($onPath) { $PIO = $onPath.Source }
}

# Exit code of the last pio run. Deliberately NOT the return value of
# Invoke-PIO: pio's console output travels through the pipeline too, so
# "$rc = Invoke-PIO ..." would collect every build line into $rc alongside the
# number. A script-scope variable keeps the two apart.
$PioExit = 1

# Is there a console to ask questions on? A double-clicked deploy.bat has one;
# a scripted/redirected run does not, and there we refuse rather than block.
$IsInteractive = $true
try { if ([Console]::IsInputRedirected) { $IsInteractive = $false } } catch { }

# Which boards have already been through Test-Prerequisites in this run.
$script:PreflightDone = @{}

# ── THE BOARDS ───────────────────────────────────────────────────────────────
# Everything the tool needs to know per target. Usb is how the board shows up
# on USB (see Get-SerialPortInfo); Sim is the simulator env that matches this
# board's screen; Isolate marks the env that needs its own package/build dirs.
$BOARDS = @{
    "4" = @{
        Env     = "waveshare_esp32s3_4"
        Label   = "4-inch   480x480"
        Note    = "RELEASED PRODUCT"
        Usb     = "native"
        Sim     = "sim"
        Kind    = "esp"
        Isolate = $true
    }
    "7b" = @{
        Env     = "waveshare_esp32s3_7b"
        Label   = "7-inch  1024x600"
        Note    = "CH343 bridge, DIP switch on UART1"
        Usb     = "ch343"
        Sim     = "sim7b"
        Kind    = "esp"
        Isolate = $false
    }
    "5b" = @{
        Env     = "waveshare_esp32s3_5b"
        Label   = "5-inch  1024x600"
        Note    = "native USB, same UI as the 7-inch"
        Usb     = "native"
        Sim     = "sim7b"
        Kind    = "esp"
        Isolate = $false
    }
    "sim" = @{
        Env     = "simulator"
        Label   = "PC simulator  480x480"
        Note    = "no hardware needed"
        Usb     = ""
        Sim     = "sim"
        Kind    = "sim"
        Isolate = $false
    }
    "sim7b" = @{
        Env     = "simulator_7b"
        Label   = "PC simulator 1024x600"
        Note    = "matches the panel boards, used for the doc screenshots"
        Usb     = ""
        Sim     = "sim7b"
        Kind    = "sim"
        Isolate = $false
    }
}

# Menu order, and the order the help lists them in.
$BOARD_ORDER = @("4", "7b", "5b", "sim", "sim7b")

# What you type to pick each entry. The simulators get LETTERS on purpose.
# Numbering the list 1..5 put a "[4]" and a "[5]" on screen next to the PC
# simulators, while "4" and "5" are also the names of the 4-inch and 5-inch
# BOARDS - so the menu showed [4] = simulator to a user whose fingers meant
# the 4-inch. One of the two readings had to lose silently. Keeping the menu
# keys clear of every board name removes the collision instead of arbitrating
# it: no input below has two meanings. If a board or alias is ever added here,
# keep that true.
$BOARD_MENUKEY = @{
    "4"     = "1"
    "7b"    = "2"
    "5b"    = "3"
    "sim"   = "s"
    "sim7b" = "s7"
}

# Everything a user might reasonably type for a board.
$BOARD_ALIASES = @{
    "4"     = "4";     "4in"  = "4";    "4inch" = "4";    "480" = "4"
    "waveshare_esp32s3_4" = "4"
    "7"     = "7b";    "7b"   = "7b";   "7in"   = "7b";   "7inch" = "7b"
    "waveshare_esp32s3_7b" = "7b"
    "5"     = "5b";    "5b"   = "5b";   "5in"   = "5b";   "5inch" = "5b"
    "waveshare_esp32s3_5b" = "5b"
    "sim"   = "sim";   "simulator" = "sim"
    "sim7"  = "sim7b"; "sim7b" = "sim7b"; "sim7inch" = "sim7b"
    "simulator_7b" = "sim7b"
}

# bringup_7b and bringup_5b exist in platformio.ini but are deliberately absent
# here: they are panel-only bring-up sketches, not firmware anyone flashes from
# this tool. Build them with pio directly.

# ── THE 4-INCH BOARD'S PRIVATE DIRECTORIES ───────────────────────────────────
# These mirror tools/build4.ps1 exactly. Two variables, two separate reasons:
#
#   PLATFORMIO_PACKAGES_DIR - the 4-inch platform (espressif32@6.12.0) and the
#     panel boards' pioarduino platform resolve packages BY NAME in the one
#     shared package dir, and tool-esptoolpy lives there in two incompatible
#     flavours. Every board switch evicted the other side's copy. A private
#     package dir for this rarely-built legacy env is the clean cut.
#
#   PLATFORMIO_BUILD_DIR - changing the package dir makes PlatformIO consider
#     the project configuration different, so it WIPES the shared build_dir
#     from platformio.ini and takes the 5B and 7B firmware.bin with it. That
#     is silent: the build reports success and the binaries are simply gone at
#     flash time. Measured twice on 2026-08-22.
#
# Set inline rather than by calling tools/build4.ps1, because this script has
# to stream pio's output live and keep its exit code - and because "& build4"
# would set these same process-wide variables anyway without ever unsetting
# them. Invoke-PIO saves and restores both around EVERY call, so a 4-inch
# action cannot leak its directories into the next action of the same
# interactive session. That leak is the one bug that would quietly send the
# panel builds into the 4-inch directories, so it is handled in one place.
# Derived from $PIO_CORE, not from $env:USERPROFILE, so these agree with
# tools\build4.ps1 - which computes exactly the same two paths. They used to
# differ once PLATFORMIO_CORE_DIR was set, and then the launcher and the direct
# script built into two different directories.
$PKGDIR_4INCH   = Join-Path $PIO_CORE "packages-4inch"
$BUILDDIR_4INCH = Join-Path $PIO_CORE "build\nauticpinnace-4inch"

# The simulator needs the same separation, for the same reason and a different
# cause. It runs on the `native` platform while the panel boards run on
# espressif32, so PlatformIO sees a different project configuration and wipes
# the shared build_dir - which is where the 5-inch and 7-inch firmware.bin
# live. Measured on 2026-08-22: after a simulator build the build directory
# held nothing but `simulator_7b`, and a flash attempt failed with the binaries
# simply gone. It costs one full simulator rebuild, once.
#
# Only the BUILD dir is separated here, not the packages dir: the native
# platform's toolchain does not collide with anything: that fight is specific
# to tool-esptoolpy between the two ESP32 platforms.
# Same root as build_dir in platformio.ini and as tools\build4.ps1 uses, so all
# build output of this project sits together under PlatformIO's own directory
# instead of in three different places. $PIO_CORE is resolved at the top.
$BUILDDIR_SIM   = Join-Path $PIO_CORE "build\nauticpinnace-sim"

# Add MinGW from MSYS2 to PATH (if present)
$mingwPaths = @(
    "C:\msys64\mingw64\bin",
    "C:\mingw64\bin",
    "C:\MinGW\bin",
    "$env:USERPROFILE\scoop\apps\gcc\current\bin"
)
foreach ($mp in $mingwPaths) {
    if (Test-Path "$mp\g++.exe") {
        if ($env:PATH -notlike "*$mp*") {
            $env:PATH = "$mp;$env:PATH"
        }
        break
    }
}

# ── PREFLIGHT ────────────────────────────────────────────────────────────────
# Everything this project needs that it does NOT bring along, checked before the
# first compile rather than discovered halfway through one.
#
# The rule throughout: install only what is safe and reversible, and only after
# asking. Anything that touches a driver, the registry or needs elevation is
# reported with the exact command, never run from here.

# True if a real executable of that name is on PATH. -CommandType Application
# keeps PowerShell aliases and functions out of the answer.
function Test-Tool {
    param([string]$Name)
    return [bool](Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue)
}

function Write-Check {
    param([string]$Label, [bool]$Ok, [string]$Detail = "", [switch]$Warn)
    $mark = if ($Ok) { "  ok  " } elseif ($Warn) { " warn " } else { " MISS " }
    $col  = if ($Ok) { "Green" } elseif ($Warn) { "Yellow" } else { "Red" }
    Write-Host "  [" -NoNewline
    Write-Host $mark -ForegroundColor $col -NoNewline
    Write-Host "] " -NoNewline
    Write-Host $Label -NoNewline
    if ($Detail -ne "") { Write-Host "  $Detail" -ForegroundColor DarkGray } else { Write-Host "" }
}

# winget, with the agreement prompts pre-answered. Returns $true on success.
# Deliberately NOT silent: the user should see what is being installed.
function Install-WithWinget {
    param([string]$Id, [string]$Label)
    if (-not (Test-Tool "winget")) {
        Write-Host "    winget is not available - install $Label by hand." -ForegroundColor Yellow
        return $false
    }
    Write-Host "    Installing $Label via winget..." -ForegroundColor Cyan
    & winget install --id $Id -e --source winget `
        --accept-source-agreements --accept-package-agreements
    return ($LASTEXITCODE -eq 0)
}

function Confirm-Install {
    param([string]$Question)
    if (-not $IsInteractive) { return $false }
    Write-Host "    $Question [y/N] " -ForegroundColor Cyan -NoNewline
    $a = Read-Host
    return ($a -match '^(y|j)')
}

# The whole check. $BoardKey selects which of them apply; $Action decides
# whether the flashing-only ones run at all.
#
# Returns $true when the build may proceed.
function Test-Prerequisites {
    param([string]$BoardKey, [string]$Action)

    $isSim   = ($BoardKey -eq "sim" -or $BoardKey -eq "sim7b")
    $isPanel = ($BoardKey -eq "7b" -or $BoardKey -eq "5b")
    $blocking = @()

    Write-Host ""
    Write-Host "Checking prerequisites..." -ForegroundColor White

    # ---- PlatformIO Core -----------------------------------------------------
    # Was a one-line dead end before: it printed a path the user cannot create
    # and exited. PlatformIO is not on winget (verified), so this offers the
    # two real routes instead of pretending it can fix it.
    $pioOk = Test-Path $PIO
    Write-Check "PlatformIO Core" $pioOk $(if ($pioOk) { $PIO } else { "not found" })
    if (-not $pioOk) {
        Write-Host ""
        Write-Host "    PlatformIO builds this firmware. It is not on winget; two ways:" -ForegroundColor Yellow
        Write-Host "      a) VS Code + the 'PlatformIO IDE' extension - brings its own Python."
        Write-Host "         winget install --id Microsoft.VisualStudioCode -e"
        Write-Host "      b) CLI: install Python, then run the official bootstrap:"
        Write-Host "         winget install --id Python.Python.3.12 -e"
        Write-Host "         py -3 -c `"import urllib.request;exec(urllib.request.urlopen('https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py').read())`""
        Write-Host ""
        Write-Host "    Searched: $PIO" -ForegroundColor DarkGray
        Write-Host "    ...and PATH (a pip/pipx install is found there too)." -ForegroundColor DarkGray
        $blocking += "PlatformIO Core"
    }

    # ---- Git -----------------------------------------------------------------
    # Two independent hard requirements, neither of them obvious:
    #   1. The pioarduino platform used by 7B/5B runs shutil.which("git") when
    #      platform.py is imported and exits with "Git not found in PATH" - long
    #      before any library is touched.
    #   2. lib_deps pins libraries by git URL: three on the 4-inch (NMEA2000 and
    #      the two async ones), one on 7B/5B (NMEA2000).
    # Only the two simulator environments get by without it.
    if (-not $isSim) {
        $gitOk = Test-Tool "git.exe"
        Write-Check "Git" $gitOk $(if ($gitOk) { (& git --version) } else { "required for the pinned libraries" })
        if (-not $gitOk) {
            Write-Host "    PlatformIO fetches several libraries straight from git," -ForegroundColor Yellow
            Write-Host "    and the 7B/5B platform refuses to load without it." -ForegroundColor Yellow
            if (Confirm-Install "Install Git for Windows now?") {
                if (Install-WithWinget "Git.Git" "Git for Windows") {
                    Write-Host "    Installed. Close this window and start deploy.bat again -" -ForegroundColor Green
                    Write-Host "    PATH changes only reach NEW processes." -ForegroundColor Green
                }
            } else {
                Write-Host "    winget install --id Git.Git -e" -ForegroundColor DarkGray
            }
            $blocking += "Git"
        }
    }

    # ---- Whitespace in the toolchain paths (7B/5B only) ----------------------
    # The single most likely silent failure on a fresh Windows, and nobody's
    # fault: the HybridCompile path checks
    #     if any(" " in p for p in (FRAMEWORK_DIR, BUILD_DIR))
    # and aborts. FRAMEWORK_DIR lives under the PlatformIO core directory, which
    # defaults into the user's home - and Windows 11 builds the account name
    # from the Microsoft account, so "Hans Meier" is ordinary. build_dir in
    # platformio.ini only fixes BUILD_DIR; FRAMEWORK_DIR needs the packages
    # directory moved as well. Both are documented environment variables, so
    # this one CAN be fixed here, for this process only.
    if ($isPanel) {
        $pkgDir = if ($env:PLATFORMIO_PACKAGES_DIR) { $env:PLATFORMIO_PACKAGES_DIR }
                  else { Join-Path $PIO_CORE "packages" }
        $bldDir = if ($env:PLATFORMIO_BUILD_DIR) { $env:PLATFORMIO_BUILD_DIR }
                  else { Join-Path $PIO_CORE "build\nauticpinnace" }
        $spaced = ($pkgDir -like '* *') -or ($bldDir -like '* *')
        Write-Check "Toolchain path without spaces" (-not $spaced) `
            $(if ($spaced) { "'$pkgDir' contains a space" } else { $pkgDir })
        if ($spaced) {
            Write-Host "    The 1024x600 build rebuilds the Arduino IDF layer and refuses" -ForegroundColor Yellow
            Write-Host "    any space in its paths. Your home directory has one." -ForegroundColor Yellow
            Write-Host "    Redirecting both to C:\pio for this run." -ForegroundColor Cyan
            Write-Host "    (First build there downloads the toolchains again, ~5 GB.)" -ForegroundColor DarkGray
            Set-EnvVar "PLATFORMIO_PACKAGES_DIR" "C:\pio\packages"
            Set-EnvVar "PLATFORMIO_BUILD_DIR"    "C:\pio\build\nauticpinnace"
            Write-Host "    To make it permanent, create C:\.platformio BEFORE installing" -ForegroundColor DarkGray
            Write-Host "    PlatformIO - it prefers a drive-root .platformio over the home dir." -ForegroundColor DarkGray
        }

        # ---- MAX_PATH ------------------------------------------------------
        # This one actually stops the build, and the error it produces names
        # the wrong problem. esp32-core-3.3.11-libs.tar.xz carries the whole
        # Matter/connectedhomeip tree; its longest member is 184 characters of
        # relative path, and unpacking adds 48 more for
        # \tmp\pkg-installing-XXXXXXXX\esp32-arduino-libs\. Anything over 260
        # in total fails while Windows says ERROR_PATH_NOT_FOUND - which Python
        # reports as "FileNotFoundError: No such file or directory", pointing
        # at a file it was trying to CREATE. Observed on a fresh machine with
        # user "Test": the cache path came to 264 characters and the install
        # died four characters over the line.
        #
        # Two ways out. Enabling long paths is the real fix but needs admin and
        # a reboot. Shortening the two directories needs neither, and the
        # arithmetic above says exactly how short they have to be:
        #   cache    <= 27 characters  (259 - 184 - 48)
        #   core dir <= 31 characters  (259 - 184 - len("\packages\framework-arduinoespressif32-libs\"))
        $lp = 0
        try {
            $lp = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' `
                   -Name LongPathsEnabled -ErrorAction SilentlyContinue).LongPathsEnabled
        } catch { $lp = 0 }
        $lpOk = ($lp -eq 1)

        if ($lpOk) {
            Write-Check "Windows long path support" $true "enabled"
        } else {
            $cacheDir = if ($env:PLATFORMIO_CACHE_DIR) { $env:PLATFORMIO_CACHE_DIR }
                        else { Join-Path $PIO_CORE ".cache" }
            $cacheFits = ($cacheDir.Length -le 27)
            $coreFits  = ((($env:PLATFORMIO_PACKAGES_DIR) -ne $null -and $env:PLATFORMIO_PACKAGES_DIR.Length -le 44) `
                          -or $PIO_CORE.Length -le 31)
            $pathsOk = $cacheFits -and $coreFits

            Write-Check "Path length (long paths are off)" $pathsOk `
                $(if ($pathsOk) { "short enough without them" }
                  else { "cache path is $($cacheDir.Length) chars, needs <= 27" })

            if (-not $pathsOk) {
                Write-Host "    Long paths are disabled and your PlatformIO directory is too" -ForegroundColor Yellow
                Write-Host "    deep for the Arduino IDF package (its longest file needs 232" -ForegroundColor Yellow
                Write-Host "    characters of room). Unpacking would fail with a misleading" -ForegroundColor Yellow
                Write-Host "    'FileNotFoundError - No such file or directory'." -ForegroundColor Yellow
                Write-Host "    Redirecting cache and packages to C:\pio for this run." -ForegroundColor Cyan
                Set-EnvVar "PLATFORMIO_CACHE_DIR"    "C:\pio\cache"
                if (-not $cacheFits -or -not $coreFits) {
                    Set-EnvVar "PLATFORMIO_PACKAGES_DIR" "C:\pio\packages"
                    Set-EnvVar "PLATFORMIO_BUILD_DIR"    "C:\pio\build\nauticpinnace"
                }
                Write-Host "    The permanent fix, once, in an ADMIN PowerShell + reboot:" -ForegroundColor DarkGray
                Write-Host "    New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force" -ForegroundColor DarkGray
            }
        }
    }

    # ---- Simulator toolchain -------------------------------------------------
    # 'platform = native' ships no compiler: SCons calls env.Tool("g++") and
    # takes whatever is on PATH. Without it SCons still sets CXX to the literal
    # "g++" and the build runs until the first compile fails - which is why this
    # is checked up front rather than left to a confusing linker error.
    if ($isSim) {
        $gxx = Get-Command g++.exe -CommandType Application -ErrorAction SilentlyContinue
        if (-not $gxx) {
            foreach ($d in @("C:\msys64\mingw64\bin", "C:\mingw64\bin", "C:\MinGW\bin",
                             (Join-Path $HOME "scoop\apps\gcc\current\bin"), "C:\w64devkit\bin")) {
                if (Test-Path (Join-Path $d "g++.exe")) { $gxx = @{ Source = (Join-Path $d "g++.exe") }; break }
            }
        }
        $gxxOk = [bool]$gxx
        Write-Check "MinGW g++ (simulator)" $gxxOk $(if ($gxxOk) { $gxx.Source } else { "not found" })
        if (-not $gxxOk) {
            Write-Host "    The simulator is built with GCC, never MSVC (-lmingw32, -lSDL2main)." -ForegroundColor Yellow
            if (Confirm-Install "Install MSYS2 now? (the GCC package still needs a second step)") {
                if (Install-WithWinget "MSYS2.MSYS2" "MSYS2") {
                    Write-Host "    Now run, once:" -ForegroundColor Green
                    Write-Host "      C:\msys64\usr\bin\pacman.exe -Sy --noconfirm" -ForegroundColor Green
                    Write-Host "      C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-x86_64-gcc" -ForegroundColor Green
                }
            } else {
                Write-Host "    winget install --id MSYS2.MSYS2 -e" -ForegroundColor DarkGray
                Write-Host "    or unpack a WinLibs UCRT build to C:\mingw64" -ForegroundColor DarkGray
            }
            $blocking += "MinGW g++"
        }

        # SDL2: the HEADERS are what the compile needs. The old check looked for
        # SDL2.dll alone and accepted a scoop or Chocolatey copy - neither of
        # which puts include/ or lib/ where platformio.ini points
        # (-IC:/SDL2/include -LC:/SDL2/lib), so the build failed with
        # "SDL2/SDL.h: No such file or directory" while the check said fine.
        $sdlHdr = Test-Path "C:\SDL2\include\SDL2\SDL.h"
        $sdlLib = Test-Path "C:\SDL2\lib\libSDL2main.a"
        $sdlDll = Test-Path "C:\SDL2\bin\SDL2.dll"
        $sdlOk  = $sdlHdr -and $sdlLib -and $sdlDll
        Write-Check "SDL2 development package" $sdlOk `
            $(if ($sdlOk) { "C:\SDL2" } elseif ($sdlDll) { "DLL only - headers missing" } else { "not found" })
        if (-not $sdlOk) {
            Write-Host "    Downloaded automatically when you run the simulator action." -ForegroundColor DarkGray
        }
    }

    # ---- Disk space ----------------------------------------------------------
    # First build pulls whole toolchains. Measured on the author's machine:
    # ~10.7 GB in the PlatformIO directory once all three boards have been built
    # (the 4-inch keeps a second, private package directory on purpose).
    $needGb = if ($isSim) { 2 } elseif ($isPanel) { 8 } else { 4 }
    try {
        $drive = (Get-Item $PIO_CORE -ErrorAction SilentlyContinue)
        $root  = if ($drive) { [System.IO.Path]::GetPathRoot($drive.FullName) }
                 else { [System.IO.Path]::GetPathRoot($PIO_CORE) }
        $free  = (Get-PSDrive -Name $root.Substring(0,1) -ErrorAction SilentlyContinue).Free
        if ($free) {
            $freeGb = [math]::Round($free / 1GB, 1)
            $spaceOk = ($freeGb -ge $needGb)
            Write-Check "Disk space on $root" $spaceOk "$freeGb GB free, about $needGb GB needed" -Warn:(-not $spaceOk)
        }
    } catch { }

    Write-Host ""
    if ($blocking.Count -gt 0) {
        Write-Host "Missing: $($blocking -join ', ')" -ForegroundColor Red
        Write-Host "Install the above, open a NEW window, and start again." -ForegroundColor Yellow
        Write-Host ""
        return $false
    }
    return $true
}

# ── HELPERS ──────────────────────────────────────────────────────────────────

function Resolve-BoardKey {
    param([string]$Raw)
    if ($null -eq $Raw) { return "" }
    $k = $Raw.Trim().ToLower()
    if ($k -eq "") { return "" }
    if ($BOARD_ALIASES.ContainsKey($k)) { return $BOARD_ALIASES[$k] }
    return ""
}

function Set-EnvVar {
    param([string]$Name, $Value)
    # Assigning an empty value leaves an empty variable behind instead of
    # removing it, and "empty" is not the same as "never set" to PlatformIO.
    # So restore the absent case by actually removing the entry.
    if ($null -eq $Value -or $Value -eq "") {
        if (Test-Path "Env:\$Name") { Remove-Item "Env:\$Name" -Force }
    } else {
        Set-Item "Env:\$Name" -Value $Value
    }
}

# THE CHOSEN PORT NEVER TRAVELS IN THE ENVIRONMENT - it goes on the command
# line as --upload-port, and callers pass it inside $PArgs.
#
# PLATFORMIO_UPLOAD_PORT looks like the convenient way to hand the port down to
# child processes, and it is a trap. upload_port is declared with
# sysenvvar="PLATFORMIO_UPLOAD_PORT", so PlatformIO folds the variable into the
# project configuration it serialises and hashes. "pio run" compares that hash
# against build_dir\project.checksum FIRST THING and, on a mismatch, deletes
# the ENTIRE build_dir before it builds anything. Measured on this machine:
# the shared build dir's stored checksum is 1a7ada71..., unchanged with no
# variable set, but a54af3ba... with PLATFORMIO_UPLOAD_PORT=COM12 and
# 85a69b73... with COM8. So one upload would wipe the 4-inch, 5B, 7B and
# simulator output out of the build dir, and the next plain build would flip
# the hash back and wipe it again - the exact silent wipe tools\build4.ps1
# was written to stop.
# The command-line --upload-port is safe: pio hands it to the environment
# processor only AFTER the build-dir check has already run, so it never
# reaches the hashed configuration.
function Invoke-PIO {
    param(
        [string]$BoardKey,        # which board this call is for
        [string[]]$PArgs          # pio arguments WITHOUT -e (added here)
    )

    $envId = $BOARDS[$BoardKey].Env
    $full  = @() + $PArgs + @("-e", $envId)

    # Preflight, once per board per run. This function is the single funnel
    # every action goes through - build, upload, uploadfs, deploy, monitor,
    # clean, simulator - so hooking it here covers the menu and the one-shot
    # command forms without either having to remember. A "deploy" runs pio
    # twice; the second call skips the check.
    #
    # It runs BEFORE the save/restore below on purpose: if the check redirects
    # the package and build directories (the whitespace case on 7B/5B), that
    # redirect is then part of the saved state and survives the call.
    if (-not $script:PreflightDone.ContainsKey($BoardKey)) {
        if (-not (Test-Prerequisites $BoardKey $(if ($PArgs.Count -gt 0) { $PArgs[0] } else { "" }))) {
            $script:PioExit = 1
            return
        }
        $script:PreflightDone[$BoardKey] = $true
    }

    # Saved so the restore below is exact - including "was not set at all",
    # which is the normal case.
    $savedPkg = $env:PLATFORMIO_PACKAGES_DIR
    $savedBld = $env:PLATFORMIO_BUILD_DIR

    $script:PioExit = 1
    try {
        if ($BOARDS[$BoardKey].Isolate) {
            Set-EnvVar "PLATFORMIO_PACKAGES_DIR" $PKGDIR_4INCH
            Set-EnvVar "PLATFORMIO_BUILD_DIR"    $BUILDDIR_4INCH
            Write-Host ""
            Write-Host "  4-inch isolation active for this call:" -ForegroundColor DarkGray
            Write-Host "    PLATFORMIO_PACKAGES_DIR = $PKGDIR_4INCH" -ForegroundColor DarkGray
            Write-Host "    PLATFORMIO_BUILD_DIR    = $BUILDDIR_4INCH" -ForegroundColor DarkGray
        }
        elseif ($BOARDS[$BoardKey].Kind -eq "sim") {
            # Build dir only - see the note next to $BUILDDIR_SIM. Without this
            # a simulator build deletes the two panel boards' firmware.bin and
            # the next flash fails with nothing to flash.
            Set-EnvVar "PLATFORMIO_BUILD_DIR" $BUILDDIR_SIM
            Write-Host ""
            Write-Host "  Simulator build dir for this call:" -ForegroundColor DarkGray
            Write-Host "    PLATFORMIO_BUILD_DIR    = $BUILDDIR_SIM" -ForegroundColor DarkGray
        }

        Write-Host ""
        Write-Host ("  pio " + ($full -join " ")) -ForegroundColor Cyan
        Write-Host ""
        & $PIO @full
        $script:PioExit = $LASTEXITCODE
    } finally {
        # Unconditional: even a failed or interrupted call must not leave the
        # 4-inch directories behind for the next action.
        Set-EnvVar "PLATFORMIO_PACKAGES_DIR" $savedPkg
        Set-EnvVar "PLATFORMIO_BUILD_DIR"    $savedBld
    }
}

# "Press any key" that a scripted run cannot hang on. RawUI.ReadKey needs a
# real console: with input redirected it blocks or throws, and an unattended
# deploy.bat would sit there forever waiting for a keyboard nobody is at.
function Wait-ForKey {
    param([string]$Message = "  Press any key to continue...")
    if (-not $IsInteractive) { return }
    Write-Host $Message -ForegroundColor DarkGray
    try {
        $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    } catch {
        # Hosts without a RawUI keyboard (the ISE, for one) throw instead.
        # Falling back to ENTER is better than dying on the way out.
        $null = Read-Host
    }
}

function Show-Result {
    param([int]$Code, [string]$OkMsg, [string]$FailMsg)
    if ($Code -eq 0) {
        Write-Host ""
        Write-Host "  $OkMsg" -ForegroundColor Green
    } else {
        Write-Host ""
        Write-Host "  $FailMsg (exit: $Code)" -ForegroundColor Red
    }
    Write-Host ""
    Read-Host "  Press ENTER to return to the menu"
}

# Locate the SDL2 DLL (for the simulator)
function Find-SDL2Dll {
    $candidates = @(
        "$env:USERPROFILE\scoop\apps\sdl2\current\bin\SDL2.dll",
        "C:\SDL2\bin\SDL2.dll",
        "C:\ProgramData\chocolatey\lib\sdl2\tools\x64\SDL2.dll",
        "$PSScriptRoot\tools\sdl2\bin\SDL2.dll"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

# Where PlatformIO actually puts its build output. NOT hard-coded: the project
# moved build_dir out of the project tree once already (the panel builds refuse
# a path containing whitespace, and "Waveshare ESP32-S3" has a space in it), so
# the value is read back from the same places PlatformIO reads it from - the
# environment first, then the [platformio] section of platformio.ini, then the
# built-in default.
function Get-BuildDir {
    if ($env:PLATFORMIO_BUILD_DIR -and $env:PLATFORMIO_BUILD_DIR -ne "") {
        return $env:PLATFORMIO_BUILD_DIR
    }
    $ini = Join-Path $PROJECT "platformio.ini"
    if (Test-Path $ini) {
        $inCore = $false
        foreach ($raw in (Get-Content $ini)) {
            $line = $raw.Trim()
            if ($line -eq "" -or $line.StartsWith(";") -or $line.StartsWith("#")) { continue }
            if ($line -match '^\[(.+)\]\s*$') {
                $inCore = ($Matches[1].Trim() -eq "platformio")
                continue
            }
            if (-not $inCore) { continue }
            if ($line -match '^build_dir\s*=\s*(.+)$') {
                $val  = $Matches[1]
                $semi = $val.IndexOf(";")
                if ($semi -ge 0) { $val = $val.Substring(0, $semi) }
                $val = $val.Trim()
                # platformio.ini may interpolate. build_dir is written as
                # ${platformio.core_dir}/build/nauticpinnace so the file carries
                # no author's home directory - PlatformIO expands that, this
                # script has to do it too or it hands back a literal "${...}"
                # that no Test-Path will ever match.
                if ($val -match '\$\{platformio\.core_dir\}') {
                    $core = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR }
                            else { Join-Path $HOME ".platformio" }
                    $val = $val -replace '\$\{platformio\.core_dir\}', $core.Replace('\', '\\')
                }
                # Anything still unexpanded is a path we cannot resolve; saying
                # "unknown" is honest, returning a broken path is not.
                if ($val -match '\$\{') { return "" }
                if ($val -ne "") { return $val }
            }
        }
    }
    return (Join-Path $PROJECT ".pio\build")
}

# The tried-and-failed list, for a useful error message.
$SimTried = @()

function Resolve-SimulatorExe {
    param([string]$SimEnv)
    $tries = @()
    # The simulator now builds into its own directory (see $BUILDDIR_SIM), so
    # that is where it lands - look there before anything the ini says.
    $tries += (Join-Path $BUILDDIR_SIM "$SimEnv\program.exe")
    $bd = Get-BuildDir
    if ($bd -ne "") { $tries += (Join-Path $bd "$SimEnv\program.exe") }
    # Fallbacks, in case the ini is unreadable or build_dir moves again.
    $tries += (Join-Path $PROJECT ".pio\build\$SimEnv\program.exe")
    $tries += (Join-Path $env:USERPROFILE ".pio-build\nauticpinnace\$SimEnv\program.exe")
    $script:SimTried = $tries
    foreach ($t in $tries) {
        if (Test-Path $t) { return $t }
    }
    return ""
}

# Every COM port Windows knows about, with the USB VID/PID that identifies it.
#   VID_1A86 (CH343 bridge)        -> the 7-inch board, unambiguous
#   VID_303A PID_1001 (native USB) -> the 4-inch OR the 5-inch; the ESP32-S3's
#                                     own USB-Serial/JTAG reports the same IDs
#                                     on both, so these CANNOT be told apart.
function Get-SerialPortInfo {
    $result = @()
    try {
        $devs = @(Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
                  Where-Object { $_.Name -match 'COM\d+' })
    } catch {
        Write-Host "  Could not query the serial ports: $_" -ForegroundColor Yellow
        return $result
    }
    foreach ($d in $devs) {
        $m = [regex]::Match($d.Name, '\((COM\d+)\)')
        if (-not $m.Success) { continue }

        $hw = ""
        if ($d.HardwareID) { $hw = [string]::Join(" ", $d.HardwareID) }

        # NOTE: not $vid/$pid - $PID is a read-only automatic variable and
        # assigning to it throws.
        $vidHex = ""
        $pidHex = ""
        $vm = [regex]::Match($hw, 'VID_([0-9A-Fa-f]{4})')
        if ($vm.Success) { $vidHex = $vm.Groups[1].Value.ToUpper() }
        $pm = [regex]::Match($hw, 'PID_([0-9A-Fa-f]{4})')
        if ($pm.Success) { $pidHex = $pm.Groups[1].Value.ToUpper() }

        $usb = "other"
        if ($vidHex -eq "303A" -and $pidHex -eq "1001") { $usb = "native" }
        elseif ($vidHex -eq "1A86")                     { $usb = "ch343" }

        $result += New-Object PSObject -Property @{
            Port = $m.Groups[1].Value
            Name = $d.Name
            Vid  = $vidHex
            Pid  = $pidHex
            Usb  = $usb
        }
    }
    return @($result | Sort-Object { [int](($_.Port) -replace 'COM', '') })
}

function Format-PortLine {
    param($P, [int]$Index)
    $ids = "VID/PID unknown"
    if ($P.Vid -ne "") { $ids = "VID_$($P.Vid) PID_$($P.Pid)" }
    $tag = "not a NauticPinnace board"
    if ($P.Usb -eq "ch343")  { $tag = "CH343 bridge -> 7-inch" }
    if ($P.Usb -eq "native") { $tag = "native USB   -> 4-inch OR 5-inch" }
    Write-Host ("   [{0}] {1,-7} {2,-22} {3}" -f $Index, $P.Port, $ids, $tag)
    Write-Host ("        {0}" -f $P.Name) -ForegroundColor DarkGray
}

# Decide which COM port an upload/monitor should use. Returns "" when the user
# cancels or when nothing can be decided - the caller then aborts the action.
# The rule: pick automatically ONLY when exactly one port matches the selected
# board, and always say which port and why. Never guess between two.
function Select-UploadPort {
    param([string]$BoardKey)

    $wantUsb = $BOARDS[$BoardKey].Usb
    $all     = @(Get-SerialPortInfo)

    Write-Host ""
    Write-Host "  SERIAL PORT" -ForegroundColor Cyan
    if ($all.Count -eq 0) {
        Write-Host "  No serial ports found at all." -ForegroundColor Red
        Write-Host "  Plug the board in (and for the 7-inch: DIP switch on UART1)." -ForegroundColor Yellow
        return ""
    }

    $cand = @($all | Where-Object { $_.Usb -eq $wantUsb })

    if ($cand.Count -eq 1) {
        $p = $cand[0]
        if ($wantUsb -eq "ch343") {
            Write-Host ("  Using $($p.Port) - the only CH343 bridge (VID_$($p.Vid) PID_$($p.Pid)) attached,") -ForegroundColor Green
            Write-Host  "  and the CH343 is what identifies the 7-inch board." -ForegroundColor Green
        } else {
            Write-Host ("  Using $($p.Port) - the only native USB-Serial/JTAG port (VID_303A PID_1001) attached.") -ForegroundColor Green
            Write-Host  "  CAUTION: the 4-inch and the 5-inch report the SAME USB ID, so this port" -ForegroundColor Yellow
            Write-Host ("           could belong to either. You selected: {0} ({1})." -f $BOARDS[$BoardKey].Label, $BOARDS[$BoardKey].Env) -ForegroundColor Yellow
        }
        Write-Host ("  $($p.Name)") -ForegroundColor DarkGray
        return $p.Port
    }

    if ($cand.Count -eq 0) {
        $expect = "VID_303A PID_1001 (native USB-Serial/JTAG)"
        if ($wantUsb -eq "ch343") { $expect = "VID_1A86 (CH343 bridge)" }
        Write-Host ("  No attached port matches the {0}; expected {1}." -f $BOARDS[$BoardKey].Label, $expect) -ForegroundColor Yellow
        Write-Host  "  Listing every port instead - pick one only if you know it is right." -ForegroundColor Yellow
        $cand = $all
    } else {
        Write-Host ("  {0} ports match the {1}. They cannot be told apart by USB ID," -f $cand.Count, $BOARDS[$BoardKey].Label) -ForegroundColor Yellow
        Write-Host  "  so this tool will not guess. Pick the right one:" -ForegroundColor Yellow
    }

    if (-not $IsInteractive) {
        Write-Host "  The port is ambiguous and there is no console to ask on." -ForegroundColor Red
        Write-Host "  Re-run with -Port COMx (or deploy.bat <action> <board> COMx)." -ForegroundColor Red
        return ""
    }

    while ($true) {
        Write-Host ""
        for ($i = 0; $i -lt $cand.Count; $i++) {
            Format-PortLine ($cand[$i]) ($i + 1)
        }
        Write-Host "   [0] Cancel - flash nothing"
        Write-Host ""
        $sel = Read-Host "  Port (0-$($cand.Count), or the port name such as COM8)"
        if ($null -eq $sel) { $sel = "" }
        $sel = $sel.Trim()
        if ($sel -eq "0") { return "" }

        # A bare number has two readings here: the position in the list above,
        # and the COM number itself. Someone with COM8 in mind types "8", and
        # if eight ports are listed that is also entry 8 - a different board.
        # Both readings are worked out and a clash is put back to the user;
        # this is the one prompt in the tool where guessing wrong means
        # flashing the wrong board.
        $byIndex = ""
        $n = 0
        if ([int]::TryParse($sel, [ref]$n)) {
            if ($n -ge 1 -and $n -le $cand.Count) { $byIndex = $cand[$n - 1].Port }
        }
        $wanted = $sel.ToUpper()
        if ($wanted -notlike "COM*") { $wanted = "COM$wanted" }
        $byName = ""
        foreach ($p in $cand) {
            if ($p.Port -eq $wanted) { $byName = $p.Port }
        }

        if ($byIndex -ne "" -and $byName -ne "" -and $byIndex -ne $byName) {
            Write-Host ""
            Write-Host "  '$sel' could mean two different ports:" -ForegroundColor Yellow
            Write-Host ("    [a] entry {0} in the list = {1}" -f $sel, $byIndex)
            Write-Host ("    [b] the port named {0}" -f $byName)
            $which = Read-Host "  Which one (a/b)"
            if ($null -eq $which) { $which = "" }
            $which = $which.Trim().ToLower()
            if ($which -eq "a") { return $byIndex }
            if ($which -eq "b") { return $byName }
            Write-Host "  Nothing chosen - here is the list again." -ForegroundColor Yellow
            continue
        }
        if ($byIndex -ne "") { return $byIndex }
        if ($byName  -ne "") { return $byName }
        Write-Host "  Not a valid choice." -ForegroundColor Yellow
    }
}

function Show-Banner {
    Write-Host ""
    Write-Host "  =====================================" -ForegroundColor Cyan
    Write-Host "   NauticPinnace - Deploy Tool         " -ForegroundColor Cyan
    Write-Host "  =====================================" -ForegroundColor Cyan
    Write-Host ""
}

# Returns a board key, or "" for cancel/quit.
function Read-BoardChoice {
    while ($true) {
        Write-Host "  SELECT THE TARGET BOARD" -ForegroundColor Cyan
        Write-Host ""
        foreach ($k in $BOARD_ORDER) {
            $bd = $BOARDS[$k]
            Write-Host ("   [{0,-2}] {1,-22} {2,-22} {3}" -f $BOARD_MENUKEY[$k], $bd.Label, $bd.Env, $bd.Note)
        }
        Write-Host "   [0 ] Quit"
        Write-Host ""
        Write-Host "  The board's own name works too: 4  7b  5b  sim  sim7b" -ForegroundColor DarkGray
        Write-Host ""
        $sel = Read-Host "  Board"
        Write-Host ""
        if ($null -eq $sel) { $sel = "" }
        $sel = $sel.Trim().ToLower()
        if ($sel -eq "0") { return "" }
        # By menu key first - that is what is on the screen.
        foreach ($k in $BOARD_ORDER) {
            if ($sel -eq $BOARD_MENUKEY[$k].ToLower()) { return $k }
        }
        # Then by name: "5b", "7inch", "sim7" and friends.
        $byName = Resolve-BoardKey $sel
        if ($byName -ne "") { return $byName }
        Write-Host "  Not a valid choice." -ForegroundColor Yellow
    }
}

# Returns an action key, or "board" / "quit" / "" (redraw).
function Read-ActionChoice {
    param([string]$BoardKey)
    $bd = $BOARDS[$BoardKey]
    Clear-Host
    Show-Banner
    # The selection stays on screen the whole time an action is being chosen -
    # no action in this tool is ever performed without the board being visible.
    Write-Host ("   Target: {0}   [{1}]" -f $bd.Label, $bd.Env) -ForegroundColor Yellow
    if ($bd.Isolate) {
        Write-Host  "           released product - builds in its own package/build dirs" -ForegroundColor DarkGray
    }
    Write-Host ""
    if ($bd.Kind -eq "sim") {
        Write-Host "  [4] Compile only             (no flash)"
        Write-Host "  [5] Build and run the simulator"
        Write-Host "  [7] Clear the build cache    (this env only)"
        Write-Host "  [8] Change the target board"
        Write-Host "  [0] Quit"
        Write-Host ""
        Write-Host "  Flashing and the serial monitor need real hardware: [8] to switch board." -ForegroundColor DarkGray
    } else {
        Write-Host "  [1] Flash firmware           (approx. 35 s)"
        Write-Host "  [2] LittleFS + firmware      (after web UI changes, approx. 4 min)"
        Write-Host "  [3] LittleFS only            (web UI files only)"
        Write-Host "  [4] Compile only             (no flash)"
        Write-Host ("  [5] Start the PC simulator   (runs {0})" -f $BOARDS[$bd.Sim].Env)
        Write-Host "  [6] Serial monitor           (view logs)"
        Write-Host "  [7] Clear the build cache    (this env only)"
        Write-Host "  [8] Change the target board"
        Write-Host "  [0] Quit"
    }
    Write-Host ""
    $sel = Read-Host "  Choice (0-8)"
    Write-Host ""
    switch ($sel) {
        "1" { return "upload" }
        "2" { return "deploy" }
        "3" { return "uploadfs" }
        "4" { return "build" }
        "5" { return "simulator" }
        "6" { return "monitor" }
        "7" { return "clean" }
        "8" { return "board" }
        "0" { return "quit" }
    }
    return ""
}

# ── ARGUMENT HANDLING ────────────────────────────────────────────────────────
# deploy.bat compatibility: map the -Action string onto the switches, exactly
# as before, so every existing invocation keeps its meaning.
$badAction = ""
if ($Action -ne "") {
    switch ($Action.Trim().ToLower()) {
        "upload"    { }
        "flash"     { }
        "deploy"    { $FullDeploy = $true }
        "uploadfs"  { $FsOnly     = $true }
        "fs"        { $FsOnly     = $true }
        "build"     { $Build      = $true }
        "monitor"   { $Monitor    = $true }
        "simulator" { $Simulator  = $true }
        "sim"       { $Simulator  = $true }
        "clean"     { $Clean      = $true }
        "help"      { $Help       = $true }
        "-h"        { $Help       = $true }
        "--help"    { $Help       = $true }
        "/?"        { $Help       = $true }
        default {
            # Not an action. "deploy.bat 5b" names only a board: accept it and
            # let the menu ask what to do with it.
            $maybe = Resolve-BoardKey $Action
            if ($maybe -ne "") {
                if ($Board -eq "") { $Board = $maybe }
                $Action = ""
            } else {
                $badAction = $Action
            }
        }
    }
}

# Collapse the switches into one action key, in the SAME precedence order the
# old if-chain used, so a mixed invocation behaves as it always did.
$ActionKey = ""
if     ($Clean)          { $ActionKey = "clean" }
elseif ($Monitor)        { $ActionKey = "monitor" }
elseif ($Simulator)      { $ActionKey = "simulator" }
elseif ($Build)          { $ActionKey = "build" }
elseif ($FsOnly)         { $ActionKey = "uploadfs" }
elseif ($FullDeploy)     { $ActionKey = "deploy" }
elseif ($Action -ne "")  { $ActionKey = "upload" }

if ($Help) {
    Write-Host ""
    Write-Host "NauticPinnace - Build and Deploy"
    Write-Host "=================================="
    Write-Host "THREE BOARDS, AND YOU ALWAYS PICK ONE. Nothing is flashed until you have."
    Write-Host ""
    Write-Host "  -Board 4       4-inch   480x480   waveshare_esp32s3_4   RELEASED PRODUCT"
    Write-Host "  -Board 7b      7-inch  1024x600   waveshare_esp32s3_7b"
    Write-Host "  -Board 5b      5-inch  1024x600   waveshare_esp32s3_5b"
    Write-Host "  -Board sim     PC simulator  480x480   simulator"
    Write-Host "  -Board sim7b   PC simulator 1024x600   simulator_7b"
    Write-Host ""
    Write-Host "  4inch / 7 / 7inch / 5 / 5inch / simulator / sim7 are accepted too."
    Write-Host "  Leave -Board out and the tool asks; it never picks a board for you."
    Write-Host ""
    Write-Host "DIRECT:"
    Write-Host "  .\build_deploy.ps1 -Board 5b -Action upload   Flash firmware"
    Write-Host "  .\build_deploy.ps1 -Board 5b                  Menu, 5-inch preselected (flashes nothing)"
    Write-Host "  .\build_deploy.ps1 -Board 7b -FullDeploy      LittleFS + firmware"
    Write-Host "  .\build_deploy.ps1 -Board 7b -FsOnly          LittleFS only"
    Write-Host "  .\build_deploy.ps1 -Board sim7b -Simulator    Start the PC simulator"
    Write-Host "  .\build_deploy.ps1 -Board 4  -Build           Compile only"
    Write-Host "  .\build_deploy.ps1 -Board 7b -Monitor         Serial monitor"
    Write-Host "  .\build_deploy.ps1 -Board 5b -Clean           Clear the build cache"
    Write-Host "  -Port COMx pins the serial port for any of the above."
    Write-Host ""
    Write-Host "VIA deploy.bat:"
    Write-Host "  deploy.bat                     Interactive menu (board first, then action)"
    Write-Host "  deploy.bat upload 5b           Flash firmware"
    Write-Host "  deploy.bat deploy 7b           LittleFS + firmware"
    Write-Host "  deploy.bat uploadfs 7b         LittleFS only"
    Write-Host "  deploy.bat build 4             Compile only"
    Write-Host "  deploy.bat monitor 7b COM7     Serial monitor on a fixed port"
    Write-Host "  deploy.bat simulator sim7b     PC simulator, 1024x600"
    Write-Host "  deploy.bat simulator           PC simulator, 480x480 (no board needed)"
    Write-Host "  deploy.bat clean 5b            Clear the build cache"
    Write-Host "  deploy.bat 7b                  Menu with the 7-inch preselected"
    Write-Host "  deploy.bat help  (also -h, --help, /?)"
    Write-Host "  Leave the board off (deploy.bat upload) and you are asked for it."
    Write-Host ""
    Write-Host "SERIAL PORTS - the tool lists them with their USB IDs:"
    Write-Host "  VID_1A86            CH343 bridge, that is the 7-inch board"
    Write-Host "  VID_303A PID_1001   native USB - the 4-inch AND the 5-inch look"
    Write-Host "                      identical here, so with both attached you"
    Write-Host "                      are asked which port to use. No guessing."
    Write-Host ""
    Write-Host "THE 4-INCH BUILDS IN ITS OWN DIRECTORIES:"
    Write-Host "  PLATFORMIO_PACKAGES_DIR = $PKGDIR_4INCH"
    Write-Host "  PLATFORMIO_BUILD_DIR    = $BUILDDIR_4INCH"
    Write-Host "  Same as tools\build4.ps1. Without them the 4-inch build wipes the"
    Write-Host "  shared build dir and the 5B/7B firmware.bin go with it."
    Write-Host ""
    Write-Host "SIMULATOR - SDL2 and MinGW are detected automatically."
    Write-Host "  SDL2 manually: C:\SDL2\include\SDL2\SDL.h"
    Write-Host "  MinGW: C:\msys64\mingw64\bin\g++.exe (MSYS2)"
    Write-Host "  simulator = 480x480, simulator_7b = 1024x600 (the doc screenshots)."
    Write-Host ""
    Write-Host "NOT IN THIS TOOL: bringup_7b / bringup_5b (panel-only bring-up)."
    Write-Host "  pio run -e bringup_7b -t upload"
    Write-Host ""
    Write-Host "CHANGE UI PARAMETERS:"
    Write-Host "  src/display/UiConfig.h   colours, sizes, positions"
    Write-Host "  src/display/Theme.h      fonts, colour macros"
    Write-Host "  data/config.json         WiFi, brightness"
    exit 0
}

if ($badAction -ne "") {
    Write-Host ""
    Write-Host "  Unknown action: $badAction" -ForegroundColor Red
    Write-Host "  Actions: build upload uploadfs deploy monitor simulator clean help"
    Write-Host "  Boards:  4  7b  5b  sim  sim7b"
    Write-Host "  Example: deploy.bat upload 5b"
    Write-Host ""
    exit 2
}

Set-Location $PROJECT

$BoardKey = Resolve-BoardKey $Board
if ($Board -ne "" -and $BoardKey -eq "") {
    Write-Host ""
    Write-Host "  Unknown board: $Board" -ForegroundColor Red
    Write-Host "  Boards: 4 (4-inch)  7b (7-inch)  5b (5-inch)  sim  sim7b"
    Write-Host ""
    exit 2
}

# A direct invocation runs one action and exits; anything else is the menu.
$OneShot = ($ActionKey -ne "")

# The menu is unusable without a console to type into - Read-Host would return
# an empty string forever and the loop would spin. Say so and stop instead.
if ((-not $OneShot) -and (-not $IsInteractive)) {
    Write-Host ""
    Write-Host "  No action given, and there is no console to show the menu on." -ForegroundColor Red
    Write-Host "  Name an action and a board, e.g.  deploy.bat upload 5b" -ForegroundColor Red
    Write-Host "  Full list: deploy.bat help" -ForegroundColor Red
    Write-Host ""
    exit 2
}

# ── MAIN LOOP (menu) ─────────────────────────────────────────────────────────
$keepRunning = $true
while ($keepRunning) {

    if (-not $OneShot) {
        # Board first. Always. The action menu is not even drawn until a board
        # has been named, which is what makes "flashed the wrong board" hard.
        if ($BoardKey -eq "") {
            Clear-Host
            Show-Banner
            $BoardKey = Read-BoardChoice
            if ($BoardKey -eq "") {
                Wait-ForKey "  Quitting - press any key..."
                $keepRunning = $false
            }
            continue
        }

        $ActionKey = Read-ActionChoice $BoardKey
        if ($ActionKey -eq "quit") {
            Wait-ForKey "  Quitting - press any key..."
            $keepRunning = $false
            continue
        }
        if ($ActionKey -eq "board") { $BoardKey = ""; continue }
        if ($ActionKey -eq "")      { continue }
    }

    # ── RESOLVE THE TARGET ───────────────────────────────────────────────────
    # A one-shot invocation can still be missing its board (deploy.bat upload).
    # Ask when there is a console, refuse when there is not - but never fall
    # back to a default. Defaulting to platformio.ini's default_envs is exactly
    # the bug this tool used to have, and that default is the released 4-inch
    # product: the worst board to flash by accident.
    #
    # The one exception is the simulator, below: it cannot touch hardware, and
    # "deploy.bat simulator" with no board has always just built and run the
    # 480x480 one. Stopping to ask would have broken that unattended form.
    if ($BoardKey -eq "" -and $ActionKey -eq "simulator") {
        $BoardKey = "sim"
        Write-Host ""
        Write-Host ("  No board given - running {0} ({1}), which is what this tool has" -f $BOARDS["sim"].Label, $BOARDS["sim"].Env) -ForegroundColor DarkGray
        Write-Host  "  always started without a board. 1024x600: deploy.bat simulator sim7b" -ForegroundColor DarkGray
    }
    if ($BoardKey -eq "") {
        Write-Host ""
        Write-Host "  No board selected, and this tool does not pick one for you." -ForegroundColor Yellow
        if ($IsInteractive) {
            Show-Banner
            $BoardKey = Read-BoardChoice
        } else {
            Write-Host "  Name one: 4 | 7b | 5b | sim | sim7b" -ForegroundColor Red
            Write-Host "  e.g. deploy.bat $ActionKey 5b" -ForegroundColor Red
            Write-Host ""
        }
        if ($BoardKey -eq "") { exit 2 }
    }

    $bd    = $BOARDS[$BoardKey]
    $isSim = ($bd.Kind -eq "sim")

    # The simulator action always runs a simulator env. Asking for the
    # simulator while a panel board is selected runs that board's screen twin,
    # which is what anyone means by it.
    $simKey = $bd.Sim
    if ($ActionKey -eq "simulator" -and -not $isSim) {
        Write-Host ""
        Write-Host ("  {0} selected - running its simulator twin: {1}" -f $bd.Label, $BOARDS[$simKey].Env) -ForegroundColor DarkGray
    }

    # The simulator has no serial port, so the flashing actions make no sense.
    if ($isSim -and @("upload", "uploadfs", "deploy", "monitor") -contains $ActionKey) {
        Write-Host ""
        Write-Host ("  '{0}' needs real hardware; {1} is the PC simulator." -f $ActionKey, $bd.Env) -ForegroundColor Red
        Write-Host  "  Pick board 4, 7b or 5b for that." -ForegroundColor Red
        if ($OneShot) { exit 2 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $ActionKey = ""; continue
    }

    # ── THE PORT ─────────────────────────────────────────────────────────────
    $portArg = ""
    $abort   = $false
    if (@("upload", "uploadfs", "deploy", "monitor") -contains $ActionKey) {
        if ($Port -ne "") {
            $portArg = $Port
            Write-Host ""
            Write-Host "  Serial port pinned with -Port: $portArg" -ForegroundColor Green
        } else {
            $portArg = Select-UploadPort $BoardKey
            if ($portArg -eq "") { $abort = $true }
        }
    }
    if ($abort) {
        Write-Host ""
        Write-Host "  Cancelled - nothing was flashed." -ForegroundColor Yellow
        if ($OneShot) { exit 2 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $ActionKey = ""; continue
    }

    # ── ACTIONS ──────────────────────────────────────────────────────────────

    if ($ActionKey -eq "clean") {
        Write-Host ""
        Write-Host ("CLEARING THE BUILD CACHE - {0}" -f $bd.Env) -ForegroundColor Yellow
        Invoke-PIO $BoardKey @("run", "--target", "clean")
        $rc = $PioExit
        if ($OneShot) { exit $rc }
        Show-Result $rc "Cache cleared." "Failed to clear the cache."
        $ActionKey = ""; continue
    }

    if ($ActionKey -eq "monitor") {
        Write-Host ""
        Write-Host ("SERIAL MONITOR on {0} - {1} (Ctrl+C to quit)" -f $portArg, $bd.Env) -ForegroundColor Cyan
        # -e comes from Invoke-PIO and brings monitor_speed plus the
        # esp32_exception_decoder filter, which needs the .elf from this
        # board's build dir - another reason the 4-inch isolation belongs here.
        Invoke-PIO $BoardKey @("device", "monitor", "--port", $portArg, "--baud", "115200")
        if ($OneShot) { exit 0 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $ActionKey = ""; continue
    }

    if ($ActionKey -eq "simulator") {
        Write-Host ""
        Write-Host ("PC SIMULATOR - {0}" -f $BOARDS[$simKey].Env) -ForegroundColor Magenta

        # Check for SDL2
        $sdlDll = Find-SDL2Dll
        if (-not $sdlDll) {
            # Try to download SDL2
            Write-Host "SDL2 not found." -ForegroundColor Yellow
            Write-Host "Downloading SDL2 from GitHub..."
            $sdl2Url = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-mingw.zip"
            $zipPath = "$env:TEMP\SDL2-mingw.zip"
            try {
                Invoke-WebRequest -Uri $sdl2Url -OutFile $zipPath -UseBasicParsing
                Expand-Archive -Path $zipPath -DestinationPath "$env:TEMP\sdl2_extract" -Force
                $inner = Get-ChildItem "$env:TEMP\sdl2_extract" -Directory | Select-Object -First 1
                if ($inner) {
                    $sub = Join-Path $inner.FullName "x86_64-w64-mingw32"
                    if (Test-Path $sub) {
                        New-Item -ItemType Directory -Path "C:\SDL2" -Force | Out-Null
                        Copy-Item "$sub\*" "C:\SDL2" -Recurse -Force
                        $sdlDll = "C:\SDL2\bin\SDL2.dll"
                        Write-Host "SDL2 installed to C:\SDL2" -ForegroundColor Green
                    }
                }
            } catch {
                Write-Host "Download failed: $_" -ForegroundColor Red
                if ($OneShot) { exit 1 }
                Write-Host ""; Read-Host "  ENTER for the menu"
                $ActionKey = ""; continue
            }
        }

        Write-Host "Building the simulator..." -ForegroundColor Cyan
        # Build output goes straight to the window (not captured) so everything
        # is visible.
        Invoke-PIO $simKey @("run")
        $rc = $PioExit
        if ($rc -ne 0) {
            Write-Host ""
            Write-Host "  *** BUILD FAILED (exit: $rc) ***" -ForegroundColor Red
            Write-Host ""
            Write-Host "  The error message(s) are above." -ForegroundColor Yellow
            Write-Host ("  Tip: run 'pio run -e {0}' manually to see more." -f $BOARDS[$simKey].Env)
            Write-Host ""
            # Exit code FIRST. Waiting for a key here used to come before this
            # line, so "deploy.bat simulator" from a script or a build step hung
            # on a failed build instead of returning the failure.
            if ($OneShot) { exit $rc }
            Wait-ForKey "  Any key for the main menu..."
            $ActionKey = ""; continue
        }

        $exe = Resolve-SimulatorExe ($BOARDS[$simKey].Env)
        if ($exe -ne "") {
            $exeDir = Split-Path $exe -Parent

            # Copy SDL2.dll next to the exe (only if found)
            if ($sdlDll -and (Test-Path $sdlDll)) {
                $dst = Join-Path $exeDir "SDL2.dll"
                if (-not (Test-Path $dst)) {
                    Copy-Item $sdlDll $dst -ErrorAction SilentlyContinue
                }
            }

            # Copy the MinGW runtime DLLs (libstdc++, libgcc, libwinpthread)
            # The simulator needs these if it crashes with "missing DLL"
            foreach ($mingwDll in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
                foreach ($mp in @("C:\msys64\mingw64\bin", "C:\mingw64\bin")) {
                    $src = Join-Path $mp $mingwDll
                    if ((Test-Path $src) -and (-not (Test-Path (Join-Path $exeDir $mingwDll)))) {
                        Copy-Item $src $exeDir -ErrorAction SilentlyContinue
                        break
                    }
                }
            }

            Write-Host "Starting the simulator..." -ForegroundColor Green
            Write-Host "  $exe" -ForegroundColor DarkGray
            Write-Host "  (close the window or press ESC to quit)" -ForegroundColor DarkGray
            Write-Host ""

            # Start the simulator and show its output in the console
            $proc = Start-Process -FilePath "$exe" -WorkingDirectory $exeDir -NoNewWindow -PassThru -Wait
            $exitCode = $proc.ExitCode

            if ($exitCode -ne 0) {
                Write-Host ""
                Write-Host "  Simulator exited with an error (exit code: $exitCode)" -ForegroundColor Red
                Write-Host ""
                Write-Host "  Possible causes:" -ForegroundColor Yellow
                Write-Host "    - SDL2.dll missing or wrong version (in $exeDir)"
                Write-Host "    - MinGW DLLs missing (libstdc++-6.dll, libgcc_s_seh-1.dll)"
                Write-Host "    - Run the simulator manually to see the error message:"
                Write-Host "      cd `"$exeDir`" then program.exe" -ForegroundColor Cyan
            } else {
                Write-Host "Simulator exited normally." -ForegroundColor Yellow
            }
        } else {
            Write-Host ""
            Write-Host "  Executable not found. Looked in:" -ForegroundColor Red
            foreach ($t in $SimTried) { Write-Host "    $t" -ForegroundColor Red }
            Write-Host "  The build reported success, so build_dir has probably moved again;" -ForegroundColor Yellow
            Write-Host "  check the [platformio] build_dir key in platformio.ini." -ForegroundColor Yellow
        }
        if ($OneShot) { exit 0 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $ActionKey = ""; continue
    }

    if ($ActionKey -eq "build") {
        Write-Host ""
        Write-Host ("COMPILING - {0}" -f $bd.Env) -ForegroundColor Cyan
        Invoke-PIO $BoardKey @("run")
        $rc = $PioExit
        if ($OneShot) { exit $rc }
        Show-Result $rc "Build OK" "Build failed."
        $ActionKey = ""; continue
    }

    if ($ActionKey -eq "uploadfs") {
        Write-Host ""
        Write-Host ("FILESYSTEM (LittleFS) -> {0} on {1}" -f $bd.Label, $portArg) -ForegroundColor Yellow
        Stop-Process -Name "python", "pio" -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Invoke-PIO $BoardKey @("run", "-t", "uploadfs", "--upload-port", $portArg)
        $rc = $PioExit
        if ($OneShot) { exit $rc }
        Show-Result $rc "LittleFS OK" "LittleFS failed."
        $ActionKey = ""; continue
    }

    if ($ActionKey -eq "deploy") {
        Write-Host ""
        Write-Host ("FULL DEPLOY - LittleFS + firmware -> {0} on {1} (approx. 3-4 min)" -f $bd.Label, $portArg) -ForegroundColor Green
        Stop-Process -Name "python", "pio" -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        # A full deploy is TWO separate pio invocations: uploadfs, then upload.
        # A combined "targets = uploadfs, upload" would silently skip the
        # firmware (both targets resolve to littlefs.bin).
        #
        # Run here rather than through the "deploy" target in extra_script.py,
        # even though that target does the same two steps: it spawns them as
        # CHILD pio processes with no port, and the port cannot be handed down
        # through the environment either (see Invoke-PIO - that variable wipes
        # the build dir). Those children would auto-detect, and with a 4-inch
        # and a 5-inch attached auto-detection is a coin toss between two
        # identical-looking ports. Doing the steps here keeps --upload-port on
        # both of them, so the port the user picked is the port both use.
        # Same order, and the same stop-on-failure, as the custom target.
        Write-Host ""
        Write-Host "  Step 1 of 2 - LittleFS" -ForegroundColor Green
        Invoke-PIO $BoardKey @("run", "-t", "uploadfs", "--upload-port", $portArg)
        $rc = $PioExit
        if ($rc -eq 0) {
            Write-Host ""
            Write-Host "  Step 2 of 2 - firmware" -ForegroundColor Green
            Invoke-PIO $BoardKey @("run", "-t", "upload", "--upload-port", $portArg)
            $rc = $PioExit
        } else {
            Write-Host ""
            Write-Host "  LittleFS upload failed - stopping before the firmware step." -ForegroundColor Red
        }
        if ($OneShot) { exit $rc }
        Show-Result $rc "Full deploy OK" "Full deploy failed."
        $ActionKey = ""; continue
    }

    # Default: flash the firmware (ActionKey = "upload")
    Write-Host ""
    Write-Host ("FLASHING FIRMWARE -> {0} [{1}] on {2}" -f $bd.Label, $bd.Env, $portArg) -ForegroundColor Cyan
    Stop-Process -Name "python", "pio" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Invoke-PIO $BoardKey @("run", "-t", "upload", "--upload-port", $portArg)
    $rc = $PioExit
    if ($OneShot) { exit $rc }
    Show-Result $rc "Firmware OK" "Flash failed."
    $ActionKey = ""; continue

} # end of while loop
