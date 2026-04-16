# Mini2 / WN2 Thermal Camera Driver for Raspberry Pi 5

Linux kernel driver for the Mini2 (aka WN2) thermal camera module on Raspberry Pi 5. Outputs YUYV422 video over CSI-2 for live viewing with standard V4L2 tools.

## Hardware Requirements

- Raspberry Pi 5 (4GB+ recommended)
- Mini2 / WN2 thermal camera module (384x288, 256x192, or 640x512)
- 22-to-15 pin CSI ribbon cable
- 5V/3A+ power supply

## Install

```bash
git clone https://github.com/Kodrea/mini2-thermal-driver.git
cd mini2-thermal-driver/platforms/raspberry-pi/rpi5
sudo ./install.sh
sudo reboot
```

The installer will ask which module you have and your preferred frame rate.

### Scripted Install

For CI or headless setups, `--auto` skips dependency prompts and accepts resolution/fps as arguments:

```bash
sudo ./install.sh --auto 384 30 y   # 384x288 @ 30fps, auto-confirm
sudo ./install.sh --auto 640 60 y   # 640x512 @ 60fps
sudo ./install.sh --auto 256 25 y   # 256x192 @ 25fps
```

### What Gets Installed

- **DKMS kernel module** (`rs300.ko`) rebuilt automatically on kernel updates
- **Device tree overlay** (`rs300.dtbo`) and `config.txt` entry
- **Init script and systemd service** that re-enables media links after every boot
- **CLI tools** including `rs300-status`, `rs300-test`, `rs300-stream`, `rs300-healthcheck` in `/usr/local/bin`
- **Module config** for resolution and FPS stored in `/etc/modprobe.d/rs300.conf`

## After Reboot

```bash
rs300-status
```

Sample output when everything works:

```
RS300 Thermal Camera Status
===========================

  Driver:    ok    installed (rs300 0.0.1, kernel 6.12.25+rpt-rpi-2712)
  Sensor:    ok    /dev/v4l-subdev2 on /dev/media0
  Init:      ok    active (since 2026-03-16 10:00:00)
  Config:    ok    /run/rs300/devices (mode: yuyv)
  Video:     ok    /dev/video0 (YUYV 384/288)
  Comms:     ok    sensor responding (output_mode=0)

All checks passed.
```

Comprehensive healthcheck (22 checks):

```bash
rs300-healthcheck
```

Live thermal preview:

```bash
rs300-test
```

Press `c` to cycle colormaps, `f` for FFC, `q` to quit.

Zero-copy GStreamer live view:

```bash
rs300-stream
rs300-stream --show-fps
rs300-stream --fps 30
rs300-stream --resolution 640x512
```

### rs300-test Controls

| Key | Action |
|-|-|
| `c` | Cycle colormap |
| `v` | Cycle scene mode |
| `f` | Trigger FFC |
| `a` / `z` | Brightness +10 / -10 |
| `s` / `x` | Contrast +10 / -10 |
| `d` / `e` | Spatial NR +10 / -10 |
| `g` / `b` | Temporal NR +10 / -10 |
| `h` / `n` | DDE +10 / -10 |
| `r` | Reset all to defaults |
| `q` / ESC | Quit |

## Configuration

```bash
# View current config
cat /etc/modprobe.d/rs300.conf

# Change mode (0=640x512, 1=256x192, 2=384x288)
echo "options rs300 mode=2 fps=60" | sudo tee /etc/modprobe.d/rs300.conf
sudo reboot
```

## Troubleshooting

### Camera not detected

```bash
i2cdetect -y 10    # expect device at 0x3c
dtoverlay -l       # expect rs300 listed
grep rs300 /boot/firmware/config.txt
```

### Black or frozen image

Some modules freeze on the first frame until FFC is triggered. `rs300-test` does this automatically. To trigger manually:

```bash
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl ffc_trigger=1
sleep 2
```

### Device numbers changed after reboot

Video device numbers can change between boots. Stable paths are written to `/run/rs300/devices`:

```bash
cat /run/rs300/devices
```

## Uninstall

```bash
sudo ./build/uninstall.sh
sudo reboot
```

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source |
| `src/rs300-overlay.dts` | Device tree overlay |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
