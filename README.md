# mini2-thermal-driver

Linux kernel driver for the Mini2 / WN2 thermal camera module (Infisense RS300). Supports multiple SBCs via platform-specific installers and device tree overlays.

> **Imaging only.** This camera produces thermal video for visualization. It does not support radiometric temperature measurement. Pixel values represent relative heat intensity, not absolute temperatures.

## Supported Platforms

### Raspberry Pi

| Platform | SoC | Status | Install |
|-|-|-|-|
| Raspberry Pi 5 | BCM2712 | Working | [platforms/raspberry-pi/rpi5](platforms/raspberry-pi/rpi5/README.md) |
| Raspberry Pi 4B | BCM2711 | Refactoring | [platforms/raspberry-pi/rpi4b](platforms/raspberry-pi/rpi4b/README.md) |
| Raspberry Pi CM5 | BCM2712 | Coming soon | [platforms/raspberry-pi/cm5](platforms/raspberry-pi/cm5/README.md) |
| Raspberry Pi Zero 2W | BCM2710 | Working (see notes) | [platforms/raspberry-pi/zero2w](platforms/raspberry-pi/zero2w/README.md) |

### Radxa

| Platform | SoC | Status | Install |
|-|-|-|-|
| Rock 5B+ | RK3588 | Working | [platforms/radxa/rock5bp](platforms/radxa/rock5bp/README.md) |
| Zero 3W | RK3566 | Working | [platforms/radxa/zero3w](platforms/radxa/zero3w/README.md) |

### NVIDIA

| Platform | SoC | Status | Install |
|-|-|-|-|
| Jetson Orin Nano | Tegra Orin | In development | [platforms/nvidia/orin-nano](platforms/nvidia/orin-nano/README.md) |

## Quick Start

Navigate to your platform's directory and follow its README.

## Driver Architecture

The driver (`src/rs300.c`) is a V4L2 camera sensor subdevice. It communicates with the camera over I2C for control and receives video over MIPI CSI-2 (2 lanes, 80 MHz). All SoC-specific routing is handled by the platform's device tree overlay.

Each platform carries its own copy of `rs300.c`. See [SYNC.md](SYNC.md) for the policy on keeping them in sync.

## Camera Specifications

- **Sensor:** RS300 (Infisense)
- **Interface:** I2C (0x3c) + MIPI CSI-2 (2 lanes, 80 MHz)
- **Pixel format:** UYVY

### Modules

| Module | Resolution | FPS |
|-|-|-|
| Mini2 256 | 256×192 | 25 / 50 fps |
| Mini2 384 | 384×288 | 30 / 60 fps |
| Mini2 640 | 640×512 | 30 / 60 fps |

Resolution is fixed per physical module and set at driver load time via the `mode` parameter. Runtime switching is not supported.

> **Hardware variation notice.** The Mini2 / WN2 module is sold by multiple vendors. Units from different sources may contain different internal processing chips or firmware versions. This driver is developed and tested against specific hardware. If you experience issues, please note your vendor, firmware version, and module resolution when reporting.
>
> I recommend purchasing from **[Purple River](https://www.thermal-image.com/product/mini2-640512-9mm-thermal-imaging-camera-module-for-drones/)**. Select your resolution and lens from the dropdown on their product page.
