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
VERSION="1.0.0"
INSTALL_BIN="/usr/local/bin"
SERVICE_FILE="/etc/systemd/system/slim2diretta@.service"
CONFIG_FILE="/etc/default/slim2diretta"

# Auto-detect latest Diretta SDK version
detect_latest_sdk() {
    local sdk_found=$(find "$HOME" . .. /opt "$HOME/audio" /usr/local \
        -maxdepth 1 -type d -name 'DirettaHostSDK_*' 2>/dev/null | sort -V | tail -1)

    if [ -n "$sdk_found" ]; then
        echo "$sdk_found"
    else
        echo "$HOME/DirettaHostSDK"
    fi
}

SDK_PATH="${DIRETTA_SDK_PATH:-$(detect_latest_sdk)}"

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
                flac-devel
            ;;
        ubuntu|debian)
            print_info "Using APT package manager..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                cmake \
                pkg-config \
                libflac-dev
            ;;
        arch|archarm|manjaro)
            print_info "Using Pacman package manager..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                cmake \
                pkgconf \
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
    cd build

    # Configure with CMake
    print_info "Configuring with CMake..."
    export DIRETTA_SDK_PATH="$SDK_PATH"
    cmake ..

    # Build
    print_info "Building slim2diretta..."
    make -j$(nproc)

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

    print_info "1. Installing binary..."
    sudo cp "$BINARY_PATH" "$INSTALL_BIN/slim2diretta"
    sudo chmod +x "$INSTALL_BIN/slim2diretta"
    print_success "Binary installed: $INSTALL_BIN/slim2diretta"

    print_info "2. Installing systemd service..."
    sudo cp "$SCRIPT_DIR/slim2diretta@.service" "$SERVICE_FILE"
    print_success "Service file installed: $SERVICE_FILE"

    print_info "3. Installing configuration file..."
    if [ ! -f "$CONFIG_FILE" ]; then
        sudo cp "$SCRIPT_DIR/slim2diretta.default" "$CONFIG_FILE"
        print_success "Configuration file installed: $CONFIG_FILE"
    else
        print_info "Configuration file already exists, keeping current settings"
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
        print_info "5. Enabling service for target $TARGET_NUM..."
        sudo systemctl enable "slim2diretta@${TARGET_NUM}.service"
        print_success "Service slim2diretta@${TARGET_NUM} enabled (starts on boot)"
    fi

    echo ""
    print_success "Systemd Service Installation Complete!"
    echo ""
    echo "  Binary:        $INSTALL_BIN/slim2diretta"
    echo "  Service file:  $SERVICE_FILE"
    echo "  Configuration: $CONFIG_FILE"
    echo ""
    echo "  The service uses a template: slim2diretta@<target>"
    echo "  The target number is passed as --target <N>"
    echo ""
    echo "  Next steps:"
    echo "    1. Edit configuration (optional):"
    echo "       sudo nano $CONFIG_FILE"
    echo "       - Set LMS server IP if auto-discovery doesn't work"
    echo "       - Set player name, verbose mode, etc."
    echo ""
    echo "    2. Start the service:"
    echo "       sudo systemctl start slim2diretta@${TARGET_NUM:-1}"
    echo ""
    echo "    3. Check status:"
    echo "       sudo systemctl status slim2diretta@${TARGET_NUM:-1}"
    echo ""
    echo "    4. View logs:"
    echo "       sudo journalctl -u slim2diretta@${TARGET_NUM:-1} -f"
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

    # Stop running instances
    local running_instances=$(systemctl list-units --type=service --state=running \
        'slim2diretta@*' 2>/dev/null | grep slim2diretta | awk '{print $1}')

    if [ -n "$running_instances" ]; then
        print_info "Stopping running instances..."
        for svc in $running_instances; do
            sudo systemctl stop "$svc"
            print_info "  Stopped $svc"
        done
    fi

    # Copy new binary
    sudo cp "$BINARY_PATH" "$INSTALL_BIN/slim2diretta"
    sudo chmod +x "$INSTALL_BIN/slim2diretta"
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
    timeout 10 sudo "$BINARY" --list-targets 2>&1 || {
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            print_info "Target search timed out (normal if no targets found)"
        else
            print_warning "Could not list Diretta targets"
            print_info "Make sure a Diretta device is connected to your network"
        fi
    }

    # Check running instances
    echo ""
    local running=$(systemctl list-units --type=service --state=running \
        'slim2diretta@*' 2>/dev/null | grep slim2diretta || true)
    if [ -n "$running" ]; then
        print_success "Running instances:"
        echo "$running" | awk '{print "  " $1 " - " $3}'
    else
        print_info "No running instances"
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
    echo "  - Configuration: $CONFIG_FILE (optional)"
    echo ""

    if ! confirm "Proceed with uninstall?" "N"; then
        print_info "Uninstall cancelled"
        return 0
    fi

    # Stop and disable all instances
    local instances=$(systemctl list-units --type=service --all \
        'slim2diretta@*' 2>/dev/null | grep slim2diretta | awk '{print $1}')
    for svc in $instances; do
        sudo systemctl stop "$svc" 2>/dev/null || true
        sudo systemctl disable "$svc" 2>/dev/null || true
        print_info "Stopped and disabled $svc"
    done

    # Remove binary
    if [ -f "$INSTALL_BIN/slim2diretta" ]; then
        sudo rm "$INSTALL_BIN/slim2diretta"
        print_success "Binary removed"
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
    echo " slim2diretta v$VERSION - Installation"
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
    if [ "$OS" = "fedora" ]; then
    echo "  7) Aggressive Fedora optimization"
    echo "     - For dedicated audio servers only"
    echo ""
    fi
    echo "  u) Uninstall"
    echo "  q) Quit"
    echo ""
}

run_full_installation() {
    install_dependencies
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
    echo "  2. Start the service (replace 1 with your target number):"
    echo "     sudo systemctl start slim2diretta@1"
    echo ""
    echo "  3. Check status:"
    echo "     sudo systemctl status slim2diretta@1"
    echo ""
    echo "  4. View logs:"
    echo "     sudo journalctl -u slim2diretta@1 -f"
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

        local max_option=6
        [ "$OS" = "fedora" ] && max_option=7

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
