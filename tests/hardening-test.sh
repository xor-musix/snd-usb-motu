#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Hardware test for the snd-usb-motu hardening changes.
#
# Run as your normal user from the repository root; commands that need
# root go through sudo. See "Testing" in readme.md for the required setup
# (loopback routing, device clock, PipeWire/JACK stopped).
#
#   tests/hardening-test.sh [options]
#
# Options:
#   --device SPEC     device to test when several MOTU devices are connected:
#                     USB path (e.g. 3-4.3), serial number, or part of the
#                     product name (e.g. ultralite); asks if not given
#   --phases LIST     comma separated phases to run (default: all automatic)
#                     load,hwparams,constraints,baseline,stress,inject,
#                     premise,ratecheck,unbind,reload,cfc (preflight always runs)
#   --with-suspend    also run the suspend/resume phase (suspends the machine)
#   --with-unplug     also run the interactive unplug/replug phase
#   --with-jack       also run the JACK round-trip phase
#   --map SPEC        loopback mapping, 'identity' (default) or capture:playback
#                     pairs, 1-based, e.g. '1:1,2:2,3:3,4:4' for four cables
#   --rate HZ         stream rate; the device clock must be set to it (48000)
#   --period FRAMES   ALSA period size (256)
#   --duration S      baseline run length in seconds (30)
#   --stress-duration S  stress run length in seconds (90)
#   --keep-audio      keep the raw recordings (about 3.5 MB per second)
#   --no-restore      leave snd-usb-motu loaded at the end
#   --yes             do not ask before stopping PipeWire/JACK
#
# Results and logs go to tests/results/<timestamp>/. Exit status is 0 only
# if no phase failed.

set -u -o pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
HELPER="$REPO/tests/motu_signal.py"
MODULE="$REPO/snd-usb-motu.ko"
# resolved from the selected USB device once the card exists (resolve_card):
# with several devices the ALSA ids are motuusb, motuusb_1, ... in probe order
CARD=motuusb
PCM="hw:CARD=$CARD,DEV=0"
CHANNELS=24
FORMAT=S24_3LE

PHASES=load,hwparams,constraints,baseline,stress,inject,premise,ratecheck,unbind,reload,cfc
WITH_SUSPEND=0
WITH_UNPLUG=0
WITH_JACK=0
MAP=identity
RATE=48000
PERIOD=256
DURATION=30
STRESS_DURATION=90
KEEP_AUDIO=0
RESTORE=1
ASSUME_YES=0
DEVICE_SPEC=

while [ $# -gt 0 ]; do
    case "$1" in
        --phases)          PHASES=$2; shift ;;
        --with-suspend)    WITH_SUSPEND=1 ;;
        --with-unplug)     WITH_UNPLUG=1 ;;
        --with-jack)       WITH_JACK=1 ;;
        --map)             MAP=$2; shift ;;
        --rate)            RATE=$2; shift ;;
        --period)          PERIOD=$2; shift ;;
        --duration)        DURATION=$2; shift ;;
        --stress-duration) STRESS_DURATION=$2; shift ;;
        --keep-audio)      KEEP_AUDIO=1 ;;
        --no-restore)      RESTORE=0 ;;
        --yes)             ASSUME_YES=1 ;;
        --device)          DEVICE_SPEC=$2; shift ;;
        -h|--help)         sed -n '3,34p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[ $WITH_SUSPEND = 1 ] && PHASES="$PHASES,suspend"
[ $WITH_UNPLUG = 1 ] && PHASES="$PHASES,unplug"
[ $WITH_JACK = 1 ] && PHASES="$PHASES,jack"

RES="$REPO/tests/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RES"
SUMMARY="$RES/summary.txt"
exec > >(tee -a "$RES/test.log") 2>&1

# ---------------------------------------------------------------- helpers

declare -a RESULTS=()
FAILED=0
TOUCHED=0       # set once modules or sound servers may have been changed
ORIG_MODULES=() # USB audio drivers loaded before the test, restored after it
LOOPBACK_OK=1
STOPPED_USER_UNITS=()
BG_PIDS=()
USB_DEV=
USB_PRODUCT=
USB_SERIAL=
HAS_CFC=unknown

log()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }
record() {  # status phase message
    RESULTS+=("$(printf '%-5s %-12s %s' "$1" "$2" "$3")")
    printf '   [%s] %s: %s\n' "$1" "$2" "$3"
    [ "$1" = FAIL ] && FAILED=1
    return 0
}
pass() { record PASS "$PHASE" "$*"; }
fail() { record FAIL "$PHASE" "$*"; }
skip() { record SKIP "$PHASE" "$*"; }
info() { record INFO "$PHASE" "$*"; }

want_phase() { [[ ",$PHASES," == *",$1,"* ]]; }

kmsg() { echo "snd-usb-motu-test: $*" | sudo tee /dev/kmsg >/dev/null; }

# kernel log since epoch second $1
klog_since() {
    journalctl -k --no-pager -o short-monotonic --since "@$1" 2>/dev/null ||
        sudo dmesg
}

KERNEL_BAD='WARNING:|BUG:|Oops|general protection|KASAN|UBSAN|inconsistent lock state|possible circular locking|possible recursive locking|scheduling while atomic|sleeping function called from invalid context|refcount_t:|list_(add|del) corruption|submitted while active'

# Start a phase: marker in the kernel log, remember the start time.
begin_phase() {
    PHASE=$1
    log "$1: $2"
    PHASE_START=$(( $(date +%s) - 1 ))
    kmsg "phase $1 start"
}

# End a phase: save its kernel log and fail on warnings/oopses.
end_phase() {
    local f="$RES/$PHASE.kernel.log"
    klog_since "$PHASE_START" > "$f"
    if grep -Eq "$KERNEL_BAD" "$f"; then
        fail "kernel warning/oops logged, see $(basename "$f"):"
        grep -E "$KERNEL_BAD" "$f" | head -5 | sed 's/^/      /'
    fi
}

phase_klog() { klog_since "$PHASE_START"; }

stat_get() {  # field
    sed -n "s/^$1: //p" "/proc/asound/$CARD/stream_stats" 2>/dev/null
}

# This driver registers its card on the USB device itself (snd-usb-audio on
# an interface), so the selected device's sound/ directory names our card.
resolve_card() {
    local c n
    for c in "/sys/bus/usb/devices/$USB_DEV/sound"/card*; do
        [ -e "$c" ] || continue
        n=$(basename "$c")
        [ -e "/proc/asound/$n/stream_stats" ] || continue
        CARD=$(cat "/proc/asound/$n/id")
        PCM="hw:CARD=$CARD,DEV=0"
        return 0
    done
    return 1
}

card_present() { resolve_card; }

# The proc files exist before udev has created and opened up the device
# nodes, so also wait for the PCM nodes to be accessible.
wait_card() {  # timeout seconds
    local i n
    for i in $(seq $(( ${1:-10} * 5 ))); do
        if card_present; then
            udevadm settle --timeout=5 2>/dev/null
            n=$(basename "$(readlink -f "/proc/asound/$CARD")")
            n=${n#card}
            if [ -r "/dev/snd/pcmC${n}D0p" ] && [ -w "/dev/snd/pcmC${n}D0p" ] &&
               [ -r "/dev/snd/pcmC${n}D0c" ] && [ -r "/dev/snd/controlC$n" ]; then
                return 0
            fi
        fi
        sleep 0.2
    done
    return 1
}

stop_audio_clients() {
    pkill -x aplay 2>/dev/null
    pkill -x arecord 2>/dev/null
    pkill -x jack_iodelay 2>/dev/null
    pkill -x jackd 2>/dev/null
    sleep 0.5
}

unload_all() {
    stop_audio_clients
    sudo rmmod snd_usb_motu 2>/dev/null
    sudo rmmod snd_usb_audio 2>/dev/null
    sudo rmmod motu 2>/dev/null       # drumfix driver, if loaded
    return 0
}

# Processes that have one of the card's device nodes open.
card_holders() {
    local n
    n=$(basename "$(readlink -f "/proc/asound/$CARD" 2>/dev/null)" 2>/dev/null)
    n=${n#card}
    [ -n "$n" ] || return 0
    fuser /dev/snd/*C"$n"D* /dev/snd/controlC"$n" 2>/dev/null | tr -s ' ' '\n' |
        grep -E '^[0-9]+$' | sort -u |
        while read -r pid; do
            [ "$pid" = $$ ] && continue
            printf '%s(%s) ' "$(ps -o comm= -p "$pid")" "$pid"
        done
}

# Fail early, and clearly, if something other than this test holds the card.
check_card_free() {
    local h
    h=$(card_holders)
    if [ -n "$h" ]; then
        fail "card is held by: $h- stop it (sound server?) and re-run"
        return 1
    fi
}

load_driver() {  # module parameters...
    # not 'lsmod | grep -q': with pipefail, grep's early exit fails the pipe
    if grep -q '^snd_usb_motu ' /proc/modules; then
        if ! sudo rmmod snd_usb_motu 2>/dev/null; then
            note "cannot unload snd-usb-motu, in use by: $(card_holders)"
            return 1
        fi
    fi
    note "insmod snd-usb-motu.ko $*"
    if ! sudo insmod "$MODULE" "$@"; then
        return 1
    fi
    wait_card 15
}

# All MOTU 07fd:0005 devices: "path<TAB>product<TAB>serial" per line.
list_devices() {
    local d
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        [ "$(cat "$d/idVendor"):$(cat "$d/idProduct")" = 07fd:0005 ] || continue
        printf '%s\t%s\t%s\n' "$(basename "$d")" "$(cat "$d/product" 2>/dev/null)" \
            "$(cat "$d/serial" 2>/dev/null)"
    done
}

# Choose the device under test: --device, the only one, or ask.
select_device() {
    local devs=() d path product serial i choice spec
    mapfile -t devs < <(list_devices)
    if [ ${#devs[@]} = 0 ]; then
        echo "No MOTU device 07fd:0005 found." >&2
        return 1
    fi
    if [ -n "$DEVICE_SPEC" ]; then
        spec=${DEVICE_SPEC,,}
        local hits=()
        for d in "${devs[@]}"; do
            IFS=$'\t' read -r path product serial <<< "$d"
            if [ "$path" = "$DEVICE_SPEC" ] || [ "$serial" = "$DEVICE_SPEC" ] ||
               [[ ${product,,} == *"$spec"* ]]; then
                hits+=("$d")
            fi
        done
        if [ ${#hits[@]} != 1 ]; then
            echo "--device $DEVICE_SPEC matches ${#hits[@]} devices:" >&2
            printf '   %s\n' "${devs[@]}" >&2
            return 1
        fi
        d=${hits[0]}
    elif [ ${#devs[@]} = 1 ]; then
        d=${devs[0]}
    elif [ $ASSUME_YES = 1 ]; then
        echo "Several MOTU devices connected, choose one with --device:" >&2
        printf '   %s\n' "${devs[@]}" >&2
        return 1
    else
        echo "Several MOTU devices are connected:"
        for i in "${!devs[@]}"; do
            IFS=$'\t' read -r path product serial <<< "${devs[$i]}"
            printf '   %d) %-14s usb %-12s serial %s\n' $(( i + 1 )) "$product" "$path" "$serial"
        done
        while :; do
            read -r -p "   Test which one? [1-${#devs[@]}] " choice
            [[ $choice =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le ${#devs[@]} ] && break
        done
        d=${devs[$(( choice - 1 ))]}
    fi
    IFS=$'\t' read -r USB_DEV USB_PRODUCT USB_SERIAL <<< "$d"
}

# The selected device's current USB path; it may change after a replug.
find_usb_device() {
    local path product serial
    while IFS=$'\t' read -r path product serial; do
        if [ -z "$USB_SERIAL" ] || [ "$serial" = "$USB_SERIAL" ]; then
            echo "$path"
            return 0
        fi
    done < <(list_devices)
    return 1
}

intf_driver() {  # interface number
    local l="/sys/bus/usb/devices/$USB_DEV:1.$1/driver"
    [ -L "$l" ] && basename "$(readlink -f "$l")" || echo none
}

xruns_in() { grep -Eci 'underrun|overrun' "$1" 2>/dev/null || true; }

# Continuous loopback run.
#   loop_test NAME SECONDS EXPECT SEGMENTS [ACTION_AT ACTION]...
# ACTION is a shell command run ACTION_AT seconds after playback is
# launched, i.e. about ACTION_AT + 0.5 s into the recording. SEGMENTS
# times are seconds since the start of the recording.
LAST_RC=0
loop_test() {
    local name=$1 secs=$2 expect=$3 segs=$4
    shift 4
    local rec="$RES/$name.raw" stats="$RES/$name.stats.log"
    local buf=$(( PERIOD * 4 ))
    local gaps0 restarts0 rpid ppid spid

    gaps0=$(stat_get gaps); restarts0=$(stat_get gap_restarts)

    arecord -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
        --period-size="$PERIOD" --buffer-size=$buf -d $(( secs + 3 )) \
        "$rec" 2> "$RES/$name.arecord.log" &
    rpid=$!
    sleep 0.5
    python3 -I "$HELPER" stream --rate "$RATE" --channels $CHANNELS \
        --seconds "$secs" |
        aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
            --period-size="$PERIOD" --buffer-size=$buf \
            2> "$RES/$name.aplay.log" &
    ppid=$!

    ( while kill -0 $rpid 2>/dev/null; do
          printf '%s ' "$(date +%s.%N)"
          tr '\n' ' ' < "/proc/asound/$CARD/stream_stats" 2>/dev/null
          echo
          sleep 1
      done ) > "$stats" &
    spid=$!

    local apids=()
    while [ $# -ge 2 ]; do
        ( sleep "$1"; note "t=$1 s: $2"; eval "$2" ) &
        apids+=($!)
        shift 2
    done

    wait $ppid; wait $rpid
    [ ${#apids[@]} -gt 0 ] && wait "${apids[@]}" 2>/dev/null
    kill $spid 2>/dev/null; wait $spid 2>/dev/null

    LOOP_GAPS=$(( $(stat_get gaps || echo 0) - ${gaps0:-0} ))
    LOOP_RESTARTS=$(( $(stat_get gap_restarts || echo 0) - ${restarts0:-0} ))
    LOOP_XRUNS=$(( $(xruns_in "$RES/$name.aplay.log") + $(xruns_in "$RES/$name.arecord.log") ))
    note "gaps +$LOOP_GAPS, gap restarts +$LOOP_RESTARTS, aplay/arecord xruns $LOOP_XRUNS, last_gap: $(stat_get last_gap)"

    if grep -qh 'audio open error' "$RES/$name.arecord.log" "$RES/$name.aplay.log"; then
        note "PCM open failed: $(grep -h 'audio open error' "$RES/$name.arecord.log" "$RES/$name.aplay.log" | head -1)"
        LAST_RC=4
        return
    fi
    if [ ! -s "$rec" ]; then
        note "no recording, see $name.arecord.log"
        LAST_RC=3
        return
    fi

    python3 -I "$HELPER" analyze "$rec" --rate "$RATE" --channels $CHANNELS \
        --map "$MAP" --expect "$expect" ${segs:+--segments "$segs" --time-base record} \
        --json "$RES/$name.analysis.json" | sed 's/^/   /'
    LAST_RC=${PIPESTATUS[0]}
    [ $KEEP_AUDIO = 1 ] || rm -f "$rec"
}

loop_result() {  # description; interprets LAST_RC of loop_test
    case $LAST_RC in
        0) pass "$1" ;;
        2) fail "$1: no test tones recorded (loopback not set up?)"; LOOPBACK_OK=0 ;;
        3) fail "$1: capture failed" ;;
        4) fail "$1: could not open the PCM (held by: $(card_holders))" ;;
        *) fail "$1: channel mapping check failed" ;;
    esac
}

# After loop_test: true if the streams actually ran, so the checks mean something.
stream_ran() { [ "$LAST_RC" != 3 ] && [ "$LAST_RC" != 4 ]; }

need_loopback() {
    if [ $LOOPBACK_OK = 0 ]; then
        skip "needs a working loopback (baseline failed)"
        return 1
    fi
}

inject_gap() { echo "${1:-1}" | sudo tee "/proc/asound/$CARD/inject_gap" >/dev/null; }

# Silence the last loop_test recorded after its first signal, in seconds.
dropout_s() {  # analysis json
    python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("silent", 0) * 0.5)' "$1" 2>/dev/null
}
soft_reset() { echo 1 | sudo tee "/proc/asound/$CARD/soft_reset" >/dev/null; }

start_load() {
    local n
    n=$(nproc)
    for _ in $(seq "$n"); do
        ( while :; do :; done ) &
        BG_PIDS+=($!)
    done
    ( while :; do
          dd if=/dev/zero of="$RES/io.tmp" bs=1M count=1024 conv=fsync status=none
          rm -f "$RES/io.tmp"
      done ) &
    BG_PIDS+=($!)
    if command -v stress-ng >/dev/null; then
        stress-ng --vm 2 --vm-bytes 25% --timeout 0 --quiet &
        BG_PIDS+=($!)
    fi
    note "load: $n CPU spinners, disk writer$(command -v stress-ng >/dev/null && echo ', stress-ng --vm')"
}

stop_load() {
    local p
    [ ${#BG_PIDS[@]} -gt 0 ] || return 0
    for p in "${BG_PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait "${BG_PIDS[@]}" 2>/dev/null
    BG_PIDS=()
    rm -f "$RES/io.tmp"
}

cleanup() {
    stop_load
    stop_audio_clients
    [ -n "${SUDO_KEEPALIVE:-}" ] && kill "$SUDO_KEEPALIVE" 2>/dev/null
    if [ $RESTORE = 1 ] && [ $TOUCHED = 1 ]; then
        sudo rmmod snd_usb_motu 2>/dev/null
        local m
        # in the original order, so the same driver gets the devices first
        for m in "${ORIG_MODULES[@]:-snd_usb_audio}"; do
            sudo modprobe "$m" 2>/dev/null
        done
    fi
    local u
    for u in "${STOPPED_USER_UNITS[@]}"; do
        systemctl --user start "$u" 2>/dev/null
    done

    {
        echo "snd-usb-motu hardening test, $(date)"
        echo "kernel $(uname -r), branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD) $(git -C "$REPO" rev-parse --short HEAD)"
        echo "device $USB_PRODUCT, usb $USB_DEV, serial $USB_SERIAL"
        echo
        printf '%s\n' "${RESULTS[@]}"
    } > "$SUMMARY"
    log "Summary (also in $SUMMARY)"
    printf '%s\n' "${RESULTS[@]}"
    [ $FAILED = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
    if [ $RESTORE = 1 ] && [ $TOUCHED = 1 ]; then
        echo
        echo "Note: snd-usb-audio and the sound server now own the device again and may"
        echo "have switched its clock (e.g. to 44.1 kHz). Check the rate in the web UI"
        echo "before the next test run."
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# snd-usb-motu does not set the device clock, but snd-usb-audio does: it
# sends the UAC2 sampling frequency request when a stream is configured.
# Open the device at --rate through snd-usb-audio once, with no sound
# server around that could switch it back (PulseAudio opens at 44.1 kHz).
set_device_clock() {
    local c id=
    if grep -q '^snd_usb_motu ' /proc/modules; then
        sudo rmmod snd_usb_motu 2>/dev/null || return 1
    fi
    # another driver (e.g. drumfix 'motu', loaded first at boot) may own the
    # streaming interfaces; then snd-usb-audio only has MIDI and must rebind
    if grep -q '^motu ' /proc/modules; then
        sudo rmmod motu 2>/dev/null || return 1
    fi
    if [ "$(intf_driver 1)" != snd-usb-audio ]; then
        sudo rmmod snd_usb_audio 2>/dev/null
        sudo modprobe snd_usb_audio || return 1
    fi
    # all MOTU models share the USB ID: match the bus/device number
    local bus
    bus=$(printf '%03d/%03d' "$(cat "/sys/bus/usb/devices/$USB_DEV/busnum")" \
                             "$(cat "/sys/bus/usb/devices/$USB_DEV/devnum")")
    for _ in $(seq 50); do
        for c in /proc/asound/card*; do
            [ "$(cat "$c/usbbus" 2>/dev/null)" = "$bus" ] &&
                grep -q 'Playback:' "$c/stream0" 2>/dev/null && id=$(cat "$c/id")
        done
        [ -n "$id" ] && break
        sleep 0.2
    done
    [ -n "$id" ] || return 1
    udevadm settle --timeout=5 2>/dev/null
    # snd-usb-audio skips the request if it believes the clock is already
    # at the rate (the UltraLite AVB kept running at 44.1 kHz that way), so
    # go through another rate first. It waits until the clock reports valid
    # (MOTU devices take about 2 s per switch).
    local other=44100 r
    [ "$RATE" = 44100 ] && other=48000
    for r in $other "$RATE"; do
        aplay -q -D "hw:CARD=$id" -t raw -f $FORMAT -c $CHANNELS -r "$r" -d 1 /dev/zero \
            2>> "$RES/set-clock.log" || return 1
    done
    sleep 1
}

# --------------------------------------------------------------- phases

phase_preflight() {
    begin_phase preflight "environment, build, audio servers"

    local rel
    rel=$(uname -r)
    case $rel in
        7.3.0-rc1*) info "kernel $rel lacks xHCI fix 05506a76: auto mode must pick URB_ISO_ASAP, forced CFC may show gaps" ;;
        *)          info "kernel $rel" ;;
    esac

    pass "testing $USB_PRODUCT at usb $USB_DEV (serial $USB_SERIAL)"
    local others
    others=$(list_devices | awk -F'\t' -v me="$USB_DEV" '$1 != me { printf "%s (%s) ", $2, $1 }')
    [ -n "$others" ] && info "also connected, bound to snd-usb-motu during the test but not tested: $others"

    local pci hcc
    pci=$(readlink -f "/sys/bus/usb/devices/$USB_DEV" | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | tail -1)
    hcc=$( (journalctl -k -b --no-pager 2>/dev/null || sudo dmesg) |
           grep -oE "xhci_hcd $pci: hcc params 0x[0-9a-f]+" | tail -1 | awk '{print $NF}')
    if [ -n "$hcc" ]; then
        if (( hcc & 0x800 )); then HAS_CFC=yes; else HAS_CFC=no; fi
        info "xHCI $pci hcc params $hcc, CFC: $HAS_CFC"
    else
        info "could not read hcc params for controller ${pci:-?}"
    fi

    note "building module"
    if make -C "$REPO" clean >/dev/null && make -C "$REPO" > "$RES/build.log" 2>&1; then
        if grep -q 'warning:' "$RES/build.log"; then
            fail "build has warnings, see build.log"
        else
            pass "module built"
        fi
    else
        fail "build failed, see build.log"
        exit 1
    fi

    local units=() u
    for u in pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service wireplumber.service \
             pulseaudio.socket pulseaudio.service; do
        systemctl --user is-active --quiet "$u" 2>/dev/null && units+=("$u")
    done
    if [ ${#units[@]} -gt 0 ]; then
        if [ $ASSUME_YES = 0 ]; then
            read -r -p "   A sound server is running (${units[*]}) and would grab the card. Stop it for the test (restarted afterwards)? [Y/n] " a
            [[ $a =~ ^[Nn] ]] && { fail "the sound server must not hold the card"; exit 1; }
        fi
        systemctl --user stop "${units[@]}"
        STOPPED_USER_UNITS=("${units[@]}")
        info "stopped ${units[*]}"
    fi
    if pgrep -x jackd >/dev/null || pgrep -x jackdbus >/dev/null; then
        if [ $ASSUME_YES = 0 ]; then
            read -r -p "   JACK is running. Kill it for the test? [Y/n] " a
            [[ $a =~ ^[Nn] ]] && { fail "JACK must not hold the card"; exit 1; }
        fi
        pkill -x jackd; pkill -x jackdbus
        info "killed JACK"
    fi
    if pgrep -x pulseaudio >/dev/null; then
        # started outside systemd, or autospawned by a client
        pulseaudio --kill 2>/dev/null
        sleep 1
        pgrep -x pulseaudio >/dev/null && info "pulseaudio still running (autospawn?); the card check will tell"
    fi

    if set_device_clock; then
        pass "device clock set to $RATE Hz via snd-usb-audio"
    else
        info "could not set the device clock via snd-usb-audio (see set-clock.log); it must already be at $RATE Hz"
    fi
    end_phase
}

phase_load() {
    begin_phase load "load with defaults + test hooks"
    unload_all
    if ! load_driver test_hooks=1; then
        fail "card did not appear"; end_phase; exit 1
    fi
    if phase_klog | grep -q "usb $USB_DEV: Probe Success"; then pass "probe succeeded"; else fail "no 'Probe Success' for usb $USB_DEV in kernel log"; fi
    note "card $CARD: $(grep -A1 -E "\[$CARD +\]" /proc/asound/cards | tail -1 | sed 's/^ *//')"

    local i d ok=1
    for i in 0 1 2; do
        d=$(intf_driver $i)
        [ "$d" = snd-usb-motu ] || { ok=0; note "interface $i bound to $d"; }
    done
    [ $ok = 1 ] && pass "interfaces 0-2 bound to snd-usb-motu" || fail "not all interfaces bound to snd-usb-motu"

    local bad
    bad=$(stat -c '%a %n' /sys/module/snd_usb_motu/parameters/* | awk '$1 != 444')
    [ -z "$bad" ] && pass "module parameters read-only" || fail "writable parameters: $bad"

    local sched want
    sched=$(stat_get scheduling)
    if [ "$HAS_CFC" = yes ] && [[ $(uname -r) != 7.3.0-rc1* ]] && [ "$(printf '%s\n' "$(uname -r)" 7.3 | sort -V | head -1)" = 7.3 ]; then
        want=cfc
    else
        want=iso_asap
    fi
    [ "$sched" = "$want" ] && pass "auto scheduling: $sched" || fail "auto scheduling is '$sched', expected '$want'"
    note "$(phase_klog | grep -E "usb $USB_DEV: (Isochronous scheduling|use_cfc)" | tail -1 | sed 's/^.*\] //')"
    [ "$(stat_get frame_check)" = on ] && pass "frame-number deadline check enabled" || info "frame_check off (not xHCI?)"
    [ -e "/proc/asound/$CARD/inject_gap" ] && pass "test hook present" || fail "inject_gap missing with test_hooks=1"
    sleep 2     # give a sound server the chance to grab the new card
    check_card_free && pass "no other process holds the card"

    # the driver does not set the device clock; everything below needs it at --rate
    local m try
    for try in 1 2 3 4 5 6; do
        arecord -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 2 /dev/null 2>/dev/null
        m=$(stat_get measured_rate)
        [ -n "$m" ] && [ "$m" -gt 0 ] && (( m * 50 >= RATE * 49 && m * 50 <= RATE * 51 )) && break
        if [ "$try" -ge 3 ] && [ $ASSUME_YES = 0 ]; then
            # nothing will touch the clock now: the sound servers are stopped
            read -r -p "   Device clock is ${m:-?} Hz. Set it to $RATE Hz in the web UI (Device tab), wait until it is locked, then press Enter (or type q to stop). " a
            [ "$a" = q ] && break
        else
            sleep 2     # the clock may still be switching
        fi
    done
    if [ -n "$m" ] && [ "$m" -gt 0 ] && (( m * 50 >= RATE * 49 && m * 50 <= RATE * 51 )); then
        pass "device clock runs at $m Hz"
    else
        fail "device clock runs at ${m:-?} Hz, not $RATE Hz"
        end_phase
        exit 1
    fi
    end_phase
}

phase_hwparams() {
    begin_phase hwparams "rates and hardware parameters offered"
    local out
    out=$(aplay -D "$PCM" --dump-hw-params -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
              -d 1 /dev/zero 2>&1)
    echo "$out" > "$RES/hwparams.txt"
    if grep -q 'audio open error' "$RES/hwparams.txt"; then
        fail "could not open the PCM: $(head -1 "$RES/hwparams.txt")"
        end_phase; return
    fi
    note "$(grep -E '^(RATE|PERIOD_SIZE|PERIODS|CHANNELS)' "$RES/hwparams.txt" | tr '\n' ' ')"
    if grep -qE '^CHANNELS: 24' "$RES/hwparams.txt"; then pass "24 channels"; else fail "unexpected channel range"; fi
    if grep -qE '^PERIODS: \[?[0-9]+ [0-9]+\]?$|^PERIODS: [0-9]+$' "$RES/hwparams.txt"; then
        pass "integer period count"
    else
        info "PERIODS line: $(grep '^PERIODS' "$RES/hwparams.txt")"
    fi
    if phase_klog | grep -q 'does not fit the endpoints'; then fail "rate fit error logged"; fi
    end_phase
}

phase_constraints() {
    begin_phase constraints "capture must follow the playback rate and period size"
    local other=96000 out p
    [ "$RATE" = 96000 ] && other=48000

    aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
        --period-size="$PERIOD" --buffer-size=$(( PERIOD * 4 )) -d 6 /dev/zero \
        2> "$RES/constraints.aplay.log" &
    local apid=$!
    sleep 1.5
    if ! kill -0 $apid 2>/dev/null; then
        fail "playback did not start: $(head -1 "$RES/constraints.aplay.log")"
        end_phase; return
    fi

    out=$(arecord -v -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
              --period-size=$(( PERIOD * 2 )) -d 1 /dev/null 2>&1)
    echo "$out" > "$RES/constraints.arecord-period.log"
    p=$(echo "$out" | grep -oE 'period_size *: *[0-9]+' | head -1 | grep -oE '[0-9]+$')
    [ "$p" = "$PERIOD" ] && pass "capture period pinned to $PERIOD (asked $(( PERIOD * 2 )))" \
                         || fail "capture got period ${p:-?}, expected $PERIOD"

    out=$(arecord -v -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r $other \
              -d 1 /dev/null 2>&1)
    local rc=$?
    echo "$out" > "$RES/constraints.arecord-rate.log"
    if echo "$out" | grep -q 'audio open error'; then
        fail "capture could not be opened: $(echo "$out" | head -1)"
    elif echo "$out" | grep -q "got = ${RATE}Hz" ||
         { [ $rc != 0 ] && echo "$out" | grep -qiE 'hw_params|set_params|rate'; }; then
        pass "capture at $other Hz refused while playback runs at $RATE Hz"
    else
        fail "capture at $other Hz accepted while playback runs at $RATE Hz"
    fi
    wait $apid
    end_phase
}

phase_baseline() {
    begin_phase baseline "${DURATION}s loopback, no load"
    loop_test baseline "$DURATION" clean ''
    loop_result "channel mapping intact for ${DURATION}s"
    stream_ran || { end_phase; return; }
    [ "$LOOP_GAPS" = 0 ] && pass "no stream gaps" || fail "$LOOP_GAPS stream gaps without load"
    [ "$LOOP_XRUNS" = 0 ] && pass "no xruns" || fail "$LOOP_XRUNS xruns"
    local m
    m=$(tail -1 "$RES/baseline.stats.log" | grep -oE 'measured_rate: [0-9]+' | grep -oE '[0-9]+$')
    if [ -n "$m" ] && (( m * 50 >= RATE * 49 && m * 50 <= RATE * 51 )); then
        pass "measured device rate $m Hz"
    else
        fail "measured device rate ${m:-?} Hz, stream $RATE Hz (device clock set to $RATE?)"
    fi
    end_phase
}

phase_stress() {
    begin_phase stress "${STRESS_DURATION}s loopback under CPU and disk load"
    need_loopback || { end_phase; return; }
    start_load
    loop_test stress "$STRESS_DURATION" recovered ''
    stop_load
    loop_result "mapping correct at the end of the run"
    stream_ran || { end_phase; return; }
    if [ "$LOOP_GAPS" = 0 ]; then
        pass "no gaps under load"
    elif [ "$LOOP_RESTARTS" -ge 1 ]; then
        info "$LOOP_GAPS gaps under load, $LOOP_RESTARTS restarts (recovery exercised)"
    else
        fail "$LOOP_GAPS gaps but no restart"
    fi
    end_phase
}

phase_inject() {
    begin_phase inject "injected gap, automatic restart"
    need_loopback || { end_phase; return; }
    loop_test inject 30 segments '3-10:ok,17-:ok' 10 inject_gap
    loop_result "mapping ok before the gap and again after the restart"
    stream_ran || { end_phase; return; }
    [ "$LOOP_GAPS" -ge 1 ] && pass "gap detected ($(stat_get last_gap))" || fail "injected gap not detected"
    [ "$LOOP_RESTARTS" -ge 1 ] && pass "streams restarted" || fail "no restart after the gap"
    phase_klog | grep -q 'Test hook: playback gap' || fail "hook did not fire"
    [ "$LOOP_XRUNS" -ge 1 ] && pass "xrun reported to the applications" || info "no xrun seen by aplay/arecord"
    info "audio dropout around the restart: about $(dropout_s "$RES/inject.analysis.json") s"
    end_phase
}

phase_premise() {
    begin_phase premise "injected gap without auto restart, then soft_reset"
    need_loopback || { end_phase; return; }
    load_driver test_hooks=1 auto_restart=0 || { fail "reload failed"; end_phase; return; }
    # a 300 ms gap: the minimal one may be too short for the device to notice
    loop_test premise 40 segments '3-10:ok,31-:ok' 10 'inject_gap 300' 25 soft_reset
    loop_result "mapping ok before the gap and after soft_reset"
    if ! stream_ran; then
        load_driver test_hooks=1 || fail "reload with defaults failed"
        end_phase; return
    fi
    [ "$LOOP_GAPS" -ge 1 ] && pass "gap detected and logged" || fail "gap not detected"
    [ "$LOOP_RESTARTS" = 0 ] && pass "no automatic restart with auto_restart=0" || fail "restarted although auto_restart=0"
    [ -f "$RES/premise.analysis.json" ] && python3 -I - "$RES/premise.analysis.json" <<'EOF' | sed 's/^/   /'
import json, sys
r = json.load(open(sys.argv[1]))
# windows between the gap (~10.5 s) and soft_reset (~25.5 s), recording time
mid = ''.join({'ok': '.', 'bad': 'X', 'silent': '_'}[st]
              for t, st in r.get('detail', []) if 12 <= t < 25)
print('device channel mapping between gap and soft_reset:',
      'shifted' if mid.count('X') > len(mid) // 2 else 'intact', '(' + mid + ')')
EOF
    info "a shift between gap and soft_reset confirms the device behaviour; intact means the device tolerated this gap"
    load_driver test_hooks=1 || fail "reload with defaults failed"
    end_phase
}

phase_ratecheck() {
    begin_phase ratecheck "clock mismatch warning (stream 44100 Hz, device $RATE Hz)"
    if [ "$RATE" = 44100 ]; then skip "stream rate already 44100"; end_phase; return; fi
    arecord -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r 44100 -d 4 /dev/null \
        2> "$RES/ratecheck.arecord.log"
    if grep -q 'audio open error' "$RES/ratecheck.arecord.log"; then
        fail "could not open the PCM: $(head -1 "$RES/ratecheck.arecord.log")"
    elif phase_klog | grep -q 'but the stream runs at 44100'; then
        pass "mismatch reported: $(phase_klog | grep -o 'Device delivers.*' | tail -1)"
    elif grep -q 'got = ' "$RES/ratecheck.arecord.log"; then
        skip "44100 Hz not offered by the endpoints"
    else
        fail "no clock mismatch warning"
    fi
    end_phase
}

phase_unbind() {
    begin_phase unbind "sysfs unbind of single interfaces, rebind"
    local drv=/sys/bus/usb/drivers/snd-usb-motu i

    aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 10 /dev/zero \
        2> "$RES/unbind.aplay.log" &
    local apid=$!
    sleep 2
    echo "$USB_DEV:1.1" | sudo tee "$drv/unbind" >/dev/null
    sleep 1
    card_present && fail "card still present after unbinding interface 1" \
                 || pass "card removed when interface 1 was unbound while streaming"
    local bound=0
    for i in 0 1 2; do [ "$(intf_driver $i)" = snd-usb-motu ] && bound=1; done
    [ $bound = 0 ] && pass "all interfaces released" || fail "interfaces still bound: 0=$(intf_driver 0) 1=$(intf_driver 1) 2=$(intf_driver 2)"
    wait $apid 2>/dev/null

    echo "$USB_DEV:1.0" | sudo tee "$drv/bind" >/dev/null
    if wait_card 10; then pass "rebind of interface 0 succeeded"; else fail "rebind failed: $(phase_klog | grep -E 'in use|Probe Error' | tail -1)"; fi

    echo "$USB_DEV:1.0" | sudo tee "$drv/unbind" >/dev/null
    sleep 1
    bound=0
    for i in 1 2; do [ "$(intf_driver $i)" = snd-usb-motu ] && bound=1; done
    [ $bound = 0 ] && pass "unbinding interface 0 releases 1 and 2" || fail "streaming interfaces still bound"
    echo "$USB_DEV:1.0" | sudo tee "$drv/bind" >/dev/null
    wait_card 10 || { fail "second rebind failed"; end_phase; return; }

    if [ $LOOPBACK_OK = 1 ]; then
        loop_test unbind-after 8 clean ''
        loop_result "streaming works after rebind"
    fi
    end_phase
}

phase_reload() {
    begin_phase reload "module unload/load cycles, busy module, invalid parameter"
    aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 5 /dev/zero 2>/dev/null &
    local apid=$!
    sleep 1
    if sudo rmmod snd_usb_motu 2>/dev/null; then
        fail "module unloaded while the PCM was open"
        wait $apid 2>/dev/null
        load_driver test_hooks=1
    else
        pass "rmmod refused while the PCM is open"
        wait $apid 2>/dev/null
    fi

    local n ok=1
    for n in 1 2 3 4 5; do
        load_driver test_hooks=1 || { ok=0; break; }
        aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 2 /dev/zero 2>/dev/null &
        apid=$!
        arecord -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 2 /dev/null 2>/dev/null
        wait $apid
    done
    if [ $ok = 0 ]; then
        fail "cycle $n failed"
        end_phase; return
    fi
    pass "5 unload/load/stream cycles"

    load_driver test_hooks=1 num_urbs=2 || fail "load with num_urbs=2 failed"
    phase_klog | grep -q 'Invalid num_urbs=2' && pass "num_urbs=2 rejected and clamped to 4" \
                                               || fail "num_urbs=2 not rejected"
    load_driver test_hooks=1
    end_phase
}

phase_cfc() {
    begin_phase cfc "forced scheduling modes"
    local mode sched
    for mode in 0 1; do
        if [ $mode = 1 ] && [ "$HAS_CFC" != yes ]; then
            skip "controller has no CFC"; continue
        fi
        load_driver test_hooks=1 use_cfc=$mode || { fail "load use_cfc=$mode failed"; continue; }
        sched=$(stat_get scheduling)
        note "use_cfc=$mode -> $sched"
        [ $LOOPBACK_OK = 1 ] || { skip "use_cfc=$mode: needs loopback"; continue; }
        loop_test cfc$mode 30 segments '3-10:ok,17-:ok' 10 inject_gap
        stream_ran || { loop_result "use_cfc=$mode"; continue; }
        if [ $mode = 1 ] && [[ $(uname -r) == 7.3.0-rc1* ]]; then
            info "use_cfc=1 on rc1: gaps $LOOP_GAPS, restarts $LOOP_RESTARTS, scheduling now $(stat_get scheduling)"
            [ $LAST_RC = 0 ] && pass "use_cfc=1: mapping recovered" || fail "use_cfc=1: mapping not recovered"
        else
            loop_result "use_cfc=$mode ($sched): gap recovery"
            [ "$LOOP_GAPS" -le 1 ] || info "use_cfc=$mode: $LOOP_GAPS gaps (1 injected)"
        fi
        if [ "$(stat_get scheduling)" != "$sched" ]; then
            info "fell back from $sched to $(stat_get scheduling) after repeated gaps"
        fi
    done
    load_driver test_hooks=1
    end_phase
}

phase_suspend() {
    begin_phase suspend "system suspend and resume while streaming"
    grep -qw mem /sys/power/state || { skip "suspend to RAM not available"; end_phase; return; }
    local before after
    before=$(readlink -f "/proc/asound/$CARD")
    aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 40 /dev/zero \
        2> "$RES/suspend.aplay.log" &
    local apid=$!
    sleep 3
    note "suspending for 15 s (rtcwake)"
    sudo rtcwake -m mem -s 15 >/dev/null
    sleep 3
    kill $apid 2>/dev/null; wait $apid 2>/dev/null
    after=$(readlink -f "/proc/asound/$CARD" 2>/dev/null)
    [ -n "$after" ] && [ "$after" = "$before" ] && pass "card kept its slot ($(basename "$after"))" \
                                                 || fail "card changed from $(basename "$before") to $(basename "${after:-none}")"
    phase_klog | grep -q 'Probing Interface' && fail "driver was re-probed (no PM support?)" \
                                              || pass "no unbind/rebind on resume"
    if [ $LOOPBACK_OK = 1 ]; then
        loop_test suspend-after 10 clean ''
        loop_result "streaming works after resume"
    fi
    end_phase
}

phase_unplug() {
    begin_phase unplug "physical unplug while streaming (interactive)"
    aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 60 /dev/zero \
        2> "$RES/unplug.aplay.log" &
    local apid=$!
    sleep 2
    read -r -p "   Unplug the MOTU USB cable now, then press Enter. " _
    sleep 1
    card_present && fail "card still present after unplug" || pass "card removed"
    kill $apid 2>/dev/null; wait $apid 2>/dev/null
    read -r -p "   Plug the cable back in, wait for the device, then press Enter. " _
    if wait_card 20; then
        pass "card back after replug"
        USB_DEV=$(find_usb_device)
        if [ $LOOPBACK_OK = 1 ]; then
            loop_test unplug-after 10 clean ''
            loop_result "streaming works after replug"
        fi
    else
        fail "card did not come back: $(phase_klog | grep -E 'in use|Probe Error|Unexpected' | tail -1)"
    fi
    end_phase
}

phase_jack() {
    begin_phase jack "JACK at $PERIOD frames, round trip on channel 1"
    command -v jackd >/dev/null && command -v jack_iodelay >/dev/null ||
        { skip "jackd/jack_iodelay not installed"; end_phase; return; }
    need_loopback || { end_phase; return; }
    jackd -R -d alsa -d "hw:$CARD" -r "$RATE" -p "$PERIOD" -n 3 -i $CHANNELS -o $CHANNELS \
        > "$RES/jackd.log" 2>&1 &
    local jpid=$!
    sleep 4
    jack_iodelay > "$RES/jack_iodelay.log" 2>&1 &
    local dpid=$!
    sleep 1
    jack_connect jack_delay:out system:playback_1 && jack_connect system:capture_1 jack_delay:in
    sleep 20
    kill $dpid $jpid 2>/dev/null; wait $dpid $jpid 2>/dev/null
    local lat
    lat=$(grep -oE '[0-9.]+ frames +[0-9.]+ ms total roundtrip latency' "$RES/jack_iodelay.log" | tail -5 | sort -u)
    if [ "$(echo "$lat" | grep -c .)" = 1 ]; then
        pass "stable round trip: $lat"
    elif [ -n "$lat" ]; then
        fail "round trip latency changed: $(echo "$lat" | tr '\n' ';')"
    else
        fail "no latency measured, see jack_iodelay.log"
    fi
    local x
    x=$(grep -ci 'xrun' "$RES/jackd.log")
    [ "$x" = 0 ] && pass "no JACK xruns" || info "$x JACK xrun messages"
    end_phase
}

# ------------------------------------------------------------------ main

echo "Results: $RES"
select_device || exit 1
echo "Device under test: $USB_PRODUCT (usb $USB_DEV, serial $USB_SERIAL)"

sudo -v || exit 1
( while :; do sudo -n true; sleep 50; done ) 2>/dev/null &
SUDO_KEEPALIVE=$!
# drivers that own MOTU devices outside the test (motu = drumfix driver)
for m in motu snd_usb_audio; do
    grep -q "^$m " /proc/modules && ORIG_MODULES+=("$m")
done
[ ${#ORIG_MODULES[@]} -gt 0 ] && echo "USB audio drivers to restore afterwards: ${ORIG_MODULES[*]}"
TOUCHED=1

# preflight always runs: it checks the device and stops the audio servers
phase_preflight
for p in load hwparams constraints baseline stress inject premise ratecheck unbind reload cfc suspend unplug jack; do
    want_phase $p || continue
    if [ $p != load ] && { ! card_present || [ ! -e "/proc/asound/$CARD/inject_gap" ]; }; then
        # the phase needs this driver with test hooks; (re)load it
        unload_all
        load_driver test_hooks=1 || { PHASE=$p; fail "card not available"; continue; }
    fi
    if [ $p != load ]; then
        PHASE=$p
        check_card_free || continue
    fi
    "phase_$p"
done

exit $FAILED
