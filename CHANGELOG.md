# Changelog

All notable changes to slim2diretta are documented in this file.

## v1.3.0 (unreleased)

### Added

- **Multi-core CPU affinity**: `--cpu-audio` and `--cpu-other` now accept a comma-separated list of cores (e.g., `--cpu-audio 3,4`). When multiple cores are provided, the kernel scheduler may distribute the thread between them. Single-core syntax (e.g., `--cpu-audio 3`) remains fully supported. The SDK itself still receives only the first core of the list (SDK limitation). Aligned with DirettaRendererUPnP v2.3.0.
- **Configurable buffer settings**: New CLI options to tune the Diretta ring buffer and prefill duration, exposed in the web UI under a new "Buffer Configuration" section:
  - `--pcm-buffer-seconds <s>` — PCM buffer size in seconds (default 0.5s)
  - `--dsd-buffer-seconds <s>` — DSD buffer size in seconds (default 0.8s)
  - `--pcm-prefill-ms <ms>` — PCM prefill duration in ms (default 80ms)
  - `--dsd-prefill-ms <ms>` — DSD prefill duration in ms (default 200ms)
- **Advanced System & Network Tuning** (mirrored from DirettaRendererUPnP v2.4.0): three wrapper-level audiophile-tuning features grouped in a new web UI section, all exposed as shell vars in `/etc/default/slim2diretta` and applied by `start-slim2diretta.sh` before launching the binary:
  - **Target NIC link tuning** — `TARGET_INTERFACE` / `TARGET_SPEED` (10/100/1000) / `TARGET_DUPLEX` force the host NIC speed/duplex via `ethtool`. Some audiophile users perceive a sound-quality difference when constraining the link. The wrapper logs a warning and skips link tuning if `ethtool` is missing. Web UI and `.default` comments include a bandwidth-vs-format reminder so users don't pick a link speed too narrow for hi-res PCM or DSD512+. `ethtool` is added to the base dependency list installed by `install.sh` (dnf/apt/pacman).
  - **IRQ affinity** — `IRQ_INTERFACE` / `IRQ_CPUS` pin all hardware IRQs (including MSI-X queues) of one or more NICs to a specific CPU list at service start. `IRQ_INTERFACE` accepts a single name (e.g. `enp1s0`) or a comma-separated list (e.g. `enp1s0,enp2s0`) to cover hosts with separate NICs for the upstream LMS source and the Diretta target. Pairs naturally with `--cpu-audio` to keep network IRQ activity off the audio worker core, a known source of jitter on busy LANs. Kernel-managed IRQs that refuse runtime reassignment are reported as "skipped" in the launcher log without failure.
  - **SMT (Hyper-Threading) toggle** — `SMT` accepts `on` / `off` / `forceoff` / empty. The wrapper writes the chosen value to `/sys/devices/system/cpu/smt/control` before launching slim2diretta, so any subsequent `--cpu-audio` / `--cpu-other` pinning sees the right topology. System-wide setting; non-persistent across kernel reboots (wrapper re-applies on each service start). BIOS lock or kernel-restricted control is detected and reported as a warning rather than a failure.

### Changed

- Config fields `cpuAudio` and `cpuOther` are now strings (previously `int`). Existing single-core values keep working unchanged.

## v1.2.8 (2026-04-18)

### Added

- **Web UI Stop button**: Added a Stop button alongside the existing Save & Restart and Restart Only buttons. Useful for users running slim2diretta on their own Linux distributions to stop the service directly from the web UI — e.g., to release the Diretta target for another player or before maintenance. Includes a confirmation dialog.

---

## v1.2.7 (2026-04-16)

### Added

- **Main thread CPU affinity**: The main thread is now also pinned to the `--cpu-other` core when CPU affinity is configured. Previously it was the only thread left unpinned, still managed by the OS scheduler. (PR #9, contributed by sheviks)

---

## v1.2.6 (2026-04-14)

### Added

- **Resilient Diretta target discovery at startup**: Instead of exiting when the target is not found, the player now retries every 2s and logs every 5s until the target becomes available (or the process is cancelled with Ctrl+C). Makes the player robust to targets that start later, are temporarily unavailable, or are restarted — especially important on systems without systemd auto-restart (e.g., GentooPlayer with OpenRC). (Suggested by Filippo, GentooPlayer)

- **Clang + LTO build option**: New `ENABLE_LTO` CMake option enables `-flto` on compile and link flags. Recommended combination for best audio quality — multiple testers report a clearly preferable sound with clang+LTO builds. Three ways to enable:
  ```bash
  # Single-switch shortcut (clang + LTO + lld linker):
  env LLVM=1 ./install.sh -b
  #   or: LLVM=1 cmake ..

  # Manual CMake flags:
  CC=clang CXX=clang++ cmake -DENABLE_LTO=ON -DUSE_LLD=ON ..

  # Interactive prompt (auto-detects clang):
  ./install.sh
  ```
  The `LLVM=1` shortcut mirrors the convention used by the Linux kernel and DirettaRendererUPnP's Makefile. It automatically sets `CC=clang`, `CXX=clang++`, enables LTO, and uses the `lld` linker via `-fuse-ld=lld`. (Inspired by PR #8 from sheviks)

- **Verbose build output**: Set `VERBOSE=1` or `V=1` to make `install.sh` pass `VERBOSE=1` to make, showing the full compiler command lines. Useful for debugging build issues. (Inspired by PR #8 from sheviks)

- **CPU affinity (thread pinning)**: Two new CLI options and Web UI fields pin critical threads to specific cores, reducing jitter on systems with CPU isolation.
  - `--cpu-audio <core>`: pins the Diretta SDK worker thread (hot path). Automatically adds the `OCCUPIED` flag (bit 16) to the SDK thread mode and also pins the worker manually via `pthread_setaffinity_np` (belt-and-suspenders, because SDK pinning doesn't always work — e.g., on RPi 4).
  - `--cpu-other <core>`: pins the audio/decode thread (HTTP reader + decoder + ring buffer push) and the Slimproto TCP receive thread.
  - Exposed in the Web UI under a new "CPU Affinity" section.
  - Default: no pinning (-1). Aligns with the existing implementation in DirettaRendererUPnP.

- **Graceful fallback when DAC doesn't support native DSD**: Previously slim2diretta crashed with an unhandled exception when the Diretta target reported no DSD support. It now logs an error and skips the track instead of crashing. Users should configure LMS/Roon to send DoP (DSD over PCM) when native DSD is unavailable. (Reported by lalekuku)

## v1.2.5 (2026-04-11)

### Fixed

- **100% CPU spin in drain loop when target is auto-released**: When the audio thread was draining the decode cache after HTTP EOF and the Diretta target got auto-released (5s inactivity), `sendAudio` returned 0 forever and the drain loop spun at 100% CPU. The audio thread never terminated, blocking subsequent track transitions indefinitely. Fixed with two safeguards: bail out of the drain loop when the target is no longer open, and add a 5ms sleep on `framesWritten==0` as a defensive measure for any other transient zero-write conditions. (Reported by cmr75)

### Changed

- **Larger PCM buffer for CDN resilience**: Multiple users (katywu, Hoorna, Progman, Ikyo) reported intermittent buffer underruns when playing Qobuz streams via LMS or Roon, even for standard CD quality (16/44). Unlike DirettaRendererUPnP, slim2diretta cannot distinguish remote from local streams (both look like local Slimproto streams), so the larger buffer is applied to all PCM playback.
  - `PCM_BUFFER_SECONDS`: 0.5s → 3.0s (6x larger buffer)
  - `PCM_PREFILL_MS`: 50ms → 500ms
  - `PREFILL_MS_COMPRESSED`: 200ms → 800ms
  - `PREFILL_MS_UNCOMPRESSED`: 100ms → 500ms
  - `REBUFFER_THRESHOLD_PCT`: 20% → 50% (more resilient recovery after underrun)
  - Trade-off: ~500ms slower initial track start and longer recovery after CDN hiccups. Acceptable for streaming where latency doesn't matter like it does for local playback.

## v1.2.4 (2026-03-31)

### Fixed

- **Tracks skipping during gapless format transitions**: When `open()` failed during a gapless PCM format change (e.g., 44.1kHz→96kHz), slim2diretta sent STMn (error) followed by STMu (track ended), causing LMS to skip to the next track prematurely. STMu is now suppressed after an open failure. (Reported by Jeep972)

- **Worker thread join timeout**: Replaced bare `m_workerThread.join()` calls (which could block indefinitely) with `joinWorkerWithTimeout(1000ms)` in all format transition paths. Prevents hangs when the SDK worker thread is unresponsive during format changes.

## v1.2.3 (2026-03-21)

### Fixed

- **Roon: next track not starting after gapless transition**: When the decode cache drained and no next track arrived within the 2-second gapless wait, the ring buffer would underrun. Roon interprets underruns as errors and refuses to start the next track. Now stops playback gracefully with silence buffers before the ring buffer drains, so Roon receives a clean end-of-track signal (STMu) instead of an underrun. (Reported by PatrickW)

## v1.2.2 (2026-03-20)

### Performance

- **RT-safe async logging**: Hot-path logging in `sendAudio` and `getNewStream` callbacks now uses `snprintf` on a stack-local buffer (`DIRETTA_LOG_ASYNC_FMT`) instead of `std::ostringstream` which heap-allocates on every call. Zero heap allocation on the audio critical path. (Inspired by leeeanh's RT jitter reduction work)

- **PcmDecoder read-offset**: Replace `O(n)` `vector::erase` on every `readDecoded()` call with a read-offset (`m_dataPos`). Compaction only occurs when the offset exceeds 64 KB. Eliminates `memmove` from the PCM decode hot path. (Inspired by leeeanh's RT jitter reduction work)

### Fixed

- **install.sh: SDK path broken after `cd build`**: `find` could return relative paths (e.g., `./DirettaHostSDK_148`) which became invalid after `cd build`. Now resolved to absolute paths via `realpath`. (Reported by sheviks, [#6](https://github.com/cometdom/slim2Diretta/issues/6))

- **install.sh: "Failed to stop inactive.service"**: Option 4 (Update binary) tried to stop `inactive.service` when the service was already stopped, because `systemctl is-active` returns the text "inactive". Now uses `--quiet` flag to check exit code only.

- **install.sh: uninstall did not remove web UI**: The uninstall function now also stops/disables `slim2diretta-webui.service` and removes `/opt/slim2diretta/webui/` when installed.

- **No sound on 16-bit FLAC with 24-bit DACs**: The bit depth formula mapped 16-bit sources to 32-bit Diretta connections (`== 24` only matched 24-bit). DACs that report 32-bit support but physically only handle 24-bit produced no sound. Changed to `<= 24` so both 16-bit and 24-bit sources open at 24-bit. Only true 32-bit content opens at 32-bit.

## v1.2.1 (2026-03-18)

### Changed

- **Resilient LMS autodiscovery**: When no LMS server is specified (`-s`) and the server is not immediately discoverable, slim2diretta now retries indefinitely (every 2 seconds) instead of exiting with an error. This is consistent with the Diretta target discovery behavior and prevents startup failures when LMS is temporarily offline or still booting. Press Ctrl+C to cancel. (Suggested by Filippo/GentooPlayer)

### Fixed

- **32-bit WAV playback broken with FFmpeg decoder**: `FfmpegDecoder` bit depth detection fell back to 24-bit when FFmpeg did not populate `bits_per_raw_sample` for raw PCM codecs, causing `m_s32Shift = 8` which corrupted the audio data. For raw PCM, `m_rawBitDepth` (from the Slimproto strm command) is now used as the authoritative source, making 32-bit WAV files play correctly via FFmpeg. (Reported by Mani)

- **White noise on 24-bit DACs with FFmpeg decoder**: Two root causes fixed: (1) `audioFmt.bitDepth` was hardcoded to 32, causing the Diretta connection to open at 32-bit even for 24-bit sources — `main.cpp` now passes the actual source bit depth, and `configureSinkPCM` in DirettaSync now only offers 32-bit negotiation when the source is actually 32-bit; (2) FFmpeg's S32/S32P output is sign-extended (LSB-aligned), whereas all other decoders produce MSB-aligned int32_t — `FfmpegDecoder` now applies a left-shift (`32 - bitsPerRawSample`) to MSB-align samples before writing to the ring buffer. libFLAC was unaffected because its MSB-aligned output survived ALSA's 32→24-bit truncation correctly. (Reported by progman)

- **High-rate WAV files wrong sample rate with FFmpeg decoder**: WAV files at non-standard rates (e.g., 705600 Hz) were played at 44100 Hz when using `--decoder ffmpeg`. The FFmpeg raw PCM path relied on the Slimproto `rate` field which cannot encode rates above ~192kHz, while the native PcmDecoder correctly reads the WAV header. PCM/WAV/AIFF format (`format=p`) now always uses the native PcmDecoder regardless of `--decoder` setting. FFmpeg is only used for compressed formats (FLAC, MP3, AAC, OGG). (Reported by abase)

## v1.2.0 (2026-03-15)

### Added

- **Resilient target discovery**: When the Diretta target is not available at startup, the player now retries every 2 seconds (with status logged every 5 seconds) instead of exiting immediately. This is especially important on systems without systemd auto-restart (e.g., GentooPlayer with OpenRC). (Suggested by Filippo/GentooPlayer)

- **Graceful DSD fallback**: DACs that do not support native DSD via Diretta no longer cause a crash. The error is logged and the track is skipped, with a suggestion to configure LMS for DoP output. (Reported by lalekuku)

- **FFmpeg decoder backend** (`--decoder ffmpeg`): Alternative audio decoder using FFmpeg's libavcodec. Users can switch between the native backend (libFLAC/libmpg123/libvorbis) and FFmpeg for a different sonic signature. The native backend offers a brighter, more detailed sound; FFmpeg provides a warmer, more enveloping presentation with a wider soundstage. Both are lossless and theoretically bit-identical — the perceived difference likely stems from internal processing patterns (memory allocation, frame buffering, decode timing).
  - Parser-based architecture (no avformat needed) — lightweight, only libavcodec + libavutil
  - Supports FLAC, MP3, AAC, OGG, ALAC, PCM via a single unified decoder
  - DSD remains handled natively (raw bitstream, not decoded)
  - Selectable via CLI (`--decoder ffmpeg`) or Web UI (Decoder Backend dropdown)
  - Build-time optional: auto-detected, graceful fallback to native if FFmpeg not installed

- **Startup codec listing**: Build capabilities now show all available codecs and FFmpeg availability at startup

### Fixed

- **DSD64 DoP playback**: Fixed continuous ~485 Hz whistle tone when playing DSD64 via Roon (DoP). DoP frames are now passed through as 24-bit PCM to the Diretta Target, which handles DoP marker detection and DAC forwarding natively. Previously, `convertDopToNative()` destroyed the DoP markers causing frame misalignment. This matches the working behavior of squeeze2upnp→DirettaRendererUPnP. (Credit: hoorna, PR #4)

- **DSD128 DoP underruns**: Fixed systematic buffer underruns when Roon downsamples DSD128 to DSD64 DoP (176.4 kHz carrier). Ring buffer increased from 0.5s to 6.0s (12MB) for rates ≥176.4kHz, adaptive rebuffer threshold set to 50% (vs 20% normal) providing ~4.2s headroom after underrun, prebuffer increased to 3000ms for high-rate streams. The high-rate threshold was lowered from 192kHz to 176kHz to capture DSD64 DoP's 176.4kHz carrier. Roon's DSD128→DoP transcoding has a cold-start phase where throughput is ~35% of real-time; second play is fine. (Reported by hoorna)

- **FFmpeg 24-bit PCM decoding**: Fixed continuous decode errors (`Invalid PCM packet, data has size 2 but at least a size of 6 was expected`) when playing 24-bit content via FFmpeg backend. Three-part fix: (1) align raw PCM packets to `block_align` — for stereo 24-bit, 8192 % 6 = 2 remainder rejected by FFmpeg; (2) disable FFmpeg's PCM parser which splits data without respecting `block_align`; (3) explicitly set `block_align = channels × bytes_per_sample` before opening the codec, as FFmpeg leaves it at 0 when no demuxer is used. 16-bit was unaffected (8192 % 4 = 0). (Reported by progman)

- **Audio data loss in push loop**: Fixed `sendAudio` return value being ignored, causing `decodeCachePos` to advance past data that wasn't actually written to the ring buffer. Multi-chunk push (4×2048 frames) is now limited to high sample rates (>176kHz) where it's needed; normal rates use single 1024-frame push like v1.2.0.

- **FFmpeg gapless click**: Fixed audible click at gapless track transitions when using FFmpeg decoder. The FFmpeg parser (`av_parser_parse2`) buffers partial codec frames internally; at stream EOF, this buffered data was never flushed, losing the last few samples of each track. Now flushes the parser with `(NULL, 0)` before flushing the decoder, recovering the final frame.

### Build Dependencies

New optional dependency for FFmpeg backend:

| Distribution | Command |
|-------------|---------|
| **Fedora** | `sudo dnf install ffmpeg-free-devel` |
| **Ubuntu/Debian** | `sudo apt install libavcodec-dev libavutil-dev` |
| **Arch** | `sudo pacman -S ffmpeg` |

To disable FFmpeg backend: `cmake -DENABLE_FFMPEG=OFF ..`

---

## v1.1.1 (2026-03-11) — internal release, not published

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
  - DoP (DSD over PCM): automatic detection and passthrough as 24-bit PCM to Diretta Target
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
