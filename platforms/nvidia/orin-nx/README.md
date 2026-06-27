# Mini2 / WN2 RS300/WN2640 Thermal Camera - NVIDIA Jetson Orin NX

Status: tested on CAM1 with an Auvidea JNX Orin carrier, Jetson Linux
`5.15.185-tegra`, I2C address `0x3c`, and a module identifying as
`Camera WN2640`.

This is an imaging driver only. The stream is relative thermal image data, not
radiometric temperature data.

## Hardware Path

- Connector: CAM1 / CSI1 on the Auvidea JNX carrier
- I2C: `i2c-10`, mux channel 1, camera address `0x3c`
- Video node: `/dev/video1`
- Subdevice: usually `/dev/v4l-subdev2`
- CSI: 2 lanes, 80 MHz link frequency
- Fixed sensor mode: `640x512` at 60 fps
- Tegra media bus format: `UYVY8_2X8`
- Camera-side YUV order: `YUYV`
- Power enable: CAM1 `PAC.00`, visible as `gpiochip0` line `138`; in the
  Tegra234 device tree this is GPIO ID `160` (`TEGRA234_MAIN_GPIO(AC, 0)`).

The Jetson VI exposes the stream as `UYVY`. For human display the useful
thermal luma is interpreted as `YUY2`; see the GStreamer section.

## Install

Build and install on the Jetson:

```sh
cd mini2-thermal-driver/platforms/nvidia/orin-nx
sudo ./install.sh --set-default
sudo reboot
```

The installer:

- Builds `rs300.ko`
- Builds and installs `/boot/rs300-cam1.dtbo`
- Installs `/lib/modules/$(uname -r)/extra/rs300.ko`
- Writes `/etc/modprobe.d/rs300.conf`
- Adds or updates the `rs300_cam1` extlinux boot entry
- With `--set-default`, also mirrors the RS300 DTB/overlay into the NVIDIA
  `primary` extlinux entry. Some Jetson UEFI setups keep booting `primary`
  even when `DEFAULT` points at another label.
- Merges the overlay into `/boot/rs300-cam1-active.dtb` and writes it to the
  active `A_kernel-dtb`/`B_kernel-dtb` partition with a backup under
  `/boot/rs300-backups`. Use `--no-slot-dtb` only if your bootloader is known
  to honor extlinux `FDT`/`OVERLAYS` directly.
- Enables `rs300-load.service`
- Disables the legacy `cam1-pac00-high.service` if it exists

After reboot:

```sh
systemctl status rs300-load.service
v4l2-ctl -d /dev/video1 --list-formats-ext
v4l2-ctl -d /dev/v4l-subdev2 -l
```

Expected video format:

```text
UYVY 640x512 60 fps
```

## Validate Video

Capture 180 frames:

```sh
v4l2-ctl -d /dev/video1 \
  --set-fmt-video=width=640,height=512,pixelformat=UYVY \
  --stream-mmap=4 --stream-count=180 --stream-to=/tmp/rs300.raw
stat -c%s /tmp/rs300.raw
```

Expected size:

```text
117964800
```

Check that later frames are not the blank `00 80` pattern:

```sh
FRAME=$((640 * 512 * 2))
for n in 0 10 60 120 179; do
  dd if=/tmp/rs300.raw bs="$FRAME" skip="$n" count=1 2>/dev/null |
    od -An -tu1 -v |
    awk -v frame="$n" '{ for (i = 1; i <= NF; i++) c[$i] = 1 }
      END { printf("frame %s unique bytes: %d\n", frame, length(c)) }'
done
```

A healthy frame has far more than 10 unique byte values.

## V4L2 Controls

List controls:

```sh
v4l2-ctl -d /dev/v4l-subdev2 -l
```

Important controls:

| Control | Type | Meaning |
|-|-|-|
| `brightness` | int | GET/SET brightness, 0-100 |
| `contrast` | int | SET contrast; this WN2640-like module accepts the verified default value `50` |
| `colormap` | menu | GET/SET palette |
| `ffc_trigger` | button | Manual FFC command; the tested WN2640-like module rejects it, so periodic `auto_shutter` is the validated calibration path |
| `scene_mode` | menu | SET scene mode; verified mode is `3` / General Mode |
| `digital_detail_enhancement` | int | SET DDE; verified value is `50` |
| `spatial_noise_reduction` | int | SET spatial NR; verified value is `50` |
| `temporal_noise_reduction` | int | SET temporal NR; verified value is `50` |
| `output_mode` | menu | YUV or Y16 output mode |
| `auto_shutter` | bool | Periodic internal shutter/FFC |
| `auto_shutter_min_interval` | int | Minimum FFC interval |
| `auto_shutter_max_interval` | int | Maximum FFC interval |
| `shutter_state` | menu | Open/close shutter |
| `detector_frame_rate` | menu | 25/30/50/60 Hz detector setting |
| `yuv_format` | menu | Camera-side YUV byte order |
| `zoom_absolute` | int | SET digital zoom, 1-8 |

Examples:

```sh
v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=brightness
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=brightness=55
v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl=colormap
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=colormap=10
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=auto_shutter=1
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=auto_shutter_min_interval=10
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=auto_shutter_max_interval=120
```

The shutter may click periodically when `auto_shutter=1`. That is expected
thermal flat-field calibration.

## Module Parameters

Defaults in `/etc/modprobe.d/rs300.conf`:

```text
mode=0 fps=60 set_fps_cmd=0 type=16 output_source=-1 yuv_order=2 start_path=1 start_dst=1 power_on_delay_ms=8000 auto_shutter=1 startup_ffc=0
```

Useful parameters:

| Parameter | Default | Meaning |
|-|-|-|
| `mode` | `0` | `0=640x512`, `1=256x192`, `2=384x288` |
| `fps` | `60` | FPS byte in the START packet |
| `set_fps_cmd` | `0` | Send separate FPS command before START |
| `yuv_order` | `2` | `0=UYVY`, `1=VYUY`, `2=YUYV`, `3=YVYU` |
| `start_path` | `1` | START packet path byte |
| `start_dst` | `1` | START packet destination byte |
| `power_on_delay_ms` | `8000` | Delay after enabling CAM1 power |
| `auto_shutter` | `1` | Initial periodic shutter state, `-1` leaves firmware default |
| `startup_ffc` | `0` | Attempt one manual FFC before first stream; leave disabled on the tested WN2640-like module |

For firmware builds that support startup-only manual shutter calibration:

```text
options rs300 ... auto_shutter=0 startup_ffc=1
```

For fully manual shutter control:

```text
options rs300 ... auto_shutter=0 startup_ffc=0
```

## GStreamer

Stream from the Jetson to a workstation:

```sh
gst-launch-1.0 -e \
  v4l2src device=/dev/video1 io-mode=2 do-timestamp=true \
  ! 'video/x-raw,format=UYVY,width=640,height=512,framerate=60/1' \
  ! rtpvrawpay pt=96 mtu=1400 \
  ! udpsink host=<WORKSTATION_IP> port=5000 sync=false async=false
```

Display on the workstation:

```sh
gst-launch-1.0 -e \
  udpsrc port=5000 caps='application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)RAW,sampling=(string)YCbCr-4:2:2,depth=(string)8,width=(string)640,height=(string)512,colorimetry=(string)BT601-5,payload=(int)96' \
  ! rtpjitterbuffer latency=100 drop-on-latency=true \
  ! rtpvrawdepay \
  ! rawvideoparse use-sink-caps=false format=yuy2 width=640 height=512 framerate=60/1 \
  ! videoconvert \
  ! autovideosink sync=false
```

## Notes

The module is WN2640-like but speaks the same 18-byte control packet shape used
by the RS300 driver family: command class, module, sub-command, 12 parameter
bytes, and CRC-16. The driver centralizes command execution in one helper that
builds packets, polls status, decodes firmware errors, and reads results for GET
commands.

The first capture after boot can fail or produce unusable data while Tegra VI and
the camera settle. `rs300-load.service` performs a readiness validation capture
and only succeeds once a correctly sized frame set with real byte variance is
available.
