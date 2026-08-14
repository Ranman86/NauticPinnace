param(
    [string]$Action    = "",
    [switch]$FullDeploy,
    [switch]$FsOnly,
    [switch]$Simulator,
    [switch]$Build,
    [switch]$Monitor,
    [switch]$Clean,
    [switch]$Help,
    [string]$Port      = ""   # serial port for -Monitor; empty = auto-detect
)

# deploy.bat compatibility: map the -Action string onto the switches
if ($Action -ne "") {
    switch ($Action.ToLower()) {
        "upload"    { }
        "deploy"    { $FullDeploy = $true }
        "uploadfs"  { $FsOnly = $true }
        "build"     { $Build = $true }
        "monitor"   { $Monitor = $true }
        "simulator" { $Simulator = $true }
        "clean"     { $Clean = $true }
        "help"      { $Help = $true }
    }
}

$PIO     = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$PROJECT = $PSScriptRoot

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

if ($Help) {
    Write-Host ""
    Write-Host "NauticPinnace - Build and Deploy"
    Write-Host "=================================="
    Write-Host "DIRECT:"
    Write-Host "  .\build_deploy.ps1                 Flash firmware"
    Write-Host "  .\build_deploy.ps1 -FullDeploy     LittleFS + firmware"
    Write-Host "  .\build_deploy.ps1 -FsOnly         LittleFS only"
    Write-Host "  .\build_deploy.ps1 -Simulator      Start the PC simulator"
    Write-Host "  .\build_deploy.ps1 -Build          Compile only"
    Write-Host "  .\build_deploy.ps1 -Monitor        Serial monitor (-Port COMx to pin)"
    Write-Host "  .\build_deploy.ps1 -Clean          Clear the build cache"
    Write-Host ""
    Write-Host "VIA deploy.bat:"
    Write-Host "  deploy.bat               Interactive menu"
    Write-Host "  deploy.bat upload        Flash firmware"
    Write-Host "  deploy.bat deploy        LittleFS + firmware"
    Write-Host "  deploy.bat uploadfs      LittleFS only"
    Write-Host "  deploy.bat build         Compile only"
    Write-Host "  deploy.bat monitor       Serial monitor"
    Write-Host "  deploy.bat simulator     PC simulator"
    Write-Host "  deploy.bat clean         Clear the build cache"
    Write-Host ""
    Write-Host "SIMULATOR - SDL2 and MinGW are detected automatically."
    Write-Host "  SDL2 manually: C:\SDL2\include\SDL2\SDL.h"
    Write-Host "  MinGW: C:\msys64\mingw64\bin\g++.exe (MSYS2)"
    Write-Host ""
    Write-Host "CHANGE UI PARAMETERS:"
    Write-Host "  src/display/UiConfig.h   colours, sizes, positions"
    Write-Host "  src/display/Theme.h      fonts, colour macros"
    Write-Host "  data/config.json         WiFi, brightness"
    exit 0
}

function Invoke-PIO {
    param([string[]]$PArgs)
    Write-Host ""
    Write-Host "  pio $PArgs" -ForegroundColor Cyan
    Write-Host ""
    & $PIO @PArgs
    return $LASTEXITCODE
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

if (-not (Test-Path $PIO)) {
    Write-Host "PlatformIO not found: $PIO" -ForegroundColor Red
    exit 1
}
Set-Location $PROJECT

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

# ── Was a switch given? (then no menu is needed) ─────────────────────────────
$anySwitch = $FullDeploy -or $FsOnly -or $Simulator -or $Build -or $Monitor -or $Clean -or ($Action -ne "")

# ── MAIN LOOP (menu) ─────────────────────────────────────────────────────────
$keepRunning = $true
while ($keepRunning) {

    # After a direct command has run once: leave the loop
    if ($anySwitch -and (-not $keepRunning)) { break }

    # Show the menu only when no direct parameter was given
    if (-not $anySwitch) {
        Clear-Host
        Write-Host ""
        Write-Host "  =================================" -ForegroundColor Cyan
        Write-Host "   NauticPinnace - Deploy Tool    " -ForegroundColor Cyan
        Write-Host "  =================================" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "  [1] Flash firmware           (approx. 35 s)"
        Write-Host "  [2] LittleFS + firmware      (after web UI changes, approx. 4 min)"
        Write-Host "  [3] LittleFS only            (web UI files only)"
        Write-Host "  [4] Compile only             (no flash)"
        Write-Host "  [5] Start the PC simulator   (no ESP32 needed)"
        Write-Host "  [6] Serial monitor           (view logs)"
        Write-Host "  [7] Clear the build cache"
        Write-Host "  [0] Quit"
        Write-Host ""
        $choice = Read-Host "  Choice (0-7)"
        Write-Host ""
        switch ($choice) {
            "1" { $Action = "upload" }
            "2" { $FullDeploy = $true }
            "3" { $FsOnly = $true }
            "4" { $Build = $true }
            "5" { $Simulator = $true }
            "6" { $Monitor = $true }
            "7" { $Clean = $true }
            "0" {
                Write-Host "  Quitting - press any key..." -ForegroundColor DarkGray
                $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
                $keepRunning = $false; continue
            }
            default { continue }
        }
    }

    # ── ACTIONS ──────────────────────────────────────────────────────────────

    if ($Clean) {
        Write-Host "CLEARING THE BUILD CACHE" -ForegroundColor Yellow
        $rc = Invoke-PIO @("run","--target","clean")
        if ($anySwitch) { exit $rc }
        Show-Result $rc "Cache cleared." "Failed to clear the cache."
        $Clean = $false; $Action = ""; continue
    }

    if ($Monitor) {
        Write-Host "SERIAL MONITOR (Ctrl+C to quit)" -ForegroundColor Cyan
        if ($Port -ne "") {
            & $PIO "device" "monitor" "--port" $Port "--baud" "115200"
        } else {
            & $PIO "device" "monitor" "--baud" "115200"
        }
        if ($anySwitch) { exit 0 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $Monitor = $false; $Action = ""; continue
    }

    if ($Simulator) {
        Write-Host ""
        Write-Host "PC SIMULATOR" -ForegroundColor Magenta

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
                if ($anySwitch) { exit 1 }
                Write-Host ""; Read-Host "  ENTER for the menu"
                $Simulator = $false; $Action = ""; continue
            }
        }

        Write-Host "Building the simulator..." -ForegroundColor Cyan
        Write-Host ""
        # Build output goes straight to the window (not captured) so everything is visible
        & $PIO "run" "-e" "simulator"
        $rc = $LASTEXITCODE
        if ($rc -ne 0) {
            Write-Host ""
            Write-Host "  *** BUILD FAILED (exit: $rc) ***" -ForegroundColor Red
            Write-Host ""
            Write-Host "  The error message(s) are above." -ForegroundColor Yellow
            Write-Host "  Tip: run 'pio run -e simulator' manually to see more."
            Write-Host ""
            Write-Host "  Any key for the main menu..."
            $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
            if ($anySwitch) { exit $rc }
            $Simulator = $false; $Action = ""; continue
        }

        $exe = (Resolve-Path ".pio\build\simulator\program.exe" -ErrorAction SilentlyContinue)
        if ($exe -and (Test-Path $exe)) {
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
            foreach ($mingwDll in @("libstdc++-6.dll","libgcc_s_seh-1.dll","libwinpthread-1.dll")) {
                foreach ($mp in @("C:\msys64\mingw64\bin","C:\mingw64\bin")) {
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
            $proc = Start-Process -FilePath "$exe" `
                                  -WorkingDirectory $exeDir `
                                  -NoNewWindow -PassThru -Wait
            $exitCode = $proc.ExitCode

            if ($exitCode -ne 0) {
                Write-Host ""
                Write-Host "  Simulator exited with an error (exit code: $exitCode)" -ForegroundColor Red
                Write-Host ""
                Write-Host "  Possible causes:" -ForegroundColor Yellow
                Write-Host "    - SDL2.dll missing or wrong version (in $exeDir)"
                Write-Host "    - MinGW DLLs missing (libstdc++-6.dll, libgcc_s_seh-1.dll)"
                Write-Host "    - Run the simulator manually to see the error message:"
                Write-Host "      cd `"$exeDir`" && program.exe" -ForegroundColor Cyan
            } else {
                Write-Host "Simulator exited normally." -ForegroundColor Yellow
            }
        } else {
            Write-Host "Executable not found: .pio\build\simulator\program.exe" -ForegroundColor Red
        }
        if ($anySwitch) { exit 0 }
        Write-Host ""; Read-Host "  ENTER for the menu"
        $Simulator = $false; $Action = ""; continue
    }

    if ($Build) {
        Write-Host "COMPILING" -ForegroundColor Cyan
        $rc = Invoke-PIO @("run")
        if ($anySwitch) { exit $rc }
        Show-Result $rc "Build OK" "Build failed."
        $Build = $false; $Action = ""; continue
    }

    if ($FsOnly) {
        Write-Host "FILESYSTEM (LittleFS)" -ForegroundColor Yellow
        Stop-Process -Name "python","pio" -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        $rc = Invoke-PIO @("run","-t","uploadfs")
        if ($anySwitch) { exit $rc }
        Show-Result $rc "LittleFS OK" "LittleFS failed."
        $FsOnly = $false; $Action = ""; continue
    }

    if ($FullDeploy) {
        Write-Host "FULL DEPLOY - LittleFS + firmware (approx. 3-4 min)" -ForegroundColor Green
        Stop-Process -Name "python","pio" -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        # "deploy" (extra_script.py) runs as TWO separate pio invocations:
        # uploadfs, then upload. A combined "targets = uploadfs, upload" would
        # silently skip the firmware (both targets resolve to littlefs.bin).
        $rc = Invoke-PIO @("run","-t","deploy")
        if ($anySwitch) { exit $rc }
        Show-Result $rc "Full deploy OK" "Full deploy failed."
        $FullDeploy = $false; $Action = ""; continue
    }

    # Default: flash the firmware (Action="upload" or no specific switch)
    Write-Host "FLASHING FIRMWARE" -ForegroundColor Cyan
    Stop-Process -Name "python","pio" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $rc = Invoke-PIO @("run","-t","upload")
    if ($anySwitch) { exit $rc }
    Show-Result $rc "Firmware OK" "Flash failed."
    $Action = ""; continue

} # end of while loop
