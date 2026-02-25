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
    -> Decoder (FLAC/PCM/DSD)
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
| `src/DsdProcessor.cpp/h` | DSD conversions (interleaved->planar, DoP->native) |
| `diretta/DirettaSync.cpp/h` | Diretta SDK wrapper (shared with squeeze2diretta) |
| `diretta/DirettaRingBuffer.h` | Lock-free SPSC ring buffer |
| `diretta/globals.cpp/h` | Logging configuration |
| `diretta/LogLevel.h` | Centralized log level system |
| `diretta/FastMemcpy*.h` | SIMD memory operations |

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

SDK locations searched (in order):
1. `$DIRETTA_SDK_PATH`
2. `~/DirettaHostSDK_148` or `~/DirettaHostSDK_147`
3. `./DirettaHostSDK_148` or `./DirettaHostSDK_147`
4. `/opt/DirettaHostSDK_148` or `/opt/DirettaHostSDK_147`

## Important Notes

- Requires root/sudo for real-time thread priority
- Linux only
- Diretta SDK is personal-use only - never commit SDK files
- Volume forced to 100% for bit-perfect playback
- The `diretta/` folder is shared code with squeeze2diretta and DirettaRendererUPnP
- No automated tests - manual testing with LMS + DAC
