# Mini2 / WN2 Thermal Camera Driver for Raspberry Pi 4B (and CM4, Zero 2 W)

Linux kernel driver for the Infisense RS300 thermal camera on Raspberry Pi boards that use the legacy unicam camera stack (`bcm2835-unicam-legacy`). Installs via DKMS on the stock Raspberry Pi OS kernel.

**Verified:** Raspberry Pi 4 Model B Rev 1.4, Raspberry Pi OS (Trixie, 64-bit), kernel `6.12.75+rpt-rpi-v8`, 300-frame stream at 60 fps, 2026-04-21.

This installer also accepts Raspberry Pi 3 family, CM4, and Zero 2 W (any BCM2710 / BCM2711 / BCM2837 board on the legacy unicam path). It rejects Pi 5 with a pointer to `../rpi5/`, because Pi 5 uses RP1-CFE, not legacy unicam.

## Prerequisites

- Raspberry Pi 4 Model B (primary target) or any BCM2710/11/37 Pi with a single CSI connector
- Raspberry Pi OS (Bookworm or Trixie, 64-bit; Lite recommended for headless capture)
- Mini2 MIPI adapter board (wired for 5V camera power)
- Internet access (for `git clone` and apt packages)

## Install

```bash
git clone https://github.com/Kodrea/mini2-thermal-driver.git
cd mini2-thermal-driver/platforms/raspberry-pi/rpi4b
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

The media device index is not stable across Pi variants (Pi 4B typically lands at `/dev/media4`, Pi Zero 2 W at `/dev/media0`). The verification command below scans all media devices and reports the one that hosts the rs300 sensor.

```bash
# 1. Driver loaded
lsmod | grep rs300

# 2. Probe succeeded
sudo dmesg | grep -i rs300

# 3. Video device present
ls /dev/video0

# 4. Media graph reports the sensor at YUYV8_2X8/384x288
for m in /dev/media*; do
    media-ctl -d "$m" -p 2>/dev/null | grep -q "rs300 10-003c" && \
      { echo "--- $m ---"; media-ctl -d "$m" -p | grep -A2 "rs300 10-003c"; break; }
done
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

## Notes on accepted boards

The installer accepts any Pi that runs `bcm2835-unicam-legacy` (Pi 3, Pi 4, CM4, Zero 2 W). The driver and overlay themselves are byte-identical to the zero2w platform directory, because the downstream Pi unicam driver does not distinguish between these SoCs at the CSI block level. See `SYNC.md` at the repo root.

Verified today on Pi 4B. Pi 3 family, CM4, and Zero 2 W are expected to work but carry the zero2w caveats: the Zero 2 W in particular is resource-constrained on the `640x512` module.

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source (identical to zero2w) |
| `src/rs300-overlay.dts` | Device tree overlay (targets `i2c_csi_dsi`, `csi1`, `cam1_clk`) |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
