# Mini2 / WN2 Thermal Camera Driver for Raspberry Pi Zero 2W (BCM2710)

Linux kernel driver for the Infisense RS300 thermal camera on the Raspberry Pi Zero 2W. Installs via DKMS on the stock Raspberry Pi OS kernel with the legacy unicam camera stack (`bcm2835-unicam-legacy`).

**Verified:** Raspberry Pi OS (Trixie, 64-bit, Lite), kernel `6.12.75+rpt-rpi-v8`, 900-frame stream at 60 fps with FFC trigger, 2026-04-20.

## Prerequisites

- Raspberry Pi Zero 2W (BCM2710)
- Raspberry Pi OS (Bookworm or Trixie, 64-bit; Lite recommended for headless capture)
- Mini2 MIPI adapter board (wired for 5V camera power)
- Internet access (for `git clone` and apt packages)

## Install

```bash
git clone https://github.com/Kodrea/mini2-thermal-driver.git
cd mini2-thermal-driver/platforms/raspberry-pi/zero2w
sudo ./install.sh
sudo reboot
```

The installer:

- Installs `dkms`, `device-tree-compiler`, `i2c-tools`, `v4l-utils`, and kernel headers if missing
- Builds and installs the `rs300` kernel module via DKMS
- Compiles the device-tree overlay and copies it to `/boot/firmware/overlays/rs300.dtbo`
- Sets `camera_auto_detect=0` and appends `dtoverlay=rs300` to `/boot/firmware/config.txt` (timestamped backup kept)

Reboot is required for the overlay to load. Runtime overlay application via `sudo dtoverlay` is not sufficient on Pi CSI: the sensor probes cleanly but unicam captures zero bytes, because the firmware cannot replay its CSI clock and pinmux setup at runtime.

## Verify After Reboot

```bash
# 1. Driver loaded
lsmod | grep rs300

# 2. Probe succeeded
sudo dmesg | grep -i rs300

# 3. Video device present
ls /dev/video0

# 4. Media graph reports the sensor at YUYV8_2X8/384x288
media-ctl -d /dev/media0 -p | grep -A2 "rs300 10-003c"
```

## Stream

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=384,height=288,pixelformat=YUYV \
  --stream-mmap --stream-count=300 \
  --stream-to=/tmp/frames.yuv
```

Expected output size for 300 frames: `66355200` bytes (`384 * 288 * 2 * 300`).

## Flat-field correction (FFC)

The RS300 ships a frozen frame after stream start until a flat-field correction cycles the internal shutter. Trigger it via the `ffc_trigger` V4L2 button control on the subdev:

```bash
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=ffc_trigger=1
```

A common pattern is to fire FFC about 2 seconds after starting a stream. To wrap stream + FFC in one call:

```bash
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=384,height=288,pixelformat=YUYV \
  --stream-mmap --stream-count=900 \
  --stream-to=/tmp/frames-15s.yuv &
sleep 2
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=ffc_trigger=1
wait
```

## Known limitations

Pi Zero 2W has limited CPU and memory. The `640x512` module is known to have difficulty sustaining full frame rates: 60 fps is unlikely without running headless and minimising background services. The `384x288` and `256x192` modules are less affected.

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source (ported from rpi5 with YUYV8_2X8 mbus format) |
| `src/rs300-overlay.dts` | Device tree overlay (targets `i2c_csi_dsi`, `csi1`, `cam1_clk`) |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
