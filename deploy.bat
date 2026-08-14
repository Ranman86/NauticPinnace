@echo off
:: NauticPinnace - Build & Deploy launcher
:: Double-click opens the interactive menu in PowerShell.
::
:: Direct actions (pass as an argument):
::   deploy.bat build
::   deploy.bat upload
::   deploy.bat uploadfs
::   deploy.bat deploy
::   deploy.bat monitor
::   deploy.bat clean

title NauticPinnace - Build ^& Deploy

if "%~1"=="" (
    :: Interactive menu
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1"
   pause
) else (
    :: Run the action directly
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1" -Action "%~1"
    echo.
    pause
)
