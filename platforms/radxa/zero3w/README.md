# Mini2 / WN2 Thermal Camera Driver for Radxa Zero 3W (RK3566)

Linux kernel driver for the Infisense RS300 thermal camera on the Radxa Zero 3W. Installs via DKMS on the stock Radxa Bookworm BSP kernel.

**Status:** Verification in progress. Driver + overlay were captured from a working reference board (300-frame stream at 60 fps on a custom kernel with `rkcif_clr_unready_dev` patched out at source). The stock-BSP blacklist install path has not yet been verified end-to-end on this platform.

## Prerequisites

- Radxa Zero 3W running Radxa Bookworm (KDE or CLI image)
- Mini2 MIPI adapter board (22-to-15 pin FPC, with onboard 3.3V → 5V boost for camera power)
- Internet access (for `git clone` and apt packages)

## Hardware signal path

```
RS300 camera (50-pin FPC)
  → Mini2 MIPI adapter PCB (15-pin FPC)
  → 22-to-15 pin adapter cable
  → Zero 3W 22-pin CSI connector
```

| Signal | On Zero 3W | Notes |
|-|-|-|
| MIPI CSI-2 data (2 lanes) | `csi2_dphy1` split mode | lanes 0-1 of the shared DPHY |
| I2C | `i2c2` (M1 pinctrl mux) | CSI connector pins 20/21 |
| Reset GPIO | `GPIO3_C1` (pin 17) | NOT routed — RS300 internal pull-up on NRST |
| Clock (MCLK) | `CIF_CLKOUT` (pin 18) | NOT routed via 22→15 adapter; RS300 uses internal oscillator |
| Power | 3.3V rail (pin 22) | Adapter's boost converter produces 5V for the camera module |

## Install

```bash
git clone https://github.com/Kodrea/mini2-thermal-driver.git
cd mini2-thermal-driver/platforms/radxa/zero3w
sudo ./install.sh
sudo reboot
```

The installer auto-detects which of three install modes applies:

| Kernel state | Path | What the installer does |
|-|-|-|
| Built-in rs300 (`CONFIG_VIDEO_RS300=y`) | Overlay only | Compiles and installs overlay; skips DKMS and cmdline |
| Stock BSP + `rkcif_clr_unready_dev` symbol present | DKMS + overlay + blacklist | Installs DKMS module, compiles overlay, appends blacklist cmdline token |
| CIF builtin but `rkcif_clr_unready_dev` symbol absent (custom kernel) | DKMS + overlay | Installs DKMS module and overlay; skips cmdline |

Symbol detection uses `grep rkcif_clr_unready_dev /proc/kallsyms`.

### Force-skip the blacklist (for testing)

```bash
NO_BLACKLIST=1 sudo ./install.sh
```

Useful for reproducing the "driver loads but doesn't bind" failure mode on a stock BSP kernel, to confirm the blacklist is actually doing something.

## The blacklist parameter — when it applies

On Rockchip stock BSP kernels, a `late_initcall(rkcif_clr_unready_dev)` fires at boot and force-empties the V4L2 async notifier's waiting list before any DKMS-loaded module can register. The rs300 driver loads into a kernel that has already discarded its pending-sensor list, so it never binds. The fix is a single kernel cmdline parameter:

```
initcall_blacklist=rkcif_clr_unready_dev
```

The installer writes this to `/etc/kernel/cmdline` and runs `u-boot-update` to regenerate `/boot/extlinux/extlinux.conf`. Without it on affected kernels, `lsmod` will show `rs300` loaded but the camera will not appear under `v4l2-ctl --list-devices`.

Custom kernels with the offending initcall removed at source (e.g. Radxa's `-nocsf` user-customized builds that have applied a local patch) do not need the blacklist — the installer detects this and skips the step.

## Verify After Reboot

```bash
# 1. Driver loaded
lsmod | grep rs300

# 2. Probe succeeded
sudo dmesg | grep -i rs300

# 3. Camera found on I2C bus 2 at 0x3c
sudo i2cdetect -y 2       # expect UU at 0x3c

# 4. Video device present
v4l2-ctl --list-devices   # expect /dev/video0 under rkcif

# 5. If blacklist was applied: cmdline shows it
cat /proc/cmdline | grep initcall_blacklist
```

## Stream

> **Always set the format before streaming.** Skipping this step triggers a NULL dereference in the rkcif driver.

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=384,height=288,pixelformat=UYVY

v4l2-ctl -d /dev/video0 \
  --stream-mmap \
  --stream-count=300
```

Use `--stream-count=300` or more. The image stabilizes after the first ~30 frames and fewer frames may show a frozen or blank picture.

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source |
| `src/rs300-rk3566e-overlay.dts` | Device tree overlay |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
