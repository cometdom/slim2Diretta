# Changelog

All notable changes to slim2diretta are documented in this file.

## v1.1.1 (2026-03-11)

### Fixed

- **Seek causes track skip with Roon**: When seeking within a track, Roon (Squeezebox mode) would skip to the next track instead of seeking. The audio thread was sending STMu (track ended) on forced stop (`strm-q`), which Roon interpreted as the new stream ending. Now STMu is only sent on natural end-of-track (HTTP EOF). LMS tolerated the spurious STMu; Roon's stricter state machine did not.

- **High sample rate buffer underruns (>192kHz)**: Adaptive buffer sizing for sample rates above 192kHz (352.8kHz, 384kHz, 768kHz, 1536kHz). LMS streams at ~1x real-time at these rates, leaving no margin with the previous 0.5s ring buffer. New behavior:
  - Ring buffer: 0.5s → 2.0s for rates >192kHz
  - Prebuffer: 500ms → 1500ms for rates >192kHz
  - SDK prefill: 1000-1500ms for rates >192kHz (vs 100-200ms)
  - MAX_BUFFER raised to 32MB (accommodates 1536kHz/32bit/2ch @ 2s)
  - Decode cache raised to 9.2M samples (~3s at 1536kHz stereo)
  - No change for rates ≤192kHz (identical behavior to v1.1.0)

- **FLAC metadata log spam**: FLAC format information was logged repeatedly (dozens of times) for tracks with large metadata blocks (album art). The STREAMINFO callback was re-triggered on each metadata retry. Now logged only once per track.

- **Player name with spaces ignored (webui)**: When the player name contained spaces (e.g. "Devialet Target"), the startup script lost the quoting and the second word became an unknown option. Fixed with `eval exec` to preserve quoted arguments from `SLIM2DIRETTA_OPTS`.

### Added

- **Build capabilities log at startup**: Displays architecture (x86_64/aarch64/arm) and SIMD support (AVX2/NEON/scalar) for easier remote diagnostics

---

## v1.1.0 (2026-03-06)

### Added

- **Gapless playback**: Seamless track transitions without audio gaps
  - PCM/FLAC: gapless chaining with format change detection
  - DSD (DSF/DFF): gapless chaining with automatic format negotiation
  - Audio thread stays alive between tracks — no Diretta reconnection needed

- **Seek support**: In-track seeking via LMS progress bar
  - FLAC: seek to any position (format detection from frame header when STREAMINFO absent)
  - DSD: seek to any position
  - Correct thread lifecycle management (seek vs gapless path detection)

- **Web Configuration UI (diretta-webui)**: Browser-based settings interface
  - Accessible at `http://<ip>:8081` — no SSH needed to configure slim2diretta
  - Edit all settings: LMS server, player name, verbose mode
  - Advanced Diretta SDK settings: thread-mode, transfer-mode, cycle-time, info-cycle, target-profile-limit, MTU
  - Save & Restart: applies settings and restarts the systemd service in one click
  - Zero dependencies beyond Python 3 (stdlib only)
  - Separate systemd service (`slim2diretta-webui.service`) — transparent for audio quality
  - Installable via `install.sh` option 7 or `./install.sh --webui`
  - Port 8081 to avoid conflict with DirettaRendererUPnP web UI (port 8080)

### Fixed

- **DSF padding silence**: Replace zero-padding in last DSF block with DSD silence (0x69) to eliminate click at track transitions
- **FLAC seek without header**: Fallback format detection from FLAC frame header when LMS sends seek streams without STREAMINFO metadata
- **Config parser**: Handle missing `/etc/default/slim2diretta` file (create on first save instead of crash)
- **Config parser**: Skip duplicate uncommented `SLIM2DIRETTA_OPTS=` lines on save

---

## v1.0.0 (2026-02-28)

### Added

- **ICY metadata handling**: Transparent stripping of ICY metadata from internet radio streams
  - Automatic detection of `icy-metaint` header in HTTP responses
  - Metadata blocks filtered out before audio data reaches decoders
  - Supports both `HTTP/1.x` and `ICY` protocol responses

- **Advanced Diretta SDK options**: Fine-tuning of the Diretta transport layer
  - `--transfer-mode <mode>`: Transfer scheduling mode (`auto`, `varmax`, `varauto`, `fixauto`, `random`)
  - `--info-cycle <us>`: Info packet cycle in microseconds (default: 100000)
  - `--cycle-min-time <us>`: Minimum cycle time for RANDOM mode
  - `--target-profile-limit <us>`: Target profile limit (0=SelfProfile, default: 200)
  - RANDOM transfer mode support with configurable min cycle time
  - TargetProfile / SelfProfile dual-path transfer configuration

- **Auto-release of Diretta target**: Coexistence with other Diretta applications
  - Automatically releases the Diretta target after 5 seconds of idle
  - Transparent re-acquisition on next play command from LMS/Roon
  - Allows DirettaRendererUPnP or other Diretta hosts to use the same target

- **Extended sample rate support**: PCM up to 1536kHz, DSD up to DSD1024
  - `MaxSampleRate` reported to LMS raised from 768kHz to 1536kHz
  - Extended Slimproto sample rate table: 705.6kHz, 768kHz, 1411.2kHz, 1536kHz
  - DSD1024 (45.2MHz) support (already handled by decoder, now documented)

- **SDK improvements**:
  - Changed from `MSMODE_MS3` to `MSMODE_AUTO` for better device compatibility
  - Correct info cycle parameter passed to `DIRETTA::Sync::open()` (was using cycle time)

---

## v0.2.0 - Test Version (2026-02-27)

### Added

- **MP3 decoding** via libmpg123 (LGPL-2.1)
  - Full streaming support for internet radio
  - Automatic ID3v2 tag handling
  - Error recovery with auto-resync (robust for radio streams)

- **Ogg Vorbis decoding** via libvorbisfile (BSD-3-Clause)
  - Streaming with custom non-seekable callbacks
  - Chained stream support (format changes between tracks)
  - OV_HOLE gap handling (normal for radio streams)

- **AAC decoding** via fdk-aac (BSD-like license)
  - ADTS transport for internet radio streams
  - HE-AAC v2 support (SBR + Parametric Stereo)
  - Automatic sample rate detection (handles SBR upsampling)
  - Transport sync error recovery

- **Optional codec system**: All new codecs are compile-time optional via CMake
  - `ENABLE_MP3=ON/OFF` (default: ON, auto-disabled if libmpg123 not found)
  - `ENABLE_OGG=ON/OFF` (default: ON, auto-disabled if libvorbis not found)
  - `ENABLE_AAC=ON/OFF` (default: ON, auto-disabled if fdk-aac not found)

- **LMS capabilities**: Player now advertises mp3, ogg, aac support to LMS
  - LMS sends native format streams instead of transcoding
  - Internet radio stations play directly

### Build Dependencies

New optional dependencies (install for full codec support):

| Distribution | Command |
|-------------|---------|
| **Fedora** | `sudo dnf install mpg123-devel libvorbis-devel fdk-aac-free-devel` |
| **Ubuntu/Debian** | `sudo apt install libmpg123-dev libvorbis-dev libfdk-aac-dev` |
| **Arch** | `sudo pacman -S mpg123 libvorbis libfdk-aac` |

---

## v0.1.0 - Test Version (2026-02-27)

Initial test release for validation by beta testers.

### Features

- **Slimproto protocol**: Clean-room implementation from public documentation (no GPL code)
  - Player registration (HELO), stream control (strm), volume (audg), device settings (setd)
  - LMS auto-discovery via UDP broadcast
  - Heartbeat keep-alive, elapsed time reporting
  - Player name reporting to LMS/Roon

- **Audio formats**:
  - FLAC decoding via libFLAC
  - PCM/WAV/AIFF container parsing with raw PCM fallback (Roon)
  - Native DSD: DSF (LSB-first) and DFF/DSDIFF (MSB-first) container parsing
  - DSD rates: DSD64, DSD128, DSD256, DSD512
  - DoP (DSD over PCM): automatic detection and conversion to native DSD (Roon compatibility)
  - WAVE_FORMAT_EXTENSIBLE support for WAV headers

- **Diretta output** (shared DirettaSync v2.0):
  - Automatic sink format negotiation (PCM and DSD)
  - DSD bit-order and byte-swap conversion (LSB/MSB, LE/BE)
  - Lock-free SPSC ring buffer with SIMD optimizations
  - Adaptive packet sizing with jumbo frame support
  - Quick resume for consecutive same-format tracks

- **Operational**:
  - Systemd service template (`slim2diretta@<target>`) for multi-instance
  - Interactive installation script (`install.sh`)
  - Prebuffer with flow control (500ms target, adaptive for high DSD rates)
  - Pause/unpause with silence injection for clean transitions
  - SIGUSR1 runtime statistics dump

### Known Limitations (v0.1.0)

- Linux only (requires root for RT threads)
- No MP3, AAC, OGG, ALAC support (FLAC and PCM/DSD only)
- No volume control (bit-perfect: forced to 100%)
- No automated tests (manual testing with LMS + DAC)
