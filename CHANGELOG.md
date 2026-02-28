# Changelog

All notable changes to slim2diretta are documented in this file.

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

- **SDK improvements**:
  - Changed from `MSMODE_MS3` to `MSMODE_AUTO` for better device compatibility
  - Correct info cycle parameter passed to `DIRETTA::Sync::open()` (was using cycle time)

### Known Limitations

- **Roon radio**: Only FLAC-encoded internet radios work via Roon's Squeezebox emulation. MP3/AAC/OGG radios are not supported by Roon's Slimproto implementation (based on LMS 6.0.x era code). All radio formats work via LMS.

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
