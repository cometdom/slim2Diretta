#!/bin/bash
#
# slim2diretta - Installation Script
#
# This script helps install dependencies and set up slim2diretta.
# Run with: bash install.sh
#

set -e  # Exit on error

# =============================================================================
# CONFIGURATION
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_VERSION="1.1.0"
INSTALL_BIN="/usr/local/bin"
SERVICE_FILE="/etc/systemd/system/slim2diretta.service"
CONFIG_FILE="/etc/default/slim2diretta"

# Auto-detect latest Diretta SDK version
detect_latest_sdk() {
    local sdk_found=$(find "$HOME" . .. /opt "$HOME/audio" /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -V | tail -1 | xargs realpath 2>/dev/null)

    if [ -n "$sdk_found" ]; then
        echo "$sdk_found"
    else
        echo "$HOME/DirettaHostSDK"
    fi
}

SDK_PATH="${DIRETTA_SDK_PATH:-$(detect_latest_sdk)}"
FFMPEG_BUILD_DIR="/tmp/ffmpeg-build-slim2diretta"
FFMPEG_8_VERSION="8.1.2"

# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

print_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
print_header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

confirm() {
    local prompt="$1"
    local default="${2:-N}"
    local response

    if [[ "$default" =~ ^[Yy]$ ]]; then
        read -p "$prompt [Y/n]: " response
        response=${response:-Y}
    else
        read -p "$prompt [y/N]: " response
        response=${response:-N}
    fi

    [[ "$response" =~ ^[Yy]$ ]]
}

# =============================================================================
# SYSTEM DETECTION
# =============================================================================

detect_system() {
    print_header "System Detection"

    if [ "$EUID" -eq 0 ]; then
        print_error "Please do not run this script as root"
        print_info "The script will ask for sudo password when needed"
        exit 1
    fi

    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        print_success "Detected: $PRETTY_NAME"
    else
        print_error "Cannot detect Linux distribution"
        exit 1
    fi

    # Detect architecture
    ARCH=$(uname -m)
    print_info "Architecture: $ARCH"
}

# =============================================================================
# BUILD DEPENDENCIES
# =============================================================================

install_dependencies() {
    print_header "Installing Build Dependencies"

    case $OS in
        fedora|rhel|centos)
            print_info "Using DNF package manager..."
            sudo dnf install -y \
                gcc-c++ \
                make \
                cmake \
                pkg-config \
                ethtool \
                flac-devel
            ;;
        ubuntu|debian)
            print_info "Using APT package manager..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                cmake \
                pkg-config \
                ethtool \
                libflac-dev
            ;;
        arch|archarm|manjaro)
            print_info "Using Pacman package manager..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                cmake \
                pkgconf \
                ethtool \
                flac
            ;;
        *)
            print_error "Unsupported distribution: $OS"
            print_info "Please install dependencies manually:"
            print_info "  - gcc/g++ (C++17 compiler)"
            print_info "  - cmake (>= 3.10)"
            print_info "  - make"
            print_info "  - pkg-config"
            print_info "  - libFLAC development headers"
            exit 1
            ;;
    esac

    print_success "Build dependencies installed"
}

# =============================================================================
# FFMPEG FROM SOURCE (full DSD/codec decoder set)
# =============================================================================
#
# slim2diretta's own FfmpegDecoder never touches DSD codec IDs (DSD is parsed
# natively — see DsdProcessor.cpp/DsdStreamReader.cpp), so this build isn't
# needed for slim2diretta's own playback. It exists because `ffmpeg-free-devel`
# below installs a *system-wide* shared library (/usr/lib64/libavcodec.so),
# and any other program on the same host that links against system FFmpeg —
# DirettaRendererUPnP included — inherits whatever decoder set that package
# was built with. Confirmed 2026-09-08: Fedora's ffmpeg-free-devel omits
# dsd_lsbf_planar/dsd_msbf_planar (the ones a real .dsf file actually needs,
# DSF is always planar); installing it here silently broke DSD playback in
# DirettaRendererUPnP on the same machine, overwriting a complete FFmpeg that
# had been built from source previously. This option avoids that by building
# the same complete decoder set DirettaRendererUPnP's own installer does.

# Detect library directory (lib vs lib64). Fedora/RHEL use /usr/lib64 on
# every 64-bit arch they ship (x86_64, aarch64, ppc64le), not just x86_64 —
# gating this on uname -m = x86_64 sends aarch64 Fedora hosts (Raspberry Pi)
# down the /usr/lib path while pkg-config still only searches /usr/lib64,
# breaking downstream FFmpeg version detection for anything that later reads
# pkg-config on this host (same bug hit DirettaRendererUPnP's install.sh on
# Fedora 44 aarch64, 2026-09-09 — fixed there identically).
# Debian/Ubuntu (any arch) don't have a real /usr/lib64, so this still falls
# through to /usr/lib there exactly as before.
get_libdir() {
    if [ -d "/usr/lib64" ]; then
        echo "/usr/lib64"
    else
        echo "/usr/lib"
    fi
}

# Minimal FFmpeg 8.x configure options — same decoder/demuxer set as
# DirettaRendererUPnP's install.sh, kept identical on purpose: the point is
# for both projects to produce (and never silently downgrade) the same
# system-wide FFmpeg when they share a host.
get_ffmpeg_8_minimal_opts() {
    local libdir=$(get_libdir)
    cat <<OPTS
--prefix=/usr
--libdir=$libdir
--enable-shared
--disable-static
--enable-lto
--enable-gpl
--enable-version3
--enable-gnutls
--disable-everything
--disable-doc
--disable-avdevice
--disable-swscale
--enable-protocol=file,http,https,tcp,udp,hls
--enable-demuxer=flac,wav,aiff,dsf,aac,mov,mp3,ogg,hls,mpegts,pcm_s16be
--enable-decoder=flac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,pcm_s32be,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,aac,aac_fixed,aac_latm,mp3,mp3float,vorbis
--enable-parser=aac,aac_latm,mpegaudio,vorbis
--enable-muxer=flac,wav
--enable-filter=aresample
OPTS
}

install_ffmpeg_8_build_deps() {
    print_info "Installing minimal FFmpeg 8.x build dependencies..."

    case $OS in
        fedora|rhel|centos)
            sudo dnf install -y --skip-unavailable \
                gnutls-devel
            ;;
        ubuntu|debian)
            sudo apt install -y \
                libgnutls28-dev
            ;;
        arch|archarm|manjaro)
            sudo pacman -Sy --needed --noconfirm \
                gnutls
            ;;
    esac
}

# Warn (don't block) if total RAM is low for an LTO FFmpeg build — see
# DirettaRendererUPnP/install.sh for the full rationale (LTO can peak at
# 4-6 GB and gets silently OOM-killed on a swapless low-RAM host).
check_compile_ram() {
    local mem_kb mem_gb
    mem_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
    if [ -z "$mem_kb" ] || [ "$mem_kb" -eq 0 ]; then
        return 0  # can't read, don't warn
    fi
    mem_gb=$(( mem_kb / 1024 / 1024 ))
    if [ "$mem_gb" -lt 8 ]; then
        print_warning "Low system memory detected: ${mem_gb} GB total."
        print_warning "Building FFmpeg with LTO can peak at 4-6 GB; the linker may be OOM-killed without swap."
        print_info    "If the build fails near the link stage, create a temporary 4 GB swap file:"
        print_info    "  sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile"
        print_info    "  sudo mkswap /swapfile && sudo swapon /swapfile"
        print_info    "Then re-run the install. After it succeeds:"
        print_info    "  sudo swapoff /swapfile && sudo rm /swapfile"
    fi
}

build_ffmpeg_8_minimal() {
    local version="$1"

    print_info "Building FFmpeg $version (minimal audio-only, full DSD)..."
    check_compile_ram

    install_ffmpeg_8_build_deps

    mkdir -p "$FFMPEG_BUILD_DIR"
    cd "$FFMPEG_BUILD_DIR"

    local tarball="ffmpeg-${version}.tar.xz"
    local url="https://ffmpeg.org/releases/$tarball"

    if [ ! -f "$tarball" ]; then
        print_info "Downloading FFmpeg ${version}..."
        if ! wget -q --show-progress "$url"; then
            print_error "Failed to download FFmpeg $version"
            return 1
        fi
    fi

    print_info "Extracting FFmpeg..."
    tar xf "$tarball"
    cd "ffmpeg-${version}"

    print_info "Configuring FFmpeg (minimal audio-only, full DSD)..."
    make distclean 2>/dev/null || true

    local configure_opts
    configure_opts=$(get_ffmpeg_8_minimal_opts | tr '\n' ' ')

    ./configure $configure_opts

    print_info "Building FFmpeg (this may take a while)..."
    make -j$(nproc)

    print_info "Installing FFmpeg to /usr..."
    sudo make install
    sudo ldconfig

    cd "$SCRIPT_DIR"
    rm -rf "$FFMPEG_BUILD_DIR"
}

# Confirms the installed FFmpeg actually has what real DSD/PCM files need —
# in particular the _planar decoder variants (dsf is always planar), not just
# the base ones. A build missing only the _planar pair looks fine on a naive
# "dsd_lsbf present?" check while still failing every real .dsf file.
test_ffmpeg_installation() {
    local ffmpeg_bin
    ffmpeg_bin=$(which ffmpeg 2>/dev/null || echo "")
    if [ -z "$ffmpeg_bin" ] || [ ! -x "$ffmpeg_bin" ]; then
        print_warning "ffmpeg binary not found in PATH — cannot verify"
        return 1
    fi

    print_info "FFmpeg: $("$ffmpeg_bin" -version 2>&1 | head -1)"
    print_info "Checking DSD/PCM decoders..."
    local decoders all_found=true
    decoders=$("$ffmpeg_bin" -decoders 2>&1)
    for dec in dsd_lsbf dsd_msbf dsd_lsbf_planar dsd_msbf_planar flac alac pcm_s16le pcm_s24le pcm_s32le; do
        if echo "$decoders" | grep -q " $dec "; then
            echo "  [OK] $dec"
        else
            echo "  [MISSING] $dec"
            all_found=false
        fi
    done

    if [ "$all_found" = true ]; then
        print_success "All required decoders found"
    else
        print_warning "Some decoders are missing — DSD playback may fail on other programs sharing this FFmpeg"
    fi
}

install_optional_codecs() {
    print_header "Installing Optional Codec Libraries"

    echo ""
    echo "Optional codecs extend format support beyond FLAC/PCM/DSD."
    echo "FFmpeg backend provides an alternative decoder with a different"
    echo "sonic signature (warmer, wider soundstage vs native's brighter detail)."
    echo ""
    echo "  1) All optional codecs + FFmpeg backend (recommended)"
    echo "  2) FFmpeg backend only (libavcodec)"
    echo "  3) Extra codecs only (MP3, OGG, AAC — no FFmpeg)"
    echo "  4) Skip"
    echo ""
    read -rp "Choice [1-4]: " codec_choice

    case $codec_choice in
        1) local install_ffmpeg=true; local install_codecs=true ;;
        2) local install_ffmpeg=true; local install_codecs=false ;;
        3) local install_ffmpeg=false; local install_codecs=true ;;
        4|*) print_info "Skipping optional codecs"; return 0 ;;
    esac

    # FFmpeg is a system-wide shared library: a plain package install here
    # can silently replace a more complete FFmpeg another program on this
    # host (DirettaRendererUPnP included) was relying on for DSD — see the
    # comment above build_ffmpeg_8_minimal(). Ask before touching it.
    local ffmpeg_from_source=false
    if $install_ffmpeg; then
        echo ""
        echo "FFmpeg is a shared system library — installing the distro package"
        echo "can silently replace a more complete build another program on this"
        echo "host (e.g. DirettaRendererUPnP) may depend on for DSD playback."
        echo ""
        echo "  1) Build from source (full DSD decoders — recommended if you also"
        echo "     run DirettaRendererUPnP, or any other Diretta renderer, here)"
        echo "  2) Distro package (fast, but its DSD decoder support varies)"
        echo ""
        read -rp "Choice [1-2] (default: 1): " ffmpeg_choice
        ffmpeg_choice=${ffmpeg_choice:-1}
        [ "$ffmpeg_choice" = "1" ] && ffmpeg_from_source=true
    fi

    case $OS in
        fedora|rhel|centos)
            local pkgs=""
            if $install_codecs; then
                pkgs="mpg123-devel libvorbis-devel fdk-aac-free-devel"
            fi
            if $install_ffmpeg && ! $ffmpeg_from_source; then
                pkgs="$pkgs ffmpeg-free-devel"
            fi
            if [ -n "$pkgs" ]; then
                print_info "Installing: $pkgs"
                # --allowerasing lets dnf retire conflicting FFmpeg packages
                # (e.g. ffmpeg-devel from RPM Fusion already installed) when
                # the user picks ffmpeg-free-devel, or vice-versa on a re-run.
                sudo dnf install -y --allowerasing $pkgs
            fi
            ;;
        ubuntu|debian)
            local pkgs=""
            if $install_codecs; then
                pkgs="libmpg123-dev libvorbis-dev libfdk-aac-dev"
            fi
            if $install_ffmpeg && ! $ffmpeg_from_source; then
                pkgs="$pkgs libavcodec-dev libavutil-dev"
            fi
            if [ -n "$pkgs" ]; then
                print_info "Installing: $pkgs"
                sudo apt install -y $pkgs
            fi
            ;;
        arch|archarm|manjaro)
            local pkgs=""
            if $install_codecs; then
                pkgs="mpg123 libvorbis libfdk-aac"
            fi
            if $install_ffmpeg && ! $ffmpeg_from_source; then
                pkgs="$pkgs ffmpeg"
            fi
            if [ -n "$pkgs" ]; then
                print_info "Installing: $pkgs"
                sudo pacman -Sy --needed --noconfirm $pkgs
            fi
            ;;
        *)
            print_warning "Unsupported distribution for automatic codec install"
            print_info "Install manually:"
            if $install_codecs; then
                print_info "  - libmpg123-dev, libvorbis-dev, libfdk-aac-dev"
            fi
            if $install_ffmpeg && ! $ffmpeg_from_source; then
                print_info "  - libavcodec-dev, libavutil-dev (FFmpeg)"
            fi
            if ! $ffmpeg_from_source; then
                return 0
            fi
            ;;
    esac

    if $ffmpeg_from_source; then
        build_ffmpeg_8_minimal "$FFMPEG_8_VERSION"
        test_ffmpeg_installation
    fi

    print_success "Optional codec libraries installed"
    print_info "Rebuild slim2diretta for changes to take effect"
}

# =============================================================================
# DIRETTA SDK
# =============================================================================

check_diretta_sdk() {
    print_header "Diretta SDK Check"

    # Auto-detect all DirettaHostSDK_* directories
    local sdk_candidates=()
    while IFS= read -r sdk_dir; do
        sdk_candidates+=("$sdk_dir")
    done < <(find "$HOME" . .. /opt "$HOME/audio" /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)

    # Also add SDK_PATH if set
    [ -d "$SDK_PATH" ] && sdk_candidates=("$SDK_PATH" "${sdk_candidates[@]}")

    # Try each candidate
    for loc in "${sdk_candidates[@]}"; do
        if [ -d "$loc" ] && [ -d "$loc/lib" ]; then
            SDK_PATH="$loc"
            local sdk_version=$(basename "$loc" | sed 's/DirettaHostSDK_//')
            print_success "Found Diretta SDK at: $SDK_PATH"
            [ -n "$sdk_version" ] && print_info "SDK version: $sdk_version"
            return 0
        fi
    done

    print_warning "Diretta SDK not found"
    echo ""
    echo "The Diretta Host SDK is required but not included in this repository."
    echo ""
    echo "Please download it from: https://www.diretta.link/hostsdk.html"
    echo "  1. Visit the website"
    echo "  2. Download DirettaHostSDK_XXX.tar.gz (latest version)"
    echo "  3. Extract to: $HOME/"
    echo ""
    read -p "Press Enter after you've downloaded and extracted the SDK..."

    # Check again after user extraction
    while IFS= read -r sdk_dir; do
        if [ -d "$sdk_dir" ] && [ -d "$sdk_dir/lib" ]; then
            SDK_PATH="$sdk_dir"
            print_success "Found Diretta SDK at: $SDK_PATH"
            return 0
        fi
    done < <(find "$HOME" . .. /opt -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -Vr)

    print_error "SDK still not found. Please extract it and try again."
    exit 1
}

# =============================================================================
# BUILD SLIM2DIRETTA
# =============================================================================

build_slim2diretta() {
    print_header "Building slim2diretta"

    cd "$SCRIPT_DIR"

    # Clean and create build directory
    if [ -d "build" ]; then
        print_info "Cleaning previous build..."
        rm -rf build
    fi
    mkdir -p build

    # Resolve SDK path before cd build (relative paths break after cd)
    export DIRETTA_SDK_PATH="$(realpath "$SDK_PATH")"

    local cmake_opts=""

    # Environment variable shortcut: LLVM=1 -> clang + LTO + lld
    # (matches DirettaRendererUPnP Makefile convention)
    # Without LLVM=1, build with the default compiler (gcc).
    if [ -n "$LLVM" ]; then
        if command -v clang++ >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
            cmake_opts="-DLLVM=1"
            print_info "LLVM=1 detected: building with clang + LTO + lld"
        else
            print_error "LLVM=1 set but clang/clang++ not installed"
            exit 1
        fi
    else
        print_info "Building with default compiler (gcc). For clang + LTO + lld,"
        print_info "re-run with: env LLVM=1 ./install.sh -b"
    fi

    # Environment variable shortcut: ARCH_NAME=<variant> -> override the
    # auto-detected Diretta SDK library variant (e.g. to opt into a
    # GCC16-built variant such as x64-linux-16v3 — see CLAUDE.md/README).
    if [ -n "$ARCH_NAME" ]; then
        cmake_opts="$cmake_opts -DARCH_NAME=$ARCH_NAME"
        print_info "ARCH_NAME=$ARCH_NAME: overriding auto-detected SDK library variant"
    fi

    cd build

    # Configure with CMake
    print_info "Configuring with CMake..."
    cmake $cmake_opts ..

    # Build (verbose if VERBOSE=1 or V=1 is set)
    print_info "Building slim2diretta..."
    local make_args=("-j$(nproc)")
    if [[ -n "$VERBOSE" || -n "$V" ]]; then
        make_args+=("VERBOSE=1")
        print_info "Verbose build enabled"
    fi
    make "${make_args[@]}"

    # Verify build
    if [ -f "slim2diretta" ]; then
        print_success "Build successful!"
        print_info "Binary: $SCRIPT_DIR/build/slim2diretta"
    else
        print_error "Build failed. Please check error messages above."
        exit 1
    fi

    cd "$SCRIPT_DIR"
}

# =============================================================================
# NETWORK CONFIGURATION
# =============================================================================

configure_network() {
    print_header "Network Configuration"

    echo "Available network interfaces:"
    ip link show | grep -E "^[0-9]+:" | awk '{print "  " $2}' | sed 's/://g'
    echo ""

    read -p "Enter network interface for Diretta (e.g., eth0) or press Enter to skip: " IFACE

    if [ -z "$IFACE" ]; then
        print_info "Skipping network configuration"
        return 0
    fi

    if ! ip link show "$IFACE" &> /dev/null; then
        print_error "Interface $IFACE not found"
        return 1
    fi

    if confirm "Enable jumbo frames for better DSD performance?"; then
        echo ""
        echo "Select MTU size (must match your Diretta Target setting):"
        echo ""
        echo "  1) MTU 9014  - Standard jumbo frames"
        echo "  2) MTU 16128 - Maximum jumbo frames (recommended)"
        echo "  3) Skip"
        echo ""
        read -rp "Choice [1-3]: " mtu_choice

        local MTU_VALUE=""
        case $mtu_choice in
            1) MTU_VALUE=9014 ;;
            2) MTU_VALUE=16128 ;;
            3|"")
                print_info "Skipping MTU configuration"
                ;;
            *)
                print_warning "Invalid choice, skipping MTU configuration"
                ;;
        esac

        if [ -n "$MTU_VALUE" ]; then
            sudo ip link set "$IFACE" mtu "$MTU_VALUE"
            print_success "Jumbo frames enabled (MTU $MTU_VALUE)"

            if confirm "Make this permanent?"; then
                case $OS in
                    fedora|rhel|centos)
                        local conn_name
                        conn_name=$(nmcli -t -f NAME,DEVICE connection show 2>/dev/null | grep "$IFACE" | cut -d: -f1)
                        if [ -n "$conn_name" ]; then
                            sudo nmcli connection modify "$conn_name" 802-3-ethernet.mtu "$MTU_VALUE"
                            print_success "MTU configured permanently in NetworkManager"
                        else
                            print_warning "Could not find NetworkManager connection for $IFACE"
                        fi
                        ;;
                    ubuntu|debian)
                        print_info "Add 'mtu $MTU_VALUE' to /etc/network/interfaces for $IFACE"
                        ;;
                    *)
                        print_info "Manual configuration required for permanent MTU"
                        ;;
                esac
            fi
        fi
    fi

    # Network buffer optimization
    if confirm "Optimize network buffers for audio streaming (16MB)?"; then
        print_info "Setting network buffer sizes..."
        sudo sysctl -w net.core.rmem_max=16777216
        sudo sysctl -w net.core.wmem_max=16777216
        print_success "Network buffers set to 16MB"

        if confirm "Make this permanent?"; then
            sudo tee /etc/sysctl.d/99-slim2diretta.conf > /dev/null <<'SYSCTL'
# slim2diretta - Network buffer optimization
# Larger buffers help with high-resolution audio and DSD streaming
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
            sudo sysctl --system > /dev/null
            print_success "Network buffer settings saved to /etc/sysctl.d/99-slim2diretta.conf"
        fi
    fi
}

# =============================================================================
# FIREWALL CONFIGURATION
# =============================================================================

configure_firewall() {
    print_header "Firewall Configuration"

    if ! confirm "Configure firewall to allow LMS traffic?"; then
        print_info "Skipping firewall configuration"
        return 0
    fi

    case $OS in
        fedora|rhel|centos)
            if command -v firewall-cmd &> /dev/null; then
                # SlimProto port
                sudo firewall-cmd --permanent --add-port=3483/tcp
                sudo firewall-cmd --permanent --add-port=3483/udp
                # HTTP streaming
                sudo firewall-cmd --permanent --add-port=9000/tcp
                sudo firewall-cmd --reload
                print_success "Firewall configured (firewalld)"
            else
                print_info "firewalld not installed, skipping"
            fi
            ;;
        ubuntu|debian)
            if command -v ufw &> /dev/null; then
                sudo ufw allow 3483/tcp
                sudo ufw allow 3483/udp
                sudo ufw allow 9000/tcp
                print_success "Firewall configured (ufw)"
            else
                print_info "ufw not installed, skipping"
            fi
            ;;
        *)
            print_info "Manual firewall configuration required"
            print_info "Open ports: 3483/tcp, 3483/udp, 9000/tcp"
            ;;
    esac
}

# =============================================================================
# SYSTEMD SERVICE
# =============================================================================

setup_systemd_service() {
    print_header "Systemd Service Installation"

    local BINARY_PATH="$SCRIPT_DIR/build/slim2diretta"
    local service_active=false

    # Check if binary exists
    if [ ! -f "$BINARY_PATH" ]; then
        print_error "Binary not found at: $BINARY_PATH"
        print_info "Please build slim2diretta first (option 2)"
        return 1
    fi

    print_success "Binary found: $BINARY_PATH"

    if ! confirm "Install slim2diretta as system service?" "Y"; then
        print_info "Skipping systemd service setup"
        return 0
    fi

    # Stop the service if it is currently running so the binary can be replaced
    if systemctl is-active --quiet slim2diretta.service 2>/dev/null; then
        service_active=true
        print_info "Stopping slim2diretta.service..."
        sudo systemctl stop slim2diretta.service
    fi

    print_info "1. Installing binary and startup script..."
    sudo cp "$BINARY_PATH" "$INSTALL_BIN/slim2diretta"
    sudo chmod +x "$INSTALL_BIN/slim2diretta"
    sudo cp "$SCRIPT_DIR/start-slim2diretta.sh" "$INSTALL_BIN/start-slim2diretta.sh"
    sudo chmod +x "$INSTALL_BIN/start-slim2diretta.sh"
    print_success "Binary installed: $INSTALL_BIN/slim2diretta"
    print_success "Startup script installed: $INSTALL_BIN/start-slim2diretta.sh"

    print_info "2. Installing systemd service..."
    sudo cp "$SCRIPT_DIR/slim2diretta.service" "$SERVICE_FILE"
    print_success "Service file installed: $SERVICE_FILE"

    print_info "3. Installing configuration file..."
    if [ ! -f "$CONFIG_FILE" ]; then
        sudo cp "$SCRIPT_DIR/slim2diretta.default" "$CONFIG_FILE"
        print_success "Configuration file installed: $CONFIG_FILE"
    else
        # Check if the shipped default has changed (new options, etc.)
        if ! diff -q "$SCRIPT_DIR/slim2diretta.default" "$CONFIG_FILE" > /dev/null 2>&1; then
            print_warning "Configuration file has changed in this version"
            echo ""
            echo "  New options may be available (Diretta advanced tuning, etc.)"
            echo "  Your current file: $CONFIG_FILE"
            echo ""
            if confirm "Update configuration file? (old file backed up as .bak)"; then
                sudo cp "$CONFIG_FILE" "${CONFIG_FILE}.bak"
                sudo cp "$SCRIPT_DIR/slim2diretta.default" "$CONFIG_FILE"
                print_success "Configuration updated (backup: ${CONFIG_FILE}.bak)"
                print_info "Review and re-apply your custom settings from the backup"
            else
                print_info "Keeping current configuration"
                print_info "New default available at: $SCRIPT_DIR/slim2diretta.default"
            fi
        else
            print_info "Configuration file is up to date"
        fi
    fi

    print_info "4. Reloading systemd daemon..."
    sudo systemctl daemon-reload

    # Ask for target number to enable
    echo ""
    print_info "Listing available Diretta targets..."
    sudo "$INSTALL_BIN/slim2diretta" --list-targets 2>&1 || true
    echo ""

    read -p "Enter Diretta target number to enable (e.g., 1) or press Enter to skip: " TARGET_NUM

    if [ -n "$TARGET_NUM" ]; then
        # Set TARGET in config file
        if [ -f "$CONFIG_FILE" ]; then
            sudo sed -i "s/^TARGET=.*/TARGET=${TARGET_NUM}/" "$CONFIG_FILE"
        fi
        sudo systemctl enable slim2diretta.service
        print_success "Service slim2diretta enabled with target $TARGET_NUM (starts on boot)"
    fi
    SVC_NAME="slim2diretta"

    if [ "$service_active" = true ]; then
        print_info "Restarting slim2diretta.service..."
        sudo systemctl start slim2diretta.service
    fi

    echo ""
    print_success "Systemd Service Installation Complete!"
    echo ""
    echo "  Binary:        $INSTALL_BIN/slim2diretta"
    echo "  Configuration: $CONFIG_FILE"
    echo ""
    echo "  Next steps:"
    echo "    1. Edit configuration (optional):"
    echo "       sudo nano $CONFIG_FILE"
    echo "       - Set LMS server IP if auto-discovery doesn't work"
    echo "       - Set player name, verbose mode, etc."
    echo ""
    echo "    2. Start the service:"
    echo "       sudo systemctl start $SVC_NAME"
    echo ""
    echo "    3. Check status:"
    echo "       sudo systemctl status $SVC_NAME"
    echo ""
    echo "    4. View logs:"
    echo "       sudo journalctl -u $SVC_NAME -f"
    echo ""

    # Offer to edit configuration
    if confirm "Edit configuration file now?"; then
        if command -v nano &> /dev/null; then
            sudo nano "$CONFIG_FILE"
        elif command -v vi &> /dev/null; then
            sudo vi "$CONFIG_FILE"
        else
            print_warning "No editor found. Edit manually: sudo nano $CONFIG_FILE"
        fi
    fi

    # Offer web UI installation
    setup_webui
}

# =============================================================================
# UPDATE BINARY
# =============================================================================

update_binary() {
    print_header "Updating slim2diretta"

    local BINARY_PATH="$SCRIPT_DIR/build/slim2diretta"

    if [ ! -f "$BINARY_PATH" ]; then
        print_error "Binary not found. Please build first."
        return 1
    fi

    if [ ! -f "$INSTALL_BIN/slim2diretta" ]; then
        print_error "slim2diretta not installed. Use 'Install service' first."
        return 1
    fi

    # Stop running service
    local running_instances=""
    if systemctl is-active --quiet slim2diretta.service 2>/dev/null; then
        running_instances="slim2diretta.service"
    fi

    if [ -n "$running_instances" ]; then
        print_info "Stopping running instances..."
        for svc in $running_instances; do
            sudo systemctl stop "$svc"
            print_info "  Stopped $svc"
        done
    fi

    # Copy new binary and startup script
    sudo cp "$BINARY_PATH" "$INSTALL_BIN/slim2diretta"
    sudo chmod +x "$INSTALL_BIN/slim2diretta"
    sudo cp "$SCRIPT_DIR/start-slim2diretta.sh" "$INSTALL_BIN/start-slim2diretta.sh"
    sudo chmod +x "$INSTALL_BIN/start-slim2diretta.sh"
    # Update service file
    if [ -f "$SERVICE_FILE" ]; then
        sudo cp "$SCRIPT_DIR/slim2diretta.service" "$SERVICE_FILE"
        sudo systemctl daemon-reload
    fi
    print_success "Binary updated: $INSTALL_BIN/slim2diretta"

    # Restart stopped instances
    if [ -n "$running_instances" ]; then
        print_info "Restarting instances..."
        for svc in $running_instances; do
            sudo systemctl start "$svc"
            print_info "  Started $svc"
        done
    fi

    print_success "Update complete!"
}

# =============================================================================
# WEB CONFIGURATION UI
# =============================================================================

setup_webui() {
    local INSTALL_DIR="/opt/slim2diretta"
    local WEBUI_DIR="$INSTALL_DIR/webui"
    local WEBUI_SERVICE_FILE="/etc/systemd/system/slim2diretta-webui.service"
    local WEBUI_SRC="$SCRIPT_DIR/webui"

    if [ ! -d "$WEBUI_SRC" ]; then
        print_info "Web UI source not found, skipping"
        return 0
    fi

    echo ""
    if ! confirm "Install web configuration UI (accessible on port 8081)?"; then
        print_info "Skipping web UI installation"
        return 0
    fi

    # Check Python 3
    if ! command -v python3 &>/dev/null; then
        print_error "Python 3 is required for the web UI"
        print_info "Install with: sudo dnf install python3  (or sudo apt install python3)"
        return 1
    fi

    print_info "Installing web UI..."

    sudo mkdir -p "$WEBUI_DIR"
    sudo cp -r "$WEBUI_SRC/diretta_webui.py" "$WEBUI_DIR/"
    sudo cp -r "$WEBUI_SRC/config_parser.py" "$WEBUI_DIR/"
    sudo cp -r "$WEBUI_SRC/profiles" "$WEBUI_DIR/"
    sudo cp -r "$WEBUI_SRC/templates" "$WEBUI_DIR/"
    sudo cp -r "$WEBUI_SRC/static" "$WEBUI_DIR/"
    print_success "Web UI files copied to $WEBUI_DIR"

    # Install systemd service
    if [ -f "$WEBUI_SRC/slim2diretta-webui.service" ]; then
        sudo cp "$WEBUI_SRC/slim2diretta-webui.service" "$WEBUI_SERVICE_FILE"
    else
        sudo tee "$WEBUI_SERVICE_FILE" > /dev/null <<'WEBUI_SERVICE_EOF'
[Unit]
Description=slim2diretta Web Configuration Interface
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /opt/slim2diretta/webui/diretta_webui.py \
    --profile /opt/slim2diretta/webui/profiles/slim2diretta.json \
    --port 8081
Restart=on-failure
RestartSec=5
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
WEBUI_SERVICE_EOF
    fi

    sudo systemctl daemon-reload
    sudo systemctl enable slim2diretta-webui.service
    sudo systemctl restart slim2diretta-webui.service

    # Get IP for display
    local IP_ADDR=$(hostname -I 2>/dev/null | awk '{print $1}')
    [ -z "$IP_ADDR" ] && IP_ADDR="<your-ip>"

    echo ""
    print_success "Web UI installed and running!"
    echo ""
    echo "  Access the configuration interface at:"
    echo "    http://${IP_ADDR}:8081"
    echo ""
    echo "  Manage the web UI service:"
    echo "    sudo systemctl status slim2diretta-webui"
    echo "    sudo systemctl stop slim2diretta-webui"
    echo ""
}

# =============================================================================
# FEDORA AGGRESSIVE OPTIMIZATION (OPTIONAL)
# =============================================================================

optimize_fedora_aggressive() {
    print_header "Aggressive Fedora Optimization"

    if [ "$OS" != "fedora" ]; then
        print_warning "This optimization is only for Fedora systems"
        return 1
    fi

    echo ""
    echo "WARNING: This will make aggressive changes to your system:"
    echo ""
    echo "  - Remove firewalld (firewall disabled)"
    echo "  - Remove SELinux policy (security framework disabled)"
    echo "  - Disable systemd-journald (no persistent logs)"
    echo "  - Disable systemd-oomd (out-of-memory daemon)"
    echo "  - Disable systemd-homed (home directory manager)"
    echo "  - Disable auditd (audit daemon)"
    echo "  - Remove polkit (privilege manager)"
    echo "  - Replace sshd with dropbear (lightweight SSH)"
    echo ""
    echo "This is intended for DEDICATED AUDIO SERVERS ONLY."
    echo "Do NOT use on general-purpose systems."
    echo ""

    if ! confirm "Are you sure you want to proceed?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    echo ""
    if ! confirm "FINAL WARNING: This will significantly reduce system security. Continue?" "N"; then
        print_info "Optimization cancelled"
        return 0
    fi

    print_info "Starting aggressive optimization..."

    # Disable and remove security services
    print_info "Disabling security services..."

    sudo systemctl disable auditd 2>/dev/null || true
    sudo systemctl stop auditd 2>/dev/null || true

    sudo systemctl stop firewalld 2>/dev/null || true
    sudo systemctl disable firewalld 2>/dev/null || true
    sudo dnf remove -y firewalld 2>/dev/null || true

    sudo dnf remove -y selinux-policy 2>/dev/null || true

    # Disable system services that add overhead
    print_info "Disabling system overhead services..."

    sudo systemctl disable systemd-journald 2>/dev/null || true
    sudo systemctl stop systemd-journald 2>/dev/null || true

    sudo systemctl disable systemd-oomd 2>/dev/null || true
    sudo systemctl stop systemd-oomd 2>/dev/null || true

    sudo systemctl disable systemd-homed 2>/dev/null || true
    sudo systemctl stop systemd-homed 2>/dev/null || true

    sudo systemctl stop polkitd 2>/dev/null || true
    sudo dnf remove -y polkit 2>/dev/null || true

    sudo dnf remove -y gssproxy 2>/dev/null || true

    # Replace sshd with dropbear
    print_info "Installing lightweight SSH server (dropbear)..."
    sudo dnf install -y dropbear || {
        print_warning "Failed to install dropbear, keeping sshd"
    }

    if command -v dropbear &> /dev/null; then
        sudo systemctl enable dropbear || true
        sudo systemctl start dropbear || true

        sudo systemctl disable sshd 2>/dev/null || true
        sudo systemctl stop sshd 2>/dev/null || true

        print_success "Dropbear installed and running"
    fi

    # Network buffer optimization
    print_info "Optimizing network buffers..."
    sudo sysctl -w net.core.rmem_max=16777216
    sudo sysctl -w net.core.wmem_max=16777216
    sudo tee /etc/sysctl.d/99-slim2diretta.conf > /dev/null <<'SYSCTL'
# slim2diretta - Network buffer optimization
net.core.rmem_max=16777216
net.core.wmem_max=16777216
SYSCTL
    sudo sysctl --system > /dev/null
    print_success "Network buffers optimized (16MB)"

    sudo dnf install -y htop || true

    print_success "Aggressive optimization complete"
    print_warning "A reboot is recommended to apply all changes"

    if confirm "Reboot now?"; then
        sudo reboot
    fi
}

# =============================================================================
# TEST INSTALLATION
# =============================================================================

test_installation() {
    print_header "Testing Installation"

    local BINARY="$INSTALL_BIN/slim2diretta"

    # Check installed binary
    if [ -f "$BINARY" ]; then
        print_success "slim2diretta binary: $BINARY OK"
    elif [ -f "$SCRIPT_DIR/build/slim2diretta" ]; then
        BINARY="$SCRIPT_DIR/build/slim2diretta"
        print_success "slim2diretta binary (build): $BINARY OK"
    else
        print_error "slim2diretta binary: NOT FOUND"
        return 1
    fi

    # Check systemd service
    if [ -f "$SERVICE_FILE" ]; then
        print_success "Systemd service: $SERVICE_FILE OK"
    else
        print_warning "Systemd service not installed"
    fi

    # Check configuration
    if [ -f "$CONFIG_FILE" ]; then
        print_success "Configuration: $CONFIG_FILE OK"
    else
        print_warning "Configuration not installed"
    fi

    # List Diretta targets
    echo ""
    print_info "Searching for Diretta targets..."
    sudo timeout 10 "$BINARY" --list-targets 2>&1 || {
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            print_info "Target search timed out (normal if no targets found)"
        else
            print_warning "Could not list Diretta targets"
            print_info "Make sure a Diretta device is connected to your network"
        fi
    }

    # Check running service
    echo ""
    if systemctl is-active slim2diretta.service &>/dev/null; then
        print_success "Service slim2diretta is running"
    else
        print_info "Service slim2diretta is not running"
    fi

    echo ""
    print_success "Test complete!"
}

# =============================================================================
# UNINSTALL
# =============================================================================

uninstall() {
    print_header "Uninstall slim2diretta"

    echo "This will remove:"
    echo "  - Binary: $INSTALL_BIN/slim2diretta"
    echo "  - Service: $SERVICE_FILE"
    echo "  - Web UI (if installed)"
    echo "  - Configuration: $CONFIG_FILE (optional)"
    echo ""

    if ! confirm "Proceed with uninstall?" "N"; then
        print_info "Uninstall cancelled"
        return 0
    fi

    # Stop and disable service
    sudo systemctl stop slim2diretta.service 2>/dev/null || true
    sudo systemctl disable slim2diretta.service 2>/dev/null || true
    print_info "Stopped and disabled slim2diretta"

    # Remove binary and startup script
    if [ -f "$INSTALL_BIN/slim2diretta" ]; then
        sudo rm "$INSTALL_BIN/slim2diretta"
        sudo rm -f "$INSTALL_BIN/start-slim2diretta.sh"
        print_success "Binary and startup script removed"
    fi

    # Remove service file
    if [ -f "$SERVICE_FILE" ]; then
        sudo rm "$SERVICE_FILE"
        sudo systemctl daemon-reload
        print_success "Service file removed"
    fi

    # Remove configuration (ask first)
    if [ -f "$CONFIG_FILE" ]; then
        if confirm "Remove configuration file ($CONFIG_FILE)?"; then
            sudo rm "$CONFIG_FILE"
            print_success "Configuration removed"
        else
            print_info "Configuration kept: $CONFIG_FILE"
        fi
    fi

    # Remove web UI if installed
    local WEBUI_SERVICE="/etc/systemd/system/slim2diretta-webui.service"
    local WEBUI_DIR="/opt/slim2diretta/webui"
    if [ -f "$WEBUI_SERVICE" ] || [ -d "$WEBUI_DIR" ]; then
        if confirm "Remove web configuration UI?"; then
            sudo systemctl stop slim2diretta-webui.service 2>/dev/null || true
            sudo systemctl disable slim2diretta-webui.service 2>/dev/null || true
            if [ -f "$WEBUI_SERVICE" ]; then
                sudo rm "$WEBUI_SERVICE"
                sudo systemctl daemon-reload
            fi
            if [ -d "$WEBUI_DIR" ]; then
                sudo rm -rf "$WEBUI_DIR"
            fi
            # Remove parent dir if empty
            sudo rmdir /opt/slim2diretta 2>/dev/null || true
            print_success "Web UI removed"
        else
            print_info "Web UI kept"
        fi
    fi

    # Remove sysctl config if exists
    if [ -f "/etc/sysctl.d/99-slim2diretta.conf" ]; then
        if confirm "Remove network buffer settings?"; then
            sudo rm "/etc/sysctl.d/99-slim2diretta.conf"
            sudo sysctl --system > /dev/null
            print_success "Network settings removed"
        fi
    fi

    print_success "Uninstall complete"
}

# =============================================================================
# MAIN MENU
# =============================================================================

show_main_menu() {
    echo ""
    echo "============================================"
    echo " slim2diretta - Installation"
    echo "============================================"
    echo ""
    echo "Installation options:"
    echo ""
    echo "  1) Full installation (recommended)"
    echo "     - Dependencies, build, systemd service"
    echo ""
    echo "  2) Build slim2diretta only"
    echo "     - Compile (assumes dependencies installed)"
    echo ""
    echo "  3) Install systemd service only"
    echo "     - Install as system service (assumes built)"
    echo ""
    echo "  4) Update binary only"
    echo "     - Replace installed binary after rebuild"
    echo ""
    echo "  5) Configure network"
    echo "     - MTU, buffers, and firewall setup"
    echo ""
    echo "  6) Test installation"
    echo "     - Verify binaries and list Diretta targets"
    echo ""
    echo "  7) Install web configuration UI"
    echo "     - Browser-based settings (port 8081)"
    echo ""
    echo "  8) Install optional codecs (MP3, OGG, AAC, FFmpeg)"
    echo "     - Extra codec libraries for extended format support"
    echo ""
    if [ "$OS" = "fedora" ]; then
    echo "  9) Aggressive Fedora optimization"
    echo "     - For dedicated audio servers only"
    echo ""
    fi
    echo "  u) Uninstall"
    echo "  q) Quit"
    echo ""
}

run_full_installation() {
    install_dependencies
    install_optional_codecs
    check_diretta_sdk
    build_slim2diretta
    configure_network
    configure_firewall
    setup_systemd_service
    test_installation

    print_header "Installation Complete!"

    echo ""
    echo "Quick Start:"
    echo ""
    echo "  1. Edit configuration (optional, for LMS IP / player name):"
    echo "     sudo nano $CONFIG_FILE"
    echo ""
    echo "  2. Start the service:"
    echo "     sudo systemctl start ${SVC_NAME:-slim2diretta}"
    echo ""
    echo "  3. Check status:"
    echo "     sudo systemctl status ${SVC_NAME:-slim2diretta}"
    echo ""
    echo "  4. View logs:"
    echo "     sudo journalctl -u ${SVC_NAME:-slim2diretta} -f"
    echo ""
    echo "  5. Open LMS web interface and select 'slim2diretta' as player"
    echo ""
}

# =============================================================================
# ENTRY POINT
# =============================================================================

main() {
    detect_system

    # Check for command-line arguments
    case "${1:-}" in
        --full|-f)
            run_full_installation
            exit 0
            ;;
        --build|-b)
            check_diretta_sdk
            build_slim2diretta
            exit 0
            ;;
        --service|-s)
            setup_systemd_service
            exit 0
            ;;
        --update|-u)
            check_diretta_sdk
            build_slim2diretta
            update_binary
            exit 0
            ;;
        --network|-n)
            configure_network
            configure_firewall
            exit 0
            ;;
        --test|-t)
            test_installation
            exit 0
            ;;
        --webui|-w)
            setup_webui
            exit 0
            ;;
        --codecs|-c)
            install_optional_codecs
            exit 0
            ;;
        --optimize|-o)
            optimize_fedora_aggressive
            exit 0
            ;;
        --uninstall)
            uninstall
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [OPTION]"
            echo ""
            echo "Options:"
            echo "  --full, -f          Full installation"
            echo "  --build, -b         Build slim2diretta only"
            echo "  --service, -s       Install systemd service only"
            echo "  --update, -u        Rebuild and update installed binary"
            echo "  --network, -n       Configure network only"
            echo "  --test, -t          Test installation"
            echo "  --webui, -w         Install web configuration UI"
            echo "  --codecs, -c        Install optional codec libraries"
            echo "  --optimize, -o      Aggressive Fedora optimization"
            echo "  --uninstall         Remove slim2diretta"
            echo "  --help, -h          Show this help"
            echo ""
            echo "Without options, shows interactive menu."
            exit 0
            ;;
    esac

    # Interactive menu
    while true; do
        show_main_menu

        local max_option=8
        [ "$OS" = "fedora" ] && max_option=9

        read -p "Choose option [1-$max_option/u/q]: " choice

        case $choice in
            1)
                run_full_installation
                break
                ;;
            2)
                check_diretta_sdk
                build_slim2diretta
                ;;
            3)
                setup_systemd_service
                ;;
            4)
                update_binary
                ;;
            5)
                configure_network
                configure_firewall
                print_success "Network configuration complete"
                ;;
            6)
                test_installation
                ;;
            7)
                setup_webui
                ;;
            8)
                install_optional_codecs
                ;;
            9)
                if [ "$OS" = "fedora" ]; then
                    optimize_fedora_aggressive
                else
                    print_error "Invalid option"
                fi
                ;;
            u|U)
                uninstall
                ;;
            q|Q)
                print_info "Exiting..."
                exit 0
                ;;
            *)
                print_error "Invalid option: $choice"
                ;;
        esac
    done
}

# Run main
main "$@"
