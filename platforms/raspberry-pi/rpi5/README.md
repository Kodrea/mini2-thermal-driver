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
- **`rs300-stream`** live viewer (GTK + GStreamer) in `/usr/local/bin`
- **Module config** for resolution and FPS stored in `/etc/modprobe.d/rs300.conf`

## After Reboot

Verify the module loaded and the camera enumerated:

```bash
dmesg | grep rs300              # module probe messages
v4l2-ctl --list-devices         # expect /dev/video0 plus rs300 subdevs
v4l2-ctl -d /dev/v4l-subdev2 --get-subdev-fmt pad=0   # sensor resolution
```

Live thermal preview:

```bash
rs300-stream
```

The viewer auto-detects the sensor resolution (256x192, 384x288, or 640x512) at startup and opens fullscreen. Controls work from the video window or the launching terminal.

### rs300-stream Controls

| Key | Action |
|-|-|
| `f` | Trigger FFC (flat-field correction) |
| `c` | Cycle colormap (0..11) |
| `m` | Cycle scene mode (0..5) |
| `a` | Toggle auto shutter |
| `y` | Toggle output mode (YUV / Y16) |
| `+` / `-` | Brightness +10 / -10 |
| `]` / `[` | Contrast +5 / -5 |
| `}` / `{` | Digital detail enhancement +5 / -5 |
| `q` / ESC | Quit |
| `Ctrl+C` | Quit (from launching terminal) |

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

Some modules freeze on the first frame until FFC is triggered. `rs300-stream` does this automatically. To trigger manually:

```bash
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl ffc_trigger=1
sleep 2
```

### Device numbers changed after reboot

Video device numbers can change between boots. List the current nodes and find the rs300 entry:

```bash
v4l2-ctl --list-devices
```

## Uninstall

```bash
sudo dkms remove -m rs300 -v 0.0.1 --all
sudo rm -f /usr/local/bin/rs300-stream
sudo rm -f /boot/firmware/overlays/rs300.dtbo
```

Then remove the `dtoverlay=rs300` line from `/boot/firmware/config.txt` and reboot.

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source |
| `src/rs300-overlay.dts` | Device tree overlay |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
| `helpers/rs300-stream.py` | Live viewer (GTK + GStreamer) |
