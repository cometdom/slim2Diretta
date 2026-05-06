# CLAUDE.md - slim2diretta

This file provides guidance to Claude Code when working with this repository.

## Project Overview

**slim2diretta** is a native LMS (Slimproto) player with Diretta output in a mono-process architecture. It replaces the couple squeezelite + squeeze2diretta-wrapper with a single binary that implements Slimproto directly and feeds DirettaSync without an intermediate pipe.

**License**: MIT (no GPL code copied). Slimproto protocol implemented from public documentation.

## Build Commands

```bash
# Build (auto-detects architecture and SDK)
mkdir build && cd build
cmake ..
make

# Specific architecture variants
cmake -DARCH_NAME=x64-linux-15v3 ..       # x64 AVX2 (most common)
cmake -DARCH_NAME=aarch64-linux-15 ..     # Raspberry Pi 4
cmake -DARCH_NAME=aarch64-linux-15k16 ..  # Raspberry Pi 5 (16KB pages)

# Custom SDK path
export DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_149
cmake ..

# Clang + LTO (recommended for best audio quality)
CC=clang CXX=clang++ cmake -DENABLE_LTO=ON ..
```

## Running

```bash
# List available Diretta targets
sudo ./slim2diretta --list-targets

# Basic usage
sudo ./slim2diretta -s <lms-ip> --target 1

# With player name and verbose
sudo ./slim2diretta -s <lms-ip> --target 1 -n "Living Room" -v

# With CPU affinity (single core each — cores 2,3,4 isolated via isolcpus=2,3,4)
sudo ./slim2diretta --target 1 --cpu-audio 2 --cpu-decode 3 --cpu-other 4

# Multi-core affinity (v1.3.0+): audio thread can float between cores 2,3
sudo ./slim2diretta --target 1 --cpu-audio 2,3 --cpu-other 4

# v1.3.3+: --cpu-decode pins the audio/decode thread to its own core and
# raises it to SCHED_FIFO. If --cpu-decode is empty, the audio/decode
# thread inherits --cpu-other (v1.3.2 behaviour preserved).

# Tune buffer sizes (v1.3.0+)
sudo ./slim2diretta --target 1 --pcm-buffer-seconds 1.0 --dsd-buffer-seconds 1.2
```

## Architecture

```
LMS (network)
  -> slim2diretta (single process)
    -> SlimprotoClient (TCP port 3483) : control
    -> HttpStreamClient (port 9000) : encoded audio stream
    -> Decoder (FLAC/PCM/DSD — native or FFmpeg backend)
    -> DirettaSync (ring buffer + SDK)
      -> Diretta Target (UDP/Ethernet)
        -> DAC
```

**Threading**: main (init/signals) + slimproto (TCP LMS) + audio (HTTP->decode->push) + SDK worker (DirettaSync internal)

**CPU affinity** (`--cpu-audio`, `--cpu-decode`, `--cpu-other`): optional thread pinning via `pthread_setaffinity_np`, default empty (no pinning). Since v1.3.0 each option accepts either a single core (`3`) or a comma-separated list (`3,4`). Three-tier model aligned with DirettaRendererUPnP v2.4.2:

- `--cpu-audio` pins the SDK worker thread and is also passed to `DIRETTA::Sync::open(cpuMain, cpuOther, ...)` — the SDK only takes a single core, so the first value from the list is used; the `OCCUPIED` flag (bit 16) is added to threadMode automatically when `cpuAudio` is non-empty.
- `--cpu-decode` (v1.3.3+) pins the audio/decode thread (HTTP receive → decode → push to ring buffer). When set, that thread is also raised to `SCHED_FIFO` priority via `g_rtPriority`, since the dedicated core makes that safe. Falls back to `--cpu-other` when empty (preserves v1.3.2 behaviour).
- `--cpu-other` pins the main thread and the Slimproto TCP receive thread. Also serves as fallback target for the audio/decode thread when `--cpu-decode` is empty.

All three options are exposed via CLI and Web UI (CPU Affinity group, `cli_opts` config_type).

**Buffer configuration** (v1.3.0+): `--pcm-buffer-seconds`, `--dsd-buffer-seconds`, `--pcm-prefill-ms`, `--dsd-prefill-ms`. Zero/empty means "use defaults" from the `DirettaBuffer` namespace. The override is applied in `configureRingPCM()`, `configureRingDSD()`, and `calculateAlignedPrefill()`. Exposed via CLI and Web UI (Buffer Configuration group).

**Startup**: Both Diretta target discovery and LMS autodiscovery retry indefinitely:
- `discoverTarget()` retries every 2s (log every 5s) until found or cancelled. Pass `std::atomic<bool>* stopSignal` to `enable()` to activate retry mode.
- LMS autodiscovery (when no `-s` is specified) retries every 2s in a `while(g_running)` loop until LMS responds. Ctrl+C cancels cleanly.

**Key Components**:

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, CLI, signal handling, logging init |
| `src/Config.h` | Configuration struct |
| `src/PlayerController.cpp/h` | Orchestrator: state machine, thread coordination |
| `src/SlimprotoClient.cpp/h` | Slimproto TCP protocol client |
| `src/SlimprotoMessages.h` | Binary protocol message structs |
| `src/HttpStreamClient.cpp/h` | HTTP audio stream fetcher |
| `src/Decoder.h` | Decoder abstract interface |
| `src/FlacDecoder.cpp/h` | FLAC decoder (libFLAC) |
| `src/PcmDecoder.cpp/h` | PCM/WAV/AIFF header parser |
| `src/Mp3Decoder.cpp/h` | MP3 decoder (libmpg123, optional) |
| `src/OggDecoder.cpp/h` | Ogg Vorbis decoder (libvorbisfile, optional) |
| `src/AacDecoder.cpp/h` | AAC decoder (fdk-aac, optional) |
| `src/FfmpegDecoder.cpp/h` | FFmpeg decoder backend (libavcodec, optional) |
| `src/DsdProcessor.cpp/h` | DSD conversions (interleaved->planar, DoP->native) |
| `diretta/DirettaSync.cpp/h` | Diretta SDK wrapper (shared with squeeze2diretta) |
| `diretta/DirettaRingBuffer.h` | Lock-free SPSC ring buffer |
| `diretta/globals.cpp/h` | Logging configuration |
| `diretta/LogLevel.h` | Centralized log level system |
| `diretta/FastMemcpy*.h` | SIMD memory operations |

**Web UI** (`webui/`):

| File | Purpose |
|------|---------|
| `webui/diretta_webui.py` | HTTP server (custom BaseHTTPRequestHandler, no framework) |
| `webui/config_parser.py` | Config parsers: `ShellVarConfig` (KEY=VALUE) and `CliOptsConfig` (CLI args) |
| `webui/profiles/slim2diretta.json` | Product profile defining settings groups and field types |
| `webui/templates/index.html` | HTML template with embedded JavaScript |
| `webui/static/style.css` | Minimal CSS styling |
| `webui/slim2diretta-webui.service` | Systemd service (port 8081) |

**Startup & Install**:

| File | Purpose |
|------|---------|
| `start-slim2diretta.sh` | Startup wrapper: reads `/etc/default/slim2diretta`, applies priority, link tuning (`ethtool`), IRQ affinity, SMT toggle, then `eval exec` |
| `install.sh` | Interactive installer (binary, service, webui) — pulls `ethtool` as a base dep so the wrapper-level link tuning works out of the box |
| `slim2diretta.service` | Main systemd service |

**Wrapper-level audiophile tuning (v1.3.0, mirrored from DirettaRendererUPnP v2.4.0)**: three system-wide tweaks applied by `start-slim2diretta.sh` before launching the binary, all configured via shell vars in `/etc/default/slim2diretta` and exposed in the web UI under "Advanced System & Network Tuning":

- `TARGET_INTERFACE` / `TARGET_SPEED` / `TARGET_DUPLEX` → forces NIC link via `ethtool -s`. `ethtool` missing is a non-fatal warning.
- `IRQ_INTERFACE` / `IRQ_CPUS` → walks `/proc/interrupts`, writes each `/proc/irq/N/smp_affinity_list` for IRQs whose name contains any interface listed in `IRQ_INTERFACE` (single name or comma-separated, e.g. `"enp1s0,enp2s0"` for a host with one NIC for LMS upstream and one for the Diretta target). Matches MSI-X queues like `enpXsY-rx-0`. Kernel-managed IRQs that refuse runtime reassignment are reported as "skipped".
- `SMT` (`on` / `off` / `forceoff` / empty) → writes `/sys/devices/system/cpu/smt/control`. BIOS lock or unsupported control is a non-fatal warning. Toggle is non-persistent across kernel reboots; the wrapper re-applies it on each service start. Note: SMT changes the logical-CPU numbering, so `--cpu-audio` / `--cpu-other` values must be valid in the chosen state.

## Code Style

- **C++17** standard
- **Classes**: `PascalCase`
- **Functions**: `camelCase`
- **Members**: `m_camelCase`
- **Constants**: `UPPER_SNAKE_CASE`
- **Globals**: `g_camelCase`
- **Indentation**: 4 spaces

## Slimproto Protocol

Implemented from public documentation (wiki.lyrion.org, SlimDevices wiki). Reference implementations consulted: Rust slimproto crate (MIT), Ada slimp (MIT). **No code copied from squeezelite (GPL v3).**

Key messages: HELO (registration), STAT (status), strm (stream control), audg (volume - forced 100%), setd (device name).

## Dependencies

- **Diretta Host SDK v147, v148, or v149** (proprietary, not committed, personal use). Auto-detected at CMake configure time via `Host/Release.hpp` parsing — newer releases that preserve the same header layout will be picked up automatically.
- **libFLAC** (BSD-3-Clause) for FLAC decoding
- **POSIX threads** (pthreads)
- **C++17 runtime**
- **Optional**: libmpg123 (MP3), libvorbis (Ogg), fdk-aac (AAC)
- **Optional**: libavcodec + libavutil (FFmpeg decoder backend, `--decoder ffmpeg`)

SDK locations searched (in order, newest version preferred):
1. `$DIRETTA_SDK_PATH`
2. `~/DirettaHostSDK_149`, `~/DirettaHostSDK_148`, `~/DirettaHostSDK_147`
3. `./DirettaHostSDK_149`, `./DirettaHostSDK_148`, `./DirettaHostSDK_147`
4. `/opt/DirettaHostSDK_149`, `/opt/DirettaHostSDK_148`, `/opt/DirettaHostSDK_147`


## Audio Push Strategy

The audio thread (in `main.cpp`) handles HTTP reading, decoding, and ring buffer pushing in a single thread. Key constants and patterns:

- **MAX_DECODE_FRAMES = 1024**: Decoder reads (adapts to libFLAC frame sizes)
- **PUSH_CHUNK_FRAMES = 2048**: Push to DirettaSync in multi-chunk loop (up to 4×2048 = 8192 frames per iteration)
- **Multi-chunk push**: At high sample rates (176.4kHz DoP, 192kHz+), a single 1024-frame push per loop yields insufficient throughput; the loop pushes as many chunks as possible while buffer has space
- **sendAudio return value**: All push sites use the return value (input bytes consumed) to advance `decodeCachePos` — prevents data loss when ring buffer is near-full. Normal rates (≤176kHz) use single 1024-frame push per iteration; multi-chunk only for high rates
- **Flow control**: 1ms sleep when buffer >95% full, loop back to HTTP read to keep TCP pipeline flowing
- **Decode cache**: Up to 9.2M samples with compaction every 500k consumed samples
- **Prebuffer**: 500ms normal, 3000ms for ≥176.4kHz
- **Ring buffer sizing**: 3.0s normal (PCM_BUFFER_SECONDS, raised from 0.5s in v1.2.5 for CDN resilience), 6.0s for ≥176.4kHz (PCM_HIGHRATE_BUFFER_SECONDS). HIGHRATE_THRESHOLD = 176000
- **SDK prefill**: 500ms PCM (raised from 50ms in v1.2.5), 800ms compressed / 500ms uncompressed (raised from 200/100ms)
- **Adaptive rebuffering**: 50% refill threshold after underrun (raised from 20% in v1.2.5) — more resilient recovery when Qobuz/Tidal CDN delivery stalls. High-rate streams use the same 50% threshold for ~4.2s headroom
- **Drain loop safeguard (v1.2.5)**: Drain loop bails out when the Diretta target is no longer open (auto-released after 5s idle) to prevent 100% CPU spin. Defensive 5ms sleep on `framesWritten==0`

This pattern was aligned with DirettaRendererUPnP's audio engine for consistent delivery characteristics across both projects.

## Decoder Routing

`Decoder::create()` in `Decoder.cpp` routes format codes to decoder implementations:
- **`format=p` (PCM/WAV/AIFF)**: Always uses native `PcmDecoder`, even when `--decoder ffmpeg` is active. PcmDecoder parses WAV/AIFF headers to detect the true sample rate, which is critical for high-rate files (e.g., 705600 Hz). The Slimproto `rate` field cannot encode rates above ~192kHz (sent as `rate=3` → 44100 Hz).
- **`format=d` (DSD)**: Always uses native DSD path (raw bitstream, not decoded).
- **`format=f` (FLAC), MP3, AAC, OGG**: Use FFmpeg when `--decoder ffmpeg` is active, native otherwise.

## FFmpeg Notes

**Parser flush at EOF**: `av_parser_parse2()` buffers partial codec frames; must flush with `(NULL, 0)` before flushing the decoder to recover the last audio frame (critical for gapless transitions).

**Raw PCM pitfalls** (historical — FFmpeg no longer handles raw PCM since v1.2.1):
- `block_align` is 0 without a demuxer: must be set explicitly
- PCM parser splits without respecting `block_align`: force `m_parser = nullptr`

## Bit Depth Handling

All decoders output **MSB-aligned int32_t** samples (4 bytes per sample in the ring buffer):
- 24-bit FLAC (libFLAC): `sample << 8` → upper 24 bits set, LSByte = 0x00
- 16-bit FLAC (libFLAC): `sample << 16` → upper 16 bits set, lower 2 bytes = 0x00
- FFmpeg (S32/S32P): FFmpeg decoders already produce MSB-aligned S32 output (FLAC shifts internally by `32-bps`, float codecs scale to full 32-bit range). No additional shift is needed — `m_s32Shift` was removed in v1.2.1.

`audioFmt.bitDepth` in `main.cpp` reflects the **source bit depth** (24 for ≤24-bit content, 32 otherwise). This drives two things:
1. **Diretta format negotiation** (`configureSinkPCM`): only offers 32-bit if source is ≥32-bit. Prevents white noise/silence on DACs that report 32-bit support at the Diretta target level but are physically limited to 24-bit. Both 16-bit and 24-bit sources open at 24-bit (`fmt.bitDepth <= 24`).
2. **Ring buffer input width** (`inputBps`): always 4 (int32_t), regardless of bit depth — derived from the `bitDepth == 32 || bitDepth == 24` formula.

`push24BitPacked` uses hybrid S24 auto-detection (MSB-aligned vs LSB-aligned), with `MsbAligned` hint always set for all our decoders.

**FFmpeg bit depth detection**: `bits_per_raw_sample` from FFmpeg is authoritative when set. When it is 0, raw PCM uses `m_rawBitDepth` (from the Slimproto strm command) — do NOT fall back to the `sample_fmt` heuristic for raw PCM, as S32 maps to 24 by default which corrupts 32-bit WAV data.

## Hot-Path Performance

- **`DIRETTA_LOG_ASYNC_FMT`**: RT-safe logging macro using `snprintf` on a stack-local `char[248]`. Used in `sendAudio` and `getNewStream` callbacks instead of `DIRETTA_LOG_ASYNC` (which uses `std::ostringstream` → heap allocation). The legacy `DIRETTA_LOG_ASYNC` is kept for non-critical paths.
- **PcmDecoder read-offset**: `m_dataPos` advances through `m_dataBuf` instead of `vector::erase()` on every `readDecoded()` call. Compaction only when `m_dataPos >= 64KB` (`DATA_COMPACT_THRESHOLD`). Eliminates O(n) `memmove` from the decode hot path.

## Gapless Playback Strategy

- **Shared decode cache**: `decodeCache`, `decodeCachePos`, `direttaOpened`, `audioFmt` persist across gapless same-format tracks (outside the chaining `while(true)` loop)
- **Same-format continuation**: If next track has same sample rate and channels, skip Diretta close/reopen — just keep pushing to the ring buffer
- **Format change**: Drain remaining cache, close Diretta, reopen with new format
- **STMd timing**: Sent at HTTP EOF, but decode cache may still have seconds of audio buffered — LMS handles track counter advancement
- **Clean end-of-track**: After gapless wait timeout (no next track), `stopPlayback(false)` sends silence buffers before the ring buffer drains. Prevents underruns that Roon interprets as errors (refusing to start the next track). LMS tolerates underruns; Roon does not.
- **open() failure in gapless**: If `open()` fails during a format change in gapless chaining, `openFailedInGapless` flag prevents STMu from being sent after STMn. Without this, LMS receives STMn+STMu and skips to the next track.
- **Timed worker join**: `joinWorkerWithTimeout(1000ms)` replaces bare `m_workerThread.join()` in all format transition paths to prevent indefinite blocking when the SDK worker is unresponsive.

## Web UI

- Python 3 HTTP server on port 8081 (no external dependencies)
- Reads/writes `/etc/default/slim2diretta` (EnvironmentFile for systemd)
- Config format: `SLIM2DIRETTA_OPTS="--server 192.168.1.10 --name \"My Player\" -v"`
- **Player names with spaces** must be quoted in SLIM2DIRETTA_OPTS; `start-slim2diretta.sh` uses `eval exec` to preserve quoting
- `config_parser.py` has two parsers:
  - `ShellVarConfig`: KEY=VALUE lines (shared with DirettaRendererUPnP)
  - `CliOptsConfig`: single variable with CLI args (slim2diretta)
- Settings profile in `webui/profiles/slim2diretta.json`
- Install via `./install.sh --webui` or option 7 in interactive menu

## Important Notes

- Requires root/sudo for real-time thread priority
- Linux only
- Diretta protocol uses IPv6 link-local for target communication
- Diretta SDK is personal-use only - never commit SDK files
- Volume forced to 100% for bit-perfect playback
- The `diretta/` folder is shared code with squeeze2diretta and DirettaRendererUPnP
- No automated tests - manual testing with LMS + DAC
