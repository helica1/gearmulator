# Plan: WAV sample import for the Machinedrum UW plugin

Two features, built on the existing SysEx transfer engine:

- **(a) WAV import from the menu**: right-click menu entry "Load Sample (WAV)...", pick one or more WAV files, pick the target RAM slot, convert to SDS, send.
- **(b) Drag and drop**: drop WAV files from Finder or from Bitwig (clip/browser drags arrive as files) onto the plugin panel; same conversion and transfer.

Everything below was verified against the code on branch `md-mm-remote-panel`
(repo `~/Desktop/5300/gearmulator-md-mm`, remote `fork` =
helica1/gearmulator). Build with
`export DEVELOPER_DIR=/Library/Developer/CommandLineTools; ninja -C build_plugin mdJucePlugin_All mdJucePlugin_CLAP`,
bundles land in `bin/plugins/Release/{VST3,CLAP,AU}/Gearmulator MD.*`, install by copying to
`~/Library/Audio/Plug-Ins/{VST3,CLAP,Components}/`. Do not use `timeout` on this Mac, use
`perl -e 'alarm N; exec @ARGV' <cmd>`. Never leave a `__pycache__` in `mdJucePlugin/remote/`, the asset
glob picks it up and the binary-data step crashes.

## 1. What already exists (reuse, do not rewrite)

### Transfer engine (mdLib)
- `mdLib/mdsysexfile.h`: `parseMidiSysexFile()` / `prepareMidiSysexTransfer(bytes, model, &validation)` validate a
  byte stream of complete SysEx messages and classify them (`MidiSysexMessageKind::SdsHeader`, `SdsPacket`,
  `SampleName`, ...). The SDS rules it enforces (lines ~60-140) are the spec for the bytes we must generate:
  - header `F0 7E <dev> 01 <slotLo> <slotHi> <bits> <period 3x7bit> <words 3x7bit> <loopStart 3x7bit> <loopEnd 3x7bit> <loopType> F7`,
    exactly 21 bytes; `bits` in 8..28; `period` (ns) non-zero; `words` non-zero;
    `loopType` 0, 1 or 0x7F (0x7F = no loop, then loop points are ignored); with a loop, `loopStart <= loopEnd <= words`.
  - data packets `F0 7E <dev> 02 <packet# & 0x7F> <120 data bytes> <checksum> F7`, exactly 127 bytes;
    checksum = XOR of bytes 1..124 (everything between F0 and the checksum). Words are big-endian 7-bit groups,
    `bytesPerWord = (bits + 6) / 7`, `wordsPerPacket = 120 / bytesPerWord` (16-bit: 3 bytes per word, 40 words per packet),
    left-justified: for 16-bit the sample is shifted into bits 20..5 of a 21-bit field (see `sdsTestData.h`,
    `word << 16` for 12-bit values into a 28-bit field with 4 bytes per word; for 16-bit use `(uint32_t(sample16) << 5)` in a
    21-bit field, 3 bytes: `(w >> 14) & 0x7F, (w >> 7) & 0x7F, w & 0x7F`). Unused words at the end of the last packet are 0.
  - Elektron sample name message directly after the header (before the first packet):
    `F0 00 20 3C 02 00 73 <slot 0..47> <4 name chars> F7`, exactly 13 bytes. Name is 4 characters, 7-bit ASCII.
  - The receiver is always device 0; `PreparedMidiSysexTransfer` retargets other device ids (`mdsysextransfer.h:69`).
- `mdLibTest/sdsTestData.h::sdsSample(words, bits, device, slot)` is a working generator of a complete SDS stream
  (header + name + packets) and the best template for the encoder. `mdLibTest/sdsTransferTest.cpp` and
  `sdsFirmwareTest.cpp` show how a transfer is validated and pushed into the machine in tests.
- Transfer state machine and Turbo MIDI: `mdLib/mdsysextransfer.h` (`PreparedMidiSysexTransfer`,
  `MidiSysexTransferProgressPublisher`), driven through `md::Device`:
  `beginUserSysexImport()` -> ticket, `startUserSysexImport(ticket, transfer, receiveModeConfirmed)`,
  `userSysexImportProgress()`, `cancelUserSysexImport`, `resumeUserSysexImport`, `retireUserSysexImport`
  (`mdLib/mddevice.h:149-156`).

### Editor flow (mdJucePlugin)
- Menu: `mdPluginEditorState.cpp:93-112` builds the "SysEx transfer" entries (send, resume, cancel) and calls
  `Editor::chooseUserSysexFile()`.
- `mdEditor.cpp`:
  - `chooseUserSysexFile()` (~line 1556): guards (`m_sysexChooserOpen`, `isUserSysexTransferActive()`), takes a ticket
    via `device->beginUserSysexImport()` under `getProcessor().getPlugin().withDeviceLocked(...)`, then
    `launchUserSysexFileChooser(ticket)`.
  - `launchUserSysexFileChooser()` (~1584): `juce::FileChooser` for `*.syx`, async, then `sendUserSysexFile(file, ticket)`.
  - `sendUserSysexFile()` (~1607): reads the file, `prepareMidiSysexTransfer`, and if the stream contains an
    `SdsHeader` shows the "Is Machinedrum ready to receive samples?" yes/no box, then
    `startUserSysexTransfer(transfer, file, ticket, receiveModeConfirmed)`.
  - `startUserSysexTransfer()` (~1667): `device->startUserSysexImport(...)` and maps `SysexImportStartResult`
    (Started, StaleRequest, Restoring, NotReady, Initializing) to error dialogs via `showUserSysexError()`.
  - Progress/resume/cancel UI already exists (`getUserSysexProgress`, `resumeUserSysexTransfer`,
    `cancelUserSysexTransfer`); it keys on the ticket, so anything that ends in `startUserSysexTransfer` gets it for free.
- Confirmation dialogs: `genericUI::MessageBox::showYesNo(icon, header, message, callback)`,
  `showOk(...)` (`juceUiLib/messageBox.h`). Editor lifetime guard pattern:
  `const std::weak_ptr<void> lifetime = m_lifetimeToken;` then `if(lifetime.expired()) return;` in callbacks.

### Drag and drop infrastructure (juceRmlUi)
- `juceRmlUi::RmlComponent` is a `juce::FileDragAndDropTarget`; `RmlDrag::isInterestedInFileDrag` says yes to
  everything and turns the OS drag into an RmlUi drag, so **RML elements decide**.
- An element accepts files by owning a `juceRmlUi::DragTarget` (`juceRmlUi/rmlDragTarget.h`) and overriding
  `bool canDropFiles(const Rml::Event&, const std::vector<std::string>& files)` and
  `void dropFiles(const Rml::Event&, const juceRmlUi::FileDragData*, const std::vector<std::string>& files)`.
  Working example: `jucePluginEditorLib/partbutton.h/.cpp` (`PartButton::canDropFiles` at line 122,
  `dropFiles` at 159). `DragTarget::init(element)` binds it to an RML element; the pseudo-classes it sets
  (`:drag-over` style hooks via `updatePseudoClass`) can be styled in RCSS for a highlight.
- Skin: `mdJucePlugin/skins/mdDefault/mdDefault.rml` (+ `.rcss`), elements are absolutely positioned `dp` boxes;
  the editor finds them with `findChild("id", false)` (see `Editor::createLcd`, `bindSettingsButton`).

### Machine facts
- Machinedrum UW: 48 RAM sample slots, slot index 0..47 in SDS "sample number" (low byte) and in the name message.
  ROM slots are not writable through SDS (the validator rejects name slot > 47).
- Sample memory 2.5 MB shared by all RAM slots; mono; the receiver stores 12 significant bits (test data uses
  "exactly representable 12-bit values"). Send 16-bit anyway, the machine truncates.
- Native sample rate: **unknown for certain**. `sdsTestData.h` uses a 31250 ns period (32 kHz) with the comment
  "avoids receiver resampling", which suggests the sample RAM runs at 32 kHz while the DSP runs at 44.1 kHz.
  Task 2.1 below settles this empirically before choosing the default conversion rate.
- The machine must be idle (booted, not CLEANING/LOADING) and ideally the sequencer stopped; the existing
  confirmation dialog text covers that and `startUserSysexImport` refuses while booting/restoring/initialising.
- Uploaded samples live in the emulated flash/sample RAM and are part of the plugin state, so they persist with the
  project like everything else.

## 2. Work items

### 2.1 Determine the native sample rate (half a day, do first)
Use `build/source/elektron/md/mdLibTest/mdAudioCaptureTool` (source in `mdLibTest/mdAudioCaptureTool.cpp`) as a
template for a small headless experiment:
1. Generate a 1 kHz sine SDS stream (reuse `sdsSample` shape, 16-bit) with period 22675 ns (44.1 kHz) into slot 0,
   and another with 31250 ns (32 kHz).
2. Boot the MD (stock image + factory cache `~/Documents/Gearmulator Preview/Machinedrum/nvram/md-uw-1.63-factory-v2.cache`),
   send the stream with `hardware.sendMidi(event)` (one `SMidiEvent` per SysEx message, in order; see
   `sdsFirmwareTest.cpp` for pacing and how it waits for the machine to consume), assign a ROM/RAM player
   machine on track 1 to slot 0 (the `mdUwFirmwareTest.cpp` machine picker navigation shows the panel key
   sequence; alternatively X.13's assign-machine SysEx), trigger track 1 (`PanelControl::Trigger1`),
   record the output and measure the pitch.
3. If a 44.1 kHz-period file plays at 1 kHz, convert to 44.1 kHz; if it plays detuned or the machine resamples,
   pick the rate that plays back true. Record the answer in `docs/` and in the code comment of the encoder.

### 2.2 WAV to SDS encoder (mdLib, one day)
New files `mdLib/mdsdsencode.h/.cpp` (pure C++, no JUCE, so it is testable in mdLibTest):
```cpp
namespace md::sds
{
    struct Options { uint8_t slot = 0; std::string name; /* max 4 chars */ uint32_t sampleRateHz = 44100; bool loop = false; uint32_t loopStart = 0, loopEnd = 0; };
    // mono 16-bit samples already at the target rate
    std::vector<uint8_t> encode(const std::vector<int16_t>& samples, const Options& options);
    size_t maxWordsForFreeMemory(...); // optional, see 2.5
}
```
- Output: header (21 bytes) + name message (13 bytes) + packets (127 bytes each), exactly as validated by
  `parseMidiSysexFile`. Device id 0.
- Name: uppercase, pad with spaces to 4, only 0x20..0x7E.
- Unit test `mdLibTest/sdsEncodeTest.cpp`: encode a ramp, run `validateMidiSysexStream(bytes, Machinedrum) == Valid`,
  decode packets back and compare to the input; register it in `mdLibTest/CMakeLists.txt` like `mdSdsTransferTest`.

### 2.3 Audio decode and conversion (mdJucePlugin, half a day)
New `mdJucePlugin/mdSampleImport.h/.cpp`:
- `juce::AudioFormatManager` with `registerBasicFormats()` (WAV, AIFF, FLAC, MP3/AAC via CoreAudio on macOS).
- Read with `AudioFormatReader`, sum channels to mono (average, not sum, to avoid clipping), resample with
  `juce::LagrangeInterpolator` (or `juce::ResamplingAudioSource`) to the rate from 2.1, convert to int16 with
  clipping, optionally trim leading/trailing silence (off by default), normalise off by default.
- Enforce limits before encoding: max length = 2.5 MB / 2 bytes per sample minus what is already used
  (see 2.5), reject files over the limit with a clear message ("32.1 s at 44.1 kHz exceeds the 29.7 s of free
  sample memory").
- Returns `std::vector<int16_t>` + suggested name (first 4 characters of the file stem, uppercased).

### 2.4 Menu entry and slot chooser (mdJucePlugin, one day)
- `mdPluginEditorState.cpp` next to the SysEx transfer entries: "Load Sample (WAV)..." -> `Editor::chooseSampleFiles()`.
- `Editor::chooseSampleFiles()`: same guards as `chooseUserSysexFile`, take a ticket with `beginUserSysexImport()`,
  `juce::FileChooser` with `"*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a"`, multi-select allowed
  (`canSelectMultipleItems`). On result -> `importSampleFiles(files, ticket)`.
- Slot chooser: a small RML dialog (or `genericUI` list) showing slots R01..R48 with their current names, default =
  first free slot. For several files, start slot + consecutive. Slot names come from the machine: the existing
  automation layer knows kits, not sample slots; simplest is to query the MD "sample slots" status
  (X.13 readme: status parameter 0x34 replies the slot list; on stock 1.63 the sample manager only shows names on
  the LCD). For stock firmware, fall back to "R01..R48" with no names and remember used slots in the plugin
  config (`sampleSlotNames` json) so the chooser can show what the plugin uploaded itself.
- `importSampleFiles()`: for each file, decode (2.3), encode (2.2), concatenate all SDS streams into one byte
  vector (the transfer engine handles multiple samples in one stream, see the `remainingPackets` loop in the
  validator), then call `prepareMidiSysexTransfer(bytes, model, &validation)` and hand the result to the existing
  `sendUserSysexFile`-equivalent path: factor the tail of `sendUserSysexFile` (from "auto transfer = ..." on) into
  `Editor::startPreparedTransfer(std::shared_ptr<PreparedMidiSysexTransfer>, juce::File, ticket)` and call it.
  Keep the "Is Machinedrum ready to receive samples?" dialog, it is real: the machine must be idle.
- Progress: nothing to do, the existing transfer progress/cancel/resume UI keys on the ticket.
- After completion (`SysexImportProgress` reports done, see how `getUserSysexMenuText()` detects it), show
  "R05 KICK loaded" via `showOk`, and store the name in config.

### 2.5 Free memory check (half a day, optional but recommended)
The MD reports sample slot usage through its sample manager only. Two options:
- Keep a plugin-side ledger of uploaded lengths per slot (config + project chunk), sufficient for samples the
  plugin uploaded itself.
- Or, on X.13, use status request 0x34 (sample slot query) which reports the slots; X.13 also answers SDS dump
  requests, so lengths can be read back. Implement the ledger first, add the X.13 query later.

### 2.6 Drag and drop (one day)
- In `mdDefault.rml` add a full-panel invisible drop layer `<div id="sampleDropTarget" .../>` positioned over the
  whole faceplate but with `pointer-events: none` normally; simpler: attach the `DragTarget` to the existing
  panel root element so any drop on the panel counts.
- New class `mdJucePlugin::SampleDropTarget : juceRmlUi::DragTarget` (pattern: `jucePluginEditorLib/partbutton.h`):
  - `canDropFiles`: true if every file has an audio extension (`AudioFormatManager::findFormatForFileExtension`)
    or is a `.syx`; false otherwise.
  - `dropFiles`: `.syx` -> existing `sendUserSysexFile`-style path (take a ticket first); audio -> same
    `importSampleFiles(files, ticket)` as the menu, with the slot chooser. If the drop lands on a **trig key**
    (`Trigger1..16` elements), preselect that track's current sample slot as the target and offer "assign this
    slot to track N" (assignment is a kit edit; stock 1.63 accepts the SysEx "assign machine" 0x5b command, see
    `mdUwFirmwareTest.cpp:246`, with the ROM/RAM machine id for the slot, and X.13 documents it fully).
  - Highlight: style `#panel:drag-over` (pseudo-class set by `DragTarget::updatePseudoClass`) with a dashed border.
- Bitwig specifics: dragging a clip from Bitwig's arranger exports it as a temporary WAV file path, the browser
  drags real file paths; both arrive through `filesDropped`. Test with Finder first, then Bitwig.
- Register the target in `Editor::create()` after the skin loads (next to `bindSettingsButton()`), keep it in a
  `std::unique_ptr<SampleDropTarget>` member on `Editor`.

### 2.7 Remote panel (optional, later)
The browser panel (`mdJucePlugin/remote/`) can get an HTML file input / drop zone posting the WAV to the plugin over
HTTP (`RemotePanelServer` in `mdLib/mdremotepanel.cpp` serves GET only today; add `POST /sample?slot=n`), then the
same import path runs in the plugin. Not part of (a)/(b).

## 3. Testing
- Unit: `sdsEncodeTest` (round trip + validator), run via `ctest -R sds` in `build`.
- Headless integration: extend `sdsFirmwareTest.cpp` or write `sampleImportFirmwareTest.cpp`: encode a known tone,
  send, assign, trigger, capture with `Hardware::processAudio`, assert pitch and level (this is also the 2.1 experiment).
  Firmware images: `GEARMULATOR_MD_FIRMWARE_BIN=~/Desktop/5300/roms/elektron_sps1-1uw_os1.63.bin`.
- Manual: Bitwig, load a WAV via menu, check the sample manager on the LCD (GLOBAL > FILE > sample manager) shows
  the name in the chosen slot, assign a ROM machine to it, play. Then drop a WAV from Finder and from a Bitwig clip.
- Regression: `mdProjectStateRestoreTest` (needs `GEARMULATOR_MD_FIRMWARE_BIN`, `GEARMULATOR_MM_FIRMWARE_BIN`,
  optional `GEARMULATOR_MD_ALT_FIRMWARE_BIN=~/Desktop/5300/roms/alt/elektron_sps1-1uw_os_x13.bin`),
  `mdUwFirmwareTest`, `mdSdsTransferTest`.

## 4. Pitfalls
- Never write files into `mdJucePlugin/remote/` or the skin folders during tests (asset globs).
- `withDeviceLocked` callbacks run with the audio lock held: no file I/O or decoding inside them; decode and encode
  on the message thread before taking the ticket's start call.
- The transfer engine expects complete messages in order; do not interleave two samples' packets.
- Slot 0..47 only; the validator rejects 48+ and the machine would ignore it.
- Large files: the transfer limit is 8 MiB of SysEx (`g_midiSysexTransferMaxBytes`); 16-bit SDS is 127/40 bytes per
  sample word, so about 2.6 M samples fit, more than the machine's memory anyway.
- Keep the editor alive across async callbacks (`m_lifetimeToken` weak_ptr pattern) and reset `m_sysexChooserOpen`.
