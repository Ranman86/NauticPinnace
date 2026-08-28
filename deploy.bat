@echo off
:: NauticPinnace - Build & Deploy launcher
:: Double-click opens the interactive menu in PowerShell. The menu asks for the
:: TARGET BOARD first and then for the action, and keeps the chosen board on
:: screen the whole time.
::
:: Direct actions:   deploy.bat <action> [board] [COMport]
::
::   Actions: build  upload  uploadfs  deploy  monitor  simulator  clean  help
::   Boards:  4      4-inch   480x480   waveshare_esp32s3_4   (RELEASED PRODUCT)
::            7b     7-inch  1024x600   waveshare_esp32s3_7b
::            5b     5-inch  1024x600   waveshare_esp32s3_5b
::            sim    PC simulator  480x480
::            sim7b  PC simulator 1024x600
::
::   deploy.bat build 7b
::   deploy.bat upload 5b
::   deploy.bat uploadfs 7b
::   deploy.bat deploy 4
::   deploy.bat monitor 7b COM7
::   deploy.bat simulator sim7b
::   deploy.bat simulator       PC simulator 480x480, no board needed
::   deploy.bat clean 5b
::   deploy.bat 7b              menu, 7-inch preselected
::   deploy.bat help            also -h, --help, /?
::
:: Leaving the board off (the old one-argument form, e.g. "deploy.bat upload")
:: still works: the tool then ASKS which board. It never picks one on its own -
:: it used to always hit the 4-inch board, which is the released product.
:: The exception is "deploy.bat simulator", which still runs the 480x480
:: simulator unattended, exactly as it always did. It flashes nothing, so
:: there is no wrong board it could pick.
::
:: The port is only asked for when it is ambiguous. The 7-inch is a CH343
:: bridge (VID_1A86) and identifies itself; the 4-inch and the 5-inch both
:: enumerate as VID_303A PID_1001 and cannot be told apart, so with both
:: attached you get a list to pick from.

title NauticPinnace - Build ^& Deploy

if "%~1"=="" goto menu
if "%~2"=="" goto action_only
if "%~3"=="" goto action_board
goto action_board_port

:menu
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1"
pause
goto done

:: Each form gets its own line rather than one line with empty quotes:
:: passing -Board "" to a PowerShell -File script can be read as a missing
:: argument, and then the whole invocation fails.
::
:: The COLON in -Action:"%~1" is not decoration. With a space, PowerShell reads
:: a value that starts with a dash as the NEXT PARAMETER NAME instead of as the
:: value, so "deploy.bat -h" became -Action -h and died with "Missing an
:: argument for parameter 'Action'" before the script ever started - the tool
:: crashed on the one thing a puzzled user types first. The colon form binds
:: the value even when it begins with a dash, and behaves identically for
:: everything else. -h, --help and /? now all reach the script's own help.
:action_only
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1" -Action:"%~1"
echo.
pause
goto done

:action_board
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1" -Action:"%~1" -Board:"%~2"
echo.
pause
goto done

:action_board_port
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_deploy.ps1" -Action:"%~1" -Board:"%~2" -Port:"%~3"
echo.
pause
goto done

:done
