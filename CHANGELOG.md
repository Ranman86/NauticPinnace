# Changelog

All notable changes to NauticPinnace are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version is defined once, in `src/Version.h`, and is what the device reports
to the NMEA 2000 bus - so a chartplotter's device list names the firmware that
is actually running.

Entries are marked with the boards they apply to. Unmarked entries apply to all
three.

## [1.2.0] - 2026-10-03

### Added

- Latitude and longitude can now be shown in degrees and minutes (48°07.407'N, what a chart plotter writes) or in degrees, minutes and seconds (48°07'24.4"N) instead of decimal degrees - a new "Coordinate format" setting on the Display tab, which applies to the two coordinate fields wherever they appear in a data grid or in the sidebar. A field's decimals then count for the last part, minutes or seconds. Default stays decimal, so an existing configuration looks exactly as before. Asked for in GitHub issue #3.

- The Wind & Trim instrument can show the last minute of wind speed as a curve running from the centre of the rose out to the wind pointer - the strip chart of a wind plot laid along the pointer, newest sample at the rose, older ones migrating inward, wind strength across. Four settings in the sail section of the web interface: off, true wind only, apparent wind only, both. Off by default.

- Trial: the maritime icons of the OpenBridge Icon Pack (CC BY 4.0) as an alternative to the built-in Font Awesome glyphs - the same icon for the same meaning as NauticPi (anchor, wind, depth, engine, the media controls, the alarm triangle ...), on the panel and in the web configuration. Switched with the new icon selector in the web configuration's header (`ui.icons`, `classic` by default), live and without a restart; `classic` draws exactly what the firmware drew before. The icons are rasterised at build time into small LVGL fonts by `tools/gen_icons.py` from the mapping shared with NauticPi. Everything belonging to the trial, and how to remove it again, is listed in `ICONS-TRIAL.md`.

- The anchor watch now keeps in step with NauticPi over the NMEA 2000 bus. Setting, lifting, moving, arming or clearing the anchor alarm on either device has it the same way on the other about a second later, in both directions, over the proprietary PGN 131035 (manufacturer 2046, fast packet). Both sides decide who wins from the message alone: newer revision wins, and on the same second the lower node number - so a device whose clock lags still carries its own change through. The state is broadcast on every local change plus a ten-second heartbeat, which is what brings a device that was just switched on into agreement. An adopted state re-evaluates the drag alarm at once rather than waiting for the next cycle.

- The anchor screen gained a "lift anchor" action - the set button turns into it while an anchor is down - and shows the time the anchor fell. Until now an anchor could be set but never cleared.

- The daily BSH tide download can be switched off in the web interface (Devices tab); the tide screen then falls back to the tide data on the NMEA 2000 bus, if a sender offers any. Time synchronisation over WiFi is unaffected by the switch.

- After a restart the device returns to the instrument that was showing when it was switched off, instead of always starting on the first one. Configuration and licence screens are excluded: they never count as "the last screen".

- **(Tooling)** The release package now carries all three boards instead of the 4-inch alone: one ZIP with a folder per board (`4/`, `7b/`, `5b/`, each with its flash images, the merged full-flash image and a manifest) and the flashing tool, licences and instructions once at the top. `flash.bat` and `flash.sh` take the board as their first argument (`flash.bat 7b COM5`) or list the three and ask - the wrong firmware leaves the screen dark without warning, so they never guess. FLASHING.md covers the 7B's USB driver and a fallback for the 5B should a transfer stall.

### Changed

- The media screen (formerly named after the stereo's brand) is now called "Media" ("Medien" in German), on its tile, in its title and in the documentation; the brand name no longer appears in the project's sources and documentation. It controls the stereo exactly as before - only the project's own naming changed. Saved configurations are unaffected, since screens are stored by number; the screen type the web API reports is now `media`.

### Fixed

- The WiFi hotspot's name as printed on the configuration screen, as encoded in the QR code and as broadcast by the radio could differ from one another: the MAC address the name is derived from was read into an uninitialised buffer whenever the network interface was not yet up, and each of the three read it at a different moment. All three now derive the name from the chip's permanent MAC.

- In demo mode the wind-history sampler stopped recording five seconds after boot, because the demo data never refreshed the wind timestamp. (Wind trace only; no released screen was affected.)

- With demo mode off and no stereo on the bus (or in listen-only mode), tapping next or previous on the media screen filled in a title and artist from the demo playlist, and play/pause and the mute buttons changed their icons for a stereo nobody had heard. The demo playlist now runs only while the demo does; without a stereo the controls leave the screen as it is. Switching demo mode off at runtime on a device that booted on the bus now also clears the demo values, as it already did on a device that booted in demo mode.

- The licence notices now cover everything the firmware links, read again from the linker maps of all three boards: `LICENSES/` gains the BSD-3-Clause texts of lwIP, wpa_supplicant, littlefs and the TLSF memory allocator, one MIT text carrying the copyright lines of FreeRTOS, the Xtensa HAL, esp_littlefs, cJSON and the code LVGL bundles, and the newlib C-library notices. FreeRTOS is now credited under its actual licence (MIT, Amazon) instead of Apache-2.0, the binary-only WiFi and PHY libraries under Apache-2.0 instead of an unnamed Espressif licence, and the font notices no longer claim a reserved font name that Montserrat's licence does not declare. The browser flasher page links the notices and licence texts next to its install buttons.

- The source code of the LGPL libraries in exactly the versions each board links - the Arduino-ESP32 core (2.0.17 on the 4-inch, 3.3.11 on the 7B and 5B), AsyncTCP and ESPAsyncWebServer - is now attached to every GitHub release as `src-*.zip`, and THIRD-PARTY-NOTICES.md names the exact upstream tags and commits and the build directory each board's object files land in for relinking.

- The licence screen on the device now names what that board actually links: its own core and library versions, the full copyright lines of the two web-server libraries (four holders and LGPL-3.0-or-later on the 7B and 5B), the ESP-IDF components underneath, and LovyanGFX on the 4-inch only. It says where the full texts are (THIRD-PARTY-NOTICES.md and LICENSES/ in the GitHub repository), and the OpenBridge icons are credited under a heading of their own instead of under the fonts.

- The flashing instructions and the README no longer say that a freshly flashed device starts in demo mode: out of the box it reads the NMEA 2000 bus, as it has since v1.1.0, and the demo can be switched on in the web configuration.

- The firmware-only USB update in the release package's FLASHING.md now writes `boot_app0.bin` along with the firmware. On a board that had been updated over WiFi, the command given until now wrote the new firmware into the app slot the board was no longer starting from, so the old firmware kept running as if nothing had happened. The instructions also name the web interface's Update tab as the route without a cable, and that a firmware-only update keeps the web interface already on the board.

- **(Tooling)** The `esptool.exe` in the Windows release package now comes with the notices and licence texts of everything bundled inside it - Python runtime, OpenSSL, libffi, bzip2, xz, zlib, pyserial, intelhex, the PyInstaller bootloader and the Microsoft runtime - in `esptool-4.8.1/notices/`, and with the source of pyserial and intelhex next to the esptool source.

- **(Tooling)** The release tools refuse stale or unsuitable images instead of packaging them: an image that carries paths from the build machine's user profile or leftover old names, a `data/` folder holding credentials, or a build older than the sources stops the run - for the release package and the web flasher alike, on all three boards. `platformio.ini` now pins every library to the exact version the notices name.

## [1.1.0] - 2026-08-27

Two more displays, and a great deal of work behind the picture.

NauticPinnace now runs on the 7-inch Waveshare ESP32-S3-Touch-LCD-7B and the
5-inch -5B alongside the original 4-inch board. All three share one source tree
and one user interface; the two 1024x600 boards additionally carry a navigation
rail, a configurable data sidebar and a full-screen launcher. The screen picture
can be rotated in 90-degree steps on every board, sensors can be calibrated and
duplicate data sources pinned from the web interface, and firmware can be
updated over WiFi.

Nothing here changes the stored configuration format: a config file from v1.0.0
is read unchanged, which is why this is a minor release despite its size. The
one migration is automatic - see the note on CAN pins under Changed.

### Added

- NauticPinnace now runs on two more displays - the 7-inch Waveshare ESP32-S3-Touch-LCD-7B and the 5-inch -5B, both 1024x600 - which share the identical user interface down to the last line of code and differ only in their pin map, console wiring and panel timings; each has its own build environment (waveshare_esp32s3_7b, waveshare_esp32s3_5b) and board definition.

- The screen picture can now be rotated by 0, 90, 180 or 270 degrees (Display tab in the web interface, the device restarts itself to apply it), so the unit can be mounted on its side or upside down - touch and swipe navigation turn with it.

- Eleven sensor calibrations can now be entered in the new "Devices" tab of the web interface (depth offset, wind-vane rotation and wind-speed factor, compass offset, log factor, roll, pitch and rudder zero points plus a mirrored rudder sense, water-temperature and pressure offsets); the correction is applied once as the value arrives from the bus, so every screen shows the corrected reading from the next message without a restart, and the defaults are neutral, leaving an uncalibrated device exactly as before.

- Data sources can now be pinned per category - position, compass, wind, depth, speed through water, environment and attitude - so on a boat with two GPS receivers or two compasses the display no longer just follows whichever device transmitted last: the new Devices tab lists every sender seen on the bus with its source address, message count and age, and "Auto" keeps accepting any sender.

- Firmware and filesystem can now be updated over WiFi from a new "Update" tab - no USB cable: the page names the version currently installed, shows an upload progress bar, and waits for the device to come back while it reboots itself. A firmware file that is not an ESP32 image is rejected before anything is written, and the page warns to install the filesystem before the firmware and to do updates at the dock rather than at sea.

- Tap the ring of the compass card in the centre of the Wind & Trim instrument (and of the Attitude screen that shares it) to switch the card between course-up, as before, and north-up, where a marker on the rim shows the current heading; the choice is saved and survives a restart, and only the card turns - the relative-wind ring, the boat and the wind pointers stay bow-up.

- The CAN TX/RX pins set in the web interface are now actually applied, so the firmware also runs on rewired or third-party boards - until now the bus always opened on the compile-time board pins no matter what was configured. A pin pair that cannot work is rejected in favour of the board defaults, and the boot log names the pins the bus really opened with, because a wrong pin does not announce itself: the controller comes up and simply never receives a frame.

- After an over-the-air update the boot log names the slot the firmware is actually running from, which is the only way to tell a successful update from a silent fall-back to the previous image, since both slots carry the same firmware; the image also marks itself healthy only once the panel, the configuration and all screens are up. Note that the shipped prebuilt bootloader does no automatic rollback: a truncated or corrupt upload simply never boots and the previous version is kept, but a complete image that crashes at runtime still needs a USB cable.

- CPU load and free memory can be picked as data fields like any NMEA value, so device health fits in a data-grid cell, on the engine page, or - on the 7B and 5B - in a sidebar cell.

- **(7B / 5B)** Support for the 1024x600 panels of the 7B and 5B boards: they are driven directly through the ESP-IDF RGB panel interface, because the LovyanGFX driver the 4-inch uses never produced a usable picture on them. Both run their pixel clock at 36 MHz instead of the vendor's setting (30 MHz on the 7B, 21 MHz on the 5B), whose refresh rate flickers visibly, and the panel is fed from small DMA buffers, which stops the horizontal smearing that appeared as soon as the app drew into the same memory the panel scans out of.

- **(7B / 5B)** On the 1024x600 boards the instruments are drawn natively at 1.25x the 4-inch design - bigger fonts and real geometry rather than an upscaled 480-pixel picture - inside a 600x600 square that keeps its size in portrait as well as landscape, with the demo and anchor-alarm banners now spanning that instrument column instead of the whole screen width.

- **(7B / 5B)** On the 1024x600 boards the rail's home button opens a full-screen launcher showing every active screen as a labelled tile with a large icon, plus a settings tile - tap a tile to jump straight to that screen, and close the launcher with the X in the corner or a swipe up. The grid sizes itself to the number of tiles, up to 24 of them without scrolling, and a caption too long for its tile drops to a smaller font instead of being broken mid-word.

- **(7B / 5B)** The 1024x600 boards gain permanent chrome around the instrument: an 88 px navigation rail carrying the NauticPinnace brand mark and Home / Previous / Next / Settings, with Previous and Next given the largest touch targets, plus a 336 px sidebar of up to eight configurable value cells. The cells use the same fields as the data-grid screens (label, unit, decimals) and pick up a change made in the web interface immediately, without a restart.

- **(7B / 5B)** Mounted in portrait, the 1024x600 boards rearrange their permanent chrome instead of merely turning the picture: the navigation rail becomes a 600 x 88 bar across the top, the data sidebar becomes a band along the bottom whose value cells reflow into a grid of up to four columns instead of one tall stack, Previous and Next stay the largest buttons (1.5x the width of Home and Settings), and the instrument keeps its full 600 x 600 square in either orientation.

- **(7B / 5B)** With the display rotated to portrait, the menus and the boot screen lay themselves out for the taller screen instead of running off its edge: the settings page stacks the hotspot block and its QR code under the WLAN column, the launcher drops to a three-column grid centred vertically, the language picker spreads its buttons over the extra height, the licence page keeps its accept button on screen (on first boot that button is the only way past it), and the boot screen's logo and progress groups move toward the centre instead of drifting to opposite edges.

- **(7B / 5B)** The permanent data sidebar is now configured under Display in the web interface: 1 to 8 value cells, each with field, label, unit and decimals, preset to SOG, depth, apparent wind speed, heading and battery voltage - and because the setting is stored on every board, a configuration file stays portable between the 4-inch and the large displays.

- **(7B / 5B)** The repository now carries a panel bring-up sketch with its own build environment for each of the two new boards (bringup_7b, bringup_5b): it drives the RGB panel directly, without the rest of the firmware in the way, and steps the pixel clock upwards while printing the refresh rate and the panel's PSRAM read rate for each step - the measurement that settled the 36 MHz the firmware now ships with.

- **(Tooling)** The deploy tool now identifies the serial port by its USB identity and always says which port it chose and why: the 7-inch board's CH343 bridge is unmistakable, while the 4-inch and the 5-inch report the same USB ID, so with both attached it lists the candidates and asks instead of guessing - and the port, whether detected or pinned with -Port COMx, is now used by every flashing step, including both halves of a full deploy.

- **(Simulator)** The PC simulator now has a 1024x600 variant (simulator_7b) that runs the real nav rail, sidebar and home launcher, and any simulator build can produce its own documentation set: `--shots DIR` walks every configured screen plus the home, settings and licence overlays and writes one image each, named after the screen title and skipping unconfigured grid slots, while `--settle MS` sets the render time per shot and `--home` opens the launcher directly; captured images leave out the demo banner and the fps overlay. The repository carries the resulting set from the 1024x600 build - 126 images covering every screen plus the launcher and the settings page, each in light, dark and night and in both landscape and portrait.

- **(Simulator)** The PC simulator now honours the configured screen rotation and shows exactly what a rotated panel shows - the window keeps the panel's shape and the picture lies on its side inside it - with clicks and swipes following the rotated layout, including the rule that a swipe must start on the instrument rather than on the nav rail or the value sidebar.

- **(Tooling)** Serial render diagnostics on the 1024x600 boards: the five-second heartbeat now reports the instrument's own canvas paint separately from the rest of the display work and adds the worst display refresh of that window with how much of it went into getting the pixels to the panel, and about once a minute one line lists every screen visited since boot with its slowest paint - which is how the slow screens in this release were found. The PC simulator additionally logs every PSRAM arena allocation, which is how the arena on the device is sized.

### Changed

- The wind instrument repaints markedly faster on every board: its filled discs, zone rings and sail shapes are now painted straight into the instrument image instead of being handed to LVGL one scanline at a time, which on the 5B alone accounted for 198 ms of a 261 ms repaint.

- The 4-inch board keeps building against exactly the core v1.0.0 was released and tested with, even though the two 1024x600 boards need a newer one (Arduino 3.3.11 / ESP-IDF 5.5.5, required for the RGB panel's bounce buffers) that used to overwrite it during installation.

- AIS targets on the radar now change colour at the CPA/TCPA alarm limits set in the web interface - yellow at twice those limits, red at them - so the radar warns at the same distance as the alarm instead of at fixed thresholds.

- Existing devices keep their NMEA 2000 bus after the update: the CAN pins now come from the board definition unless a pin is set explicitly for modified hardware, and a stored configuration still carrying one of the two historical pin pairs is treated as "not configured" instead of being taken at face value.

- Out of the box the device now starts with live bus data instead of the demo simulation, and it comes up in the firmware's own dark and light themes - the factory configuration no longer freezes an older black-and-blue palette over them.

- The web configuration page is now served pre-compressed, cutting it from about 130 KB to under 40 KB on the wire - a load that took 4.4 s on the 7-inch and 13.4 s on the 5-inch drops to roughly a quarter of that - and a device whose filesystem does not carry the compressed copy is still served the plain page as before.

- About 10.8 KB of internal RAM is given back on every board: the AIS target table and the wind, depth, speed, heave and pressure history rings now live in PSRAM, leaving more of the memory that WiFi, the web server and the online tide forecast were running out of.

- A panic can now be read back from flash instead of dying with "Elf write init failed" on every crash: the 16 MB layout gains a core-dump partition, paid for with 64 KB from each of the two app slots, which stay equally sized so an over-the-air image can never outgrow the inactive slot - the LittleFS partition keeps its offset and size, so settings, the web UI and the polar table survive the update.

- The README now covers all three boards side by side - panel, touch controller, CAN pins, console wiring and the extras the 5-inch adds - and gains a step-by-step guide to reaching the web configuration: hotspot or joining an existing boat network, which one to pick and why, and the promise that you cannot lock yourself out, because after three failed connection attempts the display falls back to its own hotspot, so a wrong password costs one boot rather than a serial cable. The new Devices tab (sensor calibration and N2K source selection) and the screen-orientation setting are documented, and the AIS note is corrected: target colouring follows the CPA/TCPA thresholds from the configuration.

- **(4-inch)** The 4-inch release package can still be built now that each board keeps its own toolchain directory: the packaging script takes the boot image and the flashing tool from the 4-inch board's own copy.

- **(7B / 5B)** On the 1024x600 boards every instrument page is now laid out for the larger panel instead of staying at its 480 px design size - the wind rose and its corner readouts, the anchor watch and its button band, the AIS radar, the depth echogram, the rudder arc, the autopilot compass, the battery and tank cards, the route/VMG/weather tiles, the media controls and the data grid all fill the screen, with correspondingly larger type and touch targets.

- **(7B / 5B)** Themes saved in the web interface now scale with the panel, so one stored theme produces the same design on the 1024x600 boards as on the 4-inch instead of leaving small text on a large screen.

- **(7B / 5B)** On the 1024x600 boards menu transitions and the scrolling licence text now move smoothly instead of stepping five times a second, while the screen refresh itself is deliberately held back so the web interface keeps answering while the display is busy.

- **(7B / 5B)** The settings, licence and language screens now fill the whole panel instead of a 600 x 600 square in one corner - the licence text box gets the extra height, so much more of the text is visible before scrolling, and the language buttons become larger targets.

- **(7B / 5B)** On the 1024x600 boards the settings screen uses the extra width: WLAN, display theme and the language/licence row fill the left column, the hotspot block with a larger auto-connect QR code (176 px instead of 112 px) sits on the right, and the on-screen keyboard spans the whole panel width at 280 px tall, so the keys are finger-sized. Controls that the larger fonts had outgrown get honest room as well - the WLAN label no longer sits under its own switch, the licences button no longer clips its caption at both ends, and the listen-only switch gets a row of its own.

- **(7B / 5B)** On the 1024x600 boards the clock page carries a larger world map generated at its own 600x250 resolution instead of reusing the 4-inch one, so coastlines stay fine rather than coarse, with the clock, date, sun/moon lines and tide curve moved down to match.

- **(Tooling)** Before offering any action, the deploy tool now asks which board or simulator you are targeting and keeps that choice on screen, instead of silently sending every build and flash to the environment named in platformio.ini - the released 4-inch product. Non-interactively use `deploy.bat <action> <board> [COMport]` or `-Board 4|7b|5b|sim|sim7b` (names such as 7inch or 5b are accepted); only `deploy.bat simulator` still defaults on its own, to the 480x480 simulator, because it flashes nothing.

- **(Simulator)** The PC simulator now runs on the same PSRAM budget as the board it stands in for - 3.35 MB for the 4-inch, 4.42 MB for the 1024x600 boards, instead of a flat 4 MB - so a screen that asks for too much memory fails at the desk rather than on the boat.

### Fixed

- Screens carrying a lot of text are far more responsive - scrolling the licence page went from roughly 1 to 6 frames per second on the 5B - because text is no longer re-measured several times per drawing pass for a scrollbar that could never appear.

- While a full-screen menu is open - settings, licences, the language picker and the new home launcher - the instrument behind it is no longer drawn at all instead of being fully repainted under the opaque menu, so menu scrolling and buttons respond immediately: on the 5B one frame of licence scrolling had been spending 1,160 of its 1,190 ms rendering an instrument nobody could see (all boards; the home launcher itself exists only on the 7B and 5B).

- The web interface no longer goes unreachable while the device still shows a healthy WiFi connection: the link is now checked end to end with a gateway ping every 15 seconds and rebuilt after three unanswered probes, radio power saving is forced off (with it on, large page loads stalled to nothing), and the station runs in 802.11b/g, which stopped the connection from wedging every one to two minutes while a browser was talking to it.

- Tide heights are now referenced to chart datum instead of gauge zero: the gauge's datum correction silently came out as zero at every station, so every predicted height was published far too high (302 cm at Cuxhaven).

- Turning demo mode off in the web interface no longer crashes the device, and no longer leaves a unit that had booted in demo mode with nothing reading the bus - every value blank, which looked exactly like broken hardware or a dead cable. It now restarts itself with a notice on the display.

- Overlapping saves - a save the browser re-sent, or a save and a configuration import at the same time - could silently produce a configuration parsed from a mixture of two request bodies; each request now buffers its own body, and one that is too large or cannot be buffered is answered with an error instead of being parsed (the same applies to importing a configuration and to uploading a polar table).

- An interrupted boot-logo upload no longer leaves the web interface wedged until the next restart, and two uploads arriving at once no longer corrupt each other.

- The tide forecast now picks the nearest gauge that actually carries data: stations that report no chart datum or no tide events (Meldorf, Neuwerk) used to win purely on distance and hide a complete gauge a few kilometres away. Predicted events are also kept strictly ascending in time, so the clock screen's "next tide" really is the next one; heights more than 30 m from chart datum are discarded as implausible, and a genuine reading of 0 cm (water standing exactly at gauge zero) is no longer mistaken for a missing value.

- The web interface header now shows the address the page was actually loaded from instead of always displaying the hotspot address 192.168.4.1, which was wrong whenever the device was joined to a boat network.

- A tide gauge whose name ends in a cut-off special character can no longer put stray characters into the station name on the clock screen, because transliterating it no longer reads past the end of the text.

- **(7B / 5B)** On the 1024x600 boards the settings page no longer comes up with parts of it missing - an empty screen list, the wrong IP address - which happened whenever the device ran out of memory while assembling its configuration or spliced two replies into one connection.

- **(7B / 5B)** The online tide forecast now works on the 1024x600 boards: the 433 KB reply from the BSH service used to be parsed in the small internal RAM that the secure connection and the WiFi receive buffers need at the same moment, so the download stalled and a request that had already been answered failed as incomplete - sometimes taking the device down with it; the parser's memory and the fetch task's 12 KB stack now sit in PSRAM (the 4-inch keeps its proven path unchanged).

- **(7B / 5B)** On the 1024x600 boards the picture no longer drifts out of alignment while the device writes to flash or serves the web UI: the panel interrupt now keeps running when the flash cache stalls, and the driver recovers from an underrun instead of restarting the whole frame.

- **(7B / 5B)** On the 1024x600 boards WiFi is configured for a configuration UI rather than for throughput - frame aggregation off, smaller receive and transmit queues, and the network buffers kept out of the PSRAM the panel already saturates - so the web interface keeps answering a browser's parallel requests instead of falling silent while the display still reports a connection, and TLS is given PSRAM as well, so the online tide forecast's secure connection no longer fails for lack of memory.

- **(7B / 5B)** Changing screens and opening the home launcher on the 1024x600 boards redraw far faster: a full-screen repaint used to cost 0.8-0.9 s, almost all of it work repeated once per drawing pass - scrollbar, border and rounded-corner handling on full-bleed background containers that need none of it - and the number of passes a full repaint takes has been halved from 120 to 60.

- **(7B / 5B)** On the 1024x600 boards the WiFi credentials now survive a filesystem update: they are kept in a second area that the update does not overwrite and are restored on the next boot, instead of the device dropping off the network until the WLAN was retyped on the touchscreen.

- **(7B / 5B)** On the 1024x600 boards the serial console is no longer flooded with watchdog errors - several hundred lines a second at startup and around a hundred a second in operation, which held up the rest of the boot - because the watchdog is now switched off in a single step, a step that on the newer core could otherwise abort into a boot loop.

- **(7B / 5B)** On the 1024x600 boards, opening a full-screen menu could reboot the device because the drawing needed a working buffer mid-frame and the memory pool was by then too fragmented to hand one out; those buffers are now reserved at startup while the pool is still intact.

- **(7B / 5B)** On the 1024x600 boards the NOW marker on the depth echogram and the diagonal points of the wind rose (NW, SW) no longer wrap onto a second line, where the larger fonts had outgrown label boxes that were only scaled proportionally.

- **(Tooling)** Building for one board no longer breaks the next: the 4-inch environment now has its own package and build directory and the simulator its own build directory, so a 4-inch or simulator build no longer silently wipes the 7B and 5B firmware (the build still reported success and the next flash found nothing) and no longer evicts the toolchain the panel boards need. Build output also moved out of the project tree, whose path contains a space that the 1024x600 builds refuse.

- **(Tooling)** The deploy tool can now be driven from a script or a build step: `deploy.bat -h`, `--help` and `/?` reach the help instead of aborting with a parameter error, an unknown action prints the usage and exits with code 2, and an unattended run no longer hangs on a "press any key" prompt or swallows a failed build - the exit code is returned first.

- **(Tooling)** On Windows the build no longer falls back to a full recompile after ccache was installed with winget: it is now also found in the winget install location, so a shell that still carries the old PATH keeps using the compiler cache.

## [1.0.0] - 2026-08-14

Initial release, for the 4-inch Waveshare ESP32-S3-Touch-LCD-4: NMEA 2000
instrument display with wind and trim, speed and polar, depth, engine, rudder,
AIS radar, wind history, autopilot, media (stereo remote), attitude, anchor watch, tanks,
battery, weather, clock, VMG, route and configurable data grids, a web
configuration interface, and light, dark and night themes.
