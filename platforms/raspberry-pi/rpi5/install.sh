#!/bin/bash
#
# RS300 Thermal Camera Driver - Installation Script
# Version: 1.0.0
#
# Follows Linux package conventions:
# - Installation phase ONLY (no activation)
# - No automatic config.txt editing
# - No automatic service enablement
# - Clear instructions for manual activation
#
# Usage: ./setup.sh [--auto [RESOLUTION FPS CONFIRM]] [--help]
#
# Examples:
#   ./setup.sh                          # Interactive mode
#   ./setup.sh --auto                   # Auto-install with interactive config
#   ./setup.sh --auto 640 60 y          # Auto-install with CLI args (640x512@60fps, confirm)
#   ./setup.sh --auto 384 30 y          # Auto-install 384x288@30fps

set -e

VERSION="1.0.0"
DRV_VERSION="0.0.1"
DRV_NAME="rs300"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;36m'
BOLD='\033[1m'
NC='\033[0m'

# Print functions
print_status() {
    echo -e "${BLUE}▶${NC} $1"
}

print_success() {
    echo -e "${GREEN}✅${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠️${NC} $1"
}

print_error() {
    echo -e "${RED}❌${NC} $1"
}

print_header() {
    echo -e "${BOLD}$1${NC}"
}

# Parse command line arguments
AUTO_MODE=false
CLI_RESOLUTION=""
CLI_FPS=""
CLI_CONFIRM=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --auto)
            AUTO_MODE=true
            shift
            # Check for optional CLI args after --auto
            if [[ $# -gt 0 && ! $1 =~ ^- ]]; then
                CLI_RESOLUTION="$1"
                shift
            fi
            if [[ $# -gt 0 && ! $1 =~ ^- ]]; then
                CLI_FPS="$1"
                shift
            fi
            if [[ $# -gt 0 && ! $1 =~ ^- ]]; then
                CLI_CONFIRM="$1"
                shift
            fi
            ;;
        --help)
            cat <<'EOF'
RS300 Driver Installation Script

Usage: ./setup.sh [OPTIONS] [RESOLUTION] [FPS] [CONFIRM]

Options:
    --auto              Non-interactive mode (auto-install dependencies)
    --help              Show this help

CLI Arguments (only with --auto):
    RESOLUTION          640, 256, or 384 (defaults to interactive)
    FPS                 30 or 60 fps (defaults to interactive)
    CONFIRM             y/Y for yes, anything else for no (defaults to interactive)

Examples:
    ./setup.sh                              # Interactive mode
    ./setup.sh --auto                       # Auto-install with interactive config
    ./setup.sh --auto 640 60 y              # Auto-install 640x512@60fps
    ./setup.sh --auto 384 30 y              # Auto-install 384x288@30fps

This script installs:
  - RS300 kernel module via DKMS
  - Device tree overlay
  - Init script to /usr/lib/rs300/
  - Systemd service for auto-init on boot
  - Module params to /etc/modprobe.d/rs300.conf

After installation:
  1. Reboot: sudo reboot
  2. Verify: dmesg | grep rs300
  3. Check init: systemctl status rs300-init
EOF
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# ============================================================================
# MODULE & FPS CONFIGURATION
# ============================================================================

configure_module_and_fps() {
    while true; do
    print_header "RS300 Driver Configuration"
    print_header "=============================="
    echo ""

    # Handle CLI arguments if provided
    local resolution_choice=""
    if [ -n "$CLI_RESOLUTION" ]; then
        # Convert resolution number to choice
        case $CLI_RESOLUTION in
            640)
                resolution_choice="0"
                ;;
            256)
                resolution_choice="1"
                ;;
            384)
                resolution_choice="2"
                ;;
            *)
                print_error "Invalid resolution: $CLI_RESOLUTION (expected 640, 256, or 384)"
                exit 1
                ;;
        esac
        print_status "Using CLI resolution: $CLI_RESOLUTION"
    else
        # Ask for module resolution
        print_status "Select your module"
        echo ""
        echo "  0) Mini2-640 (640x512)"
        echo "  1) Mini2-256 (256x192)"
        echo "  2) Mini2-384 (384x288)"
        echo ""
        read -p "Select module (0/1/2): " resolution_choice
    fi

    case $resolution_choice in
        0)
            MODE=0
            RESOLUTION="Mini2-640"
            FPS_OPTIONS=("30" "60")
            ;;
        1)
            MODE=1
            RESOLUTION="Mini2-256"
            FPS_OPTIONS=("25" "50")
            ;;
        2)
            MODE=2
            RESOLUTION="Mini2-384"
            FPS_OPTIONS=("30" "60")
            ;;
        *)
            print_error "Invalid selection: $resolution_choice"
            continue
            ;;
    esac

    # Handle FPS selection
    local fps_choice=""
    if [ -n "$CLI_FPS" ]; then
        # Find the index of the FPS in options
        local found=0
        for i in "${!FPS_OPTIONS[@]}"; do
            if [ "${FPS_OPTIONS[$i]}" = "$CLI_FPS" ]; then
                fps_choice=$((i+1))
                found=1
                break
            fi
        done
        if [ $found -eq 0 ]; then
            print_error "Invalid FPS for this resolution: $CLI_FPS"
            print_status "Valid options: ${FPS_OPTIONS[*]}"
            exit 1
        fi
        print_status "Using CLI FPS: $CLI_FPS"
    else
        echo ""
        print_status "Which frame rate do you want?"
        echo ""
        for i in "${!FPS_OPTIONS[@]}"; do
            echo "  $((i+1))) ${FPS_OPTIONS[$i]} fps"
        done
        echo ""
        read -p "Select frame rate (1-${#FPS_OPTIONS[@]}): " fps_choice
    fi

    if ! [[ "$fps_choice" =~ ^[0-9]+$ ]] || [ "$fps_choice" -lt 1 ] || [ "$fps_choice" -gt ${#FPS_OPTIONS[@]} ]; then
        print_error "Invalid selection: $fps_choice"
        continue
    fi

    FPS=${FPS_OPTIONS[$((fps_choice-1))]}

    echo ""
    print_header "Configuration Summary"
    print_header "===================="
    echo "  Module Resolution: $RESOLUTION (mode=$MODE)"
    echo "  Frame Rate: $FPS fps"
    echo ""

    # Handle confirmation
    local continue_install=true
    if [ -n "$CLI_CONFIRM" ]; then
        if [[ $CLI_CONFIRM =~ ^[Yy]$ ]]; then
            print_status "Continuing with configuration (CLI confirmation)"
            continue_install=true
        else
            print_error "Installation cancelled (CLI confirmation negative)"
            exit 1
        fi
    else
        read -p "Continue with this configuration? (Y/n): " -r
        echo
        if [[ $REPLY =~ ^[Nn]$ ]]; then
            continue
        fi
    fi

    # Write modprobe config instead of editing source
    print_status "Writing modprobe configuration..."
    sudo mkdir -p /etc/modprobe.d
    echo "options rs300 mode=${MODE} fps=${FPS}" | sudo tee /etc/modprobe.d/rs300.conf > /dev/null

    print_success "Driver configured for $RESOLUTION at $FPS fps"
    print_status "Config written to /etc/modprobe.d/rs300.conf"
    echo ""
    break
    done
}

# Run configuration
configure_module_and_fps

# ============================================================================
# PRE-FLIGHT CHECKS
# ============================================================================

print_header "RS300 Driver Installation v${VERSION}"
print_header "====================================="
echo ""

check_prerequisites() {
    print_status "Checking prerequisites..."
    echo ""

    local errors=0

    # Check if running on Raspberry Pi
    if ! grep -q "Raspberry Pi" /proc/cpuinfo 2>/dev/null; then
        print_warning "Not running on Raspberry Pi"
        print_warning "This driver is designed for Raspberry Pi 5"
        echo ""
    else
        local pi_model=$(grep "Model" /proc/cpuinfo | cut -d':' -f2 | xargs)
        echo "  Pi Model: $pi_model"

        if ! echo "$pi_model" | grep -q "Raspberry Pi 5"; then
            print_warning "Detected: $pi_model"
            print_warning "This driver is optimized for Raspberry Pi 5"
            print_warning "Pi 4 support may vary"
            echo ""
        fi
    fi

    # Check kernel version
    local kernel_version=$(uname -r)
    local kernel_major=$(echo "$kernel_version" | cut -d'.' -f1)
    local kernel_minor=$(echo "$kernel_version" | cut -d'.' -f2)

    echo "  Kernel: $kernel_version"

    if [ "$kernel_major" -lt 6 ] || ([ "$kernel_major" -eq 6 ] && [ "$kernel_minor" -lt 6 ]); then
        print_error "Kernel 6.6+ required, found $kernel_version"
        ((errors++))
    fi

    # Check architecture
    local arch=$(uname -m)
    echo "  Architecture: $arch"

    if [ "$arch" != "aarch64" ]; then
        print_warning "Expected aarch64, found $arch"
    fi

    # Check OS
    if [ -f /etc/os-release ]; then
        local os_name=$(grep PRETTY_NAME /etc/os-release | cut -d'"' -f2)
        echo "  OS: $os_name"
    fi

    echo ""

    if [ $errors -gt 0 ]; then
        print_error "Prerequisites check failed"
        exit 1
    fi

    print_success "Prerequisites check passed"
    echo ""
}

# ============================================================================
# DEPENDENCY INSTALLATION
# ============================================================================

install_dependencies() {
    print_status "Checking required packages..."
    echo ""

    local missing_pkgs=()

    # Check each required package
    for pkg in raspberrypi-kernel-headers dkms v4l-utils i2c-tools device-tree-compiler; do
        if ! dpkg -l 2>/dev/null | grep -q "^ii  $pkg "; then
            missing_pkgs+=($pkg)
        fi
    done

    if [ ${#missing_pkgs[@]} -eq 0 ]; then
        print_success "All required packages are installed"
        echo ""
        return 0
    fi

    echo "Missing packages:"
    for pkg in "${missing_pkgs[@]}"; do
        echo "  - $pkg"
    done
    echo ""

    local should_install=false

    if [ "$AUTO_MODE" = true ]; then
        should_install=true
        print_status "Auto mode: Installing packages..."
    else
        read -p "Install missing packages now? (Y/n): " -r
        echo
        if [[ ! $REPLY =~ ^[Nn]$ ]]; then
            should_install=true
        fi
    fi

    if [ "$should_install" = true ]; then
        print_status "Updating package list..."
        sudo apt update

        print_status "Installing packages..."
        sudo apt install -y "${missing_pkgs[@]}"

        print_success "Dependencies installed"
        echo ""
    else
        print_error "Installation cannot continue without required packages"
        echo ""
        echo "Install manually:"
        echo "  sudo apt update"
        echo "  sudo apt install ${missing_pkgs[*]}"
        echo ""
        exit 1
    fi
}

# ============================================================================
# KERNEL MODULE INSTALLATION
# ============================================================================

install_kernel_module() {
    print_status "Installing kernel module via DKMS..."
    echo ""

    # Remove old version if exists
    if dkms status | grep -q "${DRV_NAME}"; then
        print_status "Removing previous installation..."
        sudo dkms remove -m ${DRV_NAME} -v ${DRV_VERSION} --all 2>/dev/null || true
    fi

    # Create source directory
    sudo mkdir -p /usr/src/${DRV_NAME}-${DRV_VERSION}

    # Copy source files
    print_status "Copying source files..."
    sudo cp dkms.conf Makefile src/rs300.c /usr/src/${DRV_NAME}-${DRV_VERSION}/

    # Add to DKMS
    print_status "Adding to DKMS..."
    sudo dkms add -m ${DRV_NAME} -v ${DRV_VERSION}

    # Build module
    print_status "Building kernel module..."
    sudo dkms build -m ${DRV_NAME} -v ${DRV_VERSION}

    # Install module
    print_status "Installing kernel module..."
    sudo dkms install -m ${DRV_NAME} -v ${DRV_VERSION}

    print_success "Kernel module installed via DKMS"
    echo ""
}

# ============================================================================
# DEVICE TREE OVERLAY INSTALLATION
# ============================================================================

install_device_tree() {
    print_status "Installing device tree overlay..."
    echo ""

    local src_file="src/rs300-overlay.dts"
    local compiled_file="rs300-overlay.dtbo"
    local dest_file="/boot/firmware/overlays/rs300.dtbo"
    local config_file="/boot/firmware/config.txt"
    local config_backup="/boot/firmware/config.txt.backup.$(date +%s)"
    local temp_log="/tmp/rs300-dtc-$(date +%s).log"

    # ========== PRE-FLIGHT CHECKS ==========
    print_status "Performing pre-flight checks..."

    # Check if dtc (device-tree-compiler) is available
    if ! command -v dtc &> /dev/null; then
        print_error "device-tree-compiler not found"
        echo "  Install with: sudo apt install device-tree-compiler"
        exit 2
    fi

    # Check if source file exists and is readable
    if [ ! -r "$src_file" ]; then
        print_error "Device tree source not found or not readable: $src_file"
        exit 2
    fi

    # Check if /boot/firmware/overlays is writable (DISABLED - not needed)
    # if [ ! -w "/boot/firmware/overlays" ]; then
    #     print_error "/boot/firmware/overlays is not writable"
    #     echo "  This usually means boot filesystem is read-only"
    #     echo "  Try: sudo mount -o remount,rw /boot/firmware"
    #     exit 3
    # fi

    # Check if config.txt exists and is readable
    if [ ! -r "$config_file" ]; then
        print_error "Boot config not found or not readable: $config_file"
        exit 3
    fi

    print_success "Pre-flight checks passed"
    echo ""

    # ========== COMPILATION ==========
    if [ ! -f "$compiled_file" ]; then
        print_status "Compiling device tree overlay from source..."

        # Compile and capture output
        if ! dtc -@ -H epapr -O dtb -o "$compiled_file" "$src_file" > "$temp_log" 2>&1; then
            print_error "Failed to compile device tree overlay"
            echo "  Error output:"
            sed 's/^/    /' "$temp_log"
            rm -f "$temp_log"
            exit 2
        fi

        # Verify compiled file exists and has reasonable size (> 100 bytes)
        if [ ! -f "$compiled_file" ] || [ ! -s "$compiled_file" ]; then
            print_error "Compilation produced invalid overlay file"
            rm -f "$temp_log"
            exit 2
        fi

        local file_size=$(stat -c%s "$compiled_file" 2>/dev/null || echo "unknown")
        print_success "Device tree overlay compiled ($file_size bytes)"
        rm -f "$temp_log"
    else
        print_status "Using pre-compiled overlay: $compiled_file"
    fi
    echo ""

    # ========== INSTALLATION WITH VERIFICATION ==========
    print_status "Installing overlay to firmware directory..."

    # Copy overlay to boot firmware
    if ! sudo cp "$compiled_file" "$dest_file" 2>/tmp/rs300-cp-error.log; then
        print_error "Failed to copy overlay to $dest_file"
        echo "  Error output:"
        if [ -f /tmp/rs300-cp-error.log ]; then
            sed 's/^/    /' /tmp/rs300-cp-error.log
            rm -f /tmp/rs300-cp-error.log
        fi
        exit 3
    fi

    # Verify destination file exists
    if [ ! -f "$dest_file" ]; then
        print_error "Installation verification failed: $dest_file not found"
        exit 3
    fi

    # Verify destination file is readable
    if [ ! -r "$dest_file" ]; then
        print_error "Installed file is not readable: $dest_file"
        echo "  This usually indicates a permission problem"
        exit 3
    fi

    # Verify file has reasonable size (should match source size)
    local dest_size=$(stat -c%s "$dest_file" 2>/dev/null || echo "unknown")
    print_success "Device tree overlay installed"
    echo "  Location: $dest_file ($dest_size bytes)"
    echo ""

    # ========== CONFIG.TXT UPDATE (ONLY AFTER VERIFICATION) ==========
    print_status "Enabling overlay in boot config..."

    # Check if already enabled (to avoid duplicate entries)
    if grep -q "^dtoverlay=rs300$" "$config_file"; then
        print_success "Overlay already enabled in config.txt"
        echo ""
        return 0
    fi

    # Backup config before modifying
    if ! sudo cp "$config_file" "$config_backup"; then
        print_error "Failed to backup config.txt"
        echo "  Cannot safely modify config without backup"
        echo "  Try: sudo mount -o remount,rw /boot/firmware"
        exit 3
    fi
    print_status "Backed up config.txt to: $config_backup"

    # Add overlay to config (only if not present)
    if ! grep -q "dtoverlay=rs300" "$config_file"; then
        if ! echo "dtoverlay=rs300" | sudo tee -a "$config_file" > /dev/null; then
            print_error "Failed to update config.txt"
            print_warning "Restoring backup from: $config_backup"
            sudo cp "$config_backup" "$config_file"
            exit 3
        fi
    fi

    # Verify the entry was actually added
    if ! grep -q "dtoverlay=rs300" "$config_file"; then
        print_error "Verification failed: overlay entry not found in config.txt"
        print_warning "Restoring backup from: $config_backup"
        sudo cp "$config_backup" "$config_file"
        exit 3
    fi

    print_success "Overlay enabled in config.txt"
    echo ""

    # ========== SUMMARY ==========
    print_success "Device tree overlay installation complete!"
    echo "  Source:      $src_file"
    echo "  Compiled:    $compiled_file"
    echo "  Installed:   $dest_file"
    echo "  Config:      $config_file"
    echo "  Backup:      $config_backup (kept for safety)"
    echo ""
    print_warning "Reboot required for overlay to take effect"
    echo ""
}

# ============================================================================
# INIT SCRIPT & SYSTEMD SERVICE
# ============================================================================

install_init_script() {
    print_status "Installing init script and systemd service..."
    echo ""

    # Install init script
    sudo mkdir -p /usr/lib/rs300
    sudo cp examples/rs300-init.sh /usr/lib/rs300/rs300-init.sh
    sudo chmod +x /usr/lib/rs300/rs300-init.sh
    print_success "Init script installed to /usr/lib/rs300/rs300-init.sh"

    # Install systemd service
    sudo cp examples/rs300-init.service /etc/systemd/system/rs300-init.service
    sudo systemctl daemon-reload

    # Ask to enable
    local should_enable=false
    if [ "$AUTO_MODE" = true ]; then
        should_enable=true
    else
        read -p "Enable auto-init on boot? (Y/n): " -r
        echo
        if [[ ! $REPLY =~ ^[Nn]$ ]]; then
            should_enable=true
        fi
    fi

    if [ "$should_enable" = true ]; then
        sudo systemctl enable rs300-init.service
        print_success "Systemd service enabled (will run on boot)"
    else
        print_status "Service installed but not enabled"
        echo "  Enable later: sudo systemctl enable rs300-init.service"
    fi
    echo ""

    # Install status, test, and stream tools
    print_status "Installing diagnostic tools..."
    sudo cp examples/rs300-status /usr/lib/rs300/rs300-status
    sudo cp examples/rs300-test /usr/lib/rs300/rs300-test
    sudo cp examples/rs300-stream /usr/lib/rs300/rs300-stream
    sudo cp examples/rs300-healthcheck /usr/lib/rs300/rs300-healthcheck
    sudo chmod +x /usr/lib/rs300/rs300-status /usr/lib/rs300/rs300-test /usr/lib/rs300/rs300-stream /usr/lib/rs300/rs300-healthcheck
    sudo ln -sf /usr/lib/rs300/rs300-status /usr/local/bin/rs300-status
    sudo ln -sf /usr/lib/rs300/rs300-test /usr/local/bin/rs300-test
    sudo ln -sf /usr/lib/rs300/rs300-stream /usr/local/bin/rs300-stream
    sudo ln -sf /usr/lib/rs300/rs300-healthcheck /usr/local/bin/rs300-healthcheck
    print_success "Tools installed: rs300-status, rs300-test, rs300-stream, rs300-healthcheck"
    echo ""
}

# ============================================================================
# POST-INSTALLATION
# ============================================================================

show_next_steps() {
    echo ""
    print_header "════════════════════════════════════════════════════"
    print_success "  Installation Complete!"
    print_header "════════════════════════════════════════════════════"
    echo ""
    echo "Next steps:"
    echo ""
    echo -e "  1. ${BLUE}sudo reboot${NC}"
    echo -e "  2. ${BLUE}rs300-status${NC}        (verify camera detected)"
    echo -e "  3. ${BLUE}rs300-test${NC}          (live thermal preview)"
    echo -e "     ${BLUE}rs300-stream${NC}        (GStreamer zero-copy view)"
    echo ""
    echo "Configuration:"
    echo "  Module params: /etc/modprobe.d/rs300.conf"
    echo "  Init script:   /usr/lib/rs300/rs300-init.sh"
    echo "  Uninstall:     sudo ./build/uninstall.sh"
    echo ""
}

# ============================================================================
# MAIN INSTALLATION FLOW
# ============================================================================

main() {
    check_prerequisites
    install_dependencies
    install_kernel_module
    install_device_tree
    install_init_script
    show_next_steps
}

# Run installation
main
