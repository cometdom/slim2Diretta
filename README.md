# slim2diretta v0.2.0 (Test Version)

**Native LMS Player with Diretta Output - Mono-Process Architecture**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![C++17](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)

---

![Version](https://img.shields.io/badge/version-0.2.0_test-blue.svg)
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
| **Roon DSD** | DoP via Squeezelite `-D dop` | Automatic DoP detection |
| **Complexity** | Two processes + patch | Single process, no patch |

Both tools share the same **DirettaSync v2.0** engine for Diretta output.

### Why Use This?

- **Simplified setup**: Single binary, no Squeezelite to patch and compile
- **Native DSD playback**: DSF, DFF, and DoP (Roon) to native DSD
- **Bit-perfect streaming**: Bypasses OS audio stack entirely
- **High-resolution PCM**: Up to 768kHz/32-bit
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

### Audio Formats
- **FLAC**: Lossless decoding via libFLAC (all bit depths)
- **MP3**: Decoding via libmpg123 (internet radio, streaming)
- **AAC**: Decoding via fdk-aac with HE-AAC v2 / SBR / PS support (internet radio)
- **Ogg Vorbis**: Decoding via libvorbisfile (internet radio, streaming)
- **PCM**: WAV, AIFF containers + raw PCM (Roon)
- **Native DSD**: DSF (LSB-first) and DFF/DSDIFF (MSB-first)
- **DSD rates**: DSD64, DSD128, DSD256, DSD512
- **DoP**: Automatic detection and conversion to native DSD (Roon compatibility)
- **Bit-perfect**: Volume forced to 100%, no processing

### DSD Handling
- **Native DSD streaming**: Direct DSD bitstream from LMS (format code `d`)
- **DoP auto-detection**: Transparent conversion from PCM-wrapped DSD (Roon)
- **Container parsing**: DSF and DFF headers parsed in-stream
- **Dynamic conversion**: Planar, bit-reverse, byte-swap as needed by DAC
- **All rates**: DSD64 (2.8MHz) to DSD512 (22.6MHz)

### Low-Latency Architecture
- **DirettaSync v2.0**: Shared with squeeze2diretta and DirettaRendererUPnP
- **Lock-free ring buffers**: SPSC design with SIMD optimizations (AVX2/NEON)
- **Adaptive prebuffer**: 500ms target with flow control
- **Quick resume**: Same-format track transitions without full reconnection
- **Clean transitions**: Silence injection on pause/stop/format changes

### Slimproto Protocol
- **Clean-room implementation**: From public documentation (no GPL code copied)
- **LMS auto-discovery**: UDP broadcast on port 3483
- **Roon compatible**: Squeezebox mode with DoP support
- **Multi-instance**: Template systemd service for multiple Diretta targets

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
- **Full installation** (recommended) - Dependencies, build, and systemd service
- **Build only** - Compile slim2diretta (if dependencies are already installed)
- **Install systemd service** - Set up automatic startup
- **Update binary** - Rebuild and replace after code changes
- **Configure network** - MTU, buffers, and firewall
- **Test** - Verify installation and list Diretta targets
- **Aggressive optimization** (Fedora only) - For dedicated audio servers
- **Uninstall** - Clean removal

**Command-line options:**
```bash
./install.sh --full       # Full installation (non-interactive)
./install.sh --build      # Build only
./install.sh --service    # Install systemd service only
./install.sh --update     # Rebuild and update installed binary
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
```

**Ubuntu/Debian:**
```bash
# Required
sudo apt install -y build-essential cmake pkg-config libflac-dev
# Optional codecs (recommended for internet radio)
sudo apt install -y libmpg123-dev libvorbis-dev libfdk-aac-dev
```

**Arch/AudioLinux:**
```bash
# Required
sudo pacman -S base-devel cmake pkgconf flac
# Optional codecs (recommended for internet radio)
sudo pacman -S mpg123 libvorbis libfdk-aac
```

> **Note**: Codec libraries are optional. If a library is not found at build time, the corresponding codec is simply disabled. FLAC and PCM are always available.

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

**Architecture override** (if auto-detection fails):
```bash
cmake -DARCH_NAME=x64-linux-15v3 ..       # x64 with AVX2
cmake -DARCH_NAME=aarch64-linux-15k16 ..  # Raspberry Pi 5
```

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

## Configuration

### Command-Line Options

```
Usage: slim2diretta [OPTIONS]

Options:
  -s, --server <ip>       LMS server IP (default: auto-discover)
  -t, --target <number>   Diretta target number (required)
  -n, --name <name>       Player name (default: slim2diretta)
  -m, --mac <mac>         MAC address (default: auto-generated)
  -v, --verbose           Enable verbose debug output
  -q, --quiet             Quiet mode (warnings and errors only)
  --list-targets          List available Diretta targets and exit
  --version               Show version and exit
```

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

slim2diretta supports internet radio playback via the following codecs:

| Codec | Library | License | Status |
|-------|---------|---------|--------|
| **MP3** | libmpg123 | LGPL-2.1 | Optional (auto-detected) |
| **AAC** | fdk-aac | BSD-like | Optional (auto-detected) |
| **Ogg Vorbis** | libvorbisfile | BSD-3-Clause | Optional (auto-detected) |

All codecs include **error recovery** for robust radio streaming (automatic resync on corrupted frames, gap handling).

CMake reports the codec status during build:
```
-- Codecs:
--   FLAC:           ENABLED (always)
--   PCM:            ENABLED (always)
--   MP3:            ENABLED (libmpg123)
--   Ogg Vorbis:     ENABLED (libvorbisfile)
--   AAC:            ENABLED (fdk-aac)
```

To disable a specific codec: `cmake -DENABLE_MP3=OFF ..`

---

## Roon Compatibility

slim2diretta works with **Roon** in Squeezebox emulation mode:

- Roon uses an older Slimproto protocol (LMS 6.0.x era)
- PCM is limited to **24-bit / 192kHz**
- DSD is sent as **DoP** (DSD over PCM), up to **DSD128**
- slim2diretta **automatically detects DoP** and converts to native DSD

No special configuration needed for Roon. Simply enable Squeezebox support in Roon and slim2diretta will appear as a player.

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

*Last updated: 2026-02-27 (v0.2.0 - Test Version)*
