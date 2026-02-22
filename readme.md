# snd-usb-motu

A Linux ALSA USB driver for first-generation MOTU Pro Audio hardware.

Currently **only USB 2.0 models** are supported:

- 16A
- 1248
- 112D
- 8M
- Monitor 8
- 24Ai
- 24Ao
- Stage-B16
- UltraLite AVB
- UltraLite-mk4
- M64
- 8D
- LP32
- 828es
- 8pre-es

> **Note:** This driver is in **early development**. Expect limited functionality and noisy logging.


## Current Status

- **Audio streaming only**
- Control/configuration must be done via the device’s web UI over Ethernet
- Default sample rate is 48 kHz, configurable at module load via `sample_rate=<Hz>`
- `uframes_per_urb=<N>` — number of USB microframes packed into each URB (valid: 1, 2, 4, 8, 16, 32; default: **8**). Lower values reduce latency; higher values reduce CPU overhead. The driver maintains a fixed total of 256 microframes, so the URB count is `256 / uframes_per_urb`
- Runtime sample-rate changes are supported via ALSA `hw_params` (e.g., `aplay -r 96000`). The driver stops, reconfigures, and restarts USB streams automatically when the app requests a different supported rate
- Device clock must already be configured/locked to the requested rate. Clock source and rate are not changed by the driver. If the hardware isn't locked to the requested rate, audio may fail to start or underrun/overrun
- Debug logging (`sudo dmesg -w` to view)

## Planned Improvements

- Changing device clock rate from the driver
- Handling clock loss
- 64 channels

## Building and Running

Tested on **Ubuntu 24.04.2 LTS** with a fresh install.

### Install prerequisites
```bash
sudo apt update
sudo apt install build-essential
```

### Clone this repository
```bash
git clone https://github.com/dylan-motu/snd-usb-motu
cd snd-usb-motu
```

### Build and load the driver
```bash
./build_and_load.sh
```

The script will:

- Remove the generic ALSA USB Audio Class (snd-usb-audio) driver
- Remove any previously loaded snd-usb-motu module
- Compile this driver
- Insert the compiled module

This does not install the driver permanently — after a reboot, the system will return to normal.

In my experience, you can run this script with the device connected. If you encounter any issues, try these steps:
1. Disconnect the device
1. Run the script
1. Ensure the device's clock is set to SAMPLE_RATE and is locked
1. Reconnect the device

## Viewing Logs
To see the driver's debug output in real-time:
```bash
sudo dmesg -w
```

## Disclaimer

This is experimental software. Use at your own risk.

**This driver is not officially supported or endorsed by MOTU, Inc.**