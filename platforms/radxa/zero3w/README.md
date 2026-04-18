# Mini2 / WN2 Thermal Camera Driver for Radxa Zero 3W (RK3566)

Linux kernel driver for the Infisense RS300 thermal camera on the Radxa Zero 3W. Installs via DKMS on the stock Radxa Bookworm BSP kernel.

**Verified:** factory Radxa kernel `6.1.84-10-rk2410-nocsf` on the `radxa-zero3_bookworm_kde_b1` image, 300-frame stream at 60 fps, 2026-04-18. The blacklist workaround was confirmed necessary on this platform via a reproducible A/B test (see the Blacklist section below).

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

On Rockchip stock BSP kernels a `late_initcall(rkcif_clr_unready_dev)` fires at boot and closes the rkcif V4L2 async subdev notifier before any DKMS-loaded module can register with it. The fix is a single kernel cmdline parameter:

```
initcall_blacklist=rkcif_clr_unready_dev
```

The installer writes this to `/etc/kernel/cmdline` and runs `u-boot-update` to regenerate `/boot/extlinux/extlinux.conf`.

### The failure signature without the blacklist

The symptoms are subtle because the rs300 module still loads and still binds at the I2C layer. Observed on a factory Zero 3W without the blacklist:

- `lsmod | grep rs300` shows it loaded
- `i2cdetect -y 2` shows `UU` at `0x3c` (misleading — it indicates the I2C device is reserved by the driver, not that capture works)
- `v4l2-ctl --list-devices` shows `rkcif` with `/dev/video0` through `/dev/video7`
- **`v4l2-ctl -d /dev/video0 --get-fmt-video` fails with `No such device`**
- `v4l2-ctl -d /dev/video7 --stream-mmap` fails with `VIDIOC_REQBUFS returned -1 (Device or resource busy)`
- dmesg shows `rkcif rkcif_mipi_lvds: clear unready subdev num: 1` early at boot, followed by `rkcif_mipi_lvds: Async subdev notifier completed` *before* rs300 has loaded, and later a flood of `rkcif_update_sensor_info: stream[N] get remote terminal sensor failed!`

With the blacklist applied the kernel logs `blacklisting initcall rkcif_clr_unready_dev` at boot, the async notifier waits until rs300 registers (typically around t=18s), then `Async subdev notifier completed` fires cleanly and `/dev/video0` becomes usable.

Custom kernels with `rkcif_clr_unready_dev` removed at source do not need the blacklist. The installer detects this via `/proc/kallsyms` and skips the cmdline step automatically.

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
