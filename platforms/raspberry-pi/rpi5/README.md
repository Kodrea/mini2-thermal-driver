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
384x288 and 640x512 modules support 30 or 60 fps; 256x192 modules support 25 or
50 fps. Pick the higher rate unless you are short on bandwidth or CPU.

Run this at an interactive terminal. It prompts for input, so driving it through
a non-interactive SSH command exits at the first prompt without an error. For
that case use the scripted form below.

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
- **`rs300-media-setup.service`** which enables the CSI-2 capture link on every boot

## After Reboot

Verify the module loaded and the camera enumerated:

```bash
dmesg | grep rs300              # module probe messages
systemctl status rs300-media-setup   # capture link enabled
v4l2-ctl --list-devices         # expect /dev/video0 under the rp1-cfe group
v4l2-ctl -d /dev/v4l-subdev2 --get-subdev-fmt pad=0   # sensor resolution
```

The RP1 CFE creates the link that carries frames to the capture node in a
disabled state, so it has to be enabled after every boot.
`rs300-media-setup.service` does that. If capture fails with
`VIDIOC_STREAMON returned -1 (Invalid argument)` and `dmesg` shows
`csi2_ch0 node link is not enabled`, run it by hand:

```bash
sudo /usr/lib/rs300/rs300-media-setup.sh
```

### Confirm frames without a display

`rs300-stream` needs a desktop session. On a headless Pi, capture straight from
the video node instead. Substitute your module's resolution:

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=384,height=288,pixelformat=YUYV \
  --stream-mmap --stream-count=300 --stream-to=/tmp/capture.yuv
ls -l /tmp/capture.yuv
```

A 384x288 YUYV frame is 221184 bytes, so 300 frames is exactly 66355200 bytes.
Any other size means frames were dropped.

The sensor settles to a nearly constant image on a static scene, so a capture
can look frozen while the pipeline is healthy. To prove frames are live, trigger
FFC while the capture is running rather than before it:

```bash
( sleep 3; v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl ffc_trigger=1 ) &
v4l2-ctl -d /dev/video0 --set-fmt-video=width=384,height=288,pixelformat=YUYV \
  --stream-mmap --stream-count=300 --stream-to=/tmp/capture.yuv
```

Live thermal preview, on a Pi with a display:

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
<!-- Disabled. Toggling the sensor to Y16 desyncs it from the viewer, whose
     pipeline is fixed at YUYV. Y16 needs the ISP capture path, which this
     platform does not set up.
| `y` | Toggle output mode (YUV / Y16) |
-->
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
sudo rm -f /etc/udev/rules.d/99-rs300.rules
sudo rm -f /etc/modprobe.d/rs300.conf
sudo systemctl disable --now rs300-media-setup.service
sudo rm -f /etc/systemd/system/rs300-media-setup.service
sudo rm -rf /usr/lib/rs300
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
```

Then remove the overlay line from the boot config and reboot:

```bash
sudo sed -i '/^dtoverlay=rs300$/d' /boot/firmware/config.txt
grep -c rs300 /boot/firmware/config.txt   # expect 0
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
| `helpers/rs300-stream.py` | Live viewer (GTK + GStreamer) |
| `helpers/rs300-media-setup.sh` | Enables the CSI-2 capture link |
| `helpers/rs300-media-setup.service` | Runs the capture link setup at boot |
