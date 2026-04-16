#!/bin/bash
#
# RS300 Thermal Camera — Rock 5B+ (RK3588) Install Script
#
# Installs the RS300 driver via DKMS, enables the DT overlay, and
# applies the H_BLACKLIST kernel cmdline fix that lets the driver bind
# on the stock Radxa BSP kernel (6.1.84-8-rk2410).
#
# Background:
#   The Rockchip BSP kernel fires a late_initcall(rkcif_clr_unready_dev)
#   that force-empties the V4L2 async notifier's waiting list before any
#   DKMS module can load. Blacklisting that single initcall keeps the
#   waiting list open so the userspace-loaded rs300 module binds cleanly.
#   Evidence: rs300-kb/evidence/h1-phase2-verdict-2026-04-15/
#             H_BLACKLIST-VERDICT.md + H_BLACKLIST-FOLLOWUP.md
#
# Usage: sudo ./install.sh

set -e

# ── Config ───────────────────────────────────────────────────────────────────
DRV_NAME="rs300"
DRV_VERSION="0.0.1"
OVERLAY_NAME="rock-5b-plus-rs300-cam0"
DKMS_SRC="/usr/src/${DRV_NAME}-${DRV_VERSION}"
DTBO_DIR="/boot/dtbo"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BLACKLIST_TOKEN="initcall_blacklist=rkcif_clr_unready_dev"
CMDLINE_FILE="/etc/kernel/cmdline"
UBOOT_DEFAULTS="/etc/default/u-boot"
MODULES_LOAD_FILE="/etc/modules-load.d/rs300.conf"

# ── Helpers ──────────────────────────────────────────────────────────────────
info()  { echo -e "\033[1;36m▶\033[0m $1"; }
ok()    { echo -e "\033[0;32m✓\033[0m $1"; }
warn()  { echo -e "\033[1;33m!\033[0m $1"; }
fail()  { echo -e "\033[0;31m✗\033[0m $1"; exit 1; }

backup_once() {
    # Create a timestamped backup the first time we touch a file.
    local f="$1"
    [ -f "$f" ] || return 0
    local bak="${f}.bak.$(date +%Y%m%d-%H%M%S)"
    [ -f "${f}.bak."* ] 2>/dev/null || cp -a "$f" "$bak"
}

# ── Root check ───────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    fail "Must run as root: sudo $0"
fi

echo ""
echo "RS300 Thermal Camera — Rock 5B+ Installer"
echo "==========================================="
echo ""

# ── Platform check ───────────────────────────────────────────────────────────
info "Checking platform..."
if [ -f /proc/device-tree/compatible ]; then
    COMPAT=$(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null)
    if ! echo "$COMPAT" | grep -q "rockchip,rk3588"; then
        fail "This script is for RK3588 (Rock 5B+). Detected: $(echo "$COMPAT" | head -1)"
    fi
else
    fail "Cannot read /proc/device-tree/compatible"
fi
ok "Rock 5B+ (RK3588) detected"

# ── Kernel-state detection ───────────────────────────────────────────────────
# Two supported paths:
#   A) Stock Radxa BSP (CONFIG_VIDEO_RS300 not set, CONFIG_VIDEO_ROCKCHIP_CIF=y)
#      → install DKMS, overlay, AND the H_BLACKLIST cmdline param.
#   B) Custom kernel with rs300 built in (CONFIG_VIDEO_RS300=y, e.g. -999)
#      → install overlay only; DKMS is inert, cmdline param not needed.
info "Detecting kernel configuration..."
KCONFIG="/boot/config-$(uname -r)"
if [ ! -r "$KCONFIG" ]; then
    fail "Cannot read $KCONFIG — kernel config missing or unreadable."
fi
HAS_RS300_BUILTIN=$(grep -qE '^CONFIG_VIDEO_RS300=y' "$KCONFIG" && echo 1 || echo 0)
HAS_CIF_BUILTIN=$(grep -qE '^CONFIG_VIDEO_ROCKCHIP_CIF=y' "$KCONFIG" && echo 1 || echo 0)

if [ "$HAS_RS300_BUILTIN" = "1" ]; then
    INSTALL_MODE="builtin"
    ok "Built-in rs300 kernel detected — overlay-only install (skipping cmdline + DKMS)"
elif [ "$HAS_CIF_BUILTIN" = "1" ]; then
    INSTALL_MODE="blacklist"
    ok "Stock BSP detected — H_BLACKLIST install (DKMS + overlay + cmdline param)"
else
    fail "Neither CONFIG_VIDEO_RS300=y nor CONFIG_VIDEO_ROCKCHIP_CIF=y in $KCONFIG.
  This kernel does not match either supported RS300 install path on RK3588.
  See rs300-kb/evidence/h1-phase2-verdict-2026-04-15/H_BLACKLIST-VERDICT.md"
fi

# ── Dependencies ─────────────────────────────────────────────────────────────
info "Checking dependencies..."
REQUIRED_PKGS="device-tree-compiler"
if [ "$INSTALL_MODE" = "blacklist" ]; then
    REQUIRED_PKGS="$REQUIRED_PKGS dkms"
fi

MISSING=""
for pkg in $REQUIRED_PKGS; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ "$INSTALL_MODE" = "blacklist" ]; then
    KDIR="/lib/modules/$(uname -r)/build"
    if [ ! -d "$KDIR" ]; then
        MISSING="$MISSING linux-headers-$(uname -r)"
    fi
fi

if [ -n "$MISSING" ]; then
    info "Installing missing packages:$MISSING"
    apt-get update -qq
    apt-get install -y $MISSING
fi
ok "Dependencies satisfied"

# ── DKMS module (blacklist mode only) ────────────────────────────────────────
if [ "$INSTALL_MODE" = "blacklist" ]; then
    info "Installing kernel module via DKMS..."

    if /usr/sbin/dkms status 2>/dev/null | grep -q "${DRV_NAME}/${DRV_VERSION}"; then
        warn "Removing existing DKMS registration"
        /usr/sbin/dkms remove -m "$DRV_NAME" -v "$DRV_VERSION" --all 2>/dev/null || true
    fi

    mkdir -p "$DKMS_SRC"
    cp "${SCRIPT_DIR}/build/dkms.conf" "$DKMS_SRC/"
    cp "${SCRIPT_DIR}/build/Makefile"  "$DKMS_SRC/"
    cp "${SCRIPT_DIR}/src/rs300.c"     "$DKMS_SRC/"

    /usr/sbin/dkms add     -m "$DRV_NAME" -v "$DRV_VERSION"
    /usr/sbin/dkms build   -m "$DRV_NAME" -v "$DRV_VERSION"
    /usr/sbin/dkms install -m "$DRV_NAME" -v "$DRV_VERSION"

    ok "DKMS module installed: $(/usr/sbin/dkms status | grep "$DRV_NAME")"

    # ── modules-load.d (userspace autoload at boot) ──────────────────────────
    info "Enabling rs300 autoload via $MODULES_LOAD_FILE"
    echo "rs300" > "$MODULES_LOAD_FILE"
    ok "Autoload entry written"
else
    info "Skipping DKMS (rs300 already built in)"
fi

# ── Device tree overlay ─────────────────────────────────────────────────────
info "Building device tree overlay..."

DTBO_FILE="${SCRIPT_DIR}/build/${OVERLAY_NAME}.dtbo"
PREPROCESSED="/tmp/${OVERLAY_NAME}.dts.preprocessed"
HEADERS_DIR="/usr/src/linux-headers-$(uname -r)/include"
[ -d "$HEADERS_DIR" ] || HEADERS_DIR="/lib/modules/$(uname -r)/build/include"

cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$HEADERS_DIR" \
    "${SCRIPT_DIR}/src/${OVERLAY_NAME}.dts" \
    -o "$PREPROCESSED"

dtc -q -@ -H epapr -O dtb -o "$DTBO_FILE" "$PREPROCESSED"
rm -f "$PREPROCESSED"

DTBO_SIZE=$(stat -c%s "$DTBO_FILE")
ok "Overlay compiled ($DTBO_SIZE bytes)"

info "Installing overlay to $DTBO_DIR..."
mkdir -p "$DTBO_DIR"
cp "$DTBO_FILE" "$DTBO_DIR/"
ok "Overlay installed to $DTBO_DIR/${OVERLAY_NAME}.dtbo"

# ── u-boot overlay registration ──────────────────────────────────────────────
if [ -f "$UBOOT_DEFAULTS" ]; then
    info "Registering overlay in $UBOOT_DEFAULTS"
    backup_once "$UBOOT_DEFAULTS"
    if grep -qE '^U_BOOT_FDT_OVERLAYS=' "$UBOOT_DEFAULTS"; then
        CURRENT=$(grep -E '^U_BOOT_FDT_OVERLAYS=' "$UBOOT_DEFAULTS" | sed -E 's/^U_BOOT_FDT_OVERLAYS="?([^"]*)"?$/\1/')
        if echo " $CURRENT " | grep -q " ${OVERLAY_NAME}.dtbo "; then
            ok "Overlay already listed in U_BOOT_FDT_OVERLAYS"
        else
            NEW="${CURRENT:+$CURRENT }${OVERLAY_NAME}.dtbo"
            sed -i -E "s|^U_BOOT_FDT_OVERLAYS=.*|U_BOOT_FDT_OVERLAYS=\"${NEW}\"|" "$UBOOT_DEFAULTS"
            ok "Added overlay to U_BOOT_FDT_OVERLAYS"
        fi
    else
        echo "U_BOOT_FDT_OVERLAYS=\"${OVERLAY_NAME}.dtbo\"" >> "$UBOOT_DEFAULTS"
        ok "Created U_BOOT_FDT_OVERLAYS with overlay"
    fi
else
    warn "$UBOOT_DEFAULTS not present — overlay not registered for u-boot-update."
    warn "Add manually under your kernel label in /boot/extlinux/extlinux.conf:"
    echo "    fdtoverlays /boot/dtbo/${OVERLAY_NAME}.dtbo"
fi

# ── Kernel cmdline: append initcall_blacklist ────────────────────────────────
if [ "$INSTALL_MODE" = "blacklist" ]; then
    info "Applying H_BLACKLIST kernel parameter"
    if [ ! -f "$CMDLINE_FILE" ]; then
        fail "$CMDLINE_FILE not found — cannot apply cmdline param. Is u-boot-menu installed?"
    fi
    if grep -q "$BLACKLIST_TOKEN" "$CMDLINE_FILE"; then
        ok "H_BLACKLIST already present in $CMDLINE_FILE"
    else
        backup_once "$CMDLINE_FILE"
        # Keep on one line; append with a leading space if the file has content.
        CURRENT=$(tr -d '\n' < "$CMDLINE_FILE")
        NEW="${CURRENT:+$CURRENT }$BLACKLIST_TOKEN"
        printf '%s\n' "$NEW" > "$CMDLINE_FILE"
        ok "Appended $BLACKLIST_TOKEN"
    fi
fi

# ── Regenerate extlinux.conf ─────────────────────────────────────────────────
info "Regenerating boot configuration..."
if command -v /usr/sbin/u-boot-update >/dev/null 2>&1; then
    /usr/sbin/u-boot-update
    if grep -q "$OVERLAY_NAME" /boot/extlinux/extlinux.conf 2>/dev/null; then
        ok "Overlay present in extlinux.conf"
    else
        warn "Overlay not found in extlinux.conf — check $UBOOT_DEFAULTS"
    fi
    if [ "$INSTALL_MODE" = "blacklist" ] && grep -q "$BLACKLIST_TOKEN" /boot/extlinux/extlinux.conf 2>/dev/null; then
        ok "H_BLACKLIST present in extlinux.conf"
    elif [ "$INSTALL_MODE" = "blacklist" ]; then
        warn "H_BLACKLIST not in extlinux.conf — check $CMDLINE_FILE"
    fi
else
    warn "u-boot-update not found"
    warn "Edit /boot/extlinux/extlinux.conf manually:"
    echo "    append [existing-args] $BLACKLIST_TOKEN"
    echo "    fdtoverlays /boot/dtbo/${OVERLAY_NAME}.dtbo"
fi

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "==========================================="
ok "Installation complete!"
echo "==========================================="
echo ""
echo "Reboot required. After reboot verify:"
echo "  1. sudo reboot"
echo "  2. cat /proc/cmdline | grep initcall_blacklist   (expect the token present)"
echo "  3. lsmod | grep rs300                            (expect rs300 loaded)"
echo "  4. sudo dmesg | grep -i rs300                    (expect probe succeeded)"
echo "  5. sudo i2cdetect -y 3                           (expect UU at 0x3c)"
echo "  6. v4l2-ctl --list-devices                       (expect /dev/video0 under rkcif)"
echo ""
echo "First stream (always set format first — rkcif NULL-deref workaround):"
echo "  v4l2-ctl -d /dev/video0 --set-fmt-video=width=384,height=288,pixelformat=UYVY"
echo "  v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=300"
echo ""
