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
export DIRETTA_SDK_PATH=/path/to/DirettaHostSDK_148
cmake ..
```

## Running

```bash
# List available Diretta targets
sudo ./slim2diretta --list-targets

# Basic usage
sudo ./slim2diretta -s <lms-ip> --target 1

# With player name and verbose
sudo ./slim2diretta -s <lms-ip> --target 1 -n "Living Room" -v
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
| `src/FfmpegDecoder.cpp/h` | FFmpeg decoder backend (avformat/AVIO + direct codec, optional) |
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
| `start-slim2diretta.sh` | Startup wrapper: reads `/etc/default/slim2diretta`, applies priority, `eval exec` |
| `install.sh` | Interactive installer (binary, service, webui) |
| `slim2diretta.service` | Main systemd service |

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

- **Diretta Host SDK v147 or v148** (proprietary, not committed, personal use)
- **libFLAC** (BSD-3-Clause) for FLAC decoding
- **POSIX threads** (pthreads)
- **C++17 runtime**
- **Optional**: libmpg123 (MP3), libvorbis (Ogg), fdk-aac (AAC)
- **Optional**: libavcodec + libavformat + libavutil (FFmpeg decoder backend, `--decoder ffmpeg`)

SDK locations searched (in order):
1. `$DIRETTA_SDK_PATH`
2. `~/DirettaHostSDK_148` or `~/DirettaHostSDK_147`
3. `./DirettaHostSDK_148` or `./DirettaHostSDK_147`
4. `/opt/DirettaHostSDK_148` or `/opt/DirettaHostSDK_147`


## Audio Push Strategy

The audio thread (in `main.cpp`) handles HTTP reading, decoding, and ring buffer pushing in a single thread. Key constants and patterns:

- **MAX_DECODE_FRAMES = 1024**: Decoder reads (adapts to libFLAC frame sizes)
- **PUSH_CHUNK_FRAMES = 2048**: Push to DirettaSync in multi-chunk loop (up to 4×2048 = 8192 frames per iteration)
- **Multi-chunk push**: At high sample rates (176.4kHz DoP, 192kHz+), a single 1024-frame push per loop yields insufficient throughput; the loop pushes as many chunks as possible while buffer has space
- **sendAudio return value**: All push sites use the return value (input bytes consumed) to advance `decodeCachePos` — prevents data loss when ring buffer is near-full. Normal rates (≤176kHz) use single 1024-frame push per iteration; multi-chunk only for high rates
- **Flow control**: 1ms sleep when buffer >95% full, loop back to HTTP read to keep TCP pipeline flowing
- **Decode cache**: Up to 9.2M samples with compaction every 500k consumed samples
- **Prebuffer**: 500ms normal, 3000ms for ≥176.4kHz
- **Ring buffer sizing**: 0.5s normal, 6.0s for ≥176.4kHz (PCM_HIGHRATE_BUFFER_SECONDS). HIGHRATE_THRESHOLD = 176000
- **Adaptive rebuffering**: After underrun, high-rate streams use 50% refill threshold (vs 20% normal) — provides ~4.2s headroom for bursty sources like Roon DSD128→DSD64 downsampling

This pattern was aligned with DirettaRendererUPnP's audio engine for consistent delivery characteristics across both projects.

## FFmpeg Decoder Architecture

FfmpegDecoder has two internal modes:

- **Mode A — avformat/AVIO** (FLAC `'f'`, MP3 `'m'`, AAC `'a'`, Ogg `'o'`, ALAC `'l'`): Custom AVIO read callback bridges the push-based Decoder interface with FFmpeg's pull-based `avformat` demuxer. `feed()` fills an internal buffer; `av_read_frame()` triggers the AVIO callback which drains it. This is the same proven path used by DirettaRendererUPnP's AudioEngine. Requires `libavformat` in addition to `libavcodec`/`libavutil`. Minimum 128KB buffered before `avformat_open_input()` to allow header probing.

- **Mode B — direct codec** (raw PCM `'p'`): No container headers → no demuxer. Data sent directly to `avcodec` with manual `block_align` alignment. Parser disabled (some FFmpeg versions split PCM data without respecting `block_align`).

### Raw PCM Pitfalls (Mode B only)
- **`block_align` is 0**: Must be set explicitly to `channels × bytes_per_sample` before `avcodec_open2()`
- **PCM parser disabled**: Force `m_parser = nullptr` for raw PCM (format code `'p'`)

## Gapless Playback Strategy

- **Shared decode cache**: `decodeCache`, `decodeCachePos`, `direttaOpened`, `audioFmt` persist across gapless same-format tracks (outside the chaining `while(true)` loop)
- **Same-format continuation**: If next track has same sample rate and channels, skip Diretta close/reopen — just keep pushing to the ring buffer
- **Format change**: Drain remaining cache, close Diretta, reopen with new format
- **STMd timing**: Sent at HTTP EOF, but decode cache may still have seconds of audio buffered — LMS handles track counter advancement

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
