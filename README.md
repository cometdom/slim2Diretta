# slim2diretta v1.2.6

**Native LMS Player with Diretta Output - Mono-Process Architecture**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

![Version](https://img.shields.io/badge/version-1.2.6-blue.svg)
![DSD](https://img.shields.io/badge/DSD-Native-green.svg)
![SDK](https://img.shields.io/badge/SDK-DIRETTA::Sync-orange.svg)

---

## Support This Project

If you find this tool valuable, you can support development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cometdom)

**Important notes:**
- Donations are **optional** and appreciated
- Help cover test equipment and coffee
- **No guarantees** for features, support, or timelines
- The project remains free and open source for everyone

---

## IMPORTANT - PERSONAL USE ONLY

This tool uses the **Diretta Host SDK**, which is proprietary software by Yu Harada available for **personal use only**. Commercial use is strictly prohibited. See [LICENSE](LICENSE) for details.

---

## Overview

**slim2diretta** is a native LMS (Lyrion Music Server) player with Diretta output in a single-process architecture. It replaces the combination of **Squeezelite + squeeze2diretta** with a single binary that implements the Slimproto protocol directly and feeds DirettaSync without an intermediate pipe.

### What is This?

A standalone player that:
1. Implements the **Slimproto protocol** natively (clean-room, no GPL code)
2. Connects directly to **LMS** or **Roon** (Squeezebox mode) as a player
3. Decodes **FLAC**, **MP3**, **AAC**, **Ogg Vorbis**, **PCM/WAV/AIFF**, and **DSD** (DSF/DFF/DoP)
4. Streams audio to a **Diretta Target** using DirettaSync v2.0

### Why Use This Instead of squeeze2diretta?

| | squeeze2diretta | slim2diretta |
|---|---|---|
| **Architecture** | Wrapper (Squeezelite + pipe) | Mono-process (single binary) |
| **Slimproto** | Delegated to Squeezelite | Native implementation |
| **Dependencies** | Squeezelite + many audio libs | libFLAC + optional codecs |
| **Format changes** | In-band headers via pipe | Direct internal signaling |
| **Roon DSD** | DoP via Squeezelite `-D dop` | Automatic DoP detection DSD64 only |
| **Complexity** | Two processes + patch | Single process, no patch |

Both tools share the same **DirettaSync v2.0** engine for Diretta output.

### Why Use This?

- **Simplified setup**: Single binary, no Squeezelite to patch and compile
- **Native DSD playback**: DSF, DFF, and DoP passthrough (Roon compatibility)
- **Bit-perfect streaming**: Bypasses OS audio stack entirely
- **High-resolution PCM**: Up to 1536kHz/32-bit
- **Low latency**: DirettaSync v2.0 with lock-free ring buffers and SIMD
- **LMS + Roon compatible**: Works with both servers simultaneously (different instances)

---

## Architecture

```
+---------------------------+
|  Lyrion Music Server      |  (or Roon in Squeezebox mode)
+-------------+-------------+
              |
              | Slimproto (TCP 3483)
              | + HTTP Streaming (TCP 9000)
              v
+-------------------------------------------------------------+
|  slim2diretta (single process)                              |
|                                                             |
|  +------------------+        +---------------------------+  |
|  | SlimprotoClient  |        |      DirettaSync v2.0     |  |
|  | (TCP control)    |        |                           |  |
|  +--------+---------+        |  - Format negotiation     |  |
|           |                  |  - DSD conversions         |  |
|  +--------+---------+        |  - SIMD optimizations     |  |
|  | HttpStreamClient |        |  - Lock-free ring buffer  |  |
|  | (HTTP audio)     |        +-------------+-------------+  |
|  +--------+---------+                      |                |
|           |                                |                |
|  +--------+---------+                      |                |
|  | Decoder           |                     |                |
|  | - FLAC (libFLAC)  +--------------------+                |
|  | - MP3 (libmpg123) |                                     |
|  | - AAC (fdk-aac)   |                                     |
|  | - OGG (libvorbis) |                                     |
|  | - PCM/WAV/AIFF    |                                     |
|  | - DSD (DSF/DFF)   |                                     |
|  | - DoP detection   |                                     |
|  |                   |                                     |
|  | FFmpeg backend:   |                                     |
|  | - --decoder ffmpeg|                                     |
|  | - libavcodec      |                                     |
|  +-------------------+                                     |
+--------------------------------------------+---------------+
                                             |
                        Diretta Protocol (UDP/Ethernet)
                                             |
                                             v
                         +-------------------+-------------------+
                         |           Diretta TARGET              |
                         |Audiolinux, GentooPlayer, DDC-0/DDC-00 |
                         +-------------------+-------------------+
                                             |
                                             v
                                    +--------+--------+
                                    |       DAC       |
                                    +-----------------+
```

---

## Features

### Audio Formats and Decoders

- **PCM**: WAV, AIFF, and raw PCM (Roon), up to **1536 kHz / 32-bit**
- **FLAC**: lossless via libFLAC, all bit depths
- **MP3 / AAC / Ogg Vorbis**: optional, for internet radio (libmpg123, fdk-aac, libvorbisfile)
- **Native DSD**: DSF (LSB-first), DFF/DSDIFF (MSB-first), **DSD64 to DSD1024**
- **DoP (DSD over PCM)**: auto-detected and passed through as 24-bit PCM to the Diretta Target, which forwards DoP markers to the DAC (Roon compatibility, DSD64 only)
- **Bit-perfect**: volume forced to 100%, no resampling, no processing

Two decoder backends are available and can be switched at runtime:

| Backend | Option | Character |
|---|---|---|
| **Native** (default) | — | libFLAC / libmpg123 / libvorbis / fdk-aac — brighter, more detailed |
| **FFmpeg** | `--decoder ffmpeg` | libavcodec unified decoder — warmer, wider soundstage |

Both produce lossless output; the sonic difference is subtle and comes from internal processing patterns.

### Playback and Streaming

- **Gapless playback** for PCM, FLAC, and DSD
- **Seek support** via the LMS progress bar (FLAC, DSD)
- **Resilient startup**: both Diretta target discovery and LMS auto-discovery retry indefinitely with periodic status logging
- **Auto-release**: Diretta target released after 5 s idle so other Diretta hosts can coexist
- **Quick resume**: same-format track transitions skip the full Diretta reconnection
- **Multi-instance**: template systemd service for multiple Diretta targets

### Low-Latency Architecture

- **DirettaSync v2.0** shared with squeeze2diretta and DirettaRendererUPnP
- **Lock-free SPSC ring buffers** with AVX2 / NEON SIMD optimizations
- **Adaptive prebuffer**: 500 ms (standard) / 3000 ms (≥ 176 kHz)
- **Multi-chunk push** (2048-frame chunks) at high sample rates, event-based flow control
- **Clean-room Slimproto** implementation from public documentation — no GPL code copied
- **Roon compatibility** via Slimproto/Squeezebox mode with DoP passthrough
- **Optional CPU affinity** (`--cpu-audio`, `--cpu-other`) and clang+LTO build for minimum jitter

---

## Requirements

### Supported Architectures

| Architecture | Variants | Notes |
|--------------|----------|-------|
| **x64 (Intel/AMD)** | v2 (baseline), v3 (AVX2), v4 (AVX-512), zen4 | AVX2 recommended |
| **ARM64** | Standard (4KB pages), k16 (16KB pages) | Raspberry Pi 4/5 supported |
| **RISC-V** | Experimental | riscv64 |

### Platform Support

| Platform | Status |
|----------|--------|
| **Linux x64** | Fully supported (Fedora, Ubuntu, Arch, AudioLinux) |
| **Linux ARM64** | Fully supported (Raspberry Pi 4/5) |
| **Windows** | Not supported |
| **macOS** | Not supported |

### Hardware
- **Minimum**: Dual-core CPU, 512MB RAM, Gigabit Ethernet
- **Recommended**: Quad-core CPU, 1GB RAM, 2.5/10G Ethernet with jumbo frames
- **Network**: Gigabit Ethernet minimum (10G recommended for DSD256+)

### Software Requirements
- **OS**: Linux with kernel 4.x+
- **Diretta Host SDK**: Version 148 or 147 ([download here](https://www.diretta.link/hostsdk.html))
- **LMS**: Lyrion Music Server 7.x+ running on your network (or Roon with Squeezebox mode)
- **Build tools**: gcc/g++ 7.0+, make, CMake 3.10+
- **Required library**: libFLAC
- **Optional libraries** (for internet radio): libmpg123 (MP3), libvorbis (Ogg), fdk-aac (AAC)
- **Optional library** (for FFmpeg backend): libavcodec, libavutil

---

## Upgrading

Generic upgrade procedure from any previous version:

```bash
# 1. Stop the service
sudo systemctl stop slim2diretta@1

# 2. Pull the latest version
cd ~/slim2diretta
git pull

# 3. Rebuild and install (picks up any new dependencies)
./install.sh --update
# or manually: cd build && cmake .. && make -j$(nproc) && sudo cp slim2diretta /usr/local/bin/

# 4. Restart the service
sudo systemctl start slim2diretta@1
```

If a new release introduces a new optional dependency (e.g. a new codec or the FFmpeg backend), install it first with `./install.sh --codecs`. See [CHANGELOG.md](CHANGELOG.md) for per-version details and any migration notes.

---

## Quick Start

### Option A: Interactive Installer (Recommended)

```bash
# 1. Download Diretta Host SDK first
#    Visit: https://www.diretta.link/hostsdk.html
#    Extract to: ~/DirettaHostSDK_148

# 2. Clone repository
git clone https://github.com/cometdom/slim2diretta.git
cd slim2diretta

# 3. Run interactive installer
chmod +x install.sh
./install.sh
```

> **Tip: Transferring files from Windows to Linux**
>
> If you downloaded the SDK on Windows and need to transfer it to your Linux machine:
>
> **Using PowerShell or CMD** (OpenSSH is built into Windows 10/11):
> ```powershell
> scp C:\Users\YourName\Downloads\DirettaHostSDK_XXX_Y.tar.zst user@linux-ip:~/
> ```
>
> **Using WSL** (Windows Subsystem for Linux):
> ```bash
> cp /mnt/c/Users/YourName/Downloads/DirettaHostSDK_*.tar.zst ~/
> ```
>
> Then extract on Linux:
> ```bash
> cd ~
> tar --zstd -xf DirettaHostSDK_*.tar.zst
> ```

The installer provides an interactive menu with options for:
- **Full installation** (recommended) - Dependencies, optional codecs, build, and systemd service
- **Build only** - Compile slim2diretta (if dependencies are already installed)
- **Install systemd service** - Set up automatic startup
- **Update binary** - Rebuild and replace after code changes
- **Configure network** - MTU, buffers, and firewall
- **Test** - Verify installation and list Diretta targets
- **Install web configuration UI** - Browser-based settings (port 8081)
- **Install optional codecs** - MP3, OGG, AAC libraries and FFmpeg backend
- **Aggressive optimization** (Fedora only) - For dedicated audio servers
- **Uninstall** - Clean removal

**Command-line options:**
```bash
./install.sh --full       # Full installation (non-interactive)
./install.sh --build      # Build only
./install.sh --service    # Install systemd service only
./install.sh --update     # Rebuild and update installed binary
./install.sh --webui      # Install web configuration UI
./install.sh --codecs     # Install optional codec libraries (MP3, OGG, AAC, FFmpeg)
./install.sh --uninstall  # Remove slim2diretta
./install.sh --help       # Show all options
```

---

### Option B: Manual Installation

#### 1. Install Build Dependencies

**Fedora:**
```bash
# Required
sudo dnf install -y gcc-c++ make cmake pkg-config flac-devel
# Optional codecs (recommended for internet radio)
sudo dnf install -y mpg123-devel libvorbis-devel fdk-aac-free-devel
# Optional FFmpeg backend
sudo dnf install -y ffmpeg-free-devel
```

**Ubuntu/Debian:**
```bash
# Required
sudo apt install -y build-essential cmake pkg-config libflac-dev
# Optional codecs (recommended for internet radio)
sudo apt install -y libmpg123-dev libvorbis-dev libfdk-aac-dev
# Optional FFmpeg backend
sudo apt install -y libavcodec-dev libavutil-dev
```

**Arch/AudioLinux:**
```bash
# Required
sudo pacman -S base-devel cmake pkgconf flac
# Optional codecs (recommended for internet radio)
sudo pacman -S mpg123 libvorbis libfdk-aac
# Optional FFmpeg backend
sudo pacman -S ffmpeg
```

> **Note**: Codec libraries are optional. If a library is not found at build time, the corresponding codec is simply disabled. FLAC and PCM are always available. The FFmpeg backend is also optional — if not installed, the `--decoder ffmpeg` option is unavailable.

#### 2. Download Diretta Host SDK

1. Visit [diretta.link](https://www.diretta.link/hostsdk.html)
2. Download **DirettaHostSDK_148** (or latest version)
3. Extract to one of these locations:
   - `~/DirettaHostSDK_148`
   - `/opt/DirettaHostSDK_148`
   - Or set `DIRETTA_SDK_PATH` environment variable

#### 3. Clone and Build

```bash
git clone https://github.com/cometdom/slim2diretta.git
cd slim2diretta
mkdir build && cd build
cmake ..
make -j$(nproc)
```

See [Build Options](#build-options) below for clang+LTO, architecture overrides, verbose output, and individual codec toggles.

#### 4. Find Your Diretta Target

```bash
sudo ./build/slim2diretta --list-targets
```

Output example:
```
Found 2 Diretta target(s):
  [1] DDC-0_8A60 (192.168.1.50)
  [2] GentooPlayer_AB12 (192.168.1.51)
```

#### 5. Run slim2diretta

```bash
# Basic usage (auto-discover LMS)
sudo ./build/slim2diretta --target 1

# With specific LMS server and player name
sudo ./build/slim2diretta -s 192.168.1.100 --target 1 -n "Living Room"

# Verbose mode (for troubleshooting)
sudo ./build/slim2diretta -s 192.168.1.100 --target 1 -v
```

#### 6. Install as Systemd Service

```bash
# Copy files
sudo cp build/slim2diretta /usr/local/bin/
sudo cp slim2diretta@.service /etc/systemd/system/
sudo cp slim2diretta.default /etc/default/slim2diretta

# Edit configuration
sudo nano /etc/default/slim2diretta

# Enable and start (replace 1 with your target number)
sudo systemctl daemon-reload
sudo systemctl enable --now slim2diretta@1
```

#### 7. Connect from LMS

1. Open LMS web interface (usually http://lms-server:9000)
2. Go to Settings -> Player
3. You should see "slim2diretta" as a player
4. **Set volume to "Fixed at 100%"** (Settings -> Player -> Audio -> Volume Control)
5. Start playing music!

> **Important: Fixed Volume at 100%**
>
> slim2diretta forces volume to 100% for bit-perfect playback. Control volume from your amplifier or DAC.
>
> When digital volume is applied by the server:
> - **DSD playback breaks** (server converts DSD to PCM to apply volume)
> - **Bit-perfect PCM breaks** (samples are altered)
>
> Configure fixed volume:
> - **LMS**: Settings -> Player -> Audio -> Volume Control -> "Fixed at 100%"
> - **Roon**: Device Setup -> Volume Control -> "Fixed Volume"

---

## Build Options

Two build modes are supported. Advanced users can combine CMake options directly.

### Default build (gcc)

```bash
./install.sh -b
# or: mkdir build && cd build && cmake .. && make -j$(nproc)
```

### clang + LTO + lld (recommended for audio quality)

Multiple testers report a clearly better sound with clang+LTO builds compared to gcc. Enable it with a single environment variable:

```bash
env LLVM=1 ./install.sh -b
# or directly with cmake:
LLVM=1 cmake .. && make -j$(nproc)
```

The `LLVM=1` shortcut mirrors the convention used by the Linux kernel and DirettaRendererUPnP. It forces `clang`/`clang++`, enables `-flto`, and uses the `lld` linker. Requires `clang`, `clang++`, and `lld` to be installed:

| Distribution | Command |
|---|---|
| **Fedora** | `sudo dnf install clang lld` |
| **Ubuntu/Debian** | `sudo apt install clang lld` |
| **Arch** | `sudo pacman -S clang lld` |

### Verbose build output

Useful for debugging build failures — shows the full compiler command lines:

```bash
env VERBOSE=1 ./install.sh -b
# combinable with LLVM=1:
env LLVM=1 VERBOSE=1 ./install.sh -b
```

`V=1` is accepted as a synonym for `VERBOSE=1`.

### Advanced CMake options

Invoke `cmake` directly for fine-grained control:

```bash
# Architecture override (if auto-detection fails)
cmake -DARCH_NAME=x64-linux-15v3 ..       # x64 with AVX2
cmake -DARCH_NAME=aarch64-linux-15k16 ..  # Raspberry Pi 5 / 16KB pages

# LTO and linker (what LLVM=1 does under the hood)
CC=clang CXX=clang++ cmake -DENABLE_LTO=ON -DUSE_LLD=ON ..

# Disable individual codecs
cmake -DENABLE_MP3=OFF ..
cmake -DENABLE_OGG=OFF ..
cmake -DENABLE_AAC=OFF ..
cmake -DENABLE_FFMPEG=OFF ..   # Disable FFmpeg backend
```

CMake reports the active codecs and options at the end of the configure step:

```
-- Codecs:
--   FLAC:           ENABLED (always)
--   PCM:            ENABLED (always)
--   MP3:            ENABLED (libmpg123)
--   Ogg Vorbis:     ENABLED (libvorbisfile)
--   AAC:            ENABLED (fdk-aac)
-- Backends:
--   FFmpeg:         ENABLED (--decoder ffmpeg)
-- Build:
--   Compiler:       /usr/bin/clang++
--   LTO:            ENABLED
--   Target:         slim2diretta
```

---

## Configuration

### Command-Line Options

```
Usage: slim2diretta [OPTIONS]

Options:
  -s, --server <ip>              LMS server IP (default: auto-discover)
  -t, --target <number>          Diretta target number (required)
  -n, --name <name>              Player name (default: slim2diretta)
  -m, --mac <mac>                MAC address (default: auto-generated)
  -v, --verbose                  Enable verbose debug output
  -q, --quiet                    Quiet mode (warnings and errors only)
  --list-targets                 List available Diretta targets and exit
  --version                      Show version and exit
  --max-rate <hz>                Max PCM sample rate (default: 1536000)
  --no-dsd                       Disable DSD support
  --decoder <backend>            Decoder backend: native (default), ffmpeg

Diretta Advanced Options:
  --transfer-mode <mode>         Transfer scheduling mode (default: auto)
  --info-cycle <us>              Info packet cycle time in us (default: 100000)
  --cycle-time <us>              Transfer packet cycle max time in us (default: 10000)
  --cycle-min-time <us>          Transfer packet cycle min time in us (random mode only)
  --target-profile-limit <us>    Target profile limit (0=self, default: 200)
  --thread-mode <bitmask>        SDK thread mode bitmask (default: 1)
  --mtu <bytes>                  MTU size (default: auto-detect)

CPU Affinity (optional):
  --cpu-audio <core>             Pin SDK worker + Diretta hot path to this core
  --cpu-other <core>             Pin audio/decode/slimproto threads to this core
```

### CPU Affinity (Thread Pinning)

`--cpu-audio` and `--cpu-other` pin specific threads to dedicated CPU cores to reduce jitter and improve real-time performance. This is particularly beneficial on systems with CPU isolation (`isolcpus` kernel parameter).

- `--cpu-audio <core>`: pins the Diretta SDK worker thread (the hot path that sends packets to the target) to the specified core. Automatically enables the SDK `OCCUPIED` thread mode flag (bit 16).
- `--cpu-other <core>`: pins the audio/decode thread (HTTP reader + decoder + ring buffer push) and the Slimproto TCP receive thread to the specified core.

**Example**: on an 8-core system with cores 2 and 3 isolated via `isolcpus=2,3`:
```bash
sudo slim2diretta --target 1 --cpu-audio 2 --cpu-other 3
```

Both options also accept the value via the Web UI (CPU Affinity section).

### Configuration File (/etc/default/slim2diretta)

When using the systemd service, options are set in `/etc/default/slim2diretta`:

```bash
# LMS server (omit for auto-discovery):
#SLIM2DIRETTA_OPTS="-s 192.168.1.100"

# With player name:
#SLIM2DIRETTA_OPTS="-s 192.168.1.100 -n Living Room"

# Verbose + auto-discovery:
#SLIM2DIRETTA_OPTS="-n Living Room -v"

# Default: auto-discover LMS, default player name
SLIM2DIRETTA_OPTS=""
```

The target number is set by the systemd instance name:
```bash
systemctl start slim2diretta@1    # --target 1
systemctl start slim2diretta@2    # --target 2
```

### Diretta Advanced Tuning

These options control the low-level behavior of the Diretta SDK transport layer. Default values work well for most systems — only change them if you understand the implications.

#### Transfer Mode (`--transfer-mode`)

Controls how audio packets are scheduled for transmission.

| Mode | Description |
|------|-------------|
| `auto` | SDK chooses automatically (default) |
| `varauto` | Flex cycle mode — adaptive packet timing |
| `varmax` | Flex cycle mode — packets filled to maximum |
| `fixauto` | Fixed cycle mode — constant packet timing |
| `random` | Random cycle mode — use with `--cycle-min-time` |

#### Cycle Time (`--cycle-time` / `--cycle-min-time`)

- **`--cycle-time <us>`**: Transfer packet cycle maximum time in microseconds (default: 10000). This is the maximum interval between packet transmissions.
- **`--cycle-min-time <us>`**: Transfer packet cycle minimum time in microseconds. Only used with `random` transfer mode, defines the lower bound of the random interval.

#### Info Cycle (`--info-cycle`)

Information packet cycle time in microseconds (default: 100000 = 100ms). Controls how often the SDK exchanges status information with the Diretta target.

#### Target Profile (`--target-profile-limit`)

Controls how the SDK profiles and adapts to system performance.

| Value | Behavior |
|-------|----------|
| `0` | **SelfProfile** — host-side profiling only |
| `> 0` | **TargetProfile** — with limit in microseconds (default: 200) |

With TargetProfile, the SDK automatically adapts to your system's capabilities. If Diretta detects high load (dropped packets), it falls back to lighter processing to maintain stable streaming.

#### Thread Mode (`--thread-mode`)

Bitmask controlling SDK thread behavior. Combine flags by adding their values.

| Value | Flag | Description |
|-------|------|-------------|
| 1 | Critical | High priority thread scheduling |
| 2 | NoShortSleep | Disable short sleep intervals |
| 4 | NoSleep4Core | No sleep on 4+ core systems |
| 8 | SocketNoBlock | Non-blocking socket mode |
| 16 | OccupiedCPU | CPU occupation mode (busy-wait) |
| 32 | Feedback | Moving average feedback (level 1) |
| 64 | Feedback | Moving average feedback (level 2) |
| 128 | Feedback | Moving average feedback (level 3) |
| 256 | NoFastFeedback | Disable fast feedback |
| 512 | IdleOne | Idle on one core |
| 1024 | IdleAll | Idle on all cores |
| 2048 | NoSleepForce | Force no-sleep mode |
| 4096 | LimitResend | Limit packet resend |
| 8192 | NoJumboFrame | Disable jumbo frame support |
| 16384 | NoFirewall | Bypass firewall detection |
| 32768 | NoRawSocket | Disable raw socket usage |

**Examples**: `--thread-mode 1` (Critical only, default), `--thread-mode 3` (Critical + NoShortSleep), `--thread-mode 5` (Critical + NoSleep4Core).

---

## Systemd Service

### Service Management

```bash
# Start (replace 1 with your target number)
sudo systemctl start slim2diretta@1

# Stop
sudo systemctl stop slim2diretta@1

# Restart
sudo systemctl restart slim2diretta@1

# Enable auto-start on boot
sudo systemctl enable slim2diretta@1

# Check status
sudo systemctl status slim2diretta@1

# View logs (real-time)
sudo journalctl -u slim2diretta@1 -f

# View last 50 log lines
sudo journalctl -u slim2diretta@1 -n 50
```

### Multiple Instances

Run multiple players for different Diretta targets:

```bash
# Enable and start multiple zones
sudo systemctl enable --now slim2diretta@1   # Target 1: Living Room
sudo systemctl enable --now slim2diretta@2   # Target 2: Bedroom
```

Each instance appears as a separate player in LMS.

### Runtime Statistics

Send SIGUSR1 to get a real-time statistics dump:

```bash
sudo kill -USR1 $(pgrep -f "slim2diretta.*--target 1")
sudo journalctl -u slim2diretta@1 -n 20
```

---

## Internet Radio Support

slim2diretta supports internet radio via the optional MP3, AAC, and Ogg Vorbis codecs (libmpg123, fdk-aac, libvorbisfile). All three include automatic resync on corrupted frames and gap handling for robust streaming. Install them before running `cmake` so they're picked up — see [Build Options](#build-options) for how to toggle them individually.

---

## Roon Compatibility

slim2diretta works with **Roon** in Squeezebox emulation mode:

- Roon uses an older Slimproto protocol (LMS 6.0.x era)
- PCM is limited to **24-bit / 192kHz**
- DSD is sent as **DoP** (DSD over PCM), up to **DSD128**
- slim2diretta **automatically detects DoP** and passes it through as 24-bit PCM to the Diretta Target, which handles the DoP→DAC forwarding

No special configuration needed for Roon. Simply enable Squeezebox support in Roon and slim2diretta will appear as a player.

---

## Web Configuration UI

Configure slim2diretta from your browser — no SSH or manual file editing needed.

### Installation

```bash
# Via installer menu (option 7)
./install.sh

# Or directly via command line
./install.sh --webui
```

### Usage

Once installed, access the web UI at:
```
http://<your-ip>:8081
```

**Features:**
- Edit all settings (LMS server IP, player name, verbose mode)
- Advanced Diretta SDK settings (thread-mode, transfer-mode, cycle-time, etc.)
- **Save & Restart** — applies settings and restarts the service in one click
- **Restart Only** — restart the service without changing settings

**Service management:**
```bash
sudo systemctl status slim2diretta-webui
sudo systemctl stop slim2diretta-webui
sudo systemctl restart slim2diretta-webui
```

> **Note:** The web UI runs as a separate Python process (`slim2diretta-webui.service`) on port 8081 and has zero impact on audio quality or latency. Port 8081 avoids conflict with DirettaRendererUPnP web UI (port 8080) when both run on the same machine.

---

## Troubleshooting

### Player not appearing in LMS
- Check that slim2diretta is running: `sudo systemctl status slim2diretta@1`
- Verify network connectivity: `ping <lms-ip>`
- Check firewall: ports 3483/tcp, 3483/udp, 9000/tcp must be open
- Try specifying LMS IP directly: `-s 192.168.1.100`

### No sound
- Verify Diretta target is powered on and connected
- List targets: `sudo slim2diretta --list-targets`
- Check MTU matches between host and target
- Run in verbose mode (`-v`) and check for errors

### DSD not playing
- **From LMS**: Ensure DSD files (DSF/DFF) are in your library
- **From Roon**: DSD is sent as DoP automatically, check for "DoP detected" in verbose log
- Check DAC supports DSD via Diretta

### Connection drops
- Check LMS server is stable
- Verify no firewall blocking Slimproto traffic
- slim2diretta reconnects automatically on connection loss

---

## Credits

### Author
**Dominique COMET** ([@cometdom](https://github.com/cometdom)) - slim2diretta development

### Core Technologies

- **DirettaSync v2.0** - From [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP)
  - Low-latency architecture by Dominique COMET
  - Core Diretta integration by **SwissMountainsBear** (ported from [MPD Diretta Output Plugin](https://github.com/swissmountainsbear/mpd-diretta-output-plugin))
  - Performance optimizations by **leeeanh** (lock-free ring buffers, SIMD, cache-line separation)

- **Diretta Protocol & SDK** - **Yu Harada** ([diretta.link](https://www.diretta.link))

- **Lyrion Music Server** - Open source audio streaming server

### Slimproto Implementation

Clean-room implementation from public documentation:
- [wiki.lyrion.org](https://wiki.lyrion.org) - Protocol specification
- [Rust slimproto crate](https://crates.io/crates/slimproto) (MIT) - Reference
- [Ada slimp](https://github.com/music210/slimp) (MIT) - Reference

**No code copied from Squeezelite (GPL v3).**

### Special Thanks

- **SwissMountainsBear** - For the `DIRETTA::Sync` architecture and `getNewStream()` callback implementation from his MPD plugin

- **leeeanh** - For lock-free SPSC ring buffers, power-of-2 sizing, cache-line separation, and AVX2 SIMD optimizations

- **Yu Harada** - Creator of Diretta protocol and SDK

- **Beta testers** - For their patience and valuable feedback

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

**IMPORTANT**: The Diretta Host SDK is proprietary software by Yu Harada and is licensed for **personal use only**. Commercial use is prohibited.

---

## See Also

- [squeeze2diretta](https://github.com/cometdom/squeeze2diretta) - Squeezelite wrapper architecture (alternative approach)
- [DirettaRendererUPnP](https://github.com/cometdom/DirettaRendererUPnP) - UPnP/DLNA renderer with Diretta output
- [CHANGELOG.md](CHANGELOG.md) - Version history

---

**Enjoy native DSD and hi-res PCM streaming from your LMS library!**

*Last updated: 2026-04-14 (v1.2.6)*
