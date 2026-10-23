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


## Current Status

- **Audio streaming only**
- Control/configuration must be done via the device’s web UI over Ethernet
- Default sample rate is 48 kHz, configurable at module load via `sample_rate=<Hz>`
- Runtime sample-rate changes are supported via ALSA `hw_params` (e.g., `aplay -r 96000`). The driver stops, reconfigures, and restarts USB streams automatically when the app requests a different supported rate. Capture and playback always run at the same rate and period size; once one direction is configured, the other is constrained to match
- Only rates whose packets fit the device's USB endpoints are offered (high rates need endpoints with more than 1024 bytes per microframe)
- Device clock must already be configured/locked to the requested rate. Clock source and rate are not changed by the driver. The driver measures the rate the device actually delivers and logs a warning if it differs from the stream rate (see `measured_rate` in `stream_stats`)
- Stream gaps are detected and the streams restarted automatically (see [Stream Gaps and Automatic Recovery](#stream-gaps-and-automatic-recovery))
- System suspend/resume: streams are stopped on suspend; applications restart them after resume
- Debug logging (`sudo dmesg -w` to view)

## Planned Improvements

- Changing device clock rate from the driver
- Recovering from clock loss (currently only detected and logged)
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
./build_and_load.sh --sample_rate=48000 --uframes_per_urb=128 --bpf_factor=16
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


## Installing as a DKMS Module

For permanent installation with automatic kernel rebuilds:

```bash
git clone -b hardening https://github.com/dylan-motu/snd-usb-motu
cd snd-usb-motu
sudo ./dkms_install.sh
```

The script builds the module for all installed kernels (skips already-installed ones).

### Load the module

```bash
sudo modprobe snd-usb-motu
```

### Uninstall

```bash
sudo ./dkms_uninstall.sh
```

### Verify

```bash
dkms status -m snd-usb-motu
```

## Viewing Logs
To see the driver's debug output in real-time:
```bash
sudo dmesg -w
```

## Driver‑Level Performance and Buffering Controls

The driver exposes several module parameters. The buffering parameters, together with ALSA‑level settings, control the trade‑off between **latency**, **stability**, **CPU usage**, and **memory footprint**; the others control stream recovery, scheduling and card naming.

### Module Parameters

| Parameter | Valid values | Default | Description |
|---|---|---|---|
| `sample_rate` | 44100, 48000, 88200, 96000, 176400, 192000 | 48000 | Initial sample rate in Hz |
| `uframes_per_urb` | 32, 64, 128, 256, 512 | 512 | USB microframes packed into each URB |
| `num_urbs` | 4 | 4 | Isochronous URBs per stream direction. 2 is no longer accepted: playback keeps two URBs in flight and the pool must have room for the next one |
| `pb_safety_offset` | 2, 4, 8, 16, 32 | 16 | Playback startup safety offset (microframes) |
| `rec_safety_offset` | 2, 4, 8, 16, 32 | 16 | Capture startup safety offset (microframes) |
| `bpf_factor` | 16, 32 | 32 | Multiplier for minimum ALSA period size (`BYTES_PER_FRAME × bpf_factor`) |
| `use_cfc` | -1, 0, 1 | -1 | Isochronous scheduling: `-1` auto, `0` always `URB_ISO_ASAP`, `1` xHCI CFC frame scheduling if the controller supports it (see below) |
| `auto_restart` | Y, N | Y | Restart the USB streams automatically when a stream gap is detected |
| `restart_xrun` | Y, N | Y | Report an xrun to ALSA when the streams are restarted after a gap |
| `test_hooks` | Y, N | N | Create the `inject_gap` proc file used by the hardware test (write a gap length in ms, 1–1000); testing only |
| `index` | -1, 0 – 31 | -1 | ALSA card index (`-1`: first free slot) |
| `id` | string | `motuusb` | ALSA card ID, also the name of the card's `/proc/asound` directory |

All parameters are fixed at module load time and apply to every MOTU device the driver binds; with more than one device connected, leave `index` at `-1`.

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
| ↓ `pb_safety_offset` | ↓ lower | ↗ tighter margin | ≈ neutral | ≈ neutral |
| ↓ `rec_safety_offset` | ↓ lower | ↗ tighter margin | ≈ neutral | ≈ neutral |
| ↓ `bpf_factor` (32 → 16) | ↓ lower | ↗ smaller periods allowed | ↑ more period callbacks | ≈ neutral |
| ↑ sample rate | ↓ lower per‑sample | ↗ higher data rate | ↑ more data per second | ↑ larger buffers |
| ↓ ALSA buffer size | ↓ lower | ↗ less safety margin | ≈ neutral | ↓ smaller ring buffer |
| ↓ ALSA period size | ↓ lower | ↗ tighter scheduling | ↑ more wakeups | ≈ neutral |

> **Hint — low‑latency starting point:**
> `uframes_per_urb=64 pb_safety_offset=4 rec_safety_offset=4 bpf_factor=16`
> combined with a small ALSA period/buffer. Increase values if you experience xruns.

> **Hint — maximum stability:**
> Keep all defaults (`uframes_per_urb=512 num_urbs=4 pb_safety_offset=16 rec_safety_offset=16 bpf_factor=32`)
> and use larger ALSA buffers.

> **Note:** `uframes_per_urb × num_urbs` is the amount of audio queued at the
> USB host controller ahead of time. It is the driver's only protection against
> scheduling delays: if the queue runs dry, the stream gets a gap (see below).
> Small values make that much more likely.
>
> All URBs of a stream share one transfer buffer of `uframes_per_urb` packet
> slots, which the driver fills while the URBs are already queued. Additional
> URBs (`num_urbs`) add scheduling headroom, not audio buffer.

## Stream Gaps and Automatic Recovery

The device does not detect underflows of its playback buffer. After a gap in
the isochronous USB stream (microframes without a packet) the channel mapping
can come out shifted by a group of eight channels, often only seconds after
the gap, and it stays shifted until the stream is restarted. The device has no
way to report this.

The driver therefore watches for the conditions that create a gap:

- a stream's URB queue drained (no other URB in flight when one completed)
- on xHCI, the host frame counter passed the end of the queued packets
  (catches delayed URB completions)
- a URB submission failed, or a URB was still in flight when it had to be reused
- missed service intervals on capture packets, any error on playback packets,
  failed URBs, a lost interrupt URB

When one is seen, it restarts the streams the way the hardware requires: stop
the URBs, select alternate setting 0, wait a few milliseconds, select alternate
setting 1, resubmit. By default an xrun is reported to ALSA so that JACK or
PipeWire resynchronise. More than 5 gaps within 10 seconds leave the streams
stopped until the application restarts them.

Statistics are available in the card's proc directory:

```bash
cat /proc/asound/motuusb/stream_stats
```

| Field | Meaning |
|---|---|
| `streaming` | Whether the USB streams are running |
| `rate` / `measured_rate` | Configured stream rate and the rate the device actually delivers (from capture packet sizes) |
| `scheduling` | `cfc` or `iso_asap` (see below) |
| `frame_check` | Whether the host frame counter check is available (xHCI only) |
| `auto_restart` | Value of the `auto_restart` parameter |
| `gaps` / `gap_restarts` | Detected stream gaps and restarts they caused; with `auto_restart=0` every gap is counted (reports within 200 ms count as one) |
| `last_gap` | Reason for the most recent gap |

A restart can also be triggered manually:

```bash
echo 1 | sudo tee /proc/asound/motuusb/soft_reset
```

### CFC Scheduling (`use_cfc`)

xHCI controllers with the Contiguous Frame ID Capability (CFC) can schedule
isochronous transfers on explicit, contiguous frame numbers instead of "as soon
as possible". Check your controller with:

```bash
sudo dmesg | grep 'hcc params'
```

CFC is supported if bit 11 (`0x800`) of the `hcc params` value is set.

- `use_cfc=-1` (default): the driver reads the capability from the controller
  and uses CFC only if it is supported **and** the kernel's xHCI driver is known
  to schedule CFC streams without inserting gaps of its own (see
  [kernel bugzilla 220748](https://bugzilla.kernel.org/show_bug.cgi?id=220748)).
  These fixes are in Linux **7.3** and later, so auto mode selects CFC on 7.3+
  and `URB_ISO_ASAP` on older kernels. (7.3-rc1 also reports itself as 7.3 but
  lacks the follow-up regression fix; use `use_cfc=0` there.)
  The fixes were not tagged for stable and, as of October 2026, have not been
  backported to any stable or LTS branch (7.2.y, 7.1.y, 6.18.y, 6.12.y, 6.6.y,
  6.1.y, 5.15.y, 5.10.y).
- `use_cfc=1`: use CFC whenever the controller supports it, also on a kernel
  older than 7.3, e.g. a distribution kernel that carries the xHCI isochronous
  frame ID fixes (`add8469b3e00`, `b08d153c95f1`, `05506a76f13a`) as patches.
  Ignored on controllers without CFC.
- `use_cfc=0`: always use `URB_ISO_ASAP`.

If two gap restarts happen within 10 seconds while CFC is in use, the driver
falls back to `URB_ISO_ASAP` until it is reloaded. The selected mode is logged
at probe and shown in `stream_stats`.

## Testing

`tests/hardening-test.sh` exercises the driver on real hardware: probe and
parameters, ALSA constraints, a channel-mapping loopback test with and
without load, injected stream gaps with automatic and manual recovery, the
clock mismatch warning, interface unbind/rebind, module reload cycles, the
scheduling modes, and optionally suspend/resume, unplug/replug and JACK.

Each playback channel carries its own test tone. Through a loopback, the
dominant frequency of every capture channel shows which playback channel it
receives, so a shifted channel mapping after a stream gap shows up in the
recording, window by window.

### 1. Prepare the device (web UI, once)

1. Connect the MOTU via USB **and** Ethernet (or Wi-Fi/AVB) and open its web
   UI (MOTU Pro Audio Control).
2. **Device tab:** set the clock source to **Internal**. The script sets the
   sample rate itself (see below), but 48 kHz there does no harm.
3. **Routing tab — loopback:** route *From Computer* channel *N* to
   *To Computer* channel *N* for all 24 channels (the diagonal), and remove
   any other source routed to those *To Computer* channels (mixer outputs,
   inputs). Note the current routing so you can restore it afterwards.
4. **Mixer:** no effects or processing on that path; levels at unity.

If your firmware cannot route *From Computer* to *To Computer*, use cables
instead, e.g. an optical cable from ADAT out to ADAT in or analog outputs to
analog inputs, route those inputs/outputs to the corresponding Computer
channels, and tell the script which capture channel receives which playback
channel: `--map 1:1,2:2,3:3,4:4` (capture:playback, 1-based). Only mapped
channels are checked.

**Keep monitors and headphones turned down**: the test plays 24 sine tones at
-12 dBFS.

### 2. Prepare the computer

1. Use a 7.3 kernel or newer for the CFC part of the test. On 7.3-rc1 the
   driver deliberately stays on `URB_ISO_ASAP` (rc1 lacks the xHCI regression
   fix), and the forced-CFC run may show gaps; that is reported, not failed.
2. Install the tools: `alsa-utils`, `python` with `numpy`; optionally `jack2`
   (for `--with-jack`) and `stress-ng` (adds memory pressure to the stress run).
3. Close applications that use audio. The script stops PulseAudio or
   PipeWire/WirePlumber (their systemd user units) and JACK for the duration
   of the test and restarts them afterwards. A sound server left running grabs
   the card as soon as it appears (PulseAudio keeps it busy for a long time
   while probing profiles), and every stream then fails with "Device or
   resource busy"; the script checks for this and names the process. It also
   unloads `snd-usb-audio` (all USB audio devices go away until the end of the
   test) and the drumfix `motu` driver, reloads whichever of them were loaded
   (in the original order) afterwards, and kills any running `aplay`/`arecord`.
4. The driver does not set the device clock, and a rate chosen in the web UI
   does not stick while `snd-usb-audio` and a sound server own the device:
   PulseAudio opens cards at 44.1 kHz and `snd-usb-audio` then switches the
   device to that rate over USB. So after stopping the sound server the script
   opens the device once at `--rate` through `snd-usb-audio`, which sets the
   clock, and then measures it with this driver. If it still does not match, it
   asks you to set the rate in the web UI and measures again.
5. Run it from the repository root as your normal user. It asks for your sudo
   password once (for `insmod`/`rmmod`, writing proc and sysfs files, and
   `rtcwake`).

### 3. Run

```bash
tests/hardening-test.sh                 # all automatic phases, about 10 minutes
tests/hardening-test.sh --with-suspend  # adds a suspend/resume cycle (machine sleeps 15 s)
tests/hardening-test.sh --with-unplug   # adds an unplug/replug step, asks you to pull the cable
tests/hardening-test.sh --with-jack     # adds a JACK round-trip test at the period size
tests/hardening-test.sh --phases baseline,inject   # selected phases only
tests/hardening-test.sh --device ultralite         # choose the device (see below)
```

**Several MOTU devices.** All supported models share the USB ID 07fd:0005.
With more than one connected, the script lists them and asks which one to
test; `--device` selects one up front by USB path (`3-4.3`), serial number or
part of the product name (`828es`, `ultralite`). Set up the loopback routing in
the web UI of that device. The other devices are bound to snd-usb-motu as well
while the test runs (they get their own cards, named `motuusb_1` and so on in
probe order), but only the selected one is streamed to and checked. The summary
names the device that was tested.

Further options: `--device`, `--map`, `--rate`, `--period`, `--duration`,
`--stress-duration`, `--keep-audio`, `--no-restore`, `--yes`; see
`tests/hardening-test.sh --help`.

| Phase | Checks |
|---|---|
| `preflight` | Device present, CFC capability, module builds without warnings, sound servers (PulseAudio, PipeWire, JACK) stopped, device clock set to `--rate` via `snd-usb-audio` |
| `load` | Probe, all three interfaces bound, parameters read-only, automatic scheduling choice, frame check, test hook, no other process holds the card, device clock at `--rate` (the test stops otherwise) |
| `hwparams` | Channels, rates and integer periods offered |
| `constraints` | With playback running, capture is pinned to the same period size and rate |
| `baseline` | Loopback mapping intact, no gaps, no xruns, measured device rate matches |
| `stress` | Same under CPU and disk load; any gap must be detected and recovered |
| `inject` | A real gap (test hook) is detected, the streams restart, the mapping recovers, applications see an xrun |
| `premise` | With `auto_restart=0` a 300 ms gap is only logged; whether the mapping then stays shifted shows how the device handles a gap; `soft_reset` recovers it |
| `ratecheck` | Streaming at 44.1 kHz while the device runs at 48 kHz logs the clock mismatch warning |
| `unbind` | Unbinding one interface via sysfs while streaming removes the card and releases all interfaces; rebinding works |
| `reload` | `rmmod` refused while in use, five unload/load cycles, `num_urbs=2` rejected |
| `cfc` | Gap recovery with `use_cfc=0` and `use_cfc=1`, CFC fallback |
| `suspend` | Card keeps its slot across suspend/resume, no re-probe, streaming works afterwards |
| `unplug` | Card goes away on unplug and comes back on replug |
| `jack` | JACK runs at the period size with a stable round-trip latency on channel 1 |

Every phase also fails if the kernel logs a warning, oops, lockdep or KASAN
report while it runs. For deeper checking, run the test once on a kernel built
with `CONFIG_PROVE_LOCKING` and `CONFIG_KASAN`.

### 4. Results

The summary is printed at the end and the exit status is non-zero if a phase
failed. `tests/results/<timestamp>/` contains the full log, the kernel log of
each phase, per-second `stream_stats` snapshots, the `aplay`/`arecord` output
and the analysis of each recording (`*.analysis.json`; its `timeline` has one
character per half second: `.` correct mapping, `X` shifted, `_` silence).

Afterwards restore your routing in the web UI. The script reloads
`snd-usb-audio` and restarts the sound server; reload the driver you normally
use yourself. `snd-usb-audio` and the sound server may switch the device clock
(e.g. to 44.1 kHz) once they own the device again, so check the sample rate in
the web UI before the next test run.

### 5. Channel shift test

`tests/usbaudio-shift-test.sh` provokes the channel shift again and again,
by default 20 times, 8 s apart, and judges the channel mapping between one
provocation and the next. It uses the same loopback and test signal.

With `snd-usb-audio` (the default), it stops `aplay` long enough to underrun.
On each underrun snd-usb-audio restarts its playback endpoint without
switching the alternate setting; on an UltraLite AVB, 14 of 20 underruns left
the mapping shifted by eight channels, up or down. With `--driver snd-usb-motu`
the module is built from this repository and loaded with `test_hooks=1`;
`--provoke` picks what each round does:

| `--provoke` | What happens | Expected with snd-usb-motu |
|---|---|---|
| `stop` | `aplay` underruns; the USB streams keep running | no shift, no restart |
| `reset` | `soft_reset`, the driver's full restart | no shift |
| `gap` | `inject_gap`, a real gap in the USB stream, detected and repaired | no shift, one restart each |
| `gap` with `--no-auto-restart` | the same gap, detected but not repaired | shows what a gap alone does: on both test devices, shifts by eight channels that start seconds after a gap |

```bash
tests/usbaudio-shift-test.sh --device ultralite                       # snd-usb-audio
tests/usbaudio-shift-test.sh --device ultralite --driver snd-usb-motu --provoke reset
tests/usbaudio-shift-test.sh --device ultralite --driver snd-usb-motu --provoke gap
tests/usbaudio-shift-test.sh --device ultralite --driver snd-usb-motu --provoke gap --no-auto-restart
```

`--load` adds CPU and disk load, `--rounds 0 --interval 300 --load` relies on
spontaneous xruns only, `--params` passes more module parameters, and
`--help` lists the other options. The USB audio drivers loaded before the test
are loaded again at the end, or snd-usb-audio if there were none. The
recordings go to `/dev/shm` while the test runs, so that page cache writeback
cannot stall `arecord` into overruns.

Results go to `tests/results/<timestamp>-<driver>-<provoke>/`. `intervals.txt`
has one line per boundary, i.e. per provocation and, with snd-usb-audio, per
xrun (each one restarts its endpoints). With snd-usb-motu, xruns in only one
direction leave the USB streams alone and are no boundary. Below the table,
the *shift episodes* list every run of shifted windows of one second or more
that is not the transient right after a boundary, with its start relative to
the boundary before it; one that starts in the middle of a span counts too.
Exit status 0 means at least one shift episode, 1 none, 2 that the run could
not tell (setup error, no tones).

## AI Tools Usage

This fork was developed with the help and usage of AI tools.

## Disclaimer

This is experimental software. Use at your own risk.

**This driver is not officially supported by MOTU, Inc.**