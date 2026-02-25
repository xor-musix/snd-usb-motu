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

All parameters are optional — defaults are used when omitted:
```bash
./build_and_load.sh --sample_rate=48000 --uframes_per_urb=128 --num_urbs=2 --bpf_factor=16
```

The script will:

- Remove the generic ALSA USB Audio Class (snd-usb-audio) driver
- Remove any previously loaded snd-usb-motu module
- Remove any previously loaded motu module (Drumfix driver)
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

## Driver‑Level Performance and Buffering Controls

The driver exposes several module parameters that, together with ALSA‑level settings, control the trade‑off between **latency**, **stability**, **CPU usage**, and **memory footprint**.

### Module Parameters

| Parameter | Valid values | Default | Description |
|---|---|---|---|
| `sample_rate` | 44100, 48000, 88200, 96000, 176400, 192000 | 48000 | Initial sample rate in Hz |
| `uframes_per_urb` | 32, 64, 128, 256, 512 | 512 | USB microframes packed into each URB |
| `num_urbs` | 2, 4 | 4 | Isochronous URBs per stream direction |
| `pb_safety_offset` | 2, 4, 8, 16, 32 | 16 | Playback startup safety offset (microframes) |
| `rec_safety_offset` | 2, 4, 8, 16, 32 | 16 | Capture startup safety offset (microframes) |
| `bpf_factor` | 16, 32 | 32 | Multiplier for minimum ALSA period size (`BYTES_PER_FRAME × bpf_factor`) |

### ALSA Parameters (set by the application)

| Parameter | Typical range | Description |
|---|---|---|
| Sample rate | 44100 – 192000 Hz | Audio sample rate negotiated via `hw_params` |
| Buffer size | hardware‑dependent | Total ring‑buffer length in frames |
| Period size | ≥ `bpf_factor` frames | Chunk size between interrupts / callbacks |

### Impact Matrix

| Tuning action | Latency | Stability | CPU usage | Memory |
|---|---|---|---|---|
| ↓ `uframes_per_urb` | ↓ lower | ↗ may decrease | ↑ higher (more URB completions) | ↓ smaller URBs |
| ↓ `num_urbs` (4 → 2) | ↓ lower | ↗ less headroom | ≈ neutral | ↓ fewer buffers |
| ↓ `pb_safety_offset` | ↓ lower | ↗ tighter margin | ≈ neutral | ≈ neutral |
| ↓ `rec_safety_offset` | ↓ lower | ↗ tighter margin | ≈ neutral | ≈ neutral |
| ↓ `bpf_factor` (32 → 16) | ↓ lower | ↗ smaller periods allowed | ↑ more period callbacks | ≈ neutral |
| ↑ sample rate | ↓ lower per‑sample | ↗ higher data rate | ↑ more data per second | ↑ larger buffers |
| ↓ ALSA buffer size | ↓ lower | ↗ less safety margin | ≈ neutral | ↓ smaller ring buffer |
| ↓ ALSA period size | ↓ lower | ↗ tighter scheduling | ↑ more wakeups | ≈ neutral |

> **Hint — low‑latency starting point:**
> `uframes_per_urb=64 num_urbs=2 pb_safety_offset=4 rec_safety_offset=4 bpf_factor=16`
> combined with a small ALSA period/buffer. Increase values if you experience xruns.

> **Hint — maximum stability:**
> Keep all defaults (`uframes_per_urb=512 num_urbs=4 pb_safety_offset=16 rec_safety_offset=16 bpf_factor=32`)
> and use larger ALSA buffers.

## Disclaimer

This is experimental software. Use at your own risk.

**This driver is not officially supported or endorsed by MOTU, Inc.**