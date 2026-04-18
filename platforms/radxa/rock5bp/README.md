# Mini2 / WN2 Thermal Camera Driver for Rock 5B+ (RK3588)

Linux kernel driver for the Infisense RS300 thermal camera. Installs via DKMS on the stock Radxa Bookworm BSP kernel.

**Verified:** kernel `6.1.84-8-rk2410`, Radxa Bookworm SD image, 300-frame stream @ 60 fps.

## Prerequisites

- Rock 5B+ running Radxa Bookworm
- Internet access (for `git clone`)

## Install

```bash
git clone https://github.com/Kodrea/mini2-thermal-driver.git
cd mini2-thermal-driver/platforms/radxa/rock5bp
sudo ./install.sh
sudo reboot
```

The installer builds the rs300 module via DKMS, compiles and installs the device tree overlay, registers the overlay with `u-boot-update`, and appends the required kernel cmdline parameter. A reboot applies everything.

## The Blacklist Parameter

The stock Radxa BSP kernel fires `late_initcall(rkcif_clr_unready_dev)` at boot. This call force-empties the V4L2 async notifier's waiting list before any DKMS-loaded module can register. The rs300 driver loads too late and never binds.

The fix is a single kernel cmdline parameter:

```
initcall_blacklist=rkcif_clr_unready_dev
```

**The installer applies this automatically** by appending the parameter to `/etc/kernel/cmdline` and running `u-boot-update`. Without it, `lsmod` will show `rs300` loaded but the camera will not appear under `v4l2-ctl --list-devices`.

## Verify After Reboot

```bash
# 1. Blacklist is active
cat /proc/cmdline | grep initcall_blacklist

# 2. Driver loaded
lsmod | grep rs300

# 3. Probe succeeded
sudo dmesg | grep -i rs300

# 4. Camera found on I2C bus 3 at 0x3c
sudo i2cdetect -y 3       # expect UU at 0x3c

# 5. Video device present
v4l2-ctl --list-devices   # expect /dev/video0 under rkcif-mipi-lvds2
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

## Manual Install (Without the Installer)

```bash
# Install dependencies
sudo apt install -y linux-headers-$(uname -r) dkms device-tree-compiler

# DKMS module
sudo mkdir -p /usr/src/rs300-0.0.1
sudo cp dkms.conf Makefile src/rs300.c /usr/src/rs300-0.0.1/
sudo dkms add rs300/0.0.1
sudo dkms build rs300/0.0.1
sudo dkms install rs300/0.0.1

# Autoload at boot
echo rs300 | sudo tee /etc/modules-load.d/rs300.conf

# Compile and install overlay
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I /usr/src/linux-headers-$(uname -r)/include \
    src/rock-5b-plus-rs300-cam0.dts | \
  dtc -@ -I dts -O dtb -o /boot/dtbo/rock-5b-plus-rs300-cam0.dtbo

# Register overlay with u-boot-menu
echo 'U_BOOT_FDT_OVERLAYS="rock-5b-plus-rs300-cam0.dtbo"' \
  | sudo tee -a /etc/default/u-boot

# Apply blacklist
CURRENT=$(sudo cat /etc/kernel/cmdline)
echo "$CURRENT initcall_blacklist=rkcif_clr_unready_dev" \
  | sudo tee /etc/kernel/cmdline

sudo u-boot-update
sudo reboot
```

## Files

| File | Role |
|-|-|
| `install.sh` | Automated installer |
| `src/rs300.c` | Kernel driver source |
| `src/rock-5b-plus-rs300-cam0.dts` | Device tree overlay |
| `Makefile` | DKMS build |
| `dkms.conf` | DKMS config |
