# Mini2 / WN2 Thermal Camera Driver for Raspberry Pi Zero 2W

**Status: Working with limitations**

Install instructions not yet written for this platform.

## Known Limitations

The Pi Zero 2W has limited CPU and memory resources. The 640x512 module is known to have difficulty sustaining full frame rates. 60 fps is unlikely without disabling the desktop environment and minimising background services. The 384x288 and 256x192 modules are less affected.

If you are using a 640x512 module and seeing dropped frames or low fps, try running headless and use `htop` to identify competing processes.
