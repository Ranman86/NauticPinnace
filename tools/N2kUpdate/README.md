# N2kUpdate – sender for updates over NMEA 2000

The sender side of [docs/N2K-UPDATE.md](../../docs/N2K-UPDATE.md) (protocol version 1,
revision 2): one C# library that the standalone Windows updater and, later, NauticPi
use, plus a command-line tool, a scripted display for tests, and the tests. MIT, like
the rest of the repository. Only the .NET 10 SDK and `System.IO.Ports` are needed.
On the bus side: a CANable USB-CAN adapter with slcan firmware at 250 kbit/s -
recommended: the **Makerbase CANable V2.0 Pro**. Updates over NMEA 2000 are not
supported through an Actisense NGT-1: the protocol needs an adapter that can send
long fast-packet bursts, and with an NGT-1 that could not be achieved in testing.

| Project | Target | Purpose |
|---|---|---|
| `src/NauticPinnace.N2kUpdate` | net10.0 | the library (section 12); no Windows-only API, runs on the Raspberry Pi |
| `src/NauticPinnace.Updater` | net10.0-windows | the standalone updater (WinForms, German/English) |
| `src/n2kupd` | net10.0 | command line: discover, identify, update, bridges, frame recorder |
| `src/NauticPinnace.N2kUpdate.Testing` | net10.0 | virtual CAN bus, scripted display, slcan TCP/stdio server |
| `src/Shared` | (linked) | Windows timing and keep-awake, CANable port search, updater settings |
| `tests/NauticPinnace.N2kUpdate.Tests` | net10.0 | console test runner (no test package) |

```
dotnet build tools/N2kUpdate/N2kUpdate.slnx
dotnet run --project tools/N2kUpdate/tests/NauticPinnace.N2kUpdate.Tests              # all tests, about 20 s
dotnet run --project tools/N2kUpdate/tests/NauticPinnace.N2kUpdate.Tests -- realtime  # plus the 60 s wall-clock pacing test
```

The tests cover every Appendix A vector byte for byte (frames and on-wire bit counts),
codec round trips, fast-packet framing and reassembly, the rate math, token bucket,
drain estimate, adaptive back-off and BELL handling, image and release checks (with the
real v1.2.0 images when `release/` is present), the deflate cache, the address claim,
and the sender state machine against the scripted display: every outcome of 5.7 and
the rows of 5.8 (lost frames, lost ACK, lost OFFER_REPLY, HOLD, RESUME after a sender
restart, adapter unplugged, address lost, display restart, foreign transfer,
small-buffer adapter, two displays, Identify, legacy discovery).

## Library

The layers of 12.2:

| Layer | Type |
|---|---|
| `ICanFrameLink` | `SlcanFrameLink` (CANable on a serial port; slcan text over TCP, a TCP listener or a child process), `SocketCanFrameLink` (Linux can0; not yet run on hardware), `DryFrameLink` (no hardware, logs every frame, loopback hook), `RecordingFrameLink` (logs any link) |
| `ISourceAddress` | `AddressClaimer` (12.3); or the host's own claim, passed as `sharedSource` (NauticPi in slcan mode: the engine then claims nothing itself) |
| `FastPacket` | `FastPacket`, `FastPacketSequencer`, `FastPacketAssembler` |
| `Messages` | `Protocol/Messages.cs`, constants in `Protocol/Constants.cs` |
| `BusLoadLimiter` | `Bus/BusLoadLimiter.cs` (6.2-6.4) |
| `FirmwareImage` / `WebUiImage` | `Images/` (8.2, 8.6), `TransferImage` + `DeflateCache` (12.4), `ReleasePackage`, `LittleFsReader` |
| `UpdateSession` | `Session/UpdateSession.cs` (5.10), `UpdateJob` (firmware, then the companion web UI) |
| `N2kUpdateClient` | `DiscoverAsync`, `IdentifyAsync`, `UpdateAsync` |

Everything runs on one engine thread (`N2kEngine.Step()` returns the next wake-up
time), so the same code runs in wall-clock time (`EngineHost`) and in virtual time in
the tests. Pacing waits go through an injected `IHostWaiter`; the Windows hosts inject
the high-resolution waitable timer of 6.6 (`src/Shared/WindowsHostTiming.cs`), Linux
uses the default waiter.

Minimal use (NauticPi):

```csharp
var link = new SlcanFrameLink("/dev/ttyACM0");
var client = new N2kUpdateClient(link, new EngineOptions { UniqueNumber = storedRandom21Bits, PreferredAddress = 231 });
await client.StartAsync();
var found = await client.DiscoverAsync();
var release = ReleasePackage.Load("/var/lib/nauticpi/releases/NauticPinnace-v1.3.0.zip");
var d = found.Displays[0];
var bp = release.For(d.Board)!;
var report = await client.UpdateAsync(d, new UpdateRequest { Firmware = bp.Firmware, WebUi = bp.WebUi },
    new UpdateOptions { SenderName = "NauticPi", SenderKind = SenderKind.NauticPi, CacheDirectory = "/var/lib/nauticpi/n2kupd-cache" },
    new Progress<UpdateProgress>(p => ...));
```

While `UpdateProgress.MustStayConnected` is true (from APPLY mode 0 until the display
has validated the new firmware), cancelling stops the reception proof and the display
rolls back (H5); hosts warn first.

`UpdateOutcome.Note` says, as a code, what the sender itself found (display restarted,
no RESULT, adapter gone, legacy image, ...); `Message` is the English text for logs. A
UI puts the note into its own words; a display reason is in `Reason` (table 4.8).

NauticPi on a shared CANable (12.3, 12.6): pass its own claim as `sharedSource`
(`ISourceAddress`) and report its own frames with `Engine.NoteExternalTransmit`, so they
count towards the total load of 6.3 (the adapter does not echo them).

**RESUME only for an own session.** A sender offers RESUME only when the display's
INFO shows a held session whose id this sender opened for exactly this stream. The
record is kept per engine and, with `CacheDirectory`, in `sessions.txt` next to the
deflate cache, so a restarted run resumes its own session; a second sender with the
same release gets BUSY (5.8), and a run without the cache offers anew (5.3.5). RECOVER
never sends RESUME before the display has answered INFO: a display that is starting
up would refuse it, or open a new dialog for it.

## Windows updater

`NauticPinnaceUpdater.exe`: choose the CANable (found by its USB id 16D0:117E), connect,
the displays are listed (address, board, name, version, state, accept flag, web UI
version, last six hex digits of the unique id). Identify shows the identify screen of
the selected display. Install takes the checked displays one after another, asks once
on the PC, and each display asks on its touchscreen; firmware and web UI of a release
are confirmed there together (5.3.6). Bus share 10-60 % (default 30), back-off, deflate
and the APPLY mode are options. During the validation wait the window shows "Nicht
schließen …" and asks before closing. The laptop does not sleep during a session.

- Settings: `%APPDATA%\NauticPinnaceUpdater\settings.json` (the NAME's unique number is
  drawn once and kept).
- Deflate cache: `%LOCALAPPDATA%\NauticPinnaceUpdater\cache\` (`<sha256>-dotnet10-deflate-smallest.deflate`:
  a header with magic, stream length and SHA-256, then the stream; a file that does not
  match its header is rebuilt), and `sessions.txt`, the sessions this updater opened.
- Log file: `%LOCALAPPDATA%\NauticPinnaceUpdater\logs\updater-yyyyMMdd.log`.
- Test hook: with `N2KUPD_BRIDGE=tcp:127.0.0.1:5577` the port list offers that slcan
  TCP endpoint, for example `n2kupd sim-display --listen 5577`.

### Where the firmware and the web UI come from

A release folder or ZIP with the subfolders `4/`, `7b/`, `5b/` (the release layout), a
single board folder, a PlatformIO build folder, or single files. Per board folder the
web UI is taken from, first match wins:

1. `index.html.gz` in the board folder (the layout `make_release.py` produces from 11.6 on);
2. `littlefs.bin` in the board folder: `index.html.gz` is read out of the LittleFS image
   (release ZIPs up to v1.2.0 carry the web UI only there; `LittleFsReader` is a small
   read-only LittleFS v2 reader, checked against the v1.2.0 images);
3. `index.html.gz` in the chosen folder;
4. `data/index.html.gz` below the chosen folder (a repository checkout).

The web UI's version is the firmware version of the same folder (12.4). For a loose
web UI file without a firmware, the version has to be typed in; without it the file
is not sent, unless "Versionsprüfung übergehen" is ticked (ALLOW_VERSION_MISMATCH,
the OFFER then carries the display's running version).

While no update runs, a CANable that was unplugged and plugged in again is reopened by
the next search or Identify; until then the window shows the adapter as lost. After a
reopen the adapter is looked up by its USB id only - never by the remembered COM name,
which may belong to a display's ESP32-S3 by then (DTR/RTS would reset it).

## Command line

```
n2kupd discover --link slcan:auto
n2kupd update   --link slcan:COM7 --release NauticPinnace-v1.3.0.zip [--uid 02:00:00:00:00:01 | --all]
n2kupd update   --link tcp:127.0.0.1:5577 --firmware firmware.bin --webui index.html.gz --record run.canlog
n2kupd sim-display --listen 5577          # the scripted display as an slcan adapter on TCP
n2kupd record --out frames/               # frame logs of 14.3.2, see below
```

Link specs: `slcan:<port>|auto`, `tcp:<host>:<port>` (we connect), `tcp-listen:<port>`
(we accept one connection), `stdio:<program> [args]` (slcan on the child's stdin and
stdout; quote a program path with spaces), `sim` (scripted display in virtual time).
A port that is not identified as a CANable is opened without DTR/RTS, because toggling
them resets an ESP32-S3.

## Cross test (14.3)

**Bridge.** All bridges speak the slcan text of 6.4: the sender writes `C`, `S5`, `O`
once, then `T<id8><dlc><data>\r` per frame, a whole DATA message in one write; the peer
answers each line with `\r` (or BELL 0x07 when it drops a frame, which the sender
treats like the CANable's BELL) and sends received frames as `T` lines. The C++
`n2kupd_hostsim` listens on TCP and the sender connects with `--link tcp:host:port`;
or the sender starts it with `--link "stdio:<path> <args>"`. The library side is
`SlcanFrameLink.ForTcp / ForTcpListen / ForProcess`; `n2kupd sim-display` is the C#
reference peer for both transports.

**Frame logs.** `n2kupd record --out <dir>` runs the sender against the scripted
display on a virtual bus at 250 kbit/s and writes, per scenario, `<name>.canlog`,
`<name>.meta.json` (addresses, images, SHA-256, stream CRC, display timing, outcome,
the display's event timeline and the sender's log) and the images under `images/`:

| Scenario | Content |
|---|---|
| `fw_deflate` | firmware, deflate, APPLY now, restart, polls until validated |
| `webui` | web UI alone, with its own dialog |
| `release_fw_webui` | firmware with the companion web UI: one dialog, web UI without a second |
| `fw_raw_loss` | raw firmware, frames lost on the wire, bitmap recovery, verify only |

Line format: `t_ms id29 dlc hex` with integer milliseconds of virtual time at the end of
the frame on the wire, both directions in one file (the source address in the
identifier tells them apart), line order = bus order, no comments. Without
`--firmware` the recorder makes a test image from `release/NauticPinnace-v1.2.0/7b/firmware.bin`
with a 1.3.1 descriptor and fixed checksums (`n2kupd make-test-image`); such an image
passes every structural check but does not run: never flash it.

## Notes on the specification

Readings chosen where the specification leaves room; each is the safer one.

- **5.7 / 4.5 restart pending during the countdown.** INFO flag 2 ("activated but not
  running yet") is also true during the 10 s countdown. The sender reports
  `InstalledRestartPostponed` only if the flag is still reported 25 s after APPLY with
  no alarm active, or at the 5-minute limit if the display still answers with it; a
  display that went quiet is `Unknown`.
- **6.5 foreign transfer longer than 60 s.** The sender pauses while another source's
  DATA was seen within 10 s, keeps its session alive with QUERY(ACK) every 10 s, and
  after 60 s of pause ends with ABORT(SENDER_CANCELLED) and outcome Failed (BUSY).
- **12.3 address in use at start.** An address known to be in use is skipped rather
  than contested, so the updater never pushes an instrument off its address.
- **14.4 H3 "≤ 31 % in every 1 s window".** The normative bucket of 6.2 (5,120 bits)
  and the 32-frame bursts allow up to (75,000 + 5,120) / 250,000 = 32.05 % in a single
  1 s window at 30 %; the long-run share stays at 30.0 % (measured over 60 s). The
  tests assert the 6.2 bound.
- **14.2 xUnit.** The tests are a plain console runner, to stay free of packages.
- **ApplyMode None** ends with the outcome `Verified`, an addition to the list in 12.2.
- **APPLY mode 2 on a quiet bench bus.** H5 then needs any other N2K traffic; with only
  the updater on the bus the display rolls back (the tests show both cases).
- **5.3.4 RESUME against 5.8 "two senders".** RESUME only for a session the sender
  opened itself (see above); the display adds the 5 s silence rule of 5.3.5 for a
  RESUME from another address. During RECOVER a BUSY answer to the RESUME is retried
  (a sender that lost its address comes back from a new one within those 5 s).
- **5.7 T_apply without RESULT(phase 3).** When no RESULT came, T_apply is the first
  APPLY, not the moment the sender gave up: a restart in between still counts.
- **6.4 stop-and-wait** holds for the rest of the session; the next session's OFFER
  probes the adapter again.
- **6.4 SocketCAN txqueuelen.** `SocketCanFrameLink.Open` refuses an interface whose
  `tx_queue_len` is below 64.
