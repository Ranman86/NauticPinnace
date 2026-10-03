# OpenBridge icon trial

NauticPinnace can draw the maritime icons of the **OpenBridge Icon Pack** (CC BY 4.0) instead
of the Font Awesome glyphs built into LVGL - the same icon for the same meaning as NauticPi.
It is a **trial**: switchable at runtime, `classic` by default, and removable in one go. Every
place that belongs to it carries the comment `OpenBridge icon trial`.

## The switch

`config.json` → `ui.icons`:

| Value | Panel | Web configuration |
|---|---|---|
| `"classic"` (default, also when the key is missing or unknown) | the Font Awesome glyphs, **pixel-identical** to the firmware before the trial | the emoji, as before |
| `"openbridge"` | the OpenBridge icons | the OpenBridge icons, plus the attribution line at the bottom |

Set it in the web configuration with the selector in the header, next to theme and language.
It takes effect **at once**: saving triggers the same live rebuild as a theme or language
change (screens, sidebar, floating buttons, demo banner); the home launcher and the settings
overlay are built on every open anyway. The web page switches without a reload.

## How it works

- `tools/gen_icons.py` reads `tools/icons/openbridge-map.json` (shared with NauticPi, copied
  unchanged) and the Figma export (outside the repository - it contains the OpenBridge logos
  and must never be committed). The code that resolves and normalises an icon is taken
  unchanged from NauticPi's generator, so both products draw the same shapes.
- It writes one 4-bpp LVGL bitmap font per pixel size a font role uses
  (`src/display/fonts/ob_icons_<px>.c`, Private Use Area from U+E000), the code points
  (`src/display/fonts/ob_icons.h`) and the inline SVG sprite in `data/index.html`
  (between the `BEGIN/END OpenBridge icon trial` markers).
- `Theme.cpp` chains the nearest icon font behind every Montserrat size
  (Montserrat → latin_suppl → icons), so any label in any font role can show an icon, also
  inline in text (alarm banner, shallow-water and over-rev warnings).
- `src/display/Icons.{h,cpp}`: `npSym(NP_ICON_...)` returns the classic `LV_SYMBOL_*` string or
  the PUA string, depending on `ui.icons`.

Regenerate after a change to the map, the export or the generator:

    python tools/gen_icons.py --export <path>/openbridge-figma-export.json
    python tools/gen_icons.py --check      # exit 1 if anything on disk is stale

(`--export` can be left out when `OPENBRIDGE_EXPORT` is set, or when the export sits at
`Provenance/OpenBridge/` next to this repository or next to exactly one sibling folder of it -
where the NauticPi workspace keeps it. `--preview sheet.png` writes a contact sheet of every
glyph at every size.)

## Mapping

One icon per meaning, as in NauticPi. Where the table says **stays**, the pack has nothing
fitting and NauticPi keeps the old sign as well.

**Home launcher** (`HomeOverlay.cpp`)

| Screen | classic | OpenBridge |
|---|---|---|
| Wind & Trim | `LV_SYMBOL_REFRESH` | `wind` |
| Speed & Polar | `LV_SYMBOL_CHARGE` | `stw` |
| Depth | `LV_SYMBOL_DOWN` | `depth` |
| Engine | `LV_SYMBOL_POWER` | `engine` |
| Rudder | `LV_SYMBOL_SHUFFLE` | `propulsion-rudder` |
| AIS radar | `LV_SYMBOL_GPS` | `ais-target-activated-iec` |
| Wind history | `LV_SYMBOL_LIST` | `trend` |
| Autopilot | `LV_SYMBOL_PLAY` | `command-autopilot` |
| Media | `LV_SYMBOL_AUDIO` | `sound` |
| Attitude | `LV_SYMBOL_LOOP` | `roll` |
| Anchor watch | `LV_SYMBOL_DOWNLOAD` | `anchorwatch` |
| Tanks | `LV_SYMBOL_TINT` | `tank` |
| Battery | `LV_SYMBOL_BATTERY_FULL` | `battery-vertical-full` |
| Weather | `LV_SYMBOL_EYE_OPEN` | `weather` (32×32 grid) |
| Clock | `LV_SYMBOL_BELL` | `time` |
| VMG | `LV_SYMBOL_UP` | **stays** - no VMG icon |
| Route | `LV_SYMBOL_NEXT` | `waypoint-next-iec` |
| Data grids | `LV_SYMBOL_KEYBOARD` | `table` |
| Settings tile | `LV_SYMBOL_SETTINGS` | `settings-iec` |

**Panel, further places**

| Place | classic | OpenBridge |
|---|---|---|
| Sidebar home (`SideBar.cpp`, both orientations) | `LV_SYMBOL_HOME` | `home` |
| Sidebar and floating back / forward (`SideBar.cpp`, `DisplayManager.cpp`) | `LV_SYMBOL_LEFT` / `RIGHT` | `chevron-left-google` / `chevron-right-google` |
| Settings (`SideBar.cpp`, `DisplayManager.cpp`) | `LV_SYMBOL_SETTINGS` | `settings-iec` |
| Close (`ConfigOverlay.cpp`, `HomeOverlay.cpp`) | `LV_SYMBOL_CLOSE` | `close-google` |
| Anchor-drag banner (`DisplayManager.cpp`), shallow water (`DepthScreen.cpp`), over-rev (`EngineScreen.cpp`) | `LV_SYMBOL_WARNING` | `alarm` |
| Demo banner, both sides (`DisplayManager.cpp`) | `LV_SYMBOL_WARNING` | `simulation` |
| Media: back / play / pause / next (`MediaScreen.cpp`) | `LV_SYMBOL_PREV` / `PLAY` / `PAUSE` / `NEXT` | `media-skip-previous` / `media-play` / `media-pause` / `media-skip-next` |
| Media: volume / mute (`MediaScreen.cpp`) | `LV_SYMBOL_VOLUME_MAX` / `MUTE` | `sound` / `sound-muted` |
| Anchor radius − / + (`AnchorScreen.cpp`) | `LV_SYMBOL_MINUS` / `PLUS` | **stays** - no plus/minus in the pack |
| Canvas drawings: AIS triangles, hull on the wind screen, anchor glyph in the plot | own graphics | **stays** |

**Web configuration** (`data/index.html`)

| Place | classic | OpenBridge |
|---|---|---|
| Tides | 🌊 | `tide` |
| Polar data | ⛵ | **stays** - no polar icon |
| Calibration | 🧭 | `sensor-gyro` |
| Import / export | 💾 | `save-proposal` |
| CSV import / export | 📥 / 📤 | `file-download-google` / `file-upload-google` |
| CSV paste | 📋 | `clipboard` |
| Firmware update | ⬆ | `sync-google` |
| WiFi | 📶 | `wifi2-google` |
| Live sensor data, data sources, NMEA 2000 | 📡 / 📡 / 🔌 | `io` |
| Theme toggle (shows the NEXT theme) | 🌙 / 🌗 / ☀ | `palette-night` / `palette-dusk` / `palette-day` |
| Design | 🎨 | `palette-day` |
| Display | 🖥 | `display-brilliance-iec` |
| Delete grid | 🗑 | `delete` |
| "saved / applied" ticks (toasts, logo) | ✓ | `check-google` |
| Reorder ▲ / ▼, expand ▸ / ▾ | ▲ ▼ ▸ ▾ | `chevron-up-google` / `chevron-down-google` / `chevron-right-google` / `chevron-down-google` |
| Boot logo, logo preview | 🖼 | `image` |
| Demo section, demo toast, "active" dot | ⚠ / ⚠ / 🔴 | `simulation` |
| Converter script | 🐍 | **stays** |
| Online / offline dot | ● | **stays** (text) |
| Brand logo, hull, canvas plots | own graphics | **stays** |

The hint text "Reihenfolge mit ▲▼ ändern" keeps its triangles: it is prose, not a control.

## Cost

Measured with `pio run` on 2026-10-02, before and after the trial (the "after" build also
contains the media-screen rename):

| Board | icon sizes (px) | glyph data | flash before | flash after | flash Δ | RAM Δ |
|---|---|---|---|---|---|---|
| 4-inch | 10 12 14 16 24 32 40 48 | 52 144 B | 2 304 641 B | 2 361 053 B | +56 412 B | +456 B |
| 5B | 12 14 18 20 28 40 48 | 49 851 B | 2 334 693 B | 2 388 285 B | +53 592 B | +336 B |
| 7B | 12 14 18 20 28 40 48 | 49 851 B | 2 342 961 B | 2 396 613 B | +53 652 B | +336 B |

The sizes are the pixel sizes of the font roles that carry an icon, plus the smallest
Montserrat the board compiles; each Montserrat size gets the largest icon font not larger
than itself, so no icon is ever clipped by its label.

On the 7B and 5B the fonts also occupy PSRAM: `SPIRAM_RODATA` copies all read-only data there
at boot. RAM otherwise: one extra `lv_font_t` per Montserrat size (≈ 40 bytes each) for the
second link of the fallback chain. The Font Awesome glyphs stay in Montserrat during the trial;
dropping them later saves up to about 284 KB (4-inch) / 188 KB (7B/5B).

The web sprite adds about 16 KB to `index.html` (about 6 KB gzipped).

## Removing the trial

**If the OpenBridge icons stay** (the trial becomes the default): set the default of
`AppConfig::iconSet` to `ICON_SET_OPENBRIDGE`, decide whether `classic` remains selectable,
and only then consider taking the Font Awesome glyphs out of Montserrat.

**If the trial goes:**

1. Delete `src/display/Icons.{h,cpp}`, `src/display/fonts/ob_icons*.{c,h}`, `tools/gen_icons.py`,
   `tools/icons/`, `LICENSES/CC-BY-4.0.txt` and this file.
2. At every place marked `OpenBridge icon trial` put the classic symbol back:
   `npSym(NP_ICON_X)` → the `LV_SYMBOL_*` in the "classic" column above (inline uses:
   `snprintf(b, n, "%s %s", npSym(NP_ICON_WARNING), …)` → `snprintf(b, n, LV_SYMBOL_WARNING " %s", …)`).
   `git grep -n "OpenBridge icon trial"` lists them: `Config.{h,cpp}`, `WebConfig.cpp`,
   `Theme.cpp`, `HomeOverlay.cpp`, `SideBar.cpp`, `DisplayManager.{h,cpp}`
   (incl. `refreshTrialChrome()`), `ConfigOverlay.cpp`, `DepthScreen.cpp`, `EngineScreen.cpp`,
   `MediaScreen.cpp`, `LicenseText.h` (the `LIC_ICONS_DE` / `LIC_ICONS_EN` macros and the two
   lines that insert them), `platformio.ini`
   (`Icons.cpp` in the simulator list), `data/index.html` (sprite block, `.ob` style, header
   selector, `obIco`/`applyIcons`/`setIconSet`, `data-ob` attributes, attribution footer,
   the `ICONS_*` / `OB_ATTRIB` strings) and section 4 "Artwork" in `THIRD-PARTY-NOTICES.md`
   (renumber the sections after it).
   The `BTN_CSV_*` strings lost their emoji to separate spans; put the emoji back into the
   strings when removing the spans.
3. The licence documents that mention the trial without carrying the marker:
   - `LICENSES/README.md`: delete the `CC-BY-4.0.txt` row, drop "the Google Material Icons
     that some OpenBridge icons are based on" from the `Apache-2.0.txt` row and the
     "NOT the `ob_icons_*` fonts" clause from the `OFL-1.1-Montserrat.txt` row, and put the
     BSH sentence back under "Not duplicated here", as in v1.1.0: the BSH tide data fetched
     at runtime is CC BY 4.0 (<https://creativecommons.org/licenses/by/4.0/>) - data, not a
     distributed software component.
   - `README.md`, section "Licence": the clause naming the OpenBridge Icon Pack.
   - `.gitignore`: the `*figma-export*.json` and `Provenance/` lines may stay; they only
     keep a stray copy of the export out of the repository.
4. A stored `ui.icons` key in a device's `config.json` is then simply ignored.
