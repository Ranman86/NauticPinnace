# Wind and sails: implementation reference

Exact record of three features of the NauticPinnace firmware, written so that
they can be rebuilt in **NauticPi** (C# server, JavaScript canvas client) by a
developer who has not seen this code, with the same numbers:

1. **True wind computed from the apparent wind** - the display takes only the
   apparent wind from the bus and works out TWS, TWA and TWD itself, with an
   optional COG fallback for TWD.
2. **Wind damping** - four time constants, one each for apparent wind speed,
   apparent wind direction, true wind speed and true wind direction.
3. **The sail picture of the Wind & Trim screen**, including the selectable
   downwind sail: none, a symmetric spinnaker drawn in panels or as a shaded
   balloon, or a gennaker.

Status: firmware 1.3.0, all three boards (4-inch, 7B, 5B), as of
2026-10-14; 4.8 to 4.10 were re-checked against the code on 2026-11-02 (the
keel is no longer painted, the compass letters are drawn through one shared
draw context). The code is the source of truth; this document was written from
it, and every number in section 7 was printed by that code.

This is also the downwind sail's design and verification record, kept
separately until 2026-11-02 and merged in then: its setting into 6.2, its trim
into 4.2 to 4.6, the transom position into 4.8, its draw order and halo into
4.9, its reef badge into 4.11, its geometry into 5.1 to 5.4 and 5.6, its pixels
and cost per frame into 5.5, its checker results and known limits into 5.7 and
5.8. Both check programs were re-run then (7.3) and reproduce 7.2 and
`docs/downwind_vectors.txt` unchanged. For users, the README section "Wind:
apparent in, true wind computed" describes the true wind and the damping of
sections 2 and 3.

Sections 1 to 6 are normative for a port. MUST, MUST NOT, SHOULD and MAY have
their usual meaning. Where NauticPi may differ on purpose (its own boat drawing,
its frame rate), the section says so and states what has to stay equal.

| Part | Where in this repository |
|---|---|
| True wind, damping filters, pipeline | `src/nmea/WindCalc.h`, `src/nmea/WindCalc.cpp` |
| Glue between bus, config and pipeline; 1 Hz wind history | `src/nmea/WindPipeline.h`, `src/nmea/WindPipeline.cpp` |
| Bus input (PGN parsing, units, calibration) | `src/nmea/N2kHandler.cpp`: `onWind`, `onHeading`, `onMagVariation`, `onSpeed`, `onCogSog` |
| Damping limits, default, migration | `src/config/WindDampCfg.h` |
| Config keys, load and save | `src/config/Config.h` (`AppConfig`, `SpiType`), `src/config/Config.cpp` (`toJson`, `fromJson`, `spiTypeName`, `spiTypeFromName`) |
| Factory defaults | `data/config.json` (6.1) |
| Limits for the web page | `src/config/WebConfig.cpp`, `GET /api/info` |
| Web interface | `data/index.html` (Devices tab: damping; Display tab, screen list, Wind & Trim: sails, with `SPI_TYPES`, `spiTypeOf()`, `screenConfigHtml()`, strings `SAIL_SPI_*`) |
| Sail logic and drawing | `src/display/screens/WindScreen.h`, `WindScreen.cpp` |
| When a kite is shown, lee side, trim, smoothing, resets | `WindScreen::computeSailState()` (4.2), `latchLeeSide()` applied in `drawInstrument()` (4.3), `drawBoat()`, `resetRig()`, `onShow()` (4.5) |
| Downwind sail pixels | `WindScreen::drawSpinnaker()` (dispatch by type), `drawSpiGores()`, `drawSpiBalloon()`, `drawGennaker()`, `spiLines()`, `spiRigSide()` (5.5, 4.6); reef badge `drawReefBadge()` (4.11) |
| Downwind sail geometry (no LVGL) | `src/display/screens/SpiGeom.h`, `SpiGeom.cpp` |
| Downwind sail clearance checker and its test vectors | `docs/downwind_check.cpp`, `docs/downwind_vectors.txt` (5.7) |
| Generator of the test vectors in section 7 | `docs/wind_vectors.cpp` (how to run both programs: 7.3) |

---

## 1. Conventions

### 1.1 Angles, sides and frames

- **Wind angles relative to the bow** (AWA, TWA): degrees in **(-180, +180]**,
  **starboard positive, port negative**. 0 = from dead ahead, 180 = from dead
  astern (+180, never -180, see 1.3).
- **TWD**: degrees in **[0, 360)**, true, the direction the wind blows FROM.
- **Heading, COG**: degrees, as received; WindCalc normalises them to [0, 360).
- **Wind maths frame** (WindCalc): x forward along the bow, y to starboard.
  True wind is **water referenced**: the boat moves at its boat speed along its
  heading. Leeway, current and heel are ignored.
- **Drawing frame** (Wind & Trim screen, SpiGeom): **boat units**, origin at
  the boat centre, x athwartships (+ = starboard), y fore and aft in **screen
  direction** (+ = aft; the bow is at y = -32). A point (x, y) is drawn at
  `(CX + x*BS, CY + y*BS)`. See 5.1 for CX, CY and BS.
- **Lee side** `ss` (`SailState::leewardSide`): +1 = lee to starboard (wind
  from port), -1 = lee to port (wind from starboard). "Signed" trim angles
  are + towards starboard.

### 1.2 Units

Speeds in knots, angles in degrees, time constants in seconds, clocks in
milliseconds. The bus carries speeds in m/s; the firmware converts with
`kn = m/s / 0.5144` (float). A port MAY use 1852/3600 instead: on the bus's
0.01 m/s grid both give the same decisions at the COG limits of 2.6 (checked:
0.40, 0.41, 0.50, 0.51 m/s fall on the same side of 0.79 and 0.99 kn with
either factor). Angles on the bus are radians, converted with RadToDeg.

### 1.3 Normalisation

```
norm180(a): NaN -> NaN; a = fmod(a, 360);  if a > 180: a -= 360  elif a <= -180: a += 360
norm360(a): NaN -> NaN; a = fmod(a, 360);  if a < 0: a += 360;   if a >= 360: a -= 360
```

`fmod` keeps the sign of the dividend; JavaScript's `%` and C#'s `%` on
floating point numbers do the same and can be used directly. The last line of
`norm360` exists because `-1e-6 + 360` rounds to exactly 360 in float.
N2kHandler uses its own `calNorm180` / `calNorm360` (while loops, range
[-180, 180] and [0, 360)) on raw bus values; the pipeline normalises again, so
the outputs are always in the ranges of 1.1.

### 1.4 Not known = NaN = "--"

Every value that is not known is NaN, never a 0 stand-in, and every display of
a NaN prints `--`. NaN flows through the formulas on purpose: any comparison
with NaN is false, which several rules below rely on (no true wind angle = no
kite, for example). A port MUST keep NaN (JavaScript `NaN`, C# `float.NaN`)
rather than `null`/0, or restate every such rule explicitly.

### 1.5 Clock and freshness

Every input carries the time it arrived (`millis()`, 0 = never received).

```
fresh(stamp, now, window) = stamp != 0 && (int32)(now - stamp) < window
dropStale(stamp, now, window): if stamp != 0 && !fresh(stamp, now, window): stamp = 0
FRESH_MS = 5000      STW, SOG, heading, COG, a received true wind reference and the
                     COG latch count only this long; an apparent sample shuts out
                     received true wind for this long
STALE_MS = 10000     a wind group (apparent, true) not refreshed this long is blanked
```

The age is signed so that a stamp a few ms ahead of `now` (written by another
task after the caller read the clock) counts as fresh. Because a 32-bit stamp
left alone for 24.8 days would wrap round to "fresh" again, every stamp is
cleared with `dropStale` once stale (2.9). A port with a 64-bit monotonic clock
needs no wrap handling but MUST keep "0 = never", "age exactly 5000 ms is no
longer fresh" and the clearing.

---

## 2. True wind from the apparent wind

### 2.1 Bus inputs

| PGN | Field | Stored as | Calibration (config `cal.`) | Notes |
|---|---|---|---|---|
| 130306 ref 2 (apparent) | speed, angle | passed to the pipeline | `AWS * aws_fac / 100`, `calNorm180(AWA + awa_off)` | The one wind the boat measures. N/A fields become NaN. |
| 130306 ref 4, 3, 0 | speed, angle | passed to the pipeline as a received true wind | none (the sender calibrated it) | Only a fallback (2.7). Ref 1 (magnetic) is ignored. |
| 127250 | heading | `hdg = calNorm360(deg + hdg_off)`, stamp | `hdg_off` | Only when the heading is not N/A. |
| 127250 | reference | `hdgMagnetic = (ref == magnetic)` | | Written together with the heading, so only when the heading is not N/A. "error" / "unavailable" count as true. |
| 127250 | deviation | `hdgDeviation` = deg, or NaN when N/A | | Written together with the heading, so only when the heading is not N/A. Belongs to THIS reading: a frame with a heading but no deviation clears it; a frame whose heading is N/A leaves it alone. |
| 127250 | variation | `variation` = deg, when not N/A | | Written even when the heading itself is N/A. |
| 127258 | variation | `variation` = deg, when not N/A | | |
| 128259 | STW (water referenced) | `stw = m/s / 0.5144 * stw_fac / 100`, stamp | `stw_fac` | STW N/A leaves value and stamp alone. |
| 129026 | COG | `cog` = deg (not normalised), `cogMagnetic = (ref == magnetic)`, stamp | | COG N/A sets the COG stamp to 0 at once (`cog` keeps its value for other screens). |
| 129026 | SOG | `sog = m/s / 0.5144`, stamp | | SOG N/A leaves value and stamp alone. |
| 129029 | | nothing for the wind | | It does NOT carry the variation; an old bug wrote its age-of-correction field there. |

130306, 127250, 128259 and 129026 go through the source pinning first
(`sources.wind`, `sources.hdg`, `sources.stw`, `sources.pos`; -1 = any
sender); 127258 does not. Deviation and variation are east positive. The time
`now` handed to the pipeline is read after the stamps it is compared with.

The pipeline gets the boat inputs as one record:

```
BoatInputs { stw, stwMs, sog, sogMs, hdg, hdgMs, hdgMagnetic, deviation, variation, cog, cogMs, cogMagnetic }
```

### 2.2 Boat speed

```
boatSpeed = stw  if stw is not NaN and fresh(stwMs)
          = sog  else if sog is not NaN and fresh(sogMs)
          = NaN  otherwise  -> TWS, TWA and TWD are NaN; AWS and AWA still show
```

### 2.3 The formulas

```
apparent -> true:  ux = AWS cos AWA - V,  uy = AWS sin AWA
                   TWS = sqrt(ux^2 + uy^2)
                   TWA = norm180(atan2(uy, ux))   unless TWS < CALM_KN
true -> apparent:  ax = TWS cos TWA + V,  ay = TWS sin TWA
                   AWS = sqrt(ax^2 + ay^2),  AWA = norm180(atan2(ay, ax))
V = boat speed (2.2).  CALM_KN = 0.05 kn.
```

- Any NaN among AWS, AWA, V gives NaN for both outputs.
- **Calm**: when TWS < 0.05 kn the angle is rounding noise; TWA is then the
  previous **raw** TWA (`rawTwa_`, the last non-NaN raw TWA this pipeline
  computed or received), or NaN if there is none. TWS keeps its value.
- `true -> apparent` is used only by the demo mode and by the checkers.

### 2.4 True heading

```
trueHeading = NaN                        if hdg is NaN or not fresh(hdgMs)
            = norm360(hdg)               if the reference is true (or error / unavailable)
            = norm360(hdg + deviation + variation)   if magnetic, each term added only when not NaN
```

A magnetic heading with no variation anywhere on the bus is used without it
(it is then off by the local variation, as on every other screen). `hdg`
already contains `cal.hdg_off`.

```
trueCourse = NaN   unless cog is not NaN and fresh(cogMs) AND sog is not NaN and fresh(sogMs)
           = norm360(cog + variation)   if the GPS marks the COG magnetic and variation is not NaN
           = norm360(cog)               otherwise (a GPS has no deviation)
```

### 2.5 TWD and its sources

`TWD = true heading + raw TWA`, from the first source there is:

1. **the heading** (2.4). If it is there but the raw TWA is NaN (a calm
   with no previous angle), TWD is NaN - it does not fall through;
2. **another device's north-referenced TWD**: the last 130306 reference-0
   angle, `norm360(angle)`, while that reference was received less than
   FRESH_MS ago - its direction only, TWS and TWA stay the pipeline's own. It
   is recorded from every reference-0 frame with both fields valid, whether
   that frame was used or not (2.7);
3. **the COG, only if `cal.twd_cog` is on and the COG latch (2.6) is on**:
   `norm360(trueCourse + raw TWA)`; NaN if either is NaN;
4. otherwise NaN, which also keeps the wind history empty.

```
twdFrom(rawTwa):
    h = trueHeading()
    if h is not NaN:  return isNaN(rawTwa) ? NaN : norm360(h + rawTwa)
    rx = fresh(refMs[2], now, FRESH_MS) ? rxTwd : NaN     // refMs is indexed by rank (2.7): [2] = reference 0
    if rx is not NaN or not latchOn():  return rx
    c = trueCourse()
    return (isNaN(c) or isNaN(rawTwa)) ? NaN : norm360(c + rawTwa)
```

TWS and TWA never use a heading, so none of this touches them. The COG is an
approximation, hence the option (off by default): it is the boat's track, not
where the bow points, so current, leeway and drift turn TWD by the angle
between the two.

### 2.6 The COG latch

A hysteresis on the SOG that decides whether the COG may be the TWD source.

```
COG_MIN_SOG_KN = 0.99    switch ON  when SOG >= 0.99 kn
COG_OFF_SOG_KN = 0.79    stay ON while SOG >= 0.79 kn, OFF below

latchOn() = fresh(cogLatchMs, now, FRESH_MS)

trackCogLatch(b, now, twdFromCog):        // on EVERY used or standing-by sample, first thing
    limit = latchOn() ? 0.79 : 0.99
    on    = twdFromCog and not isNaN(trueCourse(b, now)) and b.sog >= limit
    cogLatchMs = on ? now : 0
```

**Why these numbers.** The owner asked for "on from 1.0 kn, off only below
0.8 kn". The bus carries SOG in steps of 0.01 m/s. A GPS doing 1 kn
(0.5144 m/s) sends 0.51 m/s, which arrives as 0.9914 kn; a limit of 1.0 would
turn it away and switch on only at the next step, 0.52 m/s = 1.0109 kn. Hence
0.99; the step below, 0.50 m/s = 0.9720 kn, stays out. Likewise a GPS doing
0.80 kn sends 0.41 m/s = 0.7970 kn, shown as "0.8", which must not count as
below 0.8: hence 0.79; 0.40 m/s = 0.7776 kn switches off. On the bus the band
in which the latch keeps its state is 0.41 to 0.50 m/s. Without the band a boat
doing about 1 kn (drifting, motoring slowly, coming alongside) made TWD flicker
between a value and `--` with every fix.

Consequences of the rule, all normative:

- It goes OFF at once, and then needs 0.99 again, when the option is off, when
  COG or SOG is not fresh or not available (a COG marked N/A clears the COG
  stamp, 2.1), on `reset()`, when the true wind goes stale (2.9), and when no
  sample has confirmed it for FRESH_MS (in such a gap the boat may have slowed
  down, or the GPS dropped out, unseen).
- Every sample moves it - apparent or received, used or standing by - and
  whichever source TWD actually comes from. Behind a heading or a
  reference-0 TWD it keeps following the SOG, so when that source goes quiet
  at, say, 0.9 kn, the COG takes over at once if the boat came down from
  1 kn or more, and does not if it never got there or has been below 0.8 kn
  since (vectors D3).
- It only decides the source; it never touches the damping. A change of
  source (heading to COG) carries the TWD filter on; only a sample with no
  direction at all (TWD NaN) restarts it.

### 2.7 Received true wind (another device)

PGN 130306 references 4 (water referenced, TWA), 3 (boat referenced over
ground, TWA) and 0 (north referenced, TWD) are **only a fallback** for a bus
without an apparent wind: such a sample is used only while no apparent sample
has arrived for FRESH_MS, and of the references still talking the best one
wins - 4, then 3, then 0 - instead of whichever frame came last.

```
receivedTrue(ref, tws, angle, b, now, damp, twdFromCog):
    rank = { 4: 0, 3: 1, 0: 2 }[ref]; any other ref: return false
    if isNaN(tws) or isNaN(angle): return false          // before the latch and the stamps
    trackCogLatch(b, now, twdFromCog)
    refMs[rank] = now
    if ref == 0: rxTwd = norm360(angle)                    // kept even when not used (2.5)
    if fresh(appMs, now, FRESH_MS): return false           // our own calculation wins
    for r in 0 .. rank-1: if fresh(refMs[r], now, FRESH_MS): return false   // a better one talks
    if ref == 0:
        rawTwd = norm360(angle)
        h = trueHeading()                                  // the heading only, never the COG
        rawTwa = isNaN(h) ? NaN : norm180(rawTwd - h)
    else:
        rawTwa = norm180(angle)
        rawTwd = twdFrom(rawTwa)                           // same stand-ins as 2.5
    updateTrue(tws, rawTwa, rawTwd, now, damp.trueSpd, damp.trueDir)
    return true
```

Note that `appMs` is set by every apparent sample, also one that produced no
true wind (no boat speed): an apparent wind on the bus always shuts the
received true wind out.

### 2.8 One apparent sample

```
apparent(aws, awa, b, now, damp, twdFromCog):
    if isNaN(aws) and isNaN(awa): return false             // nothing changes, not even the latch
    trackCogLatch(b, now, twdFromCog)
    dt = appMs ? float((int32)(now - appMs)) / 1000 : 0    // seconds, a FLOATING-POINT division (3.1)
    appMs = now
    AWS = awsFilter.update(aws, dt, damp.appSpd)           // 3.1
    AWA = awaFilter.update(awa, dt, damp.appDir)
    (rawTws, rawTwa) = apparentToTrue(aws, awa, boatSpeed(b, now), rawTwa_)   // RAW apparent wind
    if isNaN(rawTws): clearTrue(); return true             // no boat speed, or half a sample
    updateTrue(rawTws, rawTwa, twdFrom(rawTwa), now, damp.trueSpd, damp.trueDir)
    return true

updateTrue(rawTws, rawTwa, rawTwd, now, tauSpd, tauDir):
    dt = trueMs ? float((int32)(now - trueMs)) / 1000 : 0  // since the last true update, computed or received
    trueMs = now
    if not isNaN(rawTwa): rawTwa_ = rawTwa
    TWS = twsFilter.update(rawTws, dt, tauSpd)
    TWA = twaFilter.update(rawTwa, dt, tauDir)
    TWD = norm360(twdFilter.update(rawTwd, dt, tauDir))

clearApparent(): reset awsFilter, awaFilter; AWS = AWA = NaN; appMs = 0
clearTrue():     reset twsFilter, twaFilter, twdFilter; TWS = TWA = TWD = NaN; trueMs = 0; rawTwa_ = NaN
                 (the COG latch is left alone)
reset():         clearApparent(); clearTrue(); refMs[0..2] = 0; rxTwd = NaN; cogLatchMs = 0
```

True wind is computed from the **raw, undamped** apparent wind and damped
afterwards. Damping first and computing afterwards would give the same steady
state but a slower, lopsided response, and would put the apparent wind's time
constants into the true wind.

When a call returns true, the caller stamps `lastWindUpdate = now` (the "wind
is alive" stamp of the 2 Hz trace, 2.10). After every call and every tick the
five outputs are copied to the data model (`aws, awa, tws, twa, twd`), which is
what every screen, the sidebar and `/api/data` show.

The demo mode feeds the same door: its scenario produces a true wind, turns it
into an apparent wind with `trueToApparent` and hands only that over.

### 2.9 The clock (`WindPipeline::tick`, every display tick, in every mode)

```
expire(now):
    if appMs  and not fresh(appMs,  now, STALE_MS): clearApparent()
    if trueMs and not fresh(trueMs, now, STALE_MS): clearTrue(); cogLatchMs = 0
    for r in 0..2: dropStale(refMs[r], now, FRESH_MS)
    dropStale(cogLatchMs, now, FRESH_MS)
tick(now):
    expire(now); publish the five outputs
    dropStale(stwMs), dropStale(sogMs), dropStale(hdgMs), dropStale(cogMs)   (FRESH_MS)
    wind history (2.10)
```

A dead sensor goes blank after 10 s whichever page is showing; the next sample
starts the filters afresh, without a ramp from zero.

### 2.10 Wind history and trace

**Wind history** (the wind-history page): 360 samples of (TWD, TWS, time), one
per `WIND_HIST_MS = 1000` ms on a clock, so a full ring is six minutes of valid
true wind on every boat, whatever the sensor's rate.

```
P = 1000
if next != 0 and (int32)(now - next) < 0: return            // not due
next = (next != 0 and (int32)(now - next) < P) ? next + P : now + P
if not isNaN(TWD) and not isNaN(TWS): push(TWD, TWS, now)   // gaps are skipped, not stored
```

The next slot counts from the previous slot, so a late tick does not stretch
the six minutes; a tick more than a whole period late restarts the grid instead
of firing catch-up samples.

**Wind trace** (two curves inside the wind instrument, related but not part of
this port): 120 points of (TWA, TWS, AWA, AWS), one every 500 ms; a point is
all NaN when `lastWindUpdate` is 5 s old or more.

---

## 3. Damping

### 3.1 The filters

First-order low-pass for an uneven sample rate:

```
alpha(dt, tau) = 1                       if not (tau > 0)          (tau 0, negative or NaN: no damping)
               = 0                       else if not (dt > 0)      (no time has passed)
               = 1 - exp(-dt / tau)      otherwise

Scalar (speeds):
    update(x, dt, tau):
        if isNaN(x): reset(); return NaN            // the next valid sample starts afresh
        if not primed: y = x; primed = true; return y
        a = alpha(dt, tau)
        if a >= 1: y = x  else: y += a * (x - y)    // assigned, so "0 = off" is exactly off
        return y

Angle (directions), on the unit circle:
    update(xDeg, dt, tau):
        if isNaN(xDeg): reset(); return NaN
        cx = cos(xDeg), sx = sin(xDeg)
        a = primed ? alpha(dt, tau) : 1
        primed = true
        if a >= 1: c = cx; s = sx; y = norm180(xDeg); return y
        c += a * (cx - c);  s += a * (sx - s)
        if c*c + s*s > 1e-12: y = norm180(atan2(s, c))   // else keep the last output
        return y
    reset(): c = s = 0; y = NaN; primed = false
```

- **dt** is the time since the previous sample of the same group (2.8),
  `float((int32)(now - prev)) / 1000` s (the firmware's `secondsBetween`:
  `(float)(int32_t)(to - from) / 1000.f`); 0 for the first one. The division
  MUST be done in floating point: written with integers, as in C#
  `(int)(now - prev) / 1000`, it truncates to whole seconds, so every sample
  less than 1 s after the previous one gets dt = 0, alpha 0 and no update at
  all (vectors F6 catch this). There is **no clamp** on dt: a long gap simply
  gives alpha close to 1, and the stale rule (2.9) restarts a group after 10 s
  anyway.
- **First sample** after a start, a reset, a NaN or a stale gap is taken as it
  is - no ramp up from zero.
- **Angles as unit vectors**: averaging the numbers would swing from 359 to 1
  through 180, and a run alternating between +179 and -179 would read as a head
  wind. The vector is not renormalised; when it collapses to the origin (two
  exactly opposite directions mixed 50:50) the last output is kept.
- **Properties** (vectors F1-F5): after tau a step in a speed is 63.2 % through,
  and ten samples 0.1 s apart end exactly where one sample after 1 s does (for
  a constant input the result is `x (1 - exp(-t / tau))` whatever the spacing).
  For an angle the 63 % holds only for small steps, because the vector cuts
  across the circle: after tau a 90 deg step shows 66.5 %, 135 deg 74.1 %,
  170 deg 92.2 %.

### 3.2 Four time constants, four groups

| Setting | Config key (`cal.`) | Drives | dt measured since |
|---|---|---|---|
| apparent wind speed | `wind_damp_app_spd` | AWS | previous apparent sample |
| apparent wind direction | `wind_damp_app_dir` | AWA | previous apparent sample |
| true wind speed | `wind_damp_true_spd` | TWS | previous true update (computed or received) |
| true wind direction | `wind_damp_true_dir` | TWA **and** TWD (two separate filters, same tau) | previous true update |

Each group is filtered on its own; a time constant never touches another
group's outputs (vectors F6). A received true wind uses only the true pair.
The settings are read on **every** sample, so a save in the web interface
applies to the next sample, without a restart. The TWD filter restarts when TWD
is NaN for a sample (no direction source); the TWA filter does not care.

Apparent wind is for trimming and should react quickly; the true wind
direction is for tactics (the shifts, not every gust) and takes far more
damping; true wind speed lies in between. The firmware ships all four at 2 s.

### 3.3 Limits, default, clamping (`WindDampCfg.h`)

```
MIN_S = 0 (0 = off)     MAX_S = 30     DEFAULT_S = 2      (seconds, float)
clamp(v, keep) = keep            if v is NaN (key missing or not a number)
               = min(max(v, 0), 30)  otherwise           // clamped, never refused
```

### 3.4 Migration from the two older keys

Development builds before the split had two keys: `cal.wind_damp_spd` (AWS and
TWS) and `cal.wind_damp_dir` (AWA, TWA, TWD). Every config document read - the
file at boot, every web POST (which may be a partial patch), an import - goes
through:

```
appSpd  = clamp(orElse(wind_damp_app_spd,  wind_damp_spd), current appSpd)
appDir  = clamp(orElse(wind_damp_app_dir,  wind_damp_dir), current appDir)
trueSpd = clamp(orElse(wind_damp_true_spd, wind_damp_spd), current trueSpd)
trueDir = clamp(orElse(wind_damp_true_dir, wind_damp_dir), current trueDir)
orElse(a, b) = a unless a is NaN; a key that is missing, null or not a number reads as NaN
```

(Not JavaScript's `??`: a value read as NaN must fall back too.)

So a new key wins wherever it carries a number; where it does not, the old key
of the same kind stands in, so a config saved with the two settings damps
exactly as before; where neither carries a number, the value held so far is
kept, so a patch changes only what it names. `toJson` writes the four new keys only - the old ones are
read, never written again. Vectors G1.

### 3.5 Web interface (Devices tab)

In the calibration section, after "Wind: AWS factor", a hint paragraph and two
groups:

| Group heading (DE / EN) | Field (DE / EN) | id |
|---|---|---|
| Scheinbarer Wind / Apparent wind | Dämpfung Stärke, AWS (s) / Speed damping, AWS (s) | `cal-wd-app-spd` |
| | Dämpfung Richtung, AWA (s) / Direction damping, AWA (s) | `cal-wd-app-dir` |
| Wahrer Wind / True wind | Dämpfung Stärke, TWS (s) / Speed damping, TWS (s) | `cal-wd-true-spd` |
| | Dämpfung Richtung, TWA und TWD (s) / Direction damping, TWA and TWD (s) | `cal-wd-true-dir` |
| | TWD notfalls aus COG / TWD fallback from COG (checkbox) | `cal-twdcog` |

- Number inputs, `step="0.5"`. `min`/`max` are set from `GET /api/info`, field
  `limits.wind_damp_s = { "min": 0, "max": 30, "def": 2 }` - the page keeps no
  copy of the limits. Without them the fields have no min/max, which costs
  nothing: the firmware clamps whatever it is sent.
- Preselect: `new key ?? old key ?? limits.def ?? ''` (the same stand-ins as
  3.4: old speed for both speeds, old direction for both directions).
- Save (`POST`, a `cal` patch): the four new keys as numbers (`+value`, so an
  empty field is 0 = off) and `twd_cog` as a bool. Never the old keys.
- The texts, in full, are in 6.3.

---

## 4. The sail picture

Everything in this section happens once per painted frame of the Wind & Trim
screen (`WindScreen::drawInstrument` and `drawBoat`). The attitude variant of
the screen shares the outer ring but draws no boat.

### 4.1 Inputs of a frame and their NaN handling

Read under the data lock: `twa, awa, tws, stw, hdg` (the damped outputs of
section 2; `stw` is the data model's last STW as received - not the
boat-speed fallback, and not blanked when the log falls silent).

```
nTwa = isNaN(twa) ? 0 : twa;  while nTwa > 180: nTwa -= 360;  while nTwa < -180: nTwa += 360
nAwa = isNaN(awa) ? 0 : awa
haveTwa = !isNaN(twa);  haveAwa = !isNaN(awa)
shownTwa = haveTwa ? nTwa : NaN
shownAwa = haveAwa ? nAwa : NaN
knownAbs = haveTwa ? |nTwa| : NaN
sail = computeSailState(knownAbs, shownTwa, shownAwa, tws, stw)   // 4.2
then the lee latch (4.3)
```

No true wind angle (no boat speed) is a normal state. Nothing may read the 0
stand-ins as "head to wind": the corner values print `--`, and the sail picture
goes by the apparent wind (4.2). With no wind at all the sails stand neutral.

### 4.2 `computeSailState` - one frame's sail state

```
SailState defaults: boom 5, headsail 20, leewardSide -1, luffing = running = false,
                    useSpinnaker = useCodeZero = false, reefCount 0,
                    mainCamber 0.16, jibCamber 0.14, awaAbs 180, spiType none

computeSailState(absTwa, twa, awa, tws, stw):
    haveTwa = !isNaN(twa); haveAwa = !isNaN(awa)
    sideAng = haveTwa ? twa : (haveAwa ? awa : 0)
    leewardSide = (sideAng >= 0) ? -1 : +1            // wind from starboard -> lee to port
    if haveTwa:   luffing = absTwa < 28;      running = absTwa > 150
    elif haveAwa: luffing = |awa| < 18;       running = |awa| > 140
    // neither: both false

    absAwa = isNaN(awa) ? 0 : |awa|
    awaAbs = haveAwa ? |awa| : (haveTwa ? absTwa : 180)   // what the kite is trimmed to; never NaN
    boom = clamp(absAwa - 13, 4, 86)                   // ~13 deg angle of attack for the main
    headsail = boom * 0.82                             // the jib leads inboard of the main

    hasTws = !isNaN(tws)
    spi = config sail.spi_type   (read ONCE here and stored in the state, see below)
    window =
        gores, balloon: (100 < absTwa < 150 and tws < 25) or (absTwa >= 150 and tws < 20)
        gennaker:       (90 < absTwa < 150 and tws < 25) or (150 <= absTwa <= 160 and tws < 20)
        none:           false
    useSpinnaker = window and not luffing and hasTws
    useCodeZero  = not useSpinnaker and config sail.code_zero and hasTws
                   and 50 < absTwa < 95 and 2 < tws < 14

    hullSpeed = 2.43 * sqrt(lwl > 0 ? lwl : 9)         // config sail.lwl_m
    ratio = (!isNaN(stw) and hullSpeed > 0) ? stw / hullSpeed : 0
    reefCount = 0
    if hasTws:
        if   tws > 28 or ratio > 0.97: reefCount = 3
        elif tws > 22 or ratio > 0.93: reefCount = 2
        elif tws > 16 or ratio > 0.88: reefCount = 1

    if haveTwa:
        mainCamber = camberForTwa(absTwa); jibCamber = mainCamber * 0.85
    // else the defaults 0.16 / 0.14 stay

camberForTwa(a) = 0.10 if a < 35; 0.15 if a < 60; 0.19 if a < 100; 0.24 if a < 150; else 0.28
```

- The luffing limit 28 is fixed; the configurable no-go angle (`sail.nogo_deg`)
  only sets the zone ring and the point-of-sail name.
- **The window is decided by the true wind only.** `absTwa` is NaN without a
  true wind angle, and every comparison with NaN is false: no TWA = no kite, no
  TWS = no kite.
- The gennaker stops at 160: deeper than that an asymmetric kite is blanketed
  by the main, and a cruiser gybes downwind instead of running dead; from 160 on
  the normal running picture (no jib, main eased) is drawn.
- **One read of the setting per frame.** The web server can change the config
  at any moment; the window and the picture drawn MUST come from the same read
  (`SailState::spiType`: `drawSpinnaker()` draws that, not the live config).

### 4.3 Lee side held through a dead run

Dead downwind the sign of the wind angle flips with every wobble across 180,
and with it the whole rig - boom, jib and the spinnaker pole jumped across the
bow several times a second. Applied right after `computeSailState` (which stays
a const function of one frame's values):

```
sideAng = haveTwa ? shownTwa : (haveAwa ? shownAwa : NaN)
kept = latchLeeSide(sideAng, sail.leewardSide, kept)
if kept != 0: sail.leewardSide = kept

latchLeeSide(sideAng, fromSign, prev):
    if isNaN(sideAng):                   return 0          // no wind: forget
    if |sideAng| > 175 and prev != 0:    return prev       // hold
    return fromSign                                         // 175 or less decides a new side
```

The kept side covers the whole rig (main, jib and kite share `leewardSide`).
It is state of the screen object (`_leeSide`), one per screen (on the 1024x600
boards the two WindScreen instances, Wind & Trim and the attitude variant,
share a canvas, not this state), and is forgotten by `resetRig()` (4.5).
Vectors J1.

### 4.4 What stands at the bow

In this order of precedence (`drawBoat`):

1. **Butterfly** - `sail.butterfly` on and `running`: the jib poled out to
   windward (4.7). No kite is drawn, even when its window is open.
2. **Downwind sail** - `useSpinnaker` (4.2), drawn as `sail.spiType` (section 5).
3. **Code Zero** - `useCodeZero` (only without a downwind sail).
4. **Jib** - unless `running`.

The mainsail is always drawn. `spiDrawn = !butterfly && useSpinnaker` is kept
for the reef badge (4.11) and the halo of the compass letters (4.9);
`butterflyDrawn = butterfly` for the "Butterfly" label, which is printed after
the compass letters (4.9).

### 4.5 Trim targets and smoothing

```
ss = sail.leewardSide
butterfly = config sail.butterfly and sail.running
if sail.luffing:                                  // head to wind: flap round the centreline
    tgtBoom = 15 * sin(tick * 0.018)              // tick = millisecond clock (period 349 ms)
    tgtJib  = 17 * sin(tick * 0.018 + 0.7)
else:
    tgtBoom = ss * boom
    tgtJib  = butterfly ? -ss * 84 : ss * headsail
tgtSpiRot = ss * SpiGeom.symRotationDeg(awaAbs)   // 5.3
tgtGen    = ss * SpiGeom.genSheetDeg(awaAbs)      // 5.4

if not trimInit:                                  // first frame, or after resetRig()
    smBoom = tgtBoom; smJib = tgtJib; smSpiRot = tgtSpiRot; smGen = tgtGen; trimInit = true
else:
    k = 0.18                                      // per FRAME
    smBoom   += (tgtBoom   - smBoom)   * k
    smJib    += (tgtJib    - smJib)    * k
    smSpiRot += (tgtSpiRot - smSpiRot) * k
    if (tgtGen > 0) == (smGen > 0): smGen += (tgtGen - smGen) * k
    else:                           smGen  = tgtGen              // never swings across the boat
```

- All four are updated on **every** frame, whether a kite is drawn or not and
  for both kite kinds at once, so a kite that appears (wind drawn aft into its
  window) or a type changed in the web interface already sits where the boom's
  smoothing has the boom; nothing swings in.
- The gennaker never swings across: a target on the other side of its smoothed
  angle (a gybe while it was hidden - its window ends at 160 - or a jump of the
  wind) is taken at once. Swinging through 0 its clew would sweep the deck and
  the main, and it could reappear on the new side halfway across. The
  symmetric kite does swing: it stays up on a dead run, and rotation 0 is its
  square-run picture.
- `resetRig()` sets `trimInit = false` and the kept lee side to 0. It runs when
  the screen is shown again (`onShow`, also after a live theme rebuild) and at
  the top of `drawInstrument` when more than `RIG_STALE_MS = 2000` ms
  (`lv_tick_elaps`) have passed since the last painted frame: a modal overlay
  (the settings menu) stops `update()` without calling `onHide()` /
  `onShow()`. 2 s is several frame periods even on the boards that paint this
  screen at about 2 frames/s, so a slow frame never triggers it. The next frame
  then takes every trim and the lee side from the wind as it is. Without these
  resets the kite (and the boom) swung in from a wind that was minutes and
  possibly a gybe old.
- **k is per frame**, so the time constant follows the frame rate: about 0.5 s
  at 10 frames/s (`tau = -0.1 s / ln(1 - 0.18) = 0.504 s`), longer on a board
  that paints slower. A port that paints at another rate (a browser at 60
  frames/s) SHOULD use the time-based equivalent `k = 1 - 0.82^(dt / 0.1 s)`,
  i.e. tau = 0.504 s; fed with k = 0.18 per step it MUST reproduce vectors K1/K2.

### 4.6 Which side the lines go to

- **Symmetric kite**: pole, guy and sheet take the side of the **smoothed boom**,
  `side = (smBoom >= 0) ? +1 : -1`, not `ss`. In a gybe `ss` flips at once
  while the boom swings across over several frames; pole and guy on the new
  side at once would cross the mainsail still on the old one. It is also what a
  crew does: the pole is switched when the main has come across. In a steady
  state both agree. The crescent's rotation is smoothed with the boom's k, so on
  a reach it crosses 0 in the same frame as the boom (vectors K2).
- **Gennaker**: the sheet takes the sign of the smoothed sheeting angle,
  `(smGen >= 0) ? +1 : -1`.
- Main and jib: the sign of their own smoothed angle (4.7). Code Zero: `ss`.

### 4.7 Main, jib, Code Zero, butterfly

Boat units (5.1). Bow / jib tack `(0, -30)`, mast `(0, -6)`.

```
bss = smBoom >= 0 ? 1 : -1;  b = |smBoom|
main:  tack = mast,  clew = (bss * 32 sin b,  -6 + 32 cos b),   lee sign bss, camber mainCamber
fp = clamp(sail.foresail_pct / 100, 0.25, 1.60)
jss = smJib >= 0 ? 1 : -1;   j = |smJib|
jib:   tack = bow,   clew = (jss * 30 fp sin j,  -30 + 30 fp cos j),  lee sign jss, camber jibCamber
Code Zero: tack = bow, a = headsail angle (unsmoothed), clew = (ss * 34 sin a, -30 + 34 cos a),
           lee sign ss, camber 0.13
luffing: both cambers 0.05
```

**Sail foil** (`drawSailFoil`): a lens between the straight chord and a
quadratic Bezier that bellies to leeward, the draft peak at 45 % of the chord.

```
l = clew - tack;  len = |l|;  if len < 2 px: draw nothing
n = (-l.y, l.x) / len;  if (n.x < 0) != (leeSign < 0): n = -n      // normal to leeward
ctrl = tack + 0.45 l + n * camber * len
polygon: tack, then Q(i/10) for i = 1..10 of Q(t) = (1-t)^2 tack + 2(1-t)t ctrl + t^2 clew
fill the polygon; outline the 10 curve segments (the leeward edge)
```

The firmware does this in pixels and truncates every vertex to an integer.

Drawing of the rig (colours in 4.10):

- jib and main foil: fill `windSail` at 50 % (luffing: `red` at 40 %), leeward
  edge `text` colour 2 px opaque (luffing: `red`);
- jib sheet: jib clew to `(9 jss, 4)`, `green` 50 %, 1 px;
- boom spar: mast to main clew, `windRigging` opaque 2 px, over the main;
- mast: a disc of diameter `UI_S(6)` px (6 px on the 4-inch, 8 px on the
  1024x600 boards) at the mast point, `windRigging`;
- butterfly: the jib foil as above (its target is -ss 84), a whisker pole mast
  to jib clew in `windRigging` 70 % 2 px, the label "Butterfly" (accent colour,
  tiny font, centred over the bow in a text box `72 UI_SF` wide whose top is at
  `CY - 35 BS`; printed after the compass letters, 4.9);
- Code Zero: foil `yellow` at 200/255, edge `yellow` 1 px, label "C0" at the
  middle of the chord, sheet clew -> fairlead `(8.7 ss, 3.3)` `green` 60 % ->
  winch `(7 ss, 14.7)` `green` 50 %, 1 px.

### 4.8 Hull and fixed parts

The kite was tuned against this boat; a port with other proportions MUST map
its boat into these units or re-check (5.6).

Hull: 18 stations `{y, half beam}` bow to stern (stem y = -32, transom
y = +32), mirrored, filled with the inner background `windInner` and outlined
in `windHull` 2 px (the firmware
truncates the 36 vertices to integer pixels, like the foil's, and draws the
outline through the truncated points; a canvas port need not):

```
{-32.0, 0.07} {-28.2, 2.43} {-24.5, 4.36} {-20.7, 6.06} {-16.9, 7.47} {-13.2, 8.65}
{ -9.4, 9.53} { -5.6,10.13} { -1.9,10.57} {  1.9,10.88} {  5.6,11.08} {  9.4,11.19}
{ 13.2,11.12} { 16.9,10.90} { 20.7,10.53} { 24.5,10.06} { 28.2, 9.35} { 32.0, 4.82}
```

Keel: ellipse centre `(0, 3.3)`, radii 1.8 x 4.3, plus a line `(0,-1)`-`(0,7)`
3 px, both `windMast`. The firmware no longer paints it: drawn before the
opaque hull fill, it lay wholly inside it (the hull is 10 to 11 units wide
there), so dropping it changed no pixel on either board in any wind, trim or
theme (checked in the simulator). Its outline stays the reference for the reef
badge's clearance (4.11, 5.7); a port MAY leave it out as well.
Forestay: bow `(0,-30)` to mast, `windRigging` 60 % 1 px.
Rudder: pivot `(0, 24.7)`, blade half length 6 along the rudder angle, 3 px,
pivot disc radius `2.5 UI_SF` px, `windFlow`.

### 4.9 Draw order

Whole instrument (`drawInstrument`), back to front: background, bezel, zone
ring, degree ticks and labels, inner circle; then flow lines (from TWA, or AWA
with `sail.windlines_app`; none when that angle is NaN), the compass card's
ring (its ticks, and in north-up mode the heading index), the boat, the compass
card's N/E/S/W letters (German O for east), the "Butterfly" label (butterfly
mode only); then the wind traces, inner and outer border, VMG markers, TWA
pointer, AWA pointer, corner values, heading box, point-of-sail name, reef
badge, rudder arc, polar bar.

Boat (`drawBoat`): hull fill, hull outline, forestay, the foresail (4.4:
butterfly, kite, Code Zero or jib), mainsail foil, boom spar, mast, rudder (the
keel that used to come first is no longer painted, 4.8). This order is the one
the boat had before the downwind sail; only the butterfly mode's label left it
(below).

**Compass letters above the boat (owner's decision, 1.3.0).** Until then the
whole card was drawn before the boat, and near the bow the kite hid the lower
part of a letter. Now the card is drawn in two layers with the same rotation
(`drawCompassRose(hdg, CARD_RING)` before `drawBoat`, `drawCompassRose(hdg,
CARD_LETTERS)` right after it): ticks and heading index under the boat, the
letters over it - over hull, sails, kite, rig and rudder - and still under
everything drawn after the boat (wind traces, borders, VMG markers, pointers,
corner values, heading box, point-of-sail name, reef badge, rudder arc, polar
bar). The heading box (235/255 opaque) hides only the upper part of a letter
near the bow: at the bow 6 of the glyph's 10 pixel rows on the 4-inch board and
7 of 13 on the 1024x600 boards, still faintly visible through it; the lower 4 /
6 rows stand below the box, over the stem tip. The box reaches part of a letter
up to about 13 to 14.5 deg off the bow (W, the widest glyph, furthest) and its
full width only within about 8 to 9.5 deg.

**"Butterfly" label over the letters.** The label of the butterfly mode (4.7)
stands over the bow, in the letters' band: a letter within about 15 deg of the
bow overlaps it. It is printed right after the letters (`drawInstrument`, when
`drawBoat` drew the butterfly rig on the same frame, `butterflyDrawn`), not
inside `drawBoat`, so it keeps the place over the letters it had before the
letters moved over the boat. Its upper rows lie under the heading box (5 of
13 ink rows on the 4-inch board, 6 of 14 on the 1024x600 boards), as before.

The letters (for a port): each centred on radius `R_INNER - 21 UI_SF` (127
UI_SF; 33.4 units) at screen angle `bearing - rot` (bearing 0/90/180/270, `rot`
= heading in course-up mode, 0 in north-up mode or without a heading), in a
text box `16 UI_SF` wide whose top is `8 UI_SF` above that point, centred, the
theme's `small` font (14 px by default on the 4-inch grid); N in `red`, the
other three in `windTickMaj` (dark #557088, light #506880, night #A04828). Their
text boxes span about 31.3 to 35.5 units from the centre on the 4-inch board
(31.3 to 35.7 on the 1024x600 boards, where `small` is 18 px with a 21-px
line). LVGL puts a glyph at `line height - base line - glyph height` below the
box top: at the bow the glyph rows are 94..103 on the 4-inch board and
117..129 on the 1024x600 boards. Every downwind sail stays inside the
radius budget of 5.6 (outline at most 32.47 units, plus half its 2-px edge), so
it lies under at most the bottom quarter of a glyph (measured worst case, a
letter 15 deg off the bow over a dead-run crest: 3 to 4 of its 13 pixel rows on
the 7B, 2 to 3 of 10 on the 4-inch). On the kite's orange neither letter colour
reads: contrast 1.1:1 to 2.2:1 in the three themes (N on orange: dark 1.31,
light 1.50, night 1.13; E/S/W: 2.21, 1.59, 1.43), and in the night theme an S
or O lost its lower half to the crest. This was checked in light, dark and
night on both boards, with simulator shots of the letters over the balloon and
panel crests, the gennaker, the main, the jib and the transom.

**Halo over a kite.** While a downwind sail is drawn on the frame (`spiDrawn`,
4.4) a letter whose screen angle lies within `LETTER_HALO_DEG = 55` deg of the
bow is drawn five times: four copies in `windInner`, offset by one pixel to the
left, right, up and down (`(-1,0) (+1,0) (0,-1) (0,+1)`, the text box moved,
nothing else changed), then the letter in its own colour on top. That puts a
1-px rim of the inner colour round every stroke - 3.2:1 (light), 4.7:1 (night)
and 7.9:1 (dark) against the orange - and is invisible on the bare inner circle.
The screen angle is `bearing - rot` folded into -180 .. 180.

The sector was measured in 2-D: every kite as drawn (symmetric kite at every
rotation, gennaker at every sheeting angle on either side, fill and edge lines
in pixels) against each letter's glyph box at every screen angle (step 0.05
deg). The widest glyph, W (16 x 10 px on the 4-inch board, 20 x 13 on the
1024x600 boards), is reached up to 49.75 deg off the bow (49.65 on the
1024x600 boards), O up to 48.0, N, S and E up to 47; the 16 x 16 / 20 x 21 px
text boxes up to 50.5 deg. With the theme's `small` font set one or two steps
larger (16 or 18 px), a W is reached up to 53.4 deg. 55 covers all of these and
still holds at most two letters, which stand 90 deg apart, so the worst case
of the cost below is unchanged. The first tuning used 50, from a
one-dimensional estimate (47.3 deg for the text boxes' inner edge plus 3.6 for
half a box); that held the default fonts by only a quarter of a degree and
missed a larger font, hence 55. A port with another
font or letter radius MUST re-measure: the sector has to contain every screen
angle at which a kite pixel lies inside a letter's glyph box.

Cost: at most two letters lie in the sector, so at most 8 extra text draws of
one letter each per frame, and none without a kite. Each copy blends the
letter's glyph box; the widest pair, W and N both in the sector, gives at
most 1,664 blended pixels on the 1024x600 boards (6.7 KB of memory traffic)
and 1,000 on the 4-inch board (4.0 KB), against the kite's 31 KB. On the device
the four letters and their copies are drawn through one LVGL draw context
(`CdLvBatch`, `src/display/CanvasDraw.h`), so a copy pays no fake display of its
own: a plain `lv_canvas_draw_text` call would build and tear one down every
time (`init_fake_disp`, two struct memsets, an `lv_mem_alloc` / `lv_mem_free`
of the draw context, an `lv_obj_invalidate` of the canvas - about 0.1 ms per
call, measured on the 7B, whatever is drawn; the toll the firmware's own span
writes avoid, see `cdSpanH()` in `src/display/CanvasDraw.cpp`), which was what
the halo cost when it was introduced. The halo's time has not been measured on the device (the
BOAT phase of the `WS_PHASE` stopwatch with a kite up, halo on against off,
would show it). A port that draws text with an outline or shadow of its own MAY
use that instead, in the inner colour, 1 px wide.

The boom end (32.1 units) and an eased jib (up to about 35 units at 100 %
foresail) reach the letters too, and the stem tip (32.1) reaches the bottom
row of a letter at the bow, below the heading box; over a 2-px rig line or the
pale sail tone the letters keep their contrast; without a kite no letter gets a
halo.

The attitude variant keeps the whole card under its horizon
(`drawCompassRose(hdg)` = both layers, then the horizon): the horizon disc
(radius 110 UI_SF) and the bank scale (100 UI_SF) end inside the letters' band,
which starts at about 119 UI_SF. Only the pitch-ladder labels, drawn over the
letters, can reach one (each label starts `2 UI_SF` beyond its rung end, and a
rung centre may lie up to 104 UI_SF out), and only at attitudes far off a
sailing yacht's: never within 30 deg of heel; with up to 45 deg of heel from a
pitch of 22 deg (4-inch) or 33 deg (1024x600) on; at 90 deg of heel from 18 /
19.5 deg. Within 45 deg of heel and 15 deg of pitch a label stays at least 2 px
(4-inch) / 5 px (1024x600) clear of every letter.

### 4.10 Colours

All colours are theme roles (the user can edit them; defaults below).
`mix(fg, bg, m)` is LVGL's `lv_color_mix`: per channel `(fg m + bg (255 - m)) /
255`. The device works in RGB565 and quantises m to steps of 1/32
(`m' = (m + 4) >> 3`, weight m'/32); a port in 8-bit colour SHOULD use m/255
and need not reproduce the quantisation. The 8-bit column of vectors M1 rounds
each channel to the nearest integer, `(fg m + bg (255 - m) + 127) / 255` in
integer arithmetic, and a nested mix (the second panel tone) rounds its inner
mix first. An opacity o < 255 blends the colour over the pixel already there
with the same formula; 255 writes it opaque.

| Role | dark | light | night | Used for |
|---|---|---|---|---|
| `orange` (CLR_ORANGE) | #FF8A3D | #D2691E | #C25E2A | the downwind sail |
| `windInner` (C_INNER) | #081521 | #EEF2F6 | #120404 | inner circle, hull fill, kite tone mixing, halo of the compass letters (4.9) |
| `green` | #2FBF71 | #109A5A | #B24C2C | all sheets and the guy |
| `windRigging` | #558098 | #4A7088 | #6A2A1A | forestay, boom, mast, spinnaker pole, whisker pole |
| `windSail` | #CCD8F0 | #8090A8 | #D8907A | jib and main |
| `windHull` | #7898B8 | #4A6A8A | #B05A3C | hull outline |
| `windMast` | #274A6E | #8898A8 | #4A1C1C | keel (no longer painted, 4.8) |
| `windFlow` | #103054 | #6A90C0 | #8A3A22 | rudder, flow lines |
| `text` | #EAF2F8 | #16283A | #D35C3E | leech edges |
| `red` | #FF5A52 | #C0392B | #E2543A | luffing sails, compass letter N |
| `windTickMaj` | #557088 | #506880 | #A04828 | compass letters E (O) / S / W, major compass ticks |
| `yellow` | #F0A500 | #B8791A | #C25E2A | Code Zero |
| `accent` | #16B8B8 | #0F9A9A | #E2643E | the "Butterfly" label |

The kite has no palette entry of its own: all its tones are `orange` mixed into
`windInner`, so it follows every theme. The mixed tones for the defaults are in
vectors M1. LVGL opacity constants used below: `LV_OPA_40` = 102, `_50` = 127,
`_60` = 153, `_70` = 178, `COVER` = 255.

### 4.11 Reef badge

A pill `56 UI_SF` x `18 UI_SF` px, corner radius `UI_S(4)`, colour `orange`
(reef 1 and 2) or `red` (reef 3) at 220/255, label in the `text` colour, tiny
font: DE "1. Reff" / "2. Reff" / "3. Reff", EN "Reef 1" / "Reef 2" /
"Reef 3" (`drawReefBadge`). Normally centred above the bow, top edge at
`CY - 35 BS`.
**While a downwind sail is drawn** (`spiDrawn`, same frame) that is the kite's
crest, so the badge moves into the cockpit: centred, top edge at `CY + 8 BS` -
clear of the keel (down to 7.6), above the winches (14.7), inside the guy's
lead to the twing (0.7 px clear on the 4-inch board, 1.1 px on the 1024x600
boards, rounded corners included), and clear of the main because a kite flies
only with the true wind more than 90 deg aft (unless the boat is faster than
the wind, the boom is then eased 32 deg or more; the exception is in 5.8).

---

## 5. Downwind sail geometry (`SpiGeom`)

`SpiGeom.h` / `SpiGeom.cpp` are pure maths: no LVGL, float only, C++11. A port
SHOULD copy them line by line (C# and JavaScript both read like the C++) and
then compare with vectors L1-L4, and with the complete outlines in
`docs/downwind_vectors.txt` (5.7).

### 5.1 Coordinates, pixels, and the old BoatPainter units

Boat units as in 1.1. The firmware draws at `(CX + x BS, CY + y BS)`:

| Board | canvas | CX | CY | BS = 3.8 UI_SF (px per unit) | inner circle radius |
|---|---|---|---|---|---|
| 4-inch | 480 x 480 | 240 | 226 | 3.8 | 148 px = 38.95 units |
| 7B, 5B | 600 x 600 | 300 | 282.5 | 4.75 | 185 px = 38.95 units |

BS is `UI_WIND_BOAT_SCALE` (`src/display/UiConfig.h`, `BOAT_S` in
`WindScreen.h`).

Polygon vertices are rounded with `floor(v + 0.5)`; lines are drawn
anti-aliased from the unrounded float points. A canvas port needs no rounding.

**Mapping to BoatPainter units.** The Wind & Trim instrument was first ported
from NauticPi when it was still called SailTrimMonitor (its wind renderer, the
boat from its `BoatPainter`). The comments that recorded the original values
next to the Pinnace ones (the egg-shaped spinnaker of firmware 1.2.0 and the
rudder) give these pairs:

| BoatPainter | Pinnace boat units | ratio |
|---|---|---|
| spinnaker centre x `cx2 = 40 * leeSign` | `13.3 * ss` | 3.01 |
| spinnaker centre y `cy2 = -50` | `-16.6` | 3.01 |
| spinnaker radius `rx = 56` | `18.6` | 3.01 |
| spinnaker radius `ry = 46` | `15.3` | 3.01 |
| tack line start `BowDotY = -88` | `-29` | 3.03 |
| tack line end x `cx2 - 52 * leeSign = -12` | `-4.0` | 3.00 |
| sheet start y `22` | `7.3` | 3.01 |
| sheet end x `cx2 + 46 * leeSign = 86` | `28.6` | 3.01 |
| sheet end y `cy2 + 40 = -10` | `-3.3` | 3.03 |
| rudder pivot `Cy = 74` | `24.7` ("74 * 0.333") | 3.00 |

**The factor is 3.0**: 1 Pinnace boat unit = 3 BoatPainter units, every
Pinnace value lying within 0.1 unit of the BoatPainter value / 3 (mostly cut
to one decimal; the rudder pivot 74 / 3 = 24.67 was rounded to 24.7, the tack
line start 88 / 3 = 29.33 cut to a whole unit, 29). Origin (boat
centre) and axis directions are the same - negative y forward in both, and
BoatPainter's `leeSign` maps onto `ss` one to one (that is how 1.2.0 used it;
confirm the sign convention in NauticPi before relying on it). One exception:
the Code Zero, BoatPainter arm 87 ("JibArmLen(52) * 1.68"), was enlarged on
purpose to 34 units (not 29) and has no 1.18 angle factor here. Hull, mast
`(0,-6)` and boom (32) are Pinnace's own, not ports. In BoatPainter units the
kite points of section 5 are therefore `3 x` the values given here, and the
Pinnace rig lies at: stem `-96`, jib and Code Zero tack `-90`, gennaker tack
`-96.75` (the stem tip, 5.4), mast `-18`, boom length `96`, crest radius of
the symmetric kite `97.35`.

### 5.2 Rig points

```
MAST_Y = -6                                 mast at (0, -6)
quarterBlock(lee) = ( 9.6 lee, 25.0)        sheet: clew -> quarter block -> sheet winch (lee side)
sheetWinch(lee)   = ( 7.0 lee, 14.7)
twing(lee)        = (-10.6 lee, -1.0)       guy (symmetric kite only): tack -> twing -> guy winch
guyWinch(lee)     = (-7.0 lee, 14.7)                                    (windward side)
```

### 5.3 Symmetric spinnaker (`gores`, `balloon`)

A crescent ahead of the mast, seen from above, built in its own frame
(u athwartships, f forward) and turned about the **boat centre** by the trim
rotation, so it swings round the bow like a kite going from a dead run to a
broad reach. Turning about the centre changes no radius, so the kite never
grows towards the heading box.

Constants:

```
SYM_N = 24 (segments along foot and leading edge)        SYM_R_CREST = 32.45
                  square (k = 0)   turned (k = 1)
W   half width      19               17
f0  clews ahead     12               15
sag foot middle      8                6.5
b   shoulders       10               12
SYM_ROT_ZERO_AWA = 172   SYM_ROT_GAIN = 0.25   SYM_ROT_MAX = 14
SYM_GORES = 6   BALLOON_BANDS = 5   BALLOON_SEAM_A = 8   BALLOON_SEAM_B = 16
```

W is the half-width from clew to clew, f0 how far the clews sit ahead of the
centre, sag how far the middle of the foot sits ahead of the clews, and b how
far the shoulders sit outboard of the clews.

Rotation law (unsigned; the signed target is `ss * rot`, 4.5):

```
symRotationDeg(awaAbs) = 0                                         if awaAbs is NaN
                       = clamp((172 - awaAbs) * 0.25, 0, 14)       otherwise
```

0 from |AWA| 172 to 180 (square across the bow), 14 reached at |AWA| 116 and
not more: from |AWA| 99 the main stands at its 86 deg limit and bellies
forward, and a kite turned further would put its leeward clew on it.

Outline for a signed rotation `rot` (+ = crest towards starboard):

```
k   = clamp(|rot| / 14, 0, 1)
W   = 19 + (17 - 19) k;   f0 = 12 + (15 - 12) k;   sag = 8 + (6.5 - 8) k;   b = 10 + (12 - 10) k
h   = (32.45 - f0) / 0.75                 // puts the crest f0 + 0.75 h at radius 32.45
t   = rot in radians
eu  = (cos t, sin t)                      // u axis in boat units
ef  = (sin t, -cos t)                     // f axis ("forward" is -y)
for i = 0 .. 24:  s = i / 24, r = 1 - s
    foot (quadratic, (-W, f0) -> control (0, f0 + 2 sag) -> (W, f0)):
        fu = r^2 (-W) + s^2 W
        ff = r^2 f0 + 2 r s (f0 + 2 sag) + s^2 f0
    leading edge (cubic, (-W, f0), (-W - b, f0 + h), (W + b, f0 + h), (W, f0)):
        a0 = r^3, a1 = 3 r^2 s, a2 = 3 r s^2, a3 = s^3
        lu = a0 (-W) + a1 (-W - b) + a2 (W + b) + a3 W
        lf = (a0 + a3) f0 + (a1 + a2) (f0 + h)
    foot[i] = fu eu + ff ef = (fu cos t + ff sin t,  fu sin t - ff cos t)
    lead[i] = lu eu + lf ef
```

`foot[0] == lead[0]` (port clew) and `foot[24] == lead[24]` (starboard clew).
The shape eases from the wide, low "square" kite (pole well aft) to the
narrower "turned" one with higher clews.

```
tack (pole and guy)  = the windward clew: index 0 if side > 0 else 24   (side: 4.6; symTackIndex)
clew (sheet)         = 24 - tack index                                   (symClewIndex)
pole                 = mast (0, -6) -> foot[tack]
pole angle off the bow = atan2(|tack.x|, -6 - tack.y)      // 72.47 deg square, 45.45 deg turned
```

Panels (`gores`): 6 panels, panel g (g = 0..5) spans samples `4g .. 4g + 4`;
its polygon is `lead[4g .. 4g+4]` followed by `foot[4g+4 .. 4g]`. Seams
`foot[i] -> lead[i]` for i = 4, 8, 12, 16, 20.

Balloon: 5 bands between the foot (0) and the leading edge (1). Band edge j
(j = 0..5) lies at `balloonBandEdge(j) = (j / 5)^0.8` of the way (0, 0.2759,
0.4804, 0.6645, 0.8365, 1; j <= 0 gives 0 and j >= 5 gives 1), so the bands
narrow towards the leading edge. Edge point `E(j, i) = lerp(foot[i], lead[i],
balloonBandEdge(j))`; band j's polygon is `E(j+1, 0..24)` followed by
`E(j, 24..0)`. Seams `foot[i] -> lead[i]` at i = 8 and 16.

### 5.4 Gennaker (`gennaker`)

Tack on the centreline at the tip of the stem, clew out to leeward and aft; no
pole, no guy, the sheet to the leeward quarter. The sheeting angle is measured
from the **aft** centreline: 0 = clew dead aft, 90 = clew square abeam of the
tack.

```
GEN_N = 16   GEN_TACK_Y = -32.25   (tack (0, -32.25), the stem tip; jib and Code Zero stay at (0, -30))
GEN_LEE_POS = 0.55   GEN_LEE_DEPTH = 0.37   GEN_LUFF_POS = 0.45   GEN_LUFF_DEPTH = 0.15   GEN_PANEL = 0.33
GEN_SHEET_AT_90 = 47   GEN_SHEET_GAIN = 0.20   GEN_SHEET_MIN = 44   GEN_SHEET_MAX = 56
GEN_LEN_EASED = 29.5   GEN_LEN_GAIN = 0.25
GEN_SEAMS = 3   GEN_SEAM_BACK = 3   GEN_SEAM_END = { 5, 9, 13 }

genSheetDeg(awaAbs) = 44                                            if awaAbs is NaN
                    = clamp(47 + (awaAbs - 90) * 0.20, 44, 56)      otherwise

genOutline(a):                                 // a = signed sheeting angle (+ = clew to starboard)
    aLen = clamp(|a|, 44, 56)
    len  = 29.5 + (56 - aLen) * 0.25           // chord 29.5 .. 32.5, shorter when eased
    T = (0, -32.25)
    C = T + len (sin a, cos a)                 // clew
    d = C - T;  dl = |d|;  if dl < 1: fail (cannot happen with these constants)
    n = (-d.y, d.x) / dl;  if (n.x < 0) != (a < 0): n = -n         // normal to leeward
    K1 = T + 0.55 d + 0.37 dl n                // leeward edge control
    K2 = T + 0.45 d + 0.15 dl n                // luff control
    for i = 0 .. 16:  s = i / 16, r = 1 - s
        lee[i]   = r^2 T + 2 r s K1 + s^2 C
        luff[i]  = r^2 T + 2 r s K2 + s^2 C
        panel[i] = lerp(luff[i], lee[i], 0.33)
    seams: panel[e - 3] -> lee[e]  for e = 5, 9, 13
```

Never sheeted closer than 44 deg: from |AWA| 99 the main stands at its limit
and bellies forward, and a clew further aft would lie on it. The chord shortens
as the sheet is eased because an eased gennaker with the full 32.5 chord would
reach 32.8 units from the centre about 29 deg off the bow, past the radius
budget (5.6). With the law above the outline reaches at most 32.474 units, at
the eased limit (56) about 20 deg off the bow; sheeted closer than about 54.65
deg, no point of it lies further out than the tack (32.25). The clamp on |a| keeps
the length in range for any angle, also while the smoothed angle passes 0.

**Tack at the stem tip (owner's decision, 1.3.0).** The gennaker is tacked at
the tip of the stem, not at the jib's and Code Zero's tack point `(0, -30)`
(its tack until then, where the 2 units of hull ahead of it showed), so no
part of the hull shows ahead of it. The value -32.25 is chosen in pixels:
`drawBoat` truncates the hull vertices to whole pixels (4.8), which puts the
tip station `y = -32` at pixel row 104 on the 4-inch board and 130 on the
1024x600 boards (both `-32.105` units), and draws its 2-px outline through
them; that outline's front pixel row is one row further forward (103 / 129).
The tack `(0, -32.25)` lies at pixel y 103.45 / 129.3125, in that front row and
just ahead of its pixel centres (103.5 / 129.5): no hull pixel lies ahead of the
tack, and the kite's edges start on the tip of the outline (vectors L4; the
checker's test (e) in `docs/downwind_check.cpp`, 5.7). Any value from -32.316
to -32.237 does this on both boards; -32.25 is a round one inside that range.

**Leeward-edge depth 0.37.** At the stem tip the tack stands 32.25 units from
the centre, 0.38 units (1.45 px on the 4-inch board) inside the radius budget
of 5.6. The leeward edge leaves the tack towards its control point, along
`0.55 d + GEN_LEE_DEPTH dl n`; that direction points forward of square - out of
the budget straight from the tack - as soon as `GEN_LEE_DEPTH > GEN_LEE_POS /
tan(a)`, and at the eased limit `0.55 / tan 56 = 0.371`. With the first tuning's
0.50 the chord would have had to shrink to 13 units at 56 deg (from 29.5) to
stay inside the budget; with 0.37 the edge leaves the tack square to the
centreline at 56 deg and the chord law above needed no change. Searched over
depths 0.34 to 0.45 and linear chord laws (gain 0 to 1 unit per degree, each
with the longest chord that keeps the targets of 5.6 with 0.3 px to spare), the
smallest change of the outline against the first tuning moved to the stem is a
root-mean-square offset of its points of 1.01 units, and 0.37 with the
unchanged chord law reaches it (0.40 needs the eased chord cut to 25.5 units
and still changes the outline by 1.59). The crescent stays deep: its leeward
edge bellies out 18.5 % of the chord (25 % before), nearly twice the jib's
draft.

### 5.5 Pixel layer

Per type, in this order (`drawSpiGores`, `drawSpiBalloon`, `drawGennaker`).
O = `orange`, I = `windInner`. Widths in px; "opaque" = written without reading
the pixel; an opacity below 255 is blended.

**Panels (`gores`)**, from `smSpiRot`:

1. 6 panel polygons, **opaque**: even g `mix(O, I, 215)`, odd g
   `mix(mix(O, I, 140), I, 215)` - the colours that panels of O and
   `mix(O, I, 140)` at opacity 215 showed over the inner background, mixed once
   so the bow and the forestay do not show through and no pixel is read back.
2. 5 seams, O at 170/255, 1 px.
3. For each segment i = 0..23: leading edge `lead[i] -> lead[i+1]` O opaque
   2 px, then foot `foot[i] -> foot[i+1]` O opaque 1 px.
4. Pole mast -> `foot[tack]`, `windRigging` opaque 2 px.
5. Sheet `foot[clew]` -> `quarterBlock(side)` at 153 -> `sheetWinch(side)` at
   127; guy `foot[tack]` -> `twing(side)` at 153 -> `guyWinch(side)` at 127;
   `green`, 1 px (`spiLines`; the same green, width and opacities as the Code
   Zero's sheet, 4.7).

**Balloon (`balloon`)**, from `smSpiRot`:

1. 5 band polygons j = 0 (foot) .. 4 (leading edge), **opaque**, colour
   `mix(O, I, 140 + 115 j / 4)` with integer division: 140, 168, 197, 226,
   255 - pale at the foot, full orange at the leading edge, so the kite reads as
   a filled dome.
2. Seams at i = 8 and 16, `mix(O, I, 100)`, opaque, 1 px.
3. Edges, pole, sheet and guy exactly as panels 3 to 5.

**Gennaker (`gennaker`)**, from `smGen`:

1. Leeward part, polygon `lee[0..16]` then `panel[16..0]`: O at 215/255.
2. Luff panel, polygon `panel[0..16]` then `luff[16..0]`: `mix(O, I, 140)` at
   215/255.
3. 3 seams `panel[e-3] -> lee[e]`, `mix(O, I, 140)` opaque, 1 px.
4. For each segment: leeward edge O opaque 2 px, then luff O opaque 1 px.
5. Sheet only: `lee[16]` -> `quarterBlock(gs)` at 153 -> `sheetWinch(gs)` at
   127, `green` 1 px, `gs = smGen >= 0 ? 1 : -1`.

The device fills the kite's polygons without the per-20-row `renderYield()` of
`fillPolygon()` (argument `yieldRows = 0`), and `drawSpinnaker()` (the dispatch
by type) yields once after the whole kite (a 4-inch memory-bandwidth detail; a
port can ignore it): on the 4-inch board every `renderYield()` is a 1 ms sleep,
and the five balloon bands with per-row offers came to 13-15 a frame. Memory
traffic per frame, counted pixel by pixel in the PC simulator (opaque pixel =
one 2-byte write, blended pixel = 2-byte read + 2-byte write), kite with pole
and lines, mean over the 14 cases in which it is shown among |TWA| 90, 95, 100,
110, 120, 135, 150, 160, 165, 178 on both sides (TWS 14, STW 5), the same in
the light, dark and night themes:

| | 7B / 5B | 4-inch | `renderYield()` calls per frame, 4-inch (each a 1 ms sleep there) |
|---|---|---|---|
| orange ellipse (the egg of 1.2.0, 5.1) | 87.0 KB (21,738 blended px) | 56.7 KB (14,178) | 5 |
| panels (`gores`) | 31.4 KB (10,734 opaque + 2,479 blended) | 22.1 KB (7,003 + 2,027) | 1 (10-12 with per-row yields) |
| balloon (`balloon`) | 31.1 KB (11,050 + 2,237) | 21.8 KB (7,253 + 1,831) | 1 (13-15) |
| gennaker (`gennaker`) | 12.8 KB (170 + 3,115); 16.6 KB with the first tuning's deeper leeward edge (5.4) | 9.4 KB (138 + 2,280); 11.8 before | 1 (6-8) |

Not shown (`none`, or outside the window): nothing drawn, no yield.

### 5.6 What the constants guarantee, and what a port must re-check

The constants were tuned with `docs/downwind_check.cpp`, which links the real
`SpiGeom.cpp` and replicates the main (boom law, `camberForTwa`, `drawSailFoil`),
the hull and the heading box (how to run it, and its full results: 5.7). Over
|TWA| 90..180 (step 1, both sides), TWS 6/10/14/20 and STW 3/5/7, in the steady
state, wherever the kite is shown:

- the kite (outline plus half its 2-px edge) never overlaps the main: worst
  clearance on this integer-TWA grid +6.51 px (symmetric) and +16.57 px
  (gennaker) on the 7B, +4.62 / +12.68 px on the 4-inch (targets 2 / 1.5 px).
  Between the grid points the gennaker comes a little closer: on a 0.25-deg
  grid (6474 states per board, otherwise the same) +16.07 px (7B) and
  +12.29 px (4-inch), at |TWA| 154.25, TWS 6, STW 5 (|AWA| 98.8), where the
  pixel-truncated main changes in steps just below the boom's 86-deg limit; no
  state overlaps, and the symmetric kite keeps +6.51 / +4.62 there;
- it stays below the heading box: the budget is the box bottom, 33.2 units from
  the centre on both grids (`R_INNER - 1 - 21 UI_SF` px: the box hangs 1 px
  inside the inner circle and is `21 UI_SF` high), minus 1 px - 125 px on the
  4-inch board, where the symmetric kite uses it exactly (top row 101 under a
  box ending at row 99) and the gennaker
  reaches 124.40 px (top row 102; 155.25 px and row 128 on the 7B, against
  156.75 px and row 125), also over every sheeting angle from 44 to 56 deg, not
  only those of the grid; the crest at 32.45, the gennaker's depth and its chord
  law come from this;
- on a dead run (|AWA| >= 170) the stem `(0, -32)` is covered by the kite;
- the gennaker's tack lies on the tip of the hull outline: no hull pixel ahead
  of it, the tack in the outline's front pixel row (5.4), on both boards and in
  every gennaker state;
- the pole stands 70.7 to 72.5 deg off the bow at |TWA| 178, and the pole angle
  never decreases as |AWA| grows. Known exception: a boat faster than the wind
  dead downwind (TWS 6, STW 7: AWA 11.8) draws the kite fully turned with the
  pole at 45.4 deg.

These guarantees hold for **this** main and hull. If NauticPi's boom law, boom
length, mast position or hull differ (after the factor 3 of 5.1), the port MUST
re-run the checker with NauticPi's rig replicated in it before trusting them;
the kite constants themselves stay as they are unless that check fails.

### 5.7 The clearance checker and its results

A record, not normative: what 5.6 rests on, and how to re-run it.

`docs/downwind_check.cpp` links the real `SpiGeom.cpp` and replicates what
`WindScreen` draws around the kite: the mainsail foil (boom law,
`camberForTwa`, `drawSailFoil` with its truncation of the vertices to whole
pixels), the hull (truncated stations, 2-px outline), the heading box, the reef
badge, and the rasterisation behind `fillPolygon` and `linePx` (`cdFillPolyOpa`
and `cdLineAA` in `src/display/CanvasDraw.cpp`). These replicas have to be kept
in step with `WindScreen.cpp` and `CanvasDraw.cpp` when either changes. Build
and run it from the repository root (any C++11 compiler, no LVGL):

```
g++ -std=gnu++11 -O2 -I src/display/screens docs/downwind_check.cpp src/display/screens/SpiGeom.cpp -o downwind_check
./downwind_check docs/downwind_vectors.txt
```

It prints its report on stdout (the targets marked `[OK]` or `[FAIL]`; the
dead-run pole angle `[see note]`, for the exception of 5.8; the gennaker's
sheeting law and the reef badge without a mark, 7.3 says how to read them) and
rewrites `docs/downwind_vectors.txt`. It checks every state with |TWA| 90..180
(step 1, both sides), TWS 6/10/14/20 and STW 3/5/7 in which the kite is shown
(1734 states per board for the symmetric kite, 1614 for the gennaker), in the
steady state (smoothed trim = target), with AWA from
`atan2(TWS sin TWA, TWS cos TWA + STW)` (`trueToApparent`, 2.3):

| Target | 7B / 5B (4.75 px per unit) | 4-inch (3.8 px per unit) |
|---|---|---|
| (a) symmetric kite never on the mainsail: clearance >= 2 px / 1.5 px | worst +6.51 px, 0 overlapping pixels | worst +4.62 px, 0 |
| (a) gennaker likewise | worst +16.57 px, 0 (+16.07 between the grid points, 5.6) | worst +12.68 px, 0 (+12.29) |
| (c) symmetric kite below the heading box: radius + 1 px <= box - 1 px | 155.20 px <= 156.75; top row 127 (needs >= 125) | 124.36 px <= 125.00; top row 101 (needs >= 101) |
| (c) gennaker likewise, and over every sheeting angle 44..56 (step 0.05) | 155.25 px; top row 128; nothing beyond the tack (154.19 px) below 54.7 deg on this grid (54.65 sampled finer) | 124.40 px; top row 102; tack 123.55 px |
| (d) dead run (\|AWA\| >= 170): bow tip covered | -1.54 px inside, 0 hull pixels through | -1.22 px, 0 |
| (d) other states: bow covered or >= 2 px outside, never on the edge | 1640 covered, 0 touching | 1640 covered, 0 touching |
| (b) pole angle on a dead run (\|TWA\| 178) 68..80 | 70.7 .. 72.5 (one exception, 5.8) | same |
| (b) pole angle non-decreasing as \|AWA\| grows | 45.45 .. 72.47, never decreasing | same |
| (e) gennaker tack at the stem tip: no hull-outline pixel ahead of the tack (residual >= 0.25 after the kite is drawn), tack in the outline's front pixel row | tack y 129.31 px in row 129, outline front row 129; 0 of 1614 states with a hull pixel ahead | tack y 103.45 px in row 103, front row 103; 0 of 1614 |
| Reef badge in the cockpit (4.11) | guy +1.09 px, keel +1.40 px, sheet +10.2, kite and pole far off; mainsail only in the 18 states with the boat faster than the wind | guy +0.69 px, keel +1.12 px, sheet +7.6; same 18 states |

`docs/downwind_vectors.txt` holds test vectors printed by the same program from
the real `SpiGeom.cpp`: `symRotationDeg`, `genSheetDeg`, `balloonBandEdge`, the
rig points, `symOutline` at rotation 0, +7 and -14 (all 25 foot and
leading-edge points) and `genOutline` at +44, -50 and +56 (all 17 leeward,
luff and panel points). They complement L1-L3 of 7.2, which print selected
points only. A port of `SpiGeom` has to reproduce them too, to float precision
(they are printed with 4 decimals, so the tolerance of 7.1 applies; checklist
step 8 in section 8).

Not reproduced by the checker as it stands in the repository (separate
harnesses on the same replicas, not kept):

- **Transitions**, with the smoothing of 4.5 and the side rule of 4.6 at 2 to
  4 frames per second: continuous gybes through 180 at 15 and 30 deg per
  second, and every step change between two of the states above on a 10-deg
  grid (1596 per symmetric type, 1482 for the gennaker, per board) - no frame
  with the kite on the mainsail, no pole or guy across it. Re-run with the
  gennaker at the stem tip: still none; the gennaker's worst same-side
  transition on a 2-deg grid keeps +16.39 px (7B) and +12.90 px (4-inch) from
  the main, the gybes +21.5 / +16.4 px, and over a wider domain (TWS 1..30,
  STW 0..12) its radius never exceeds the budget.
- The 0.25-deg grid of 5.6, the 2-D measurement of the halo sector (4.9) and
  the search over the gennaker's leeward-edge depth and chord law (5.4).

### 5.8 Known limits

- **Boat faster than the wind, dead downwind.** The window is decided by the
  true wind only (4.2). At |TWA| 178 with TWS 6 and STW 7 the apparent wind
  comes from 11.8 deg ahead (vectors B2): the kite is drawn fully turned
  (rotation 14, pole 45.4 deg off the bow instead of about 72, vectors L2 at
  |AWA| 11.8) and the boom on the centreline - in the cockpit, under the reef
  badge if one is shown (the badge lies on the main in 18 checked states, all
  with the boat faster than the wind, 5.7). Every other checked state at
  |TWA| 178 gives a pole angle of 70.7 to 72.5. Under sail this state cannot
  occur; motoring with the kite up it can.
- **4-inch heading box.** The symmetric kite's topmost row is 101, the box's
  last row 99: exactly the 1 px gap required, nothing to spare.
  `SYM_R_CREST = 32.45` also keeps the bow tip covered on that board, where it
  has to stay within about 32.24 .. 32.63. The gennaker, tacked at the stem
  tip, reaches 124.40 px of the 125.00 px budget there (0.6 px to spare; its
  tack alone stands at 123.55 px).
- **Compass letters over a kite** get their halo by sector, not by an exact
  overlap test: a letter within 55 deg of the bow is outlined while a kite is
  up even where the kite stays below it (invisible there, apart from a 1-px
  gap in a flow line next to the letter). The halo's time on the device has not
  been measured (4.9).
- **Theme font.** The halo sector covers the default `small` font and one or
  two steps larger (4.9); a still larger one was not measured, and a kite may
  reach its letters just outside 55 deg, where they get no halo.

---

## 6. Configuration and web interface

### 6.1 Keys

| Key | Type | Default (`AppConfig`) | Factory `data/config.json` | Meaning |
|---|---|---|---|---|
| `cal.awa_off` | float deg | 0 | - | added to the measured AWA (vane rotation) |
| `cal.aws_fac` | float % | 100 | - | scales the measured AWS |
| `cal.hdg_off` | float deg | 0 | - | added to the heading |
| `cal.stw_fac` | float % | 100 | - | scales STW |
| `cal.wind_damp_app_spd` | float s | 2 | 2 | 3.2 |
| `cal.wind_damp_app_dir` | float s | 2 | 2 | 3.2 |
| `cal.wind_damp_true_spd` | float s | 2 | 2 | 3.2 |
| `cal.wind_damp_true_dir` | float s | 2 | 2 | 3.2 |
| `cal.wind_damp_spd`, `cal.wind_damp_dir` | float s | read only | - | legacy, 3.4 |
| `cal.twd_cog` | bool | false | - | 2.5, 2.6 |
| `sources.wind` / `hdg` / `stw` / `pos` | int | -1 (any) | - | source pinning, 2.1 |
| `sail.spi_type` | string | `"balloon"` | `"balloon"` | 6.2 |
| `sail.spinnaker` | bool | written as type != none | `true` | legacy, 6.2 |
| `sail.code_zero` | bool | false | true | 4.2 |
| `sail.butterfly` | bool | false | false | 4.4 |
| `sail.lwl_m` | float m | 9.0 | 9.5 | reef model, 4.2 |
| `sail.foresail_pct` | int % | 100 | 140 | jib length, 4.7 |
| `sail.nogo_deg` | int deg | 30 | 35 | zone ring and point of sail only |
| `sail.windlines_app` | bool | false | true | flow lines from AWA instead of TWA |

Every key is merged: a key missing from a document keeps the current value.
The `cal` calibration values are applied once at ingestion (2.1), so the true
wind carries them too.

### 6.2 `sail.spi_type`

| Value | Shown as (DE / EN) | Picture |
|---|---|---|
| `none` | Keins / None | no downwind sail - not every boat carries one |
| `gores` | Spinnaker (Bahnen) / Spinnaker (panels) | symmetric, six panels in two tones |
| `balloon` | Spinnaker (Ballon) / Spinnaker (balloon) | symmetric, shaded bands - **the default** |
| `gennaker` | Gennaker / Gennaker | asymmetric, tacked at the stem tip |

Firmware enum `SpiType : uint8_t { None = 0, Gores = 1, Balloon = 2, Gennaker = 3 }`,
field `AppConfig::spiType`, default `SpiType::Balloon`; only the strings are
ever stored (`spiTypeName` / `spiTypeFromName` in `Config.cpp`). It replaces
the bool `sail.spinnaker` ("Allow spinnaker"), which is still read and written
for compatibility (load and save below).

**Load** (`Config::fromJson`; every document goes through it: the file from
flash, an import, and every POST from the web page, which may be a partial
patch), exact string match (`strcmp`, so `"Balloon"` is unknown):

```
if spi_type is one of the four strings:  type = spi_type
elif spinnaker is a bool:                type = spinnaker ? balloon : none
elif spi_type is present and not null:   type = balloon      (unknown string, wrong case, a number)
else:                                    keep the current type   (on a load from flash: balloon)
```

The last row keeps a partial patch that does not touch the sail from resetting
what the user picked. It is the one deliberate exception to "no valid type =
balloon": like every other key in `fromJson()`, a key that is not there keeps
the current value; without it, any patch without a `sail` object would reset
the downwind sail to the balloon.

**Save**: `"spi_type": <string>` and `"spinnaker": type != none`. The bool is
for a downgrade: firmware before 1.3.0 reads only it, so it still hides the kite
(`none`) or shows its one spinnaker. It is never consulted while `spi_type` is
valid.

**Web interface**: Display tab, screen list, the Wind & Trim entry's settings
(`screenConfigHtml()`, screen type `wind`), between the headsail slider and
"Allow Code Zero". A `<select>` (style `_cfgIn` plus `max-width:180px`: at the
usual 150 px, "Spinnaker (Bahnen)" - 125 px of text at 14 px Arial - and
"Spinnaker (balloon)" were cut off), options in the order of
`SPI_TYPES = ['none','gores','balloon','gennaker']`, then a hint line under it,
always shown (6.3). The preselect is `spiTypeOf(cfg.sail)`: a known
`spi_type`, else `spinnaker === false ? 'none' : 'balloon'`. The save patch
sends `spi_type` **and** `spinnaker: spi_type !== 'none'` in the `sail` object.
The bool is for a page newer than the firmware behind it (the web interface
can be updated on its own): an older firmware ignores `spi_type`, and without
the bool the choice `none` would not switch its spinnaker off. Page and
firmware agree on every document the firmware writes; for a document with
neither key the page shows `balloon` while the firmware keeps its value
(vectors H1), and the firmware never writes such a document. No setting on the
device.

### 6.3 Texts (web interface, both languages)

| Key | DE | EN |
|---|---|---|
| `SAIL_SPI_TYPE` | Vorwindsegel | Downwind sail |
| `SAIL_SPI_NONE` | Keins | None |
| `SAIL_SPI_GORES` | Spinnaker (Bahnen) | Spinnaker (panels) |
| `SAIL_SPI_BALLOON` | Spinnaker (Ballon) | Spinnaker (balloon) |
| `SAIL_SPI_GENNAKER` | Gennaker | Gennaker |
| `SAIL_SPI_HINT` | Wird nur bei passendem Kurs und Wind gezeigt: Spinnaker ab 100° wahrem Windwinkel, Gennaker von 90° bis 160°. | Shown only on a matching course and wind: spinnaker from 100° true wind angle, gennaker from 90° to 160°. |
| `CAL_WDAMP_HINT` | Wind-Dämpfung: Gemessen wird nur der scheinbare Wind; den wahren Wind berechnet das Display selbst aus dem ungedämpften scheinbaren Wind, der Fahrt durchs Wasser (ersatzweise über Grund) und dem Steuerkurs und dämpft ihn erst danach — jede Einstellung wirkt nur auf ihre eigenen Werte. Angegeben ist die Zeit in Sekunden, nach der eine sprunghafte Änderung der Stärke — oder eine kleine der Richtung — zu 63 % angezeigt wird; große Richtungssprünge kommen schneller durch. 0 = aus. | Wind damping: only the apparent wind is measured; the display computes the true wind itself from the undamped apparent wind, speed through water (over ground if there is no log) and heading, and damps it only afterwards — each setting acts on its own values only. Each is the time in seconds after which a sudden change in speed — or a small one in direction — shows 63 %; large swings in direction come through faster. 0 = off. |
| `CAL_WDAMP_APP_H` | Scheinbarer Wind | Apparent wind |
| `CAL_WDAMP_APP_HINT` | Für den Segeltrimm: soll schnell reagieren, also wenig Dämpfung. | For sail trim: should react quickly, so little damping. |
| `CAL_WD_APP_SPD` | Dämpfung Stärke, AWS (s) | Speed damping, AWS (s) |
| `CAL_WD_APP_DIR` | Dämpfung Richtung, AWA (s) | Direction damping, AWA (s) |
| `CAL_WDAMP_TRUE_H` | Wahrer Wind | True wind |
| `CAL_WDAMP_TRUE_HINT` | Für die Taktik: Bei der Richtung zählen die Winddreher, nicht jede Böe — sie verträgt deutlich mehr Dämpfung. Die Stärke liegt dazwischen: Böen sollen sichtbar bleiben. | For tactics: in the direction it is the shifts that count, not every gust — it takes much more damping. Speed lies in between: gusts should stay visible. |
| `CAL_WD_TRUE_SPD` | Dämpfung Stärke, TWS (s) | Speed damping, TWS (s) |
| `CAL_WD_TRUE_DIR` | Dämpfung Richtung, TWA und TWD (s) | Direction damping, TWA and TWD (s) |
| `CAL_TWDCOG` | TWD notfalls aus COG | TWD fallback from COG |
| `CAL_TWDCOG_HINT` | Die wahre Windrichtung (TWD) wird nur dann aus dem Kurs über Grund (COG) berechnet, wenn weder ein Steuerkurs vom Kompass noch die nordbezogene wahre Windrichtung eines anderen Geräts (PGN 130306, Referenz 0) ankommt — beide haben immer Vorrang — und nur bei Fahrt über Grund (SOG): an ab 1 kn, aus erst unter 0.8 kn, damit die Anzeige um 1 kn herum nicht flackert. Der Kurs über Grund ist nicht die Richtung des Bugs: Strömung, Abdrift und Versatz verfälschen die Windrichtung um einige Grad. Windstärke und Windwinkel (TWS, TWA) bleiben unberührt. | The true wind direction (TWD) is worked out from the course over ground (COG) only when neither a compass heading nor a north-referenced true wind direction from another device (PGN 130306, reference 0) arrives — both always take precedence — and only with speed over ground (SOG): from 1 kn, off only below 0.8 kn, so the reading does not flicker at around 1 kn. Course over ground is not where the bow points: current, leeway and drift put the wind direction a few degrees off. Wind speed and angle (TWS, TWA) are not affected. |
| `CAL_AWA` / `CAL_AWSF` | Wind: Geber-Rotation (°) / Wind: AWS-Faktor (%) | Wind: vane rotation (°) / Wind: AWS factor (%) |
| `CAL_HDG` / `CAL_STWF` | Kompass: Offset (°) / Logge: STW-Faktor (%) | Compass: offset (°) / Log: STW factor (%) |

### 6.4 `GET /api/info`

```json
{ ..., "limits": { "wind_damp_s": { "min": 0, "max": 30, "def": 2 } }, ... }
```

Reported by the running firmware every time, deliberately not part of
`/api/config` (that document is config.json and would store and export the
limits as if they were settings).

---

## 7. Test vectors

### 7.1 How they were made, and the tolerance

Printed by a host program (`docs/wind_vectors.cpp`, built with g++ 15,
`-std=gnu++11 -O2 -Wall -Wextra -Wshadow`, no warnings; its header has the
build line; 7.2 is its complete output) that links the repository's own `src/nmea/WindCalc.cpp`,
`src/config/WindDampCfg.h` and `src/display/screens/SpiGeom.cpp` unchanged.
Sections A to G and L1-L3 come straight from that code. What lives in files
that need LVGL or ArduinoJson is restated in the program line by line, and
sections H, I, J, K, L4 and M (and the m/s -> kn conversion in D) come from
those restatements:

- from `WindScreen.cpp`: `camberForTwa`, `computeSailState`, `latchLeeSide`, the
  NaN handling and latch call of `drawInstrument`, the smoothing of `drawBoat`,
  `spiRigSide`, `resetRig`, the main/jib/Code Zero geometry and the foil control
  point of `drawSailFoil` (in boat units, without the pixel truncation), and the
  hull's stem-tip vertex as `drawBoat` truncates it to a pixel (L4); the pole
  angle is the checker's definition (5.3);
- from `Config.cpp`: `spiTypeFromName` and the `sail.spi_type` block of
  `fromJson`; from `index.html`: `spiTypeOf`;
- from `N2kHandler.cpp`: `kn = m/s / 0.5144`;
- LVGL's `lv_color_mix` (RGB565, `LV_COLOR_MIX_ROUND_OFS 0`), only for M1.

All Pipeline scenarios start at a clock of 100000 ms (0 means "never"). "No
damping" means all four time constants 0. Values are printed with 4 decimals;
`NaN` = not known. The flag columns are 0/1.

**Inputs the output does not print.** The inputs the program uses without
printing them are recorded here as well, so the document stands on its own; with them a
port reproduces 7.2 without having to work them back from the outputs. Unless
a row says otherwise, every boat input of a Pipeline scenario is (re)stamped
with the time of the sample it is fed with, so it is fresh; a "no heading" or
"COG not available" sets that stamp to 0. In D2, D3 and E1, `expire(now)` is
called before each sample (it changes nothing there).

| Section | Not printed |
|---|---|
| A1 | The row printed `-0.0000` is the input `-1e-6` (see the tolerance below). |
| C1, C2 | `now` = 100000; each stamp = `now` - the age printed. |
| C3 | STW 6; heading stamp 0 in the NaN row. |
| D2, D3 | Every sample: AWS 10, AWA 60 (TWA 83.4132), STW 4, COG 200 true, no damping. `cog 0` = COG stamp 0 (COG field N/A); `hdg 1` = heading 90 true, `hdg 0` = heading stamp 0; `opt` = `twd_cog`. SOG `NaN` = SOG field N/A: value and stamp stay as they were (2.1), so `SOG[kn]` shows the last value. |
| E1 | Heading 100 true in the rows up to t = 11.0, heading stamp 0 from t = 11.2 on; STW 6 at every row; no damping. |
| E2 | Heading 0 true, STW 6, all four time constants 2 s; t = 10.5 is a fresh sample with fresh stamps. |
| F2 | "ten samples" and "one sample": two fresh filters, each primed with 0 (dt 0), tau 2. |
| F4 | dt 0.25 s between samples, tau 2. |
| F5 | The six steps: a fresh filter primed with 0, then one sample at dt = tau = 2 s. |
| F7 | Heading 0 true, STW 5, no damping; the second sample 1 s after the first; "same from a fresh pipeline" = a new pipeline fed AWS 5 AWA 0 at that time; the half sample 0.2 s later, into that new pipeline. |
| I1 | `computeSailState(abs(TWA), TWA, AWA, TWS, 5)` with AWA from `trueToApparent(TWS, TWA, 5)`; `code_zero` off. The "no TWS" case uses AWA 100. |
| I2 | AWA = TWA - 15 (it does not take part in the decision), STW 5. |
| I3 | TWS 10, STW 5, `spi_type` balloon, `code_zero` off. |
| I4 | TWA NaN, TWS 10, STW 5; the "AWA NaN, TWA 130" row TWS 10, STW 5. |
| I5 | AWA = TWA, TWS 10, STW 5; "no TWA": AWA 60. |
| I6 | TWA 90, AWA 70. |
| I7 | `spi_type` none, `code_zero` on, `butterfly` off; a new screen state per row (first frame: targets at once); the lee side from the sign. |
| J1, K1-K3 | TWS 10, STW 5, `spi_type` balloon (K3 also `butterfly` on), luffing tick 0; each section starts a new screen state and carries it from frame to frame. |
| L2 | Steady state: rotation = lee x `symRotationDeg(abs(AWA))`, pole side = lee. "max r" is the largest radius among `lead[0..24]`, printed as the mirror pair i / 24 - i with i <= 12; "length" = distance mast `(0, -6)` -> `foot[tack]`. |
| L3 | Sheeting angle = lee x `genSheetDeg(abs(AWA))`; "chord" = distance `lee[0]` -> `lee[16]`; "max r" = largest radius among `lee[0..16]`, the first index on a tie. |
| L4 | Gennaker lines: tack px y = `CY + GEN_TACK_Y BS`, "pixel row" = its floor; "hull tip vertex row" = `CY - 32 BS` truncated toward zero to a whole pixel, as `drawBoat` stores the hull vertices (4.8), printed again in units; "outline front row" = that row - 1, the first row the 2-px outline reaches (5.4). |
| M1 | 8-bit column rounded as stated in 4.10. |

**Tolerance.** A port MUST reproduce every printed number to within **0.001**
(absolute, in the printed unit: kn, deg, s, boat units) and every flag, side,
count, NaN and selected type **exactly**. Angles are compared on the circle:
360.0000 and 0.0000 are the same direction, as are +180 and -180. The firmware
computes in 32-bit float; a port in double (JavaScript, C# `double`) differs
by at most one unit in the fourth decimal, so 0.001 leaves room. The one
visible float effect is A1's `-1e-6`, which normalises to exactly 0 in float
and to 359.999999 (printed 360.0000) in double. Colours (M1): within 1 per
8-bit channel of the 8-bit column; the RGB565 column is the device's own
result and is for information. Where a vector sits exactly on a threshold, the
port must use the same comparison (`<` or `<=`) as sections 2 to 5 - that is
what those vectors test.

**Checked against this document.** An independent JavaScript port written
from sections 1 to 5 alone (double precision, not from the C++ code) reproduced
A1, B1, B2, C3, D2, D3, E1, F1, F3, F4, F6, G1, I1, J1, K2, L2, L2b and L3:
every number within 0.0001, every flag and NaN equal, the only difference
being the A1 case above. A second, clean-room port in Python (double
precision), written from sections 1 to 6 before the C++ code was read,
reproduced every data line of 7.2, sections A1 to M1 (404 lines): every number
within one unit of the fourth decimal, every flag, side, count, NaN, type and
8-bit colour equal. It had to work the unprinted inputs back from the outputs
(AWS 10, AWA 60 and STW 4 behind the TWA 83.4132 of D2, for example); they are
now in the table above. When the gennaker was moved to the stem tip, that port
was updated from the changed text of 5.4 and of this section alone (tack
-32.25, leeward-edge depth 0.37, the two gennaker lines of L4) and reproduces
all 406 data lines of the 7.2 below. Against the same 7.2 its unchanged
earlier version leaves 15 lines unreproduced (`compare.py` counts 14 problems),
and changing the depth to 0.38 or the tack to -32.2 gives 12 and 14 problems.

### 7.2 Output

```
# Wind & sail test vectors - docs/WIND-AND-SAILS.md section 7
# Real code: WindCalc.cpp, WindDampCfg.h, SpiGeom.cpp. Restated: WindScreen sail logic,
# Config spi_type load, index.html spiTypeOf, lv_color_mix. Values %.4f, NaN = not known.
# Units: kn, deg, s, ms; boat units (x + starboard, y + aft, origin boat centre).
# Constants: CALM_KN 0.0500 COG_MIN_SOG_KN 0.9900 COG_OFF_SOG_KN 0.7900 FRESH_MS 5000 STALE_MS 10000
# WindDampCfg MIN_S 0.0000 MAX_S 30.0000 DEFAULT_S 2.0000

==== A1 norm180 / norm360
        in       norm180      norm360
    0.0000        0.0000       0.0000
  180.0000      180.0000     180.0000
 -180.0000      180.0000     180.0000
  181.0000     -179.0000     181.0000
 -181.0000      179.0000     179.0000
  359.0000       -1.0000     359.0000
  360.0000        0.0000       0.0000
  540.0000      180.0000     180.0000
 -540.0000      180.0000     180.0000
  720.5000        0.5000       0.5000
   -0.0000       -0.0000       0.0000
       NaN           NaN          NaN

==== B1 apparentToTrue(AWS, AWA, boat speed, prevTWA) -> TWS, TWA
       AWS        AWA        STW    prevTWA        TWS        TWA
   10.0000     0.0000     0.0000        NaN    10.0000     0.0000
   10.0000     0.0000     5.0000        NaN     5.0000     0.0000
   10.0000     0.0000    10.0000        NaN     0.0000        NaN
   10.0000     0.0000    10.0000    45.0000     0.0000    45.0000
   10.0000     0.0000    15.0000        NaN     5.0000   180.0000
   10.0000    90.0000     5.0000        NaN    11.1803   116.5650
   10.0000   -90.0000     5.0000        NaN    11.1803  -116.5650
   10.0000   180.0000     5.0000        NaN    15.0000   180.0000
   10.0000  -180.0000     5.0000        NaN    15.0000   180.0000
   10.0000    45.0000     0.0000        NaN    10.0000    45.0000
   12.0000    30.0000     6.0000        NaN     7.4359    53.7940
   15.0000  -120.0000     6.5000        NaN    19.0984  -137.1422
    8.0000   150.0000     4.0000        NaN    11.6373   159.8961
    6.8000    11.8000     7.0000        NaN     1.4324   103.8833
    0.0000     0.0000     0.0000    30.0000     0.0000    30.0000
    0.0400    10.0000     0.0000    30.0000     0.0400    30.0000
    0.0600    10.0000     0.0000    30.0000     0.0600    10.0000
       NaN    30.0000     5.0000    10.0000        NaN        NaN
   10.0000        NaN     5.0000    10.0000        NaN        NaN
   10.0000    30.0000        NaN    10.0000        NaN        NaN

==== B2 trueToApparent(TWS, TWA, boat speed) -> AWS, AWA (the demo, and the checker's grid)
       TWS        TWA        STW        AWS        AWA   back: TWS        TWA
   14.0000   150.0000     5.0000     9.9878   135.5044          14.0000   150.0000
   14.0000  -150.0000     5.0000     9.9878  -135.5044          14.0000  -150.0000
   14.0000   180.0000     5.0000     9.0000   180.0000          14.0000   180.0000
   14.0000   101.0000     5.0000    13.9387    80.3828          14.0000   101.0000
   14.0000   120.0000     5.0000    12.2882    99.3670          14.0000   120.0000
   10.0000   150.0000     7.0000     5.2684   108.3688          10.0000   150.0000
    6.0000   178.0000     7.0000     1.0253    11.7848           6.0000   178.0000
    6.0000   178.0000     3.0000     3.0037   176.0024           6.0000   178.0000
   20.0000   178.0000     7.0000    13.0066   176.9238          20.0000   178.0000
   10.0000    90.0000     5.0000    11.1803    63.4350          10.0000    90.0000
   20.0000    45.0000     6.0000    24.6111    35.0733          20.0000    45.0000
    5.0000     0.0000     0.0000     5.0000     0.0000           5.0000     0.0000
       NaN    90.0000     5.0000        NaN        NaN              NaN        NaN

==== C1 Pipeline::trueHeading (heading as received + deviation + variation), at t, stamp t unless noted
       hdg  magnetic        dev        var  age[ms]    trueHdg
   10.0000         0        NaN        NaN        0    10.0000
   10.0000         0     3.0000     5.0000        0    10.0000
  350.0000         1     3.0000     5.0000        0   358.0000
  355.0000         1     2.0000     4.0000        0     1.0000
    5.0000         1    -3.0000    -4.0000        0   358.0000
   90.0000         1        NaN     5.0000        0    95.0000
   90.0000         1     3.0000        NaN        0    93.0000
   90.0000         1        NaN        NaN        0    90.0000
   90.0000         0        NaN        NaN     4999    90.0000
   90.0000         0        NaN        NaN     5000        NaN
       NaN         0        NaN        NaN        0        NaN

==== C2 Pipeline::trueCourse (COG as a true heading; needs COG and SOG fresh)
       COG  magnetic        var        SOG  cogAge  sogAge   trueCourse
  200.0000         0        NaN     3.0000       0       0     200.0000
  200.0000         0     5.0000     3.0000       0       0     200.0000
  200.0000         1     5.0000     3.0000       0       0     205.0000
  358.0000         1     5.0000     3.0000       0       0       3.0000
  200.0000         1        NaN     3.0000       0       0     200.0000
  200.0000         0        NaN     0.2000       0       0     200.0000
  200.0000         0        NaN        NaN       0       0          NaN
  200.0000         0        NaN     3.0000    5000       0          NaN
  200.0000         0        NaN     3.0000       0    5000          NaN

==== C3 TWD through the pipeline, no damping: AWS 12, AWA 40, STW 6
       hdg  magnetic        dev        var |       TWS        TWA        TWD
  350.0000         1     3.0000     5.0000 |    8.3480    67.5157    65.5157
   10.0000         0        NaN        NaN |    8.3480    67.5157    77.5157
  200.0000         1        NaN    -4.0000 |    8.3480    67.5157   263.5157
       NaN         0        NaN        NaN |    8.3480    67.5157        NaN

==== C4 boat speed: STW if fresh, else SOG if fresh, else none (true wind NaN)
       STW  stwAge        SOG  sogAge   boatSpeed |   TWS(AWS 12, AWA 40)        TWA
    6.0000       0     7.0000       0      6.0000 |                8.3480    67.5157
    6.0000    5000     7.0000       0      7.0000 |                8.0190    74.1323
       NaN       0     7.0000       0      7.0000 |                8.0190    74.1323
    6.0000    5000     7.0000    5000         NaN |                   NaN        NaN
       NaN       0        NaN       0         NaN |                   NaN        NaN

==== D1 bus SOG steps around the COG limits (m/s -> kn as N2kHandler converts)
   0.4000 m/s -> 0.7776 kn   >= 0.99: 0   >= 0.79: 0
   0.4100 m/s -> 0.7970 kn   >= 0.99: 0   >= 0.79: 1
   0.4200 m/s -> 0.8165 kn   >= 0.99: 0   >= 0.79: 1
   0.5000 m/s -> 0.9720 kn   >= 0.99: 0   >= 0.79: 1
   0.5100 m/s -> 0.9914 kn   >= 0.99: 1   >= 0.79: 1
   0.5200 m/s -> 1.0109 kn   >= 0.99: 1   >= 0.79: 1

==== D2 COG latch: no heading, option on, samples 1 s apart (TWD NaN = latch off)
-- D2
   t[s]  SOG[m/s]   SOG[kn]  cog  hdg  opt        TWA        TWD   note
    0.0    0.4500    0.8748    1    0    1    83.4132        NaN   below 1 kn: off
    1.0    0.5000    0.9720    1    0    1    83.4132        NaN   0.972 kn: still off
    2.0    0.5100    0.9914    1    0    1    83.4132   283.4132   0.991 kn: ON
    3.0    0.4500    0.8748    1    0    1    83.4132   283.4132   0.875 kn: stays on
    4.0    0.4100    0.7970    1    0    1    83.4132   283.4132   0.797 kn: stays on
    5.0    0.4000    0.7776    1    0    1    83.4132        NaN   0.778 kn: OFF
    6.0    0.4500    0.8748    1    0    1    83.4132        NaN   0.875 kn: stays off
    7.0    0.5200    1.0109    1    0    1    83.4132   283.4132   1.011 kn: ON
   13.0    0.4500    0.8748    1    0    1    83.4132        NaN   6 s without a sample: OFF (needs 1 kn)
   14.0    0.5500    1.0692    1    0    1    83.4132   283.4132   ON
   15.0    0.6000    1.1664    0    0    1    83.4132        NaN   COG not available: OFF
   16.0    0.4500    0.8748    1    0    1    83.4132        NaN   COG back at 0.875 kn: stays off
   17.0    0.6000    1.1664    1    0    1    83.4132   283.4132   ON
   18.0    0.6000    1.1664    1    0    0    83.4132        NaN   option off: OFF
   19.0    0.4500    0.8748    1    0    1    83.4132        NaN   option on again at 0.875 kn: off
   20.0       NaN    0.8748    1    0    1    83.4132        NaN   SOG field n/a: the last SOG (1 s old) still counts

==== D3 the latch follows the SOG behind a heading
-- D3a came down from 1.17 kn
   t[s]  SOG[m/s]   SOG[kn]  cog  hdg  opt        TWA        TWD   note
    0.0    0.6000    1.1664    1    1    1    83.4132   173.4132   heading: TWD = 90 + TWA
    1.0    0.4700    0.9137    1    1    1    83.4132   173.4132   0.914 kn behind the heading
    2.0    0.4700    0.9137    1    0    1    83.4132   283.4132   heading gone: COG at once (reached 1 kn)
-- D3b never reached 1 kn
   t[s]  SOG[m/s]   SOG[kn]  cog  hdg  opt        TWA        TWD   note
    0.0    0.4700    0.9137    1    1    1    83.4132   173.4132   heading: TWD = 90 + TWA
    1.0    0.4700    0.9137    1    1    1    83.4132   173.4132   0.914 kn behind the heading
    2.0    0.4700    0.9137    1    0    1    83.4132        NaN   heading gone: no TWD (never reached 1 kn)

==== E1 received true wind (another device), no apparent wind on the bus; no damping
   t[s]  ref       TWS      angle   used        TWS        TWA        TWD   note
    0.0    3   11.0000    50.0000      1    11.0000    50.0000   150.0000   ref 3 alone: used
    0.2    4   12.0000    45.0000      1    12.0000    45.0000   145.0000   ref 4: used (best)
    0.4    3   11.0000    50.0000      0    12.0000    45.0000   145.0000   ref 3 while 4 talks: stands by
    0.6    0   12.0000   140.0000      0    12.0000    45.0000   145.0000   ref 0 while 4 talks: stands by, TWD kept
    5.6    3   11.0000    50.0000      1    11.0000    50.0000   150.0000   4 silent 5 s: ref 3 used
    5.8    0   12.0000   140.0000      0    11.0000    50.0000   150.0000   ref 0 while 3 talks: stands by
   11.0    0   12.0000   140.0000      1    12.0000    40.0000   140.0000   only ref 0: TWA = TWD - heading
   11.2    0   12.0000   140.0000      1    12.0000        NaN   140.0000   ref 0, no heading: TWA NaN, TWD 140
   11.4  app   12(AWS)    40(AWA)      1     8.3480    67.5157   140.0000   apparent sample (AWS 12 AWA 40 STW 6): TWD from ref 0
   11.6    4   12.0000    45.0000      0     8.3480    67.5157   140.0000   ref 4 while apparent is fresh: ignored

==== E2 expire(): blank after STALE_MS = 10 s, the next sample starts afresh (tau 2 s)
   t=0      AWS 10.0000 AWA 50.0000 TWS 7.6724 TWA 86.8031 TWD 86.8031
   t=9.999  AWS 10.0000 AWA 50.0000 TWS 7.6724 TWA 86.8031 TWD 86.8031 (expire, still fresh)
   t=10.0   AWS NaN AWA NaN TWS NaN TWA NaN TWD NaN (expire: blanked)
   t=10.5   AWS 20.0000 AWA -30.0000 TWS 15.1048 TWA -41.4559 TWD 318.5441 (new sample taken as is)

==== F1 lowPassAlpha(dt, tau) = 1 - exp(-dt/tau); tau <= 0 or NaN -> 1; dt <= 0 -> 0
   dt 0.1000 tau 2.0000 -> 0.0488
   dt 0.5000 tau 2.0000 -> 0.2212
   dt 1.0000 tau 2.0000 -> 0.3935
   dt 2.0000 tau 2.0000 -> 0.6321
   dt 6.0000 tau 2.0000 -> 0.9502
   dt 0.5000 tau 0.0000 -> 1.0000
   dt 0.5000 tau -1.0000 -> 1.0000
   dt 0.5000 tau NaN -> 1.0000
   dt 0.0000 tau 2.0000 -> 0.0000
   dt -0.1000 tau 2.0000 -> 0.0000
   dt 0.6931 tau 1.0000 -> 0.5000

==== F2 ScalarLowPass, tau 2 s, step 0 -> 10 at uneven dt; exact = 10 (1 - exp(-t/2))
   first sample 0 -> 0.0000
   t 0.1000 dt 0.1000 -> 0.4877   exact 0.4877
   t 0.3500 dt 0.2500 -> 1.6054   exact 1.6054
   t 0.4000 dt 0.0500 -> 1.8127   exact 1.8127
   t 1.0000 dt 0.6000 -> 3.9347   exact 3.9347
   t 2.0000 dt 1.0000 -> 6.3212   exact 6.3212
   t 2.5000 dt 0.5000 -> 7.1350   exact 7.1350
   t 4.0000 dt 1.5000 -> 8.6466   exact 8.6466
   t 7.0000 dt 3.0000 -> 9.6980   exact 9.6980
   NaN -> NaN (reset); next 7 -> 7.0000 (taken as is)
   ten samples 0.1 s apart -> 3.9347; one sample 1 s later -> 3.9347
   tau 0: 3 then 8 -> 8.0000 (input passes through)

==== F3 AngleLowPass, tau 2 s: wrap 359 -> 1 (goes through 0, never through 180)
   first sample 359 -> -1.0000
   t 0.2000 x 1 -> -0.8097
   t 0.7000 x 1 -> -0.4094
   t 1.0000 x 1 -> -0.2131
   t 2.0000 x 1 -> 0.2643
   t 4.0000 x 1 -> 0.7294
   t 8.0000 x 1 -> 0.9634

==== F4 AngleLowPass, tau 2 s: following wind alternating +179 / -179
   first 179 -> 179.0000
   -179.0000 -> 179.2350
   179.0000 -> 179.2074
   -179.0000 -> 179.4180
   179.0000 -> 179.3689
   -179.0000 -> 179.5605
   179.0000 -> 179.4947

==== F5 AngleLowPass: large steps, one sample after dt = tau (fraction of the step shown)
   0 -> 10.0000: 6.3243  (0.6324 of the step)
   0 -> 45.0000: 28.7463  (0.6388 of the step)
   0 -> 90.0000: 59.8016  (0.6645 of the step)
   0 -> 135.0000: 100.0352  (0.7410 of the step)
   0 -> 170.0000: 156.6806  (0.9217 of the step)
   0 -> -170.0000: -156.6806  (0.9217 of the step)
   170 deg step at uneven dt, tau 2:
     t 0.5000 -> 3.9171 (0.0230)
     t 1.0000 -> 17.3244 (0.1019)
     t 2.0000 -> 156.6806 (0.9217)
     t 4.0000 -> 168.1596 (0.9892)
     t 8.0000 -> 169.8109 (0.9989)
   exactly opposite, alpha 0.5 (0 -> 180, dt ln2, tau 1): vector (0.0000, -0.0000), output 0.0000 (kept)
   next 180 dt 1: 180.0000

==== F6 Pipeline: each time constant drives only its own outputs
   damping appSpd 1 appDir 2 trueSpd 4 trueDir 8; heading 30 true; STW 5
      t    AWSin  AWAin |     rawTWS     rawTWA |        AWS        AWA        TWS        TWA        TWD
     0.0     10.0   40.0 |     6.9567    67.5157 |    10.0000    40.0000     6.9567    67.5157    97.5157
     0.3     15.0   60.0 |    13.2288    79.1066 |    11.2959    42.7506     7.4099    67.9398    97.9398
     0.7     15.0   60.0 |    13.2288    79.1066 |    12.5171    45.8713     7.9636    68.4818    98.4818
     1.2     15.0   60.0 |    13.2288    79.1066 |    13.4940    49.0138     8.5823    69.1234    99.1234
     2.0     15.0   60.0 |    13.2288    79.1066 |    14.3233    52.6676     9.4246    70.0720   100.0720
     3.5     15.0   60.0 |    13.2288    79.1066 |    14.8490    56.5627    10.6142    71.6177   101.6177
     6.0     15.0   60.0 |    13.2288    79.1066 |    14.9876    59.0215    11.8293    73.6326   103.6326
    10.0     15.0   60.0 |    13.2288    79.1066 |    14.9998    59.8679    12.7139    75.7927   105.7927
    20.0     15.0   60.0 |    13.2288    79.1066 |    15.0000    59.9991    13.1865    78.1602   108.1601

==== F7 Pipeline: calm keeps the previous TWA (AWS 5 AWA 0 STW 5 -> TWS 0)
   AWS 8 AWA 50:  TWS 6.1300 TWA 88.6698 TWD 88.6698
   AWS 5 AWA 0:   TWS 0.0000 TWA 88.6698 TWD 88.6698 (previous raw TWA held)
   same from a fresh pipeline: TWS 0.0000 TWA NaN TWD NaN (no previous angle)
   AWS NaN AWA 30: AWS NaN AWA 30.0000 TWS NaN TWA NaN (half a sample clears the true wind)

==== G1 WindDampCfg::apply(cur, keys) - migration of the two old keys, clamp 0..30
   cur(appSpd appDir trueSpd trueDir) | app_spd app_dir true_spd true_dir old_spd old_dir | result
   2.0000 2.0000 2.0000 2.0000 | NaN NaN NaN NaN NaN NaN | 2.0000 2.0000 2.0000 2.0000   no key: keep
   2.0000 2.0000 2.0000 2.0000 | NaN NaN NaN NaN 5.0000 NaN | 5.0000 2.0000 5.0000 2.0000   old speed only
   2.0000 2.0000 2.0000 2.0000 | NaN NaN NaN NaN 5.0000 7.0000 | 5.0000 7.0000 5.0000 7.0000   both old keys
   2.0000 2.0000 2.0000 2.0000 | 1.0000 NaN NaN NaN 5.0000 7.0000 | 1.0000 7.0000 5.0000 7.0000   one new key + both old
   2.0000 2.0000 2.0000 2.0000 | 1.0000 3.0000 4.0000 10.0000 5.0000 7.0000 | 1.0000 3.0000 4.0000 10.0000   all new keys win
   2.0000 2.0000 2.0000 2.0000 | -3.0000 45.0000 NaN 0.0000 NaN NaN | 0.0000 30.0000 2.0000 0.0000   clamped; NaN = not a number keeps
   2.0000 2.0000 2.0000 2.0000 | NaN NaN NaN NaN NaN 40.0000 | 2.0000 30.0000 2.0000 30.0000   old dir 40 clamped to 30
   1.0000 3.0000 4.0000 10.0000 | NaN NaN NaN NaN NaN NaN | 1.0000 3.0000 4.0000 10.0000   patch without damping keys keeps
   1.0000 3.0000 4.0000 10.0000 | NaN NaN NaN NaN 6.0000 NaN | 6.0000 3.0000 6.0000 10.0000   old page posts old speed key
   2.0000 2.0000 2.0000 2.0000 | 0.2500 0.0000 30.0000 30.5000 NaN NaN | 0.2500 0.0000 30.0000 30.0000   0 = off, 30 max

==== H1 sail.spi_type on load (Config::fromJson) and preselect (index.html spiTypeOf); current value gennaker
   spi_type     spinnaker  | firmware   web page
   "gores"      (missing)  | gores      gores
   "none"       true       | none       none
   "gennaker"   false      | gennaker   gennaker
   "balloon"    (missing)  | balloon    balloon
   (missing)    true       | balloon    balloon
   (missing)    false      | none       none
   null         false      | none       none
   "Balloon"    false      | none       none
   "kite"       true       | balloon    balloon
   "kite"       (missing)  | balloon    balloon
   2            (missing)  | balloon    balloon
   null         (missing)  | gennaker   balloon
   (missing)    (missing)  | gennaker   balloon
   (missing)    2          | gennaker   balloon
   save (toJson): spi_type = name, spinnaker = (type != none):  none/false  gores/true  balloon/true  gennaker/true

==== I1 downwind sail shown (useSpinnaker), steady, STW 5, AWA from the true wind
   (after each TWS: four digits = useSpinnaker for none, gores, balloon, gennaker)
   TWA   89.9000:  TWS 19.9000 0000  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA   90.0000:  TWS 19.9000 0000  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA   90.1000:  TWS 19.9000 0001  TWS 20.0000 0001  TWS 24.9000 0001  TWS 25.0000 0000
   TWA   95.0000:  TWS 19.9000 0001  TWS 20.0000 0001  TWS 24.9000 0001  TWS 25.0000 0000
   TWA   99.9000:  TWS 19.9000 0001  TWS 20.0000 0001  TWS 24.9000 0001  TWS 25.0000 0000
   TWA  100.0000:  TWS 19.9000 0001  TWS 20.0000 0001  TWS 24.9000 0001  TWS 25.0000 0000
   TWA  100.1000:  TWS 19.9000 0111  TWS 20.0000 0111  TWS 24.9000 0111  TWS 25.0000 0000
   TWA  149.9000:  TWS 19.9000 0111  TWS 20.0000 0111  TWS 24.9000 0111  TWS 25.0000 0000
   TWA  150.0000:  TWS 19.9000 0111  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA  155.0000:  TWS 19.9000 0111  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA  160.0000:  TWS 19.9000 0111  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA  160.1000:  TWS 19.9000 0110  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA  175.0000:  TWS 19.9000 0110  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA  180.0000:  TWS 19.9000 0110  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   TWA -100.1000:  TWS 19.9000 0111  TWS 20.0000 0111  TWS 24.9000 0111  TWS 25.0000 0000
   TWA -160.0000:  TWS 19.9000 0111  TWS 20.0000 0000  TWS 24.9000 0000  TWS 25.0000 0000
   no TWS (NaN), TWA 120: balloon 0; no TWA (NaN), AWA 120, TWS 10: balloon 0

==== I2 Code Zero (sail.code_zero on): no downwind sail, 50 < |TWA| < 95, 2 < TWS < 14
   TWA 50.0000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 0 gennaker 0  TWS 13.9000 none 0 gennaker 0  TWS 14.0000 none 0 gennaker 0
   TWA 50.1000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 1 gennaker 1  TWS 13.9000 none 1 gennaker 1  TWS 14.0000 none 0 gennaker 0
   TWA 80.0000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 1 gennaker 1  TWS 13.9000 none 1 gennaker 1  TWS 14.0000 none 0 gennaker 0
   TWA 92.0000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 1 gennaker 0  TWS 13.9000 none 1 gennaker 0  TWS 14.0000 none 0 gennaker 0
   TWA 94.9000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 1 gennaker 0  TWS 13.9000 none 1 gennaker 0  TWS 14.0000 none 0 gennaker 0
   TWA 95.0000:  TWS 2.0000 none 0 gennaker 0  TWS 2.1000 none 0 gennaker 0  TWS 13.9000 none 0 gennaker 0  TWS 14.0000 none 0 gennaker 0

==== I3 luffing / running / lee side (before the latch)
   TWA 27.9000 AWA 20.0000 -> luffing 1 running 0 leewardSide -1.0000
   TWA 28.0000 AWA 21.0000 -> luffing 0 running 0 leewardSide -1.0000
   TWA 150.0000 AWA 138.0000 -> luffing 0 running 0 leewardSide -1.0000
   TWA 150.1000 AWA 138.0000 -> luffing 0 running 1 leewardSide -1.0000
   TWA -40.0000 AWA -30.0000 -> luffing 0 running 0 leewardSide 1.0000
   TWA NaN AWA 17.9000 -> luffing 1 running 0 leewardSide -1.0000
   TWA NaN AWA 18.0000 -> luffing 0 running 0 leewardSide -1.0000
   TWA NaN AWA 140.0000 -> luffing 0 running 0 leewardSide -1.0000
   TWA NaN AWA 140.1000 -> luffing 0 running 1 leewardSide -1.0000
   TWA NaN AWA -60.0000 -> luffing 0 running 0 leewardSide 1.0000
   TWA NaN AWA NaN -> luffing 0 running 0 leewardSide -1.0000
   TWA 0.0000 AWA 0.0000 -> luffing 1 running 0 leewardSide -1.0000
   TWA 180.0000 AWA 180.0000 -> luffing 0 running 1 leewardSide -1.0000

==== I4 trim targets from |AWA|: boom = clamp(|AWA| - 13, 4, 86), headsail = 0.82 boom; awaAbs
   |AWA| 0.0000 -> boom 4.0000 headsail 3.2800 awaAbs 0.0000
   |AWA| 10.0000 -> boom 4.0000 headsail 3.2800 awaAbs 10.0000
   |AWA| 17.0000 -> boom 4.0000 headsail 3.2800 awaAbs 17.0000
   |AWA| 20.0000 -> boom 7.0000 headsail 5.7400 awaAbs 20.0000
   |AWA| 45.0000 -> boom 32.0000 headsail 26.2400 awaAbs 45.0000
   |AWA| 90.0000 -> boom 77.0000 headsail 63.1400 awaAbs 90.0000
   |AWA| 99.0000 -> boom 86.0000 headsail 70.5200 awaAbs 99.0000
   |AWA| 100.0000 -> boom 86.0000 headsail 70.5200 awaAbs 100.0000
   |AWA| 150.0000 -> boom 86.0000 headsail 70.5200 awaAbs 150.0000
   |AWA| 180.0000 -> boom 86.0000 headsail 70.5200 awaAbs 180.0000
   AWA NaN, TWA 130 -> boom 4.0000 headsail 3.2800 awaAbs 130.0000
   no wind at all  -> boom 4.0000 headsail 3.2800 awaAbs 180.0000 luffing 0 running 0 lee -1.0000

==== I5 camber (belly / chord) from |TWA|; jib = 0.85 main; no TWA -> 0.16 / 0.14
   |TWA| 20.0000 -> main 0.1000 jib 0.0850
   |TWA| 34.9000 -> main 0.1000 jib 0.0850
   |TWA| 35.0000 -> main 0.1500 jib 0.1275
   |TWA| 59.9000 -> main 0.1500 jib 0.1275
   |TWA| 60.0000 -> main 0.1900 jib 0.1615
   |TWA| 99.9000 -> main 0.1900 jib 0.1615
   |TWA| 100.0000 -> main 0.2400 jib 0.2040
   |TWA| 149.9000 -> main 0.2400 jib 0.2040
   |TWA| 150.0000 -> main 0.2800 jib 0.2380
   |TWA| 180.0000 -> main 0.2800 jib 0.2380
   no TWA       -> main 0.1600 jib 0.1400

==== I6 reef count: TWS > 16/22/28 or STW / hull speed > 0.88/0.93/0.97; hull speed = 2.43 sqrt(LWL)
   LWL 9 -> hull speed 7.2900 kn
   TWS 16.0000 STW 0.0000 -> reef 0
   TWS 16.1000 STW 0.0000 -> reef 1
   TWS 22.0000 STW 0.0000 -> reef 1
   TWS 22.1000 STW 0.0000 -> reef 2
   TWS 28.0000 STW 0.0000 -> reef 2
   TWS 28.1000 STW 0.0000 -> reef 3
   TWS 10.0000 STW 6.4100 -> reef 0
   TWS 10.0000 STW 6.4200 -> reef 1
   TWS 10.0000 STW 6.7800 -> reef 2
   TWS 10.0000 STW 6.7900 -> reef 2
   TWS 10.0000 STW 7.0700 -> reef 2
   TWS 10.0000 STW 7.0800 -> reef 3
   TWS 10.0000 STW NaN -> reef 0
   TWS NaN STW 9.0000 -> reef 0
   LWL 0 (treated as 9): TWS 10 STW 6.42 -> reef 1

==== I7 main, jib and Code Zero geometry, steady state, boat units (drawBoat / drawSailFoil / drawCodeZero)
   TWS 10, STW 5, foresail_pct 100, lee from the sign; mast (0,-6), boom 32; jib tack (0,-30), length 30 fp
   TWA 45.0000 AWA 30.3612: boom -17.3612 main clew (-9.5486, 24.5422) ctrl (-8.8782, 6.3117) camber 0.1500 | jib -14.2362 clew (-7.3776, -0.9213) ctrl (-7.0274, -17.8552) camber 0.1275
   TWA 70.0000 AWA 48.1378: boom -35.1378 main clew (-18.4174, 20.1687) ctrl (-13.2599, 2.2766) camber 0.1900 | Code Zero clew (-16.3864, -0.2093) ctrl (-11.2467, -18.7244)
   TWA 90.0000 AWA 63.4350: boom -50.4350 main clew (-24.6689, 14.3825) ctrl (-14.9737, -1.5149) camber 0.1900 | Code Zero clew (-22.4653, -4.4792) ctrl (-13.4271, -21.4361)
   TWA 120.0000 AWA 90.0000: boom -77.0000 main clew (-31.1798, 1.1984) ctrl (-15.7586, -10.2439) camber 0.2400 | jib -63.1400 clew (-26.7634, -16.4456) ctrl (-14.8086, -29.3603) camber 0.2040
   TWA 150.0000 AWA 126.2060: boom -86.0000 main clew (-31.9221, -3.7678) ctrl (-14.9899, -13.9337) camber 0.2800 | jib -70.5200 clew (-28.2827, -19.9957) ctrl (-15.1083, -32.2293) camber 0.2380
   TWA 180.0000 AWA 180.0000: boom -86.0000 main clew (-31.9221, -3.7678) ctrl (-14.9899, -13.9337) camber 0.2800 | running: no jib

==== J1 lee side held through a dead run (latchLeeSide in drawInstrument), TWA sequence
   TWA  170.0000 AWA  160.0000  sign-side -1.0000  kept -1.0000  drawn -1.0000   first value: from the sign
   TWA  176.0000 AWA  172.0000  sign-side -1.0000  kept -1.0000  drawn -1.0000   
   TWA  179.0000 AWA  178.0000  sign-side -1.0000  kept -1.0000  drawn -1.0000   
   TWA  180.0000 AWA  180.0000  sign-side -1.0000  kept -1.0000  drawn -1.0000   
   TWA -179.0000 AWA -178.0000  sign-side 1.0000  kept -1.0000  drawn -1.0000   beyond 175: kept
   TWA -177.0000 AWA -175.0000  sign-side 1.0000  kept -1.0000  drawn -1.0000   
   TWA -176.0000 AWA -173.0000  sign-side 1.0000  kept -1.0000  drawn -1.0000   
   TWA -175.5000 AWA -172.0000  sign-side 1.0000  kept -1.0000  drawn -1.0000   
   TWA -175.0000 AWA -171.0000  sign-side 1.0000  kept 1.0000  drawn 1.0000   175 or less: new side
   TWA  179.0000 AWA  178.0000  sign-side -1.0000  kept 1.0000  drawn 1.0000   kept
   TWA  176.0000 AWA  172.0000  sign-side -1.0000  kept 1.0000  drawn 1.0000   
   TWA  175.0000 AWA  170.0000  sign-side -1.0000  kept -1.0000  drawn -1.0000   new side
   TWA -175.1000 AWA -171.0000  sign-side 1.0000  kept -1.0000  drawn -1.0000   kept
   TWA       NaN AWA       NaN  sign-side -1.0000  kept 0.0000  drawn -1.0000   no wind: forgotten
   TWA -179.0000 AWA -178.0000  sign-side 1.0000  kept 1.0000  drawn 1.0000   first after no wind: from the sign
   TWA  179.0000 AWA  178.0000  sign-side -1.0000  kept 1.0000  drawn 1.0000   kept
   TWA       NaN AWA  177.0000  sign-side -1.0000  kept 1.0000  drawn 1.0000   TWA lost, AWA 177: kept (|AWA| > 175)
   TWA       NaN AWA -170.0000  sign-side 1.0000  kept 1.0000  drawn 1.0000   AWA -170: new side

==== K1 smoothing, k = 0.18 per frame, balloon, TWS 10 STW 5: frame 0 TWA 60 (first frame: targets at once), then TWA 120
   frame       TWA        AWA |   smBoom    smJib smSpiRot    smGen | drawn
       0   60.0000    40.8934 | -27.8934 -22.8726 -14.0000 -44.0000 | 0
       1  120.0000    90.0000 | -36.7326 -30.1207 -14.0000 -44.5400 | 1
       2  120.0000    90.0000 | -43.9807 -36.0642 -14.0000 -44.9828 | 1
       3  120.0000    90.0000 | -49.9242 -40.9378 -14.0000 -45.3459 | 1
       4  120.0000    90.0000 | -54.7978 -44.9342 -14.0000 -45.6436 | 1
       5  120.0000    90.0000 | -58.7942 -48.2113 -14.0000 -45.8878 | 1
       6  120.0000    90.0000 | -62.0713 -50.8984 -14.0000 -46.0880 | 1
       7  120.0000    90.0000 | -64.7584 -53.1019 -14.0000 -46.2521 | 1
       8  120.0000    90.0000 | -66.9619 -54.9088 -14.0000 -46.3868 | 1

==== K2 gybe at TWA 150 -> -150 (TWS 10 STW 5): boom swings, gennaker jumps, pole side follows the boom
   frame       TWA |   smBoom smSpiRot    smGen | lee  poleSide(spiRigSide) genSide
       0  150.0000 | -86.0000 -11.4485 -54.2412 | -1.0000 -1.0000 -1.0000
       1  150.0000 | -86.0000 -11.4485 -54.2412 | -1.0000 -1.0000 -1.0000
       2 -150.0000 | -55.0400  -7.3270  54.2412 | 1.0000 -1.0000 1.0000
       3 -150.0000 | -29.6528  -3.9474  54.2412 | 1.0000 -1.0000 1.0000
       4 -150.0000 |  -8.8353  -1.1762  54.2412 | 1.0000 -1.0000 1.0000
       5 -150.0000 |   8.2351   1.0963  54.2412 | 1.0000 1.0000 1.0000
       6 -150.0000 |  22.2327   2.9597  54.2412 | 1.0000 1.0000 1.0000
       7 -150.0000 |  33.7109   4.4877  54.2412 | 1.0000 1.0000 1.0000
       8 -150.0000 |  43.1229   5.7406  54.2412 | 1.0000 1.0000 1.0000
       9 -150.0000 |  50.8408   6.7680  54.2412 | 1.0000 1.0000 1.0000
      10 -150.0000 |  57.1694   7.6105  54.2412 | 1.0000 1.0000 1.0000
      11 -150.0000 |  62.3589   8.3013  54.2412 | 1.0000 1.0000 1.0000
   resetRig(), then TWA 150: smBoom -86.0000 smSpiRot -11.4485 smGen -54.2412 (targets at once)

==== K3 butterfly (sail.butterfly on, running): jib target -lee * 84, no kite drawn
   TWA 165 AWA 150.9481: running 1 useSpinnaker 1 smJib 84.0000 smBoom -86.0000 drawn 0

==== K4 luffing (|TWA| < 28): flap targets boom 15 sin(0.018 t), jib 17 sin(0.018 t + 0.7), t = tick [ms]
   t    0 ms: boom 0.0000 jib 10.9517
   t   50 ms: boom 11.7499 jib 16.9928
   t  100 ms: boom 14.6077 jib 10.1740
   t  175 ms: boom -0.1261 jib -11.0606
   t  349 ms: boom -0.0178 jib 10.9363

==== L1 rotation and sheeting laws
   |AWA| 180.0000 -> symRotationDeg 0.0000  genSheetDeg 56.0000
   |AWA| 175.0000 -> symRotationDeg 0.0000  genSheetDeg 56.0000
   |AWA| 172.0000 -> symRotationDeg 0.0000  genSheetDeg 56.0000
   |AWA| 171.0000 -> symRotationDeg 0.2500  genSheetDeg 56.0000
   |AWA| 168.1000 -> symRotationDeg 0.9750  genSheetDeg 56.0000
   |AWA| 165.0000 -> symRotationDeg 1.7500  genSheetDeg 56.0000
   |AWA| 160.0000 -> symRotationDeg 3.0000  genSheetDeg 56.0000
   |AWA| 150.0000 -> symRotationDeg 5.5000  genSheetDeg 56.0000
   |AWA| 135.0000 -> symRotationDeg 9.2500  genSheetDeg 56.0000
   |AWA| 120.0000 -> symRotationDeg 13.0000  genSheetDeg 53.0000
   |AWA| 116.0000 -> symRotationDeg 14.0000  genSheetDeg 52.2000
   |AWA| 110.0000 -> symRotationDeg 14.0000  genSheetDeg 51.0000
   |AWA| 99.8000 -> symRotationDeg 14.0000  genSheetDeg 48.9600
   |AWA| 90.0000 -> symRotationDeg 14.0000  genSheetDeg 47.0000
   |AWA| 80.0000 -> symRotationDeg 14.0000  genSheetDeg 45.0000
   |AWA| 44.0000 -> symRotationDeg 14.0000  genSheetDeg 44.0000
   |AWA| 11.8000 -> symRotationDeg 14.0000  genSheetDeg 44.0000
   |AWA| NaN -> symRotationDeg 0.0000  genSheetDeg 44.0000
   balloonBandEdge j=0..5: 0.0000 0.2759 0.4804 0.6645 0.8365 1.0000
   rig points lee +1: quarterBlock (9.6000, 25.0000) sheetWinch (7.0000, 14.7000) twing (-10.6000, -1.0000) guyWinch (-7.0000, 14.7000)
   rig points lee -1: quarterBlock (-9.6000, 25.0000) twing (10.6000, -1.0000)

==== L2 symmetric kite (gores, balloon), steady state, boat units
   |AWA| 180.0000 lee 1.0000: rot 0.0000 | tack[0] (-19.0000, -12.0000) clew[24] (19.0000, -12.0000) | crest lead[12] (0.0000, -32.4500) r 32.4500 | max r 32.4500 at lead[12] / lead[12] (mirror pair, equal radius)
        foot[12] (0.0000, -20.0000) foot[6] (-9.5000, -18.0000) lead[6] (-15.8750, -27.3375) lead[18] (15.8750, -27.3375) | pole 72.4744 deg, length 19.9249
   |AWA| 172.0000 lee 1.0000: rot 0.0000 | tack[0] (-19.0000, -12.0000) clew[24] (19.0000, -12.0000) | crest lead[12] (0.0000, -32.4500) r 32.4500 | max r 32.4500 at lead[12] / lead[12] (mirror pair, equal radius)
        foot[12] (0.0000, -20.0000) foot[6] (-9.5000, -18.0000) lead[6] (-15.8750, -27.3375) lead[18] (15.8750, -27.3375) | pole 72.4744 deg, length 19.9249
   |AWA| 165.0000 lee 1.0000: rot 1.7500 | tack[0] (-18.3633, -12.9418) clew[24] (19.1192, -11.7966) | crest lead[12] (0.9910, -32.4349) r 32.4500 | max r 32.4500 at lead[12] / lead[12] (mirror pair, equal radius)
        foot[12] (0.6165, -20.1781) foot[6] (-8.8138, -18.5122) lead[6] (-14.9284, -27.9002) lead[18] (16.6038, -26.9368) | pole 69.2921 deg, length 19.6316
   |AWA| 150.0000 lee 1.0000: rot 5.5000 | tack[0] (-16.8673, -14.8637) clew[24] (19.3935, -11.3721) | crest lead[12] (3.1102, -32.3006) r 32.4500 | max r 32.4500 at lead[12] / lead[12] (mirror pair, equal radius)
        foot[12] (1.9734, -20.4945) foot[6] (-7.2694, -19.5232) lead[6] (-12.8358, -28.9959) lead[18] (18.1326, -26.0140) | pole 62.2784 deg, length 19.0544
   |AWA| 135.0000 lee 1.0000: rot 9.2500 | tack[0] (-15.2012, -16.6420) clew[24] (19.6962, -10.9586) | crest lead[12] (5.2161, -32.0280) r 32.4500 | max r 32.4523 at lead[11] / lead[13] (mirror pair, equal radius)
        foot[12] (3.3742, -20.7181) foot[6] (-5.6318, -20.4095) lead[6] (-10.6648, -29.9366) lead[18] (19.6127, -25.0056) | pole 55.0049 deg, length 18.5561
   |AWA| 116.0000 lee 1.0000: rot 14.0000 | tack[0] (-12.8662, -18.6671) clew[24] (20.1239, -10.4418) | crest lead[12] (7.8504, -31.4861) r 32.4500 | max r 32.4641 at lead[10] / lead[14] (mirror pair, equal radius)
        foot[12] (5.2013, -20.8614) foot[6] (-3.4393, -21.3410) lead[6] (-7.8201, -30.8971) lead[18] (21.4101, -23.6092) | pole 45.4467 deg, length 18.0553
   |AWA| 100.0000 lee 1.0000: rot 14.0000 | tack[0] (-12.8662, -18.6671) clew[24] (20.1239, -10.4418) | crest lead[12] (7.8504, -31.4861) r 32.4500 | max r 32.4641 at lead[10] / lead[14] (mirror pair, equal radius)
        foot[12] (5.2013, -20.8614) foot[6] (-3.4393, -21.3410) lead[6] (-7.8201, -30.8971) lead[18] (21.4101, -23.6092) | pole 45.4467 deg, length 18.0553
   |AWA| 80.0000 lee 1.0000: rot 14.0000 | tack[0] (-12.8662, -18.6671) clew[24] (20.1239, -10.4418) | crest lead[12] (7.8504, -31.4861) r 32.4500 | max r 32.4641 at lead[10] / lead[14] (mirror pair, equal radius)
        foot[12] (5.2013, -20.8614) foot[6] (-3.4393, -21.3410) lead[6] (-7.8201, -30.8971) lead[18] (21.4101, -23.6092) | pole 45.4467 deg, length 18.0553
   |AWA| 135.0000 lee -1.0000: rot -9.2500 | tack[24] (15.2012, -16.6420) clew[0] (-19.6962, -10.9586) | crest lead[12] (-5.2161, -32.0280) r 32.4500 | max r 32.4523 at lead[11] / lead[13] (mirror pair, equal radius)
        foot[12] (-3.3742, -20.7181) foot[6] (-11.8168, -17.5678) lead[6] (-19.6127, -25.0056) lead[18] (10.6648, -29.9366) | pole 55.0049 deg, length 18.5561
   |AWA| 11.8000 lee 1.0000: rot 14.0000 | tack[0] (-12.8662, -18.6671) clew[24] (20.1239, -10.4418) | crest lead[12] (7.8504, -31.4861) r 32.4500 | max r 32.4641 at lead[10] / lead[14] (mirror pair, equal radius)
        foot[12] (5.2013, -20.8614) foot[6] (-3.4393, -21.3410) lead[6] (-7.8201, -30.8971) lead[18] (21.4101, -23.6092) | pole 45.4467 deg, length 18.0553

==== L2b panel seams (gores) and band edges (balloon) at sample 12, lee +1
   rot 0.0000 gores seams foot[i] -> lead[i]: i4 (-12.6667, -16.4444)->(-18.9630, -23.3611) i8 (-6.3333, -19.1111)->(-11.3704, -30.1778) i12 (0.0000, -20.0000)->(0.0000, -32.4500) i16 (6.3333, -19.1111)->(11.3704, -30.1778) i20 (12.6667, -16.4444)->(18.9630, -23.3611)
   rot 0.0000 balloon band edges j=0..5 at i=12: (0.0000, -20.0000) (0.0000, -23.4355) (0.0000, -25.9816) (0.0000, -28.2735) (0.0000, -30.4146) (0.0000, -32.4500)
   rot 14.0000 gores seams foot[i] -> lead[i]: i4 (-6.4942, -20.8001)->(-11.3115, -28.2707) i8 (-0.4717, -21.5315)->(-3.1482, -32.2301) i12 (5.2013, -20.8614)->(7.8504, -31.4861) i16 (10.5249, -18.7897)->(17.9108, -26.9795) i20 (15.4991, -15.3165)->(23.2598, -19.6511)
   rot 14.0000 balloon band edges j=0..5 at i=12: (5.2013, -20.8614) (5.9323, -23.7932) (6.4741, -25.9660) (6.9617, -27.9219) (7.4173, -29.7491) (7.8504, -31.4861)

==== L3 gennaker, steady state, boat units
   |AWA| 90.0000 lee 1.0000: sheet 47.0000 ok 1 | tack (0.0000, -32.2500) clew (23.2205, -10.5966) chord 31.7500 | lee[8] (16.1966, -25.1777) luff[8] (12.6537, -23.7061) panel[8] (13.8229, -24.1918)
        lee[4] (9.2449, -29.6525) lee[12] (20.8552, -18.8258) | seams panel[2] (3.8706, -30.7545) -> lee[5] (11.1978, -28.7098) panel[6] (10.7820, -26.7254) -> lee[9] (17.5763, -23.7657) panel[10] (16.5872, -21.3121) -> lee[13] (21.6615, -16.9444) | max r 32.2500 at lee[0]
   |AWA| 99.8000 lee 1.0000: sheet 48.9600 ok 1 | tack (0.0000, -32.2500) clew (23.5779, -11.7251) chord 31.2600 | lee[8] (16.1755, -25.8364) luff[8] (12.7389, -24.2690) panel[8] (13.8730, -24.7862)
        lee[4] (9.1844, -30.0054) lee[12] (20.9733, -19.7429) | seams panel[2] (3.8590, -30.9088) -> lee[5] (11.1378, -29.1435) panel[6] (10.7955, -27.1769) -> lee[9] (17.5806, -24.4934) panel[10] (16.6899, -22.0457) -> lee[13] (21.8301, -17.9189) | max r 32.2500 at lee[0]
   |AWA| 116.0000 lee 1.0000: sheet 52.2000 ok 1 | tack (0.0000, -32.2500) clew (24.0602, -13.5870) chord 30.4500 | lee[8] (16.0843, -26.9031) luff[8] (12.8283, -25.1896) panel[8] (13.9028, -25.7550)
        lee[4] (9.0557, -30.5727) lee[12] (21.0858, -21.2412) | seams panel[2] (3.8268, -31.1581) -> lee[5] (11.0029, -29.8420) panel[6] (10.7782, -27.9106) -> lee[9] (17.5247, -25.6744) panel[10] (16.7933, -23.2449) -> lee[13] (22.0194, -19.5144) | max r 32.2500 at lee[0]
   |AWA| 135.0000 lee 1.0000: sheet 56.0000 ok 1 | tack (0.0000, -32.2500) clew (24.4566, -15.7538) chord 29.5000 | lee[8] (15.8915, -28.1140) luff[8] (12.8541, -26.2486) panel[8] (13.8564, -26.8641)
        lee[4] (8.8616, -31.2100) lee[12] (21.0899, -22.9619) | seams panel[2] (3.7694, -31.4402) -> lee[5] (10.7908, -30.6287) panel[6] (10.6976, -28.7473) -> lee[9] (17.3628, -27.0187) panel[10] (16.8118, -24.6232) -> lee[13] (22.1033, -21.3526) | max r 32.4740 at lee[5]
   |AWA| 150.0000 lee 1.0000: sheet 56.0000 ok 1 | tack (0.0000, -32.2500) clew (24.4566, -15.7538) chord 29.5000 | lee[8] (15.8915, -28.1140) luff[8] (12.8541, -26.2486) panel[8] (13.8564, -26.8641)
        lee[4] (8.8616, -31.2100) lee[12] (21.0899, -22.9619) | seams panel[2] (3.7694, -31.4402) -> lee[5] (10.7908, -30.6287) panel[6] (10.6976, -28.7473) -> lee[9] (17.3628, -27.0187) panel[10] (16.8118, -24.6232) -> lee[13] (22.1033, -21.3526) | max r 32.4740 at lee[5]
   |AWA| 116.0000 lee -1.0000: sheet -52.2000 ok 1 | tack (0.0000, -32.2500) clew (-24.0602, -13.5870) chord 30.4500 | lee[8] (-16.0843, -26.9031) luff[8] (-12.8283, -25.1896) panel[8] (-13.9028, -25.7550)
        lee[4] (-9.0557, -30.5727) lee[12] (-21.0858, -21.2412) | seams panel[2] (-3.8268, -31.1581) -> lee[5] (-11.0029, -29.8420) panel[6] (-10.7782, -27.9106) -> lee[9] (-17.5247, -25.6744) panel[10] (-16.7933, -23.2449) -> lee[13] (-22.0194, -19.5144) | max r 32.2500 at lee[0]

==== L4 kites in pixels: (CX + x BS, CY + y BS); L2 kite at rot 0 with vertices rounded floor(v + 0.5); gennaker tack row floor(y), hull tip vertex truncated
   4-inch CX 240.0000 CY 226.0000 BS 3.8000 R_INNER 148.0000 | rot 0: tack px (167.8000, 180.4000) -> (168, 180), crest px (240.0000, 102.6900) -> (240, 103)
   4-inch gennaker tack px (240.0000, 103.4500), pixel row 103 | hull tip vertex row 104 (= -32.1053 units), outline front row 103
   7B/5B  CX 300.0000 CY 282.5000 BS 4.7500 R_INNER 185.0000 | rot 0: tack px (209.7500, 225.5000) -> (210, 226), crest px (300.0000, 128.3625) -> (300, 128)
   7B/5B  gennaker tack px (300.0000, 129.3125), pixel row 129 | hull tip vertex row 130 (= -32.1053 units), outline front row 129

==== M1 kite tones: mix(fg, bg, m) = (fg m + bg (255 - m)) / 255; 8-bit ideal and the device's RGB565
   dark  ORANGE #FF8A3D  C_INNER #081521
      gores p1 = mix(O,I,215)                  #D87839   RGB565 0xD3A6
      gores p2 = mix(mix(O,I,140),I,215)       #7B4B2E   RGB565 0x7244
      balloon band 0 = mix(O,I,140)            #905530   RGB565 0x8AA5
      balloon band 1 = mix(O,I,168)            #AB6233   RGB565 0xA305
      balloon band 2 = mix(O,I,197)            #C76F37   RGB565 0xC366
      balloon band 3 = mix(O,I,226)            #E37D3A   RGB565 0xDBC6
      balloon band 4 = mix(O,I,255)            #FF8A3D   RGB565 0xFC47
      balloon seam = mix(O,I,100)              #69432C   RGB565 0x6A05
      gennaker luff/seams c2 = mix(O,I,140)    #905530   RGB565 0x8AA5
   light ORANGE #D2691E  C_INNER #EEF2F6
      gores p1 = mix(O,I,215)                  #D67E40   RGB565 0xD3E7
      gores p2 = mix(mix(O,I,140),I,215)       #E1B392   RGB565 0xDD70
      balloon band 0 = mix(O,I,140)            #DFA77F   RGB565 0xDD0E
      balloon band 1 = mix(O,I,168)            #DC9868   RGB565 0xDCAC
      balloon band 2 = mix(O,I,197)            #D8884F   RGB565 0xD428
      balloon band 3 = mix(O,I,226)            #D57937   RGB565 0xD3C6
      balloon band 4 = mix(O,I,255)            #D2691E   RGB565 0xD343
      balloon seam = mix(O,I,100)              #E3BCA1   RGB565 0xDDD3
      gennaker luff/seams c2 = mix(O,I,140)    #DFA77F   RGB565 0xDD0E
   night ORANGE #C25E2A  C_INNER #120404
      gores p1 = mix(O,I,215)                  #A65024   RGB565 0xA264
      gores p2 = mix(mix(O,I,140),I,215)       #642D16   RGB565 0x6161
      balloon band 0 = mix(O,I,140)            #733519   RGB565 0x71A2
      balloon band 1 = mix(O,I,168)            #863F1D   RGB565 0x81E3
      balloon band 2 = mix(O,I,197)            #9A4A21   RGB565 0x9A43
      balloon band 3 = mix(O,I,226)            #AE5426   RGB565 0xAA84
      balloon band 4 = mix(O,I,255)            #C25E2A   RGB565 0xC2E5
      balloon seam = mix(O,I,100)              #572713   RGB565 0x5122
      gennaker luff/seams c2 = mix(O,I,140)    #733519   RGB565 0x71A2
```

### 7.3 Check programs and how to run them

Everything this document relies on that can be run. The two programs are built
from the repository root with any C++11 compiler (g++ 15 here; no LVGL). They
link `WindCalc.cpp`, `WindDampCfg.h` and `SpiGeom.cpp` from the repository;
everything else they check is a replica kept by hand (the parts of
`WindScreen.cpp`, `CanvasDraw.cpp`, `Config.cpp`, `N2kHandler.cpp`,
`data/index.html` and LVGL listed in 7.1 and 5.7), which does not follow a
change to the code by itself and has to be kept in step with it. Re-run both
after a change to any file they link or restate (7.1, 5.7), updating the
replica first where one is affected. Their binaries and `wind_vectors_out.txt`
land in the repository root; `.gitignore` keeps them out of git.

- `docs/wind_vectors.cpp` - generator of 7.2: links `WindCalc.cpp`,
  `WindDampCfg.h` and `SpiGeom.cpp`, restates the rest (7.1). Build with
  `g++ -std=gnu++11 -O2 -Wall -Wextra -Wshadow -I src/nmea -I src/config -I src/display/screens docs/wind_vectors.cpp src/nmea/WindCalc.cpp src/display/screens/SpiGeom.cpp -o wind_vectors`,
  run `./wind_vectors > wind_vectors_out.txt`; the output must equal 7.2 line
  for line (paste it in again after a change). Do not write it to
  `docs/vectors_out.txt`: that file is the output of `docs/vectors.py`, the
  reference encoder of `docs/N2K-UPDATE.md`, and has nothing to do with this
  document.
- `docs/downwind_check.cpp` - the downwind sail's clearance checker (5.6, 5.7):
  build with `g++ -std=gnu++11 -O2 -I src/display/screens docs/downwind_check.cpp src/display/screens/SpiGeom.cpp -o downwind_check`,
  run `./downwind_check docs/downwind_vectors.txt`. Every marked target must
  read `[OK]` (the pole angle over |AWA| 0..180 reads `[OK, non-decreasing]`),
  except the dead-run pole angle, which reads `[see note]` (5.8); no line may
  read `[FAIL]`. Two checks carry no mark. The line "gennaker
  sheeting angle non-decreasing in |AWA|" must read `yes`. In the reef-badge
  block (one per board) the guy, keel, sheet, kite and pole must stay positive,
  each hidden in 0 states; the mainsail and the boom spar may show a negative
  distance ("badge hides part of it in N states"), but only where the boat is
  faster than the wind (5.8), so the line must end with the same N twice
  ("boat faster than the wind in N of them"; today N = 18).
- `docs/downwind_vectors.txt` - written by that run, not by hand (5.7): the
  complete `SpiGeom` outlines a port compares against.
- The PC simulator (documented here, not run for this record):
  `pio run -e simulator` (4-inch) or `pio run -e simulator_7b` (1024x600), then
  `.pio/build/simulator/program.exe` (or `simulator_7b`); flags
  `--light` / `--dark` / `--night`, see the README, "PC simulator". The
  downwind sail comes from the configuration: `data/config.json` (factory
  value `balloon`) or `--cfg <file>` with `"sail": {"spi_type": ...}`; the
  simulator turns `none` into `balloon` (`sim/sim_main.cpp`), so the case
  without a kite cannot be shown there - check it on a device. It shows
  the Wind & Trim screen only as the demo scenario drives it
  (`src/nmea/DemoData.cpp`): |TWA| sweeps from 20 to 165 deg and back on each
  tack, TWS 8 to 18 kn, and the boat tacks at 20 deg, so it never reaches 178
  and never gybes. No flag sets a wind angle (`--set` knows rudder, days,
  depth, roll, pitch, rot, depthhist, windhist, anchortrack and anchor);
  `--freeze-demo T` only stops the scenario at T seconds. Of the visual check
  of step 12 in section 8 it covers |TWA| 100, 135, 150 and 165 at the moments
  the sweep passes them, with whatever TWS and STW the scenario has then; not
  178 and not the gybes.

Not in the repository, so the results that rest on them cannot be re-run from
it: the JavaScript and Python ports and `compare.py` of 7.1; the separate
harnesses listed at the end of 5.7; the pixel counter behind the memory traffic
per frame in 5.5 (`sim/sim_main.cpp` has none); and whatever set the wind
angles for the simulator checks of 4.8 (the keel) and 4.9 (the legibility
shots), since the simulator alone cannot be set to them (above).

---

## 8. Porting checklist for NauticPi

Build in this order; each step is checked before the next one starts.

1. **Clock and NaN** (1.4, 1.5). One monotonic millisecond clock, "0 = never",
   `fresh()` with "age >= window is stale", and NaN for "not known" everywhere a
   value can be missing.
2. **WindCalc core in C#** (server): `norm180`, `norm360`, `apparentToTrue`,
   `trueToApparent`, `lowPassAlpha`, the scalar and the angle low-pass. Check
   against A1, B1, B2, F1-F5.
3. **Pipeline in C#** (2.2-2.9): boat speed, true heading, true course, TWD
   sources, the COG latch, received true wind with ranking, `updateTrue`,
   `expire`, `reset`. Feed it from NauticPi's N2K input exactly as 2.1 says
   (apparent ref 2 with `aws_fac` and `awa_off`; refs 4/3/0 uncalibrated; ref 1
   ignored; 127250 with `hdg_off`, reference and deviation per reading;
   127258; 128259 with `stw_fac`; 129026 with "COG N/A clears the stamp"). Call
   `expire` from a timer at least every few seconds even when no frame
   arrives. Check against C1-C4, D1-D3, E1-E2, F6-F7. All PGNs involved are
   single-frame, so a bus gateway that drops fast-packet PGNs does not affect
   them.
   Whatever NauticPi does with received true wind today is replaced by 2.7: the
   apparent wind wins.
4. **1 Hz wind history** (2.10) if NauticPi keeps one: TWD/TWS only when both
   are known, slots counted from the previous slot.
5. **Damping config** (3.3-3.5, 6.1): the four keys with the migration of
   `apply()` (check G1), clamping 0..30, default 2, `twd_cog` default false;
   read on every sample so a save applies at once; limits served to the web
   page; the Devices-tab fields with the texts of 6.3.
6. **Sail state in JavaScript** (client): `computeSailState` (4.2), the frame
   preamble (4.1) and the lee latch (4.3). Check against I1-I6 and J1. The
   window MUST be decided from one read of `spi_type` per frame.
7. **Smoothing** (4.5, 4.6): the four smoothed trims, updated every frame,
   the gennaker's no-swing rule, `resetRig` on show and after a 2 s paint gap.
   Check K1-K2 with k = 0.18 per step, then switch to the time-based k for
   NauticPi's frame rate.
8. **SpiGeom in JavaScript**: copy `SpiGeom.h/.cpp` line by line (5.2-5.4).
   Check L1-L3 (boat units) and the full outlines in `docs/downwind_vectors.txt`
   (5.7). If NauticPi draws in BoatPainter units, multiply
   by 3 (5.1) when mapping to the canvas, not inside SpiGeom, so L1-L3 still
   compare 1:1.
9. **Rig re-check** (5.6): compare NauticPi's main (mast, boom length, boom law
   `clamp(|AWA| - 13, 4, 86)`, foil) and hull with 4.7/4.8 after the factor 3,
   using I7. If anything differs, replicate NauticPi's rig in
   `docs/downwind_check.cpp` and run it (5.7, 7.3); all targets must still pass.
10. **Pixel layer** (5.5, 4.10): polygons and lines in the order given, tones as
    `orange` mixed into NauticPi's inner background (M1 shows the expected
    result for the Pinnace defaults), opaque bands/panels, the 2 px leading edge,
    pole, sheet and guy with the side rules of 4.6. Draw order 4.9, with the
    compass card's letters drawn over the boat (with their halo over a kite;
    re-measure its sector with NauticPi's font) and its ticks under it, and
    the "Butterfly" label over the letters; the reef badge moves to the
    cockpit while a kite is drawn (4.11). Check the
    gennaker's tack against NauticPi's own stem tip (5.4): it belongs on the
    tip of the drawn hull outline, not at the jib's tack point.
11. **Config and web interface** (6.1-6.3): `sail.spi_type` with the four
    strings, default `balloon`, and the load rules of 6.2. If NauticPi already
    has a spinnaker on/off setting, it plays the part of `sail.spinnaker`:
    read as the fallback, written beside the new key for a downgrade. The
    select with the four options and the hint; preselect with `spiTypeOf`.
    Check H1.
12. **Visual check** in light, dark and night against the Pinnace simulator
    (`simulator` / `simulator_7b`) at |TWA| 100, 135, 150, 165, 178 on both
    sides for each type, and through a gybe at 150 and at 178. The simulator's
    demo passes 100 to 165 only in its sweep and reaches neither 178 nor a
    gybe (7.3).
