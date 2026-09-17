# Additions in this fork

This branch (`md-mm-remote-panel` on [helica1/gearmulator](https://github.com/helica1/gearmulator/tree/md-mm-remote-panel))
builds on [joelanders/gearmulator-md-mm](https://github.com/joelanders/gearmulator-md-mm), branch
`release/md-mm-alpha` at commit `ffcee15` (September 2026). Everything below was added on top of that base.

| # | Feature | Machinedrum | Monomachine |
|---|---|---|---|
| 1 | [Third-party firmware (alternative OS images)](#1-third-party-firmware) | yes | yes |
| 2 | [Sample management with drag and drop](#2-sample-management) | yes (UW) | `.syx` drop only |
| 3 | [iPad and browser control](#3-ipad-and-browser-control) | yes | yes |
| 4 | [XY multitouch pad](#4-xy-multitouch-pad) | yes | yes |
| 5 | [Direct machine changing](#5-direct-machine-changing) | yes | yes |

Also: [other changes](#other-changes), [building and installing](#building-and-installing).

> **Firmware is not included and must not be shared.** You need your own Machinedrum UW OS 1.63 and Monomachine
> SFX-60 OS 1.32B flash images. Community OS builds come from their own projects (links below).

---

## 1. Third-party firmware

Each plugin instance can run a different OS image: the stock OS or a community build such as

- **Machinedrum OS X.13** and **Monomachine OS X.01A**, shipped with the MegaCommand/MCL releases
  ([jmamma/MCL releases](https://github.com/jmamma/MCL/releases)). According to its release notes X.13 adds the NFX
  neighbour-FX machines (EV, CO, UC), GND-SN-PRO/SW/PU oscillators, INP-CA/CB compressor inputs, tonal tuning for GND,
  TRX and EFM machines, new LFO shapes, chainable trigger groups, faster MIDI handling and an extended SysEx API
  (Enhanced mode) used by the MegaCommand.
- **Monomachine EMS firmware** ([emuyia/ems-monomachine-firmware](https://github.com/emuyia/ems-monomachine-firmware)),
  based on X.01A: per-track length and speed, trig conditions, wrap/chain settings and more.

### Using it

1. Click the **gear icon** at the top left of the faceplate (or right-click the panel and choose Settings).
2. On the GUI page, section **Firmware**, click **Load Firmware Image...** and pick an 8 MiB `.bin` image.
   **Use Stock OS** switches back.
3. Confirm. The machine restarts on the new OS from a fresh factory state.

- The chosen image becomes the **default for new instances** (config key `firmwareImagePath`).
- It is **saved with the project** (state chunk `FWIM`, path plus fingerprint), so a project reopens on the OS it was
  saved with. If the file moved, the plugin finds it again by fingerprint in `roms/` and `roms/alt/` of the data folder
  (`~/Documents/Gearmulator Preview/Machinedrum` or `.../Monomachine` on macOS).
- Kits, patterns and songs of the running machine are **not** carried across an OS switch. Back them up via SysEx first.

### Making an image from an OS upgrade `.syx`

Community OS builds are distributed as "MIDI Upgrade" SysEx files. Splice one into your stock flash image:

```bash
python3 source/elektron/md/tools/elektronOsToFlash.py \
    elektron_sps1-1uw_os1.63.bin Machinedrum_SPS1-UW_OS_X.13.syx elektron_sps1-1uw_os_x13.bin
```

The OS occupies flash from `0x4000`; the boot loader (first 16 KiB) and all data above the OS are kept from the stock
image. Put the result into `roms/alt/` of the data folder.

### How it works

- `RomLoader` accepts the two stock images (by fingerprint) and **any image whose first 16 KiB match the stock boot
  loader**. Automatic discovery still prefers the stock image.
- A non-stock Machinedrum OS gets its own factory flash cache (`nvram/md-uw-<fingerprint>-factory-v2.cache`).
- **Fix for X.13 clicks and aliasing:** the accurate model of the voice-DSP to mixer-DSP link was armed only for the
  exact OS 1.63 fingerprint. On X.13 the legacy link path dropped about a third of the voice data. The model now arms
  for every Machinedrum image with the stock boot loader (it still verifies the link mode at runtime).
- Test hook: `GEARMULATOR_FIRMWARE_IMAGE=/path/image.bin` overrides the configured image without touching the config.

---

## 2. Sample management

Load audio files straight into the Machinedrum UW's 48 RAM sample slots.

### Using it

- **Menu:** right-click the Machinedrum panel, **Load Samples (WAV, AIFF)...**, select one or more files.
- **Drag and drop:** drop audio files from Finder or from your DAW (clips and browser items) anywhere on the panel.
  Dropping a single `.syx` file sends it to either machine.
- A **slot menu** then shows the machine's actual sample directory: name, length and rate of every slot, or `empty`.
  The first free run of slots is pre-selected. Several files go into consecutive slots.
- Choosing slots that already hold samples asks before replacing them. The selection is checked against the machine's
  free sample memory.
- Confirm that the machine is ready to receive; the transfer runs with progress and cancel in the right-click menu.
- Play a slot with the ROM machine of the same number (ROM-06 plays R06), or pick it in the
  [machine rack](#5-direct-machine-changing), whose ROM tiles are labelled with the sample names.

### Conversion

- Channels are averaged to mono and converted to 16 bit.
- The file's sample rate is kept: the Machinedrum plays a sample at the rate in its SDS header (verified: 1 kHz tones
  sent at 44.1 kHz and 32 kHz both play at 1000 Hz). Files above 48 kHz are resampled to 44.1 kHz.
- The slot name is the first four characters of the file name, upper case.
- WAV and AIFF always work. MP3, M4A and CAF are decoded through macOS CoreAudio (JUCE's own FLAC/MP3/Ogg codecs are
  disabled in this build).

### How it works

- `mdLib/mdsdsencode`: 16-bit samples to MIDI Sample Dump Standard messages plus the Elektron sample name message,
  sent through the existing SysEx/Turbo MIDI transfer engine.
- `mdLib/mdsampledirectory` reads the directory from the emulated machine. Layout, identical on OS 1.63 and X.13:
  - flash `0x200000..0x79FFFF` in 64 KiB sectors; a sample starts in a sector whose first byte is `0x18`
    (slot, sample period in ns, length in frames, loop points, 24-byte header), continues in `0x1A` sectors;
    `0x7A` marks a free sector;
  - names in battery-backed RAM at `0x7244A`, 5 bytes per slot (4 characters plus one byte).

---

## 3. iPad and browser control

Each plugin instance serves its front panel to a browser on the local network. Designed for an iPad, works in any
modern browser.

### Using it

1. Load the plugin. It writes its address to `remote-panel-url.txt` in the data folder, for example
   `~/Documents/Gearmulator Preview/Machinedrum/remote-panel-url.txt`.
2. Open that address on the iPad (same network). Defaults: **port 8790** for the Machinedrum, **8792** for the
   Monomachine; if a port is taken, the next free one is used.
3. Optional: Safari's Share menu, **Add to Home Screen**, for a full-screen panel.

### What you get

- The same faceplate as the plugin, live LCD and LEDs, fully bidirectional: the machine, the plugin editor and every
  connected browser stay in sync.
- **Multitouch:** hold several keys at once, for example a trig plus an encoder for parameter locks, or FUNCTION plus a key.
- **Encoders:** drag up/right to increase. Tap to push, hold still then drag to turn while pushed.
- **Sound selection wheel:** turn it around its centre like the jog wheel, one track per 24 degrees.
- **Machinedrum:** pressing a trig key also selects that track (not while FUNCTION is held or grid recording is on);
  tapping a track name in SOUND SELECTION selects it.
- **Monomachine page** with track keys, the EDIT page strip (tap a page name to jump to it), bicolour LEDs.
- **Switch machines** with the MD/MM button at the bottom right or a four-finger sideways swipe outside the XY pad
  (iPadOS multitasking gestures may intercept the swipe; the button always works).
- Keys held from a browser are released if it disconnects.

### Configuration

Plugin config file (`config/Gearmulator MD.xml` or `Gearmulator MM.xml` in the data folder):

| Key | Default | Meaning |
|---|---|---|
| `remotePanelEnabled` | `1` | serve the panel |
| `remotePanelPort` | `8790` / `8792` | first port to try |

A `remote/` folder in the data folder overrides the embedded web files (handy when editing them).

> The panel has no authentication. Use it on a trusted local network only.

### How it works

`mdLib/mdremotepanel` is a small HTTP and WebSocket server in the plugin. It pushes binary state frames
(`'S'`, model, 1024 bytes LCD memory, 14 LED banks) at up to 60 Hz and a JSON machine catalogue (`M <json>`) when it
changes. The page sends text messages: `b <control> 1|0` (keys), `e <encoder> <delta>`, `p <encoder> 1|0`
(encoder push), `t <track>`, `m <machine id>`. Web files: `source/elektron/md/mdJucePlugin/remote/`.

---

## 4. XY multitouch pad

A permanent touch pad above DATA ENTRY on the iPad panel, split into four equal zones labelled **AE, BF, CG, DH**.

- A finger controls the encoder pair of the zone it lands in: **left/right** turns the top encoder (A, B, C or D),
  **up/down** the bottom one (E, F, G or H).
- The finger keeps its pair while it moves, even across zone borders.
- Use up to all four zones at once for eight parameters under four fingers; zones with a finger on them light up.

---

## 5. Direct machine changing

A machine rack below the faceplate, in the plugin window and on the iPad panel.

- **Family tabs:** GND, TRX, EFM, E12, P-I, INP, NFX, MID, CTR, ROM, RAM on the Machinedrum; GND, SWAVE, SID, DPRO,
  FM+, VO, FX on the Monomachine. The last selected family is remembered.
- **Tiles** show every machine of the family. **ROM** tiles show the name of the sample in that slot or `empty`,
  read live from the machine.
- Machines that exist only on community OS builds (NFX, GND-SW/PU, INP-CA/CB) appear when such an OS is running.
- A badge shows the machine's **current track** (for example `TRACK 12 · CC`), polled from the machine.
- Tap a tile, confirm, and the machine is loaded on the current track immediately. The iPad shows a confirmation card
  and a toast; the plugin uses a dialog.
- The track keeps the previous machine's parameter values, as on the hardware.

The plugin window is taller because of the rack (1100 × 766 instead of 1100 × 570 units); lower the GUI scale in
settings if needed.

### How it works

- `mdLib/mdmachines`: machine catalogue with firmware model IDs (Machinedrum IDs from 128 upwards are UW machines),
  ASSIGN MACHINE SysEx (`0x5B`), current track status request (`0x70 0x22`).
- The controller polls the current track every 300 ms; `AudioPluginAudioProcessor::assignMachineToCurrentTrack` sends
  the assignment and refreshes the kit.
- Plugin UI: `mdJucePlugin/mdMachineRack.cpp` and `skins/*/machineRack.rcss`. Browser UI: `remote/rack.js`.

---

## Other changes

- **Gear icon** at the top left of both faceplates opens the settings.
- **DSP56300 core fixes** (submodule branch `md-mm-plus-nord-fixes` on helica1/dsp56300): JIT `cmpm` clobbering A,
  `movep` into program memory skipping block invalidation, vector-area fall-through PC, interpreter DO FOREVER / MOVEP /
  LA handling. Found while emulating the Nord Micro Modular; all MD/MM tests pass with them.
- **DAW sync** works through the existing host MIDI clock (sample-accurate clock, start, continue and stop). On the
  Machinedrum set GLOBAL > SYNC > TEMPO IN to EXTERNAL.
- **Tests:** `mdSdsEncodeTest`, `mdSampleImportTest` (end to end with firmware: decode, transfer, playback pitch),
  firmware selection round trip in `mdProjectStateRestoreTest`.
- **Diagnostic tools** (not part of the plugin):
  - `mdAudioCaptureTool`: render a pattern on any firmware image, with link transport scorecard
    (build with `-DMD_TRANSPORT_DIAGNOSTICS=ON`).
  - `mdLinkTapTest`: record the voice-DSP to mixer-DSP link words (16 tracks, 32-sample blocks on OS 1.63).
  - `mdSampleRateProbe`, `mdSampleDirProbe`, `mdMachineAssignProbe`: the experiments behind sections 2 and 5.
  - `mdSkinSnapshotTool`: render the real Machinedrum editor to PNG with the software renderer.
  - `pluginTester -bpm <x> [-playcycle <s>] [-record out.f32]`: fake host transport and output recording.
  - `GEARMULATOR_MD_MIDI_TRACE=1`: log all MIDI between host and machine.

## Building and installing

macOS, command line tools:

```bash
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cmake -G Ninja -B build_plugin -DCMAKE_BUILD_TYPE=Release \
    -Dgearmulator_SYNTH_ELEKTRON=ON -Dgearmulator_BUILD_JUCEPLUGIN=ON \
    -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=ON -Dgearmulator_BUILD_JUCEPLUGIN_AU=ON .
ninja -C build_plugin mdJucePlugin_All mdJucePlugin_CLAP mmJucePlugin_All mmJucePlugin_CLAP
```

Bundles are written to `bin/plugins/Release/{VST3,CLAP,AU}/Gearmulator MD.*` and `Gearmulator MM.*`. Copy them to
`~/Library/Audio/Plug-Ins/VST3`, `CLAP` and `Components`, then restart the DAW.

Firmware: put the stock images into `~/Documents/Gearmulator Preview/Machinedrum/roms/` and
`~/Documents/Gearmulator Preview/Monomachine/roms/`, alternative OS images into the `alt/` subfolder.

Firmware tests need `GEARMULATOR_MD_FIRMWARE_BIN` and `GEARMULATOR_MM_FIRMWARE_BIN` pointing at the stock images
(and optionally `GEARMULATOR_MD_ALT_FIRMWARE_BIN`, `GEARMULATOR_MD_FACTORY_CACHE`).
