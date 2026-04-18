#!/bin/bash
#
# RS300 Thermal Camera — Radxa Zero 3W (RK3566) Install Script
#
# Installs the RS300 driver via DKMS, enables the DT overlay, and
# conditionally applies a kernel cmdline workaround that lets the
# driver bind on stock Rockchip BSP kernels.
#
# Background:
#   On stock Rockchip BSP kernels, a late_initcall named
#   rkcif_clr_unready_dev fires at boot and force-empties the V4L2
#   async notifier's waiting list before any userspace-loaded module
#   can register. Blacklisting that initcall with
#   initcall_blacklist=rkcif_clr_unready_dev keeps the waiting list
#   open so the rs300 module binds when it loads.
#
# Install modes (auto-detected, overridable with NO_BLACKLIST=1):
#   A) Built-in rs300 kernel (CONFIG_VIDEO_RS300=y)
#      → overlay only; DKMS is inert, cmdline param not needed
#   B) Stock Rockchip BSP with rkcif_clr_unready_dev present
#      (CONFIG_VIDEO_ROCKCHIP_CIF=y, symbol in /proc/kallsyms)
#      → DKMS + overlay + blacklist cmdline param
#   C) Custom kernel with rkcif_clr_unready_dev removed at source
#      (CONFIG_VIDEO_ROCKCHIP_CIF=y, symbol absent)
#      → DKMS + overlay only; blacklist unnecessary
#
# Override:
#   NO_BLACKLIST=1 sudo ./install.sh   # force-skip blacklist step
#
# Usage: sudo ./install.sh

set -e

# ── Config ───────────────────────────────────────────────────────────────────
DRV_NAME="rs300"
DRV_VERSION="0.0.1"
OVERLAY_SRC="rs300-rk3566e-overlay"
OVERLAY_NAME="radxa-zero3-rs300-thermal"
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
echo "RS300 Thermal Camera — Radxa Zero 3W Installer"
echo "================================================"
echo ""

# ── Platform check ───────────────────────────────────────────────────────────
info "Checking platform..."
if [ -f /proc/device-tree/compatible ]; then
    COMPAT=$(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null)
    if ! echo "$COMPAT" | grep -q "rockchip,rk3566"; then
        fail "This script is for RK3566 boards (Radxa Zero 3W/3E, etc.). Detected: $(echo "$COMPAT" | head -1)"
    fi
else
    fail "Cannot read /proc/device-tree/compatible"
fi
ok "RK3566 SoC detected ($(echo "$COMPAT" | head -1))"

# ── Kernel-state detection ───────────────────────────────────────────────────
info "Detecting kernel configuration..."
KCONFIG="/boot/config-$(uname -r)"
if [ ! -r "$KCONFIG" ]; then
    fail "Cannot read $KCONFIG — kernel config missing or unreadable."
fi
HAS_RS300_BUILTIN=$(grep -qE '^CONFIG_VIDEO_RS300=y' "$KCONFIG" && echo 1 || echo 0)
HAS_CIF_BUILTIN=$(grep -qE '^CONFIG_VIDEO_ROCKCHIP_CIF=y' "$KCONFIG" && echo 1 || echo 0)
HAS_UNREADY_SYMBOL=$(grep -qE '\brkcif_clr_unready_dev\b' /proc/kallsyms && echo 1 || echo 0)

if [ "$HAS_RS300_BUILTIN" = "1" ]; then
    INSTALL_MODE="builtin"
    ok "Built-in rs300 kernel — overlay-only install (skip DKMS + cmdline)"
elif [ "$HAS_CIF_BUILTIN" = "1" ] && [ "$HAS_UNREADY_SYMBOL" = "1" ]; then
    INSTALL_MODE="blacklist"
    ok "Stock BSP kernel with rkcif_clr_unready_dev present — DKMS + overlay + cmdline"
elif [ "$HAS_CIF_BUILTIN" = "1" ]; then
    INSTALL_MODE="dkms-only"
    ok "Custom kernel (CIF builtin, rkcif_clr_unready_dev patched out) — DKMS + overlay, no cmdline"
else
    fail "Neither CONFIG_VIDEO_RS300=y nor CONFIG_VIDEO_ROCKCHIP_CIF=y in $KCONFIG.
  This kernel does not match any supported RS300 install path on RK3566."
fi

# Honor opt-out
if [ "${NO_BLACKLIST:-0}" = "1" ] && [ "$INSTALL_MODE" = "blacklist" ]; then
    warn "NO_BLACKLIST=1 set — forcing dkms-only mode (skipping cmdline)"
    INSTALL_MODE="dkms-only-forced"
fi

# ── Dependencies ─────────────────────────────────────────────────────────────
info "Checking dependencies..."
REQUIRED_PKGS="device-tree-compiler"
if [ "$INSTALL_MODE" != "builtin" ]; then
    REQUIRED_PKGS="$REQUIRED_PKGS dkms"
fi

MISSING=""
for pkg in $REQUIRED_PKGS; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ "$INSTALL_MODE" != "builtin" ]; then
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

# ── DKMS module ──────────────────────────────────────────────────────────────
if [ "$INSTALL_MODE" != "builtin" ]; then
    info "Installing kernel module via DKMS..."

    if /usr/sbin/dkms status 2>/dev/null | grep -q "${DRV_NAME}/${DRV_VERSION}"; then
        warn "Removing existing DKMS registration"
        /usr/sbin/dkms remove -m "$DRV_NAME" -v "$DRV_VERSION" --all 2>/dev/null || true
    fi

    mkdir -p "$DKMS_SRC"
    cp "${SCRIPT_DIR}/dkms.conf" "$DKMS_SRC/"
    cp "${SCRIPT_DIR}/Makefile"  "$DKMS_SRC/"
    cp "${SCRIPT_DIR}/src/rs300.c" "$DKMS_SRC/"

    /usr/sbin/dkms add     -m "$DRV_NAME" -v "$DRV_VERSION"
    /usr/sbin/dkms build   -m "$DRV_NAME" -v "$DRV_VERSION"
    /usr/sbin/dkms install -m "$DRV_NAME" -v "$DRV_VERSION"

    ok "DKMS module installed: $(/usr/sbin/dkms status | grep "$DRV_NAME")"

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

mkdir -p "${SCRIPT_DIR}/build"

cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I "$HEADERS_DIR" \
    "${SCRIPT_DIR}/src/${OVERLAY_SRC}.dts" \
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
    info "Applying blacklist kernel parameter"
    if [ ! -f "$CMDLINE_FILE" ]; then
        fail "$CMDLINE_FILE not found — cannot apply cmdline param. Is u-boot-menu installed?"
    fi
    if grep -q "$BLACKLIST_TOKEN" "$CMDLINE_FILE"; then
        ok "Blacklist already present in $CMDLINE_FILE"
    else
        backup_once "$CMDLINE_FILE"
        CURRENT=$(tr -d '\n' < "$CMDLINE_FILE")
        NEW="${CURRENT:+$CURRENT }$BLACKLIST_TOKEN"
        printf '%s\n' "$NEW" > "$CMDLINE_FILE"
        ok "Appended $BLACKLIST_TOKEN"
    fi
elif [ "$INSTALL_MODE" = "dkms-only-forced" ]; then
    warn "Blacklist SKIPPED (NO_BLACKLIST=1). Driver will load but may not bind on stock BSP kernels."
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
        ok "Blacklist present in extlinux.conf"
    elif [ "$INSTALL_MODE" = "blacklist" ]; then
        warn "Blacklist not in extlinux.conf — check $CMDLINE_FILE"
    fi
else
    warn "u-boot-update not found"
    warn "Edit /boot/extlinux/extlinux.conf manually:"
    echo "    append [existing-args] $BLACKLIST_TOKEN"
    echo "    fdtoverlays /boot/dtbo/${OVERLAY_NAME}.dtbo"
fi

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "================================================"
ok "Installation complete!"
echo "================================================"
echo ""
echo "Mode: $INSTALL_MODE"
echo ""
echo "Reboot required. After reboot verify:"
echo "  1. sudo reboot"
if [ "$INSTALL_MODE" = "blacklist" ]; then
    echo "  2. cat /proc/cmdline | grep initcall_blacklist   (expect the token present)"
fi
echo "  3. lsmod | grep rs300                              (expect rs300 loaded)"
echo "  4. sudo dmesg | grep -i rs300                      (expect probe succeeded)"
echo "  5. sudo i2cdetect -y 2                             (expect UU at 0x3c)"
echo "  6. v4l2-ctl --list-devices                         (expect /dev/video0 under rkcif)"
echo ""
echo "First stream (always set format first — rkcif NULL-deref workaround):"
echo "  v4l2-ctl -d /dev/video0 --set-fmt-video=width=384,height=288,pixelformat=UYVY"
echo "  v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=300"
echo ""
