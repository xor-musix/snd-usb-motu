#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Provoke the MOTU channel shift ("modulo-8 issue") again and again and
# judge the channel mapping between one provocation and the next.
#
# With snd-usb-audio (default), playback is stopped (SIGSTOP on aplay) long
# enough to underrun; on each underrun snd-usb-audio restarts its playback
# endpoint without the alternate setting change the device is said to need.
# With snd-usb-motu, the same, or the driver's own restart (soft_reset), or
# a real gap in the USB stream (inject_gap test hook) with or without the
# automatic restart: if the driver's restart is right, none of these leaves
# the mapping shifted.
#
# Same loopback setup and test signal as hardening-test.sh (see "Testing"
# in readme.md). Run as your normal user from the repository root; commands
# that need root go through sudo. The USB audio drivers loaded before the
# test are restored at the end (snd-usb-audio if there were none).
#
#   tests/usbaudio-shift-test.sh [options]
#
# Options:
#   --driver NAME     snd-usb-audio (default) or snd-usb-motu (built from
#                     this repository and loaded with test_hooks=1)
#   --provoke KIND    stop:  SIGSTOP aplay long enough to underrun (default)
#                     reset: soft_reset, the driver's restart (snd-usb-motu)
#                     gap:   inject_gap, a real USB stream gap (snd-usb-motu)
#   --no-auto-restart load snd-usb-motu with auto_restart=0: gaps are
#                     detected but not repaired
#   --params "P=V .." more snd-usb-motu parameters, e.g. "uframes_per_urb=32"
#   --device SPEC     device to test when several MOTU devices are connected:
#                     USB path (e.g. 3-4.3), serial number, or part of the
#                     product name (e.g. ultralite); asks if not given
#   --rounds N        number of provocations (20); 0 runs --interval seconds
#                     without any, e.g. together with --load
#   --interval S      seconds between provocations (8)
#   --pauses LIST     comma separated lengths in ms, used in turn: aplay stop
#                     (up to 6 ms of random jitter added) or gap length
#                     (20,35,50,80,130,300)
#   --load            add CPU and disk load (spontaneous xruns count too)
#   --period FRAMES   ALSA period size (128)
#   --periods N       periods per buffer (4); an aplay stop longer than the
#                     buffer time underruns
#   --rate HZ         stream rate (48000)
#   --map SPEC        loopback mapping, 'identity' (default) or capture:playback
#                     pairs, 1-based, as in hardening-test.sh
#   --keep-audio      keep the raw recording (about 3.5 MB per second)
#   --yes             do not ask before stopping PipeWire/PulseAudio/JACK
#
# Results go to tests/results/<timestamp>-<driver>-<provoke>/. Exit status:
# 0 a persistent shift occurred, 1 no persistent shift, 2 the run could not
# tell (setup error, no tones, mapping wrong before the first provocation).

set -u -o pipefail
# English xrun messages from aplay/arecord, '.' in $EPOCHREALTIME
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/.." && pwd)
HELPER="$REPO/tests/motu_signal.py"
MODULE="$REPO/snd-usb-motu.ko"
CHANNELS=24
FORMAT=S24_3LE
CARD=
PCM=

DRIVER=snd-usb-audio
PROVOKE=stop
AUTO_RESTART=1
PARAMS=
ROUNDS=20
INTERVAL=8
PAUSES=20,35,50,80,130,300
LOAD=0
PERIOD=128
PERIODS=4
RATE=48000
MAP=identity
KEEP_AUDIO=0
ASSUME_YES=0
DEVICE_SPEC=
LEAD=10         # seconds of playback before the first provocation
GUARD_AFTER=3   # seconds after a provocation not judged (restart transient)
GUARD_BEFORE=0.5

while [ $# -gt 0 ]; do
    case "$1" in
        --driver)          DRIVER=$2; shift ;;
        --provoke)         PROVOKE=$2; shift ;;
        --no-auto-restart) AUTO_RESTART=0 ;;
        --params)          PARAMS=$2; shift ;;
        --device)          DEVICE_SPEC=$2; shift ;;
        --rounds)          ROUNDS=$2; shift ;;
        --interval)        INTERVAL=$2; shift ;;
        --pauses)          PAUSES=$2; shift ;;
        --load)            LOAD=1 ;;
        --period)          PERIOD=$2; shift ;;
        --periods)         PERIODS=$2; shift ;;
        --rate)            RATE=$2; shift ;;
        --map)             MAP=$2; shift ;;
        --keep-audio)      KEEP_AUDIO=1 ;;
        --yes)             ASSUME_YES=1 ;;
        -h|--help)         sed -n '4,/^$/{s/^# \{0,1\}//;p}' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

case $DRIVER in
    snd-usb-audio|snd-usb-motu) ;;
    *) echo "--driver must be snd-usb-audio or snd-usb-motu" >&2; exit 2 ;;
esac
case $PROVOKE in
    stop) ;;
    reset|gap)
        [ $DRIVER = snd-usb-motu ] ||
            { echo "--provoke $PROVOKE needs --driver snd-usb-motu" >&2; exit 2; } ;;
    *) echo "--provoke must be stop, reset or gap" >&2; exit 2 ;;
esac
if [ $AUTO_RESTART = 0 ] || [ -n "$PARAMS" ]; then
    [ $DRIVER = snd-usb-motu ] ||
        { echo "--no-auto-restart and --params need --driver snd-usb-motu" >&2; exit 2; }
fi

IFS=, read -r -a PAUSE_MS <<< "$PAUSES"
[ ${#PAUSE_MS[@]} -gt 0 ] || { echo "--pauses is empty" >&2; exit 2; }
BYTES_PER_S=$(( RATE * CHANNELS * 3 ))

RES="$REPO/tests/results/$(date +%Y%m%d-%H%M%S)-$DRIVER-$PROVOKE"
mkdir -p "$RES"
exec > >(tee -a "$RES/test.log") 2>&1

# Record into RAM: arecord's small buffer overruns whenever the page cache
# writeback stalls it (every 5 s on ext4), and each overrun is a boundary.
RECDIR=$RES
need_kb=$(( (LEAD + ROUNDS * INTERVAL + 60) * BYTES_PER_S / 1024 * 3 / 2 ))
if [ -d /dev/shm ] && [ "$(df -P -k /dev/shm | awk 'NR == 2 { print $4 }')" -gt $need_kb ]; then
    RECDIR=$(mktemp -d /dev/shm/motu-shift.XXXXXX) || RECDIR=$RES
fi

# ---------------------------------------------------------------- helpers

USB_DEV=
USB_PRODUCT=
USB_SERIAL=
STOPPED_USER_UNITS=()
BG_PIDS=()
ORIG_MODULES=()       # USB audio drivers loaded before the test, load order
ORIG_MOTU_PARAMS=()   # parameters of a snd-usb-motu loaded before the test
MODULES_TOUCHED=0
RESULT_LINE="RESULT: INCOMPLETE"
RC=2

log()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }

# Prefix every line with the wall-clock time it arrived and the size of the
# recording at that moment, which is the position in the recording.
stamp() {  # recording file
    local l
    while IFS= read -r l; do
        printf '%s %s %s\n' "$EPOCHREALTIME" "$(stat -c %s "$1" 2>/dev/null || echo 0)" "$l"
    done
}

stat_get() {  # field
    sed -n "s/^$1: //p" "/proc/asound/$CARD/stream_stats" 2>/dev/null
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
        echo "No MOTU device (07fd:0005) connected" >&2
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

intf_driver() {  # interface number
    local l="/sys/bus/usb/devices/$USB_DEV:1.$1/driver"
    [ -L "$l" ] && basename "$(readlink -f "$l")" || echo none
}

stop_audio_clients() {
    # an interrupted run may leave aplay stopped, deaf to SIGTERM
    pkill -CONT -x aplay 2>/dev/null
    pkill -x aplay 2>/dev/null
    pkill -x arecord 2>/dev/null
    sleep 0.5
}

stop_sound_servers() {
    local units=() u a
    for u in pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service wireplumber.service \
             pulseaudio.socket pulseaudio.service; do
        systemctl --user is-active --quiet "$u" 2>/dev/null && units+=("$u")
    done
    if [ ${#units[@]} -gt 0 ]; then
        if [ $ASSUME_YES = 0 ]; then
            read -r -p "   A sound server is running (${units[*]}) and would grab the card. Stop it for the test (restarted afterwards)? [Y/n] " a
            [[ $a =~ ^[Nn] ]] && { note "the sound server must not hold the card"; exit 2; }
        fi
        systemctl --user stop "${units[@]}"
        STOPPED_USER_UNITS=("${units[@]}")
        note "stopped ${units[*]}"
    fi
    if pgrep -x jackd >/dev/null || pgrep -x jackdbus >/dev/null; then
        if [ $ASSUME_YES = 0 ]; then
            read -r -p "   JACK is running. Kill it for the test? [Y/n] " a
            [[ $a =~ ^[Nn] ]] && { note "JACK must not hold the card"; exit 2; }
        fi
        pkill -x jackd; pkill -x jackdbus
        note "killed JACK"
    fi
    if pgrep -x pulseaudio >/dev/null; then
        # started outside systemd, or autospawned by a client
        pulseaudio --kill 2>/dev/null
        sleep 1
    fi
}

loaded() { grep -q "^$1 " /proc/modules; }

# Remember the USB audio drivers in the order they must be loaded again:
# whoever loads first gets the streaming interfaces (the drumfix 'motu'
# driver does at boot), snd-usb-audio then only takes MIDI.
save_drivers() {
    local m f v
    for m in motu snd_usb_motu snd_usb_audio; do
        loaded $m && ORIG_MODULES+=("$m")
    done
    if loaded snd_usb_motu; then
        for f in /sys/module/snd_usb_motu/parameters/*; do
            v=$(cat "$f")
            [ "$v" = "(null)" ] || ORIG_MOTU_PARAMS+=("$(basename "$f")=$v")
        done
    fi
    note "USB audio drivers before the test: ${ORIG_MODULES[*]:-none}"
}

unload_drivers() {
    local m
    stop_audio_clients
    for m in snd_usb_motu snd_usb_audio motu; do
        if loaded $m; then
            MODULES_TOUCHED=1
            sudo rmmod $m || return 1
        fi
    done
}

# Back to the drivers loaded before the test, or snd-usb-audio if none were.
restore_drivers() {
    local want=("${ORIG_MODULES[@]}") m
    [ $MODULES_TOUCHED = 1 ] || return 0
    [ ${#want[@]} -gt 0 ] || want=(snd_usb_audio)
    note "restoring USB audio drivers: ${want[*]}"
    if ! unload_drivers; then
        note "cannot unload the test drivers; load ${want[*]} yourself"
        return 1
    fi
    for m in "${want[@]}"; do
        case $m in
            snd_usb_motu)
                # not installed anywhere: the module of this repository
                note "insmod $MODULE ${ORIG_MOTU_PARAMS[*]}"
                sudo insmod "$MODULE" "${ORIG_MOTU_PARAMS[@]}" ;;
            *)  sudo modprobe "$m" ;;
        esac || note "failed to load $m"
    done
}

# snd-usb-audio must own the streaming interfaces: snd-usb-motu and the
# drumfix 'motu' driver take them when loaded first.
use_snd_usb_audio() {
    stop_audio_clients
    if loaded snd_usb_motu || loaded motu || [ "$(intf_driver 1)" != snd-usb-audio ]; then
        unload_drivers || return 1
        MODULES_TOUCHED=1
        sudo modprobe snd_usb_audio || return 1
    fi
}

# The snd-usb-audio card of the selected device; all MOTU models share the
# USB ID, so match the bus/device number.
resolve_usb_audio_card() {
    local bus c
    bus=$(printf '%03d/%03d' "$(cat "/sys/bus/usb/devices/$USB_DEV/busnum")" \
                             "$(cat "/sys/bus/usb/devices/$USB_DEV/devnum")")
    for _ in $(seq 50); do
        for c in /proc/asound/card*; do
            if [ "$(cat "$c/usbbus" 2>/dev/null)" = "$bus" ] &&
               grep -q 'Playback:' "$c/stream0" 2>/dev/null; then
                CARD=$(cat "$c/id")
                PCM="hw:CARD=$CARD,DEV=0"
                udevadm settle --timeout=5 2>/dev/null
                return 0
            fi
        done
        sleep 0.2
    done
    return 1
}

# snd-usb-motu registers its card on the USB device itself; the proc files
# exist before udev has made the PCM nodes accessible, so wait for those.
resolve_motu_card() {
    local c n
    for _ in $(seq 75); do
        for c in "/sys/bus/usb/devices/$USB_DEV/sound"/card*; do
            [ -e "$c" ] || continue
            n=$(basename "$c")
            [ -e "/proc/asound/$n/stream_stats" ] || continue
            udevadm settle --timeout=5 2>/dev/null
            n=${n#card}
            if [ -w "/dev/snd/pcmC${n}D0p" ] && [ -r "/dev/snd/pcmC${n}D0c" ]; then
                CARD=$(cat "/proc/asound/card$n/id")
                PCM="hw:CARD=$CARD,DEV=0"
                return 0
            fi
        done
        sleep 0.2
    done
    return 1
}

load_snd_usb_motu() {
    local params=(test_hooks=1)
    [ $AUTO_RESTART = 1 ] || params+=(auto_restart=0)
    # shellcheck disable=SC2206 # word splitting intended
    params+=($PARAMS)
    note "building snd-usb-motu"
    make -C "$REPO" > "$RES/build.log" 2>&1 || { note "build failed, see build.log"; return 1; }
    unload_drivers || return 1
    MODULES_TOUCHED=1
    note "insmod snd-usb-motu.ko ${params[*]}"
    sudo insmod "$MODULE" "${params[@]}" || return 1
    resolve_motu_card
}

# The driver does not set the device clock; its measured_rate tells the truth.
check_motu_clock() {
    local m
    for _ in 1 2 3; do
        arecord -q -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" -d 2 /dev/null 2>/dev/null
        m=$(stat_get measured_rate)
        [ -n "$m" ] && [ "$m" -gt 0 ] && (( m * 50 >= RATE * 49 && m * 50 <= RATE * 51 )) && break
        sleep 2     # the clock may still be switching
    done
    note "device clock measured by snd-usb-motu: ${m:-?} Hz"
    [ -n "$m" ] && [ "$m" -gt 0 ] && (( m * 50 >= RATE * 49 && m * 50 <= RATE * 51 ))
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

# snd-usb-audio skips the UAC2 clock request if it believes the clock is
# already at the rate, so go through another rate first. MOTU devices take
# about 2 s per switch; snd-usb-audio waits for it.
set_device_clock() {
    local other=44100 r
    [ "$RATE" = 44100 ] && other=48000
    for r in $other "$RATE"; do
        aplay -q -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$r" -d 1 /dev/zero \
            2>> "$RES/set-clock.log" || return 1
    done
    sleep 1
}

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
    note "load: $n CPU spinners, disk writer"
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
    [ "$RECDIR" != "$RES" ] && rm -rf "$RECDIR"
    restore_drivers
    [ -n "${SUDO_KEEPALIVE:-}" ] && kill "$SUDO_KEEPALIVE" 2>/dev/null
    local u
    for u in "${STOPPED_USER_UNITS[@]}"; do
        systemctl --user start "$u" 2>/dev/null
    done
    {
        echo "MOTU channel shift test, $(date)"
        echo "kernel $(uname -r), branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD) $(git -C "$REPO" rev-parse --short HEAD)"
        echo "device $USB_PRODUCT, usb $USB_DEV, serial $USB_SERIAL, card ${CARD:-?}"
        echo "driver $DRIVER$([ $DRIVER = snd-usb-motu ] && echo " (auto_restart=$AUTO_RESTART${PARAMS:+ $PARAMS})"), provoke $PROVOKE"
        echo "rounds $ROUNDS, interval $INTERVAL s, pauses $PAUSES ms, period $PERIOD x $PERIODS, rate $RATE, load $LOAD"
        echo
        cat "$RES/intervals.txt" 2>/dev/null
        [ -n "${STATS_LINE:-}" ] && echo "$STATS_LINE"
        echo "$RESULT_LINE"
    } > "$RES/summary.txt"
    log "Summary (also in $RES/summary.txt)"
    echo "$RESULT_LINE"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# One provocation, from the background loop of run_loop.
provoke_once() {  # aplay pid, length in ms
    case $PROVOKE in
        stop)
            kill -STOP "$1"
            sleep "$(awk -v m="$2" 'BEGIN { printf "%.3f", m / 1000 }')"
            kill -CONT "$1" ;;
        reset)
            echo 1 | sudo -n tee "/proc/asound/$CARD/soft_reset" >/dev/null ;;
        gap)
            echo "$2" | sudo -n tee "/proc/asound/$CARD/inject_gap" >/dev/null ;;
    esac
}

# One loopback run with the test tones.
#   run_loop NAME SECONDS ROUNDS
# Sets REC (raw recording). ROUNDS > 0 provokes ROUNDS times, LEAD + k *
# INTERVAL seconds after the recording started.
run_loop() {
    local name=$1 secs=$2 rounds=$3
    local buf=$(( PERIOD * PERIODS )) ppid t0
    REC="$RECDIR/$name.raw"
    rm -f "$REC"

    # arecord is stopped once playback has ended; -d only bounds the run
    ( exec arecord -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
          --period-size="$PERIOD" --buffer-size=$buf \
          -d $(( secs + rounds * 2 + 10 )) "$REC" \
          2> >(stamp "$REC" > "$RES/$name.arecord.log") ) &
    RPID=$!
    for _ in $(seq 1000); do
        [ -s "$REC" ] && break
        kill -0 $RPID 2>/dev/null || break
        sleep 0.01
    done
    t0=$EPOCHREALTIME
    if [ ! -s "$REC" ]; then
        note "capture did not start, see $name.arecord.log"
        kill $RPID 2>/dev/null; wait $RPID 2>/dev/null
        return 3
    fi

    python3 -I "$HELPER" stream --rate "$RATE" --channels $CHANNELS --seconds "$secs" |
        ( exec aplay -D "$PCM" -t raw -f $FORMAT -c $CHANNELS -r "$RATE" \
              --period-size="$PERIOD" --buffer-size=$buf \
              2> >(stamp "$REC" > "$RES/$name.aplay.log") ) &
    ppid=$!

    if [ $DRIVER = snd-usb-motu ]; then
        ( while kill -0 $RPID 2>/dev/null; do
              printf '%s %s ' "$EPOCHREALTIME" "$(stat -c %s "$REC" 2>/dev/null || echo 0)"
              tr '\n' ' ' < "/proc/asound/$CARD/stream_stats" 2>/dev/null
              echo
              sleep 1
          done ) > "$RES/$name.stats.log" &
    else
        ( sleep 3; cat "/proc/asound/$CARD/stream0" ) > "$RES/$name.stream0.txt" 2>&1 &
    fi

    # one line per provocation: number, length in ms, wall-clock time,
    # recording size just before it
    local tpid=
    if [ "$rounds" -gt 0 ]; then
        ( for (( k = 0; k < rounds; k++ )); do
              ms=$(( PAUSE_MS[k % ${#PAUSE_MS[@]}] ))
              [ $PROVOKE = stop ] && ms=$(( ms + RANDOM % 7 ))
              sleep "$(awk -v t="$t0" -v k=$k -v l=$LEAD -v i="$INTERVAL" -v n="$EPOCHREALTIME" \
                           'BEGIN { d = t + l + k * i - n; printf "%.3f", (d > 0 ? d : 0) }')"
              kill -0 $ppid 2>/dev/null || break
              printf '%d %d %s %s\n' $(( k + 1 )) $ms "$EPOCHREALTIME" "$(stat -c %s "$REC")"
              provoke_once $ppid $ms
          done ) > "$RES/$name.provoke.log" &
        tpid=$!
        note "provoking ($PROVOKE) $rounds times, every $INTERVAL s from t=$LEAD s (log: $name.provoke.log)"
    fi

    wait $ppid
    [ -n "$tpid" ] && wait $tpid
    sleep 1.5
    kill -INT $RPID 2>/dev/null
    wait $RPID 2>/dev/null
    sleep 0.3   # let the stamp readers finish their logs
    note "aplay underruns: $(grep -c 'underrun!!!' "$RES/$name.aplay.log"), arecord overruns: $(grep -c 'overrun!!!' "$RES/$name.arecord.log")"
    return 0
}

# The recording is analysed: keep it in the results (--keep-audio) or drop it.
drop_rec() {
    if [ $KEEP_AUDIO = 1 ] && [ "$RECDIR" != "$RES" ]; then
        mv "$REC" "$RES/"
    elif [ $KEEP_AUDIO = 0 ]; then
        rm -f "$REC"
    fi
}

# Short run without provocations: the mapping must be intact throughout.
check_clean() {  # name
    local rc
    run_loop "$1" 8 0 || return 3
    python3 -I "$HELPER" analyze "$REC" --rate "$RATE" --channels $CHANNELS \
        --map "$MAP" --expect clean --json "$RES/$1.analysis.json" | sed 's/^/   /'
    rc=${PIPESTATUS[0]}
    drop_rec
    return "$rc"
}

# ------------------------------------------------------------------ main

echo "Results: $RES"
select_device || exit 2
echo "Device under test: $USB_PRODUCT (usb $USB_DEV, serial $USB_SERIAL)"
if ! command -v python3 >/dev/null || ! python3 -I -c 'import numpy' 2>/dev/null; then
    echo "python3 with numpy is needed for the analysis" >&2
    exit 2
fi

sudo -v || exit 2
( while :; do sudo -n true; sleep 50; done ) 2>/dev/null &
SUDO_KEEPALIVE=$!
START=$(( $(date +%s) - 1 ))

log "setup"
note "kernel $(uname -r)"
save_drivers
stop_sound_servers
# the device clock is set through snd-usb-audio for either driver
use_snd_usb_audio || { RESULT_LINE="RESULT: INVALID (could not hand the device to snd-usb-audio)"; exit 2; }
resolve_usb_audio_card || { RESULT_LINE="RESULT: INVALID (no snd-usb-audio card with playback for usb $USB_DEV)"; exit 2; }
note "snd-usb-audio card $CARD ($PCM)"
if set_device_clock; then
    note "device clock set to $RATE Hz"
else
    note "could not set the device clock (see set-clock.log); it must already be at $RATE Hz"
fi
if [ $DRIVER = snd-usb-motu ]; then
    load_snd_usb_motu || { RESULT_LINE="RESULT: INVALID (snd-usb-motu did not come up, see test.log)"; exit 2; }
    note "snd-usb-motu card $CARD ($PCM), scheduling $(stat_get scheduling), auto_restart $(stat_get auto_restart)"
fi
sleep 2     # give anything left the chance to grab the card
h=$(card_holders)
[ -z "$h" ] || { RESULT_LINE="RESULT: INVALID (card is held by: $h)"; exit 2; }
if [ $DRIVER = snd-usb-motu ] && ! check_motu_clock; then
    RESULT_LINE="RESULT: INVALID (device clock is not at $RATE Hz)"
    exit 2
fi

log "sanity: 8 s loopback without provocation"
check_clean sanity
case $? in
    0) note "mapping intact" ;;
    2) RESULT_LINE="RESULT: INVALID (no test tones recorded: is the loopback routing set up?)"; exit 2 ;;
    3) RESULT_LINE="RESULT: INVALID (capture failed, see sanity.arecord.log)"; exit 2 ;;
    *) RESULT_LINE="RESULT: INVALID (mapping wrong without any provocation, see sanity.analysis.json)"; exit 2 ;;
esac

if [ "$ROUNDS" -gt 0 ]; then
    SECS=$(( LEAD + ROUNDS * INTERVAL + 2 ))
else
    SECS=$(( LEAD + INTERVAL ))
fi
log "provoke: $SECS s of playback, $ROUNDS x $PROVOKE$([ $LOAD = 1 ] && echo ', under load')"
note "recording about $(( SECS * BYTES_PER_S / 1000000 )) MB to $RECDIR"
if [ $DRIVER = snd-usb-motu ]; then
    GAPS0=$(stat_get gaps); RESTARTS0=$(stat_get gap_restarts)
fi
[ $LOAD = 1 ] && start_load
run_loop provoke "$SECS" "$ROUNDS" || { stop_load; RESULT_LINE="RESULT: INVALID (capture failed, see provoke.arecord.log)"; exit 2; }
stop_load
python3 -I "$HELPER" analyze "$REC" --rate "$RATE" --channels $CHANNELS \
    --map "$MAP" --expect clean --json "$RES/provoke.analysis.json" | sed 's/^/   /'
drop_rec
if [ $DRIVER = snd-usb-motu ]; then
    STATS_LINE="snd-usb-motu: gaps +$(( $(stat_get gaps) - GAPS0 )), gap restarts +$(( $(stat_get gap_restarts) - RESTARTS0 )), last_gap: $(stat_get last_gap)"
    note "$STATS_LINE"
fi

# Judge the mapping between consecutive provocations and xruns. A shift one
# of them causes lasts until the next one.
python3 -I - "$RES/provoke.analysis.json" "$BYTES_PER_S" "$GUARD_AFTER" "$GUARD_BEFORE" \
    "$RES/intervals.json" "$RES/provoke.aplay.log" "$RES/provoke.arecord.log" \
    "$RES/provoke.provoke.log" "$PROVOKE" "$DRIVER" "$RES/provoke.stats.log" \
    <<'EOF' | tee "$RES/intervals.txt"
import json, os, sys

(an, bps, g_after, g_before, out, aplay_log, arecord_log, provoke_log,
 provoke, driver, stats_log) = sys.argv[1:]
bps, g_after, g_before = float(bps), float(g_after), float(g_before)
WIN = 0.5
r = json.load(open(an))
detail = r.get('detail', [])
offsets = r.get('offsets') or [None] * len(detail)
if not detail:
    print('no test tones in the recording')
    sys.exit(2)

# Every event carries the recording size when it happened, i.e. its
# position in the recording, whatever the streams lost in between.
events = []
for path in (aplay_log, arecord_log):
    for line in open(path, errors='replace'):
        f = line.split(' ', 2)
        if len(f) == 3 and f[2].startswith(('underrun!!!', 'overrun!!!')):
            events.append((int(f[1]) / bps, f[2].split('!')[0]))
if os.path.exists(provoke_log):
    for line in open(provoke_log):
        f = line.split()
        if len(f) == 4:
            events.append((int(f[3]) / bps, provoke))
events.sort()

# a provocation and the xruns it causes form one boundary
bounds = []
for t, kind in events:
    if bounds and t - bounds[-1]['t'] < 1.0:
        bounds[-1]['kinds'][kind] = bounds[-1]['kinds'].get(kind, 0) + 1
        continue
    bounds.append({'t': round(t, 3), 'kinds': {kind: 1}})
# a boundary with a provocation is at the provocation, not at an xrun of the
# system that happened just before it
for b in bounds:
    tp = [t for t, kind in events if kind == provoke and 0 <= t - b['t'] < 1.0]
    if tp:
        b['t'] = round(tp[0], 3)

# snd-usb-audio restarts its endpoints on every xrun. snd-usb-motu keeps the
# USB streams running through one; only its own restart after a gap stops
# both directions at once. Other xruns (arecord stalled by the system) are
# no boundary there.
ignored = []
if driver == 'snd-usb-motu':
    keep = lambda b: (provoke in b['kinds'] or
                      ('underrun' in b['kinds'] and 'overrun' in b['kinds']))
    ignored = [b for b in bounds if not keep(b)]
    bounds = [b for b in bounds if keep(b)]

# snd-usb-motu: gaps the driver counted (stream_stats, once a second); those
# not within 1.5 s after a provocation came from the system
driver_gaps = []
if os.path.exists(stats_log):
    prev = None
    for line in open(stats_log, errors='replace'):
        f = line.split()
        if len(f) < 2 or 'gaps:' not in f:
            continue
        n = int(f[f.index('gaps:') + 1])
        if prev is not None and n > prev:
            t = int(f[1]) / bps
            provoked = any(e[1] == provoke and 0 <= t - e[0] <= 1.5 for e in events)
            if not provoked:
                driver_gaps.append({'t': round(t, 2), 'count': n - prev})
        prev = n

sig_start = detail[0][0]
sig_end = detail[-1][0] + WIN
edges = [b['t'] for b in bounds]
spans = [('start', None, sig_start + 1.0,
          (edges[0] if edges else sig_end) - g_before)]
for i, b in enumerate(bounds):
    end = edges[i + 1] if i + 1 < len(edges) else sig_end
    spans.append(('%d' % (i + 1), b, b['t'] + g_after, end - g_before))

rows = []
for name, b, a, e in spans:
    sel = [(st, off) for (t, st), off in zip(detail, offsets)
           if t >= a and t + WIN <= e]
    n = len(sel)
    n_ok = sum(st == 'ok' for st, _ in sel)
    n_bad = sum(st == 'bad' for st, _ in sel)
    n_sil = sum(st == 'silent' for st, _ in sel)
    offs = [off for st, off in sel if st == 'bad' and off is not None]
    off = max(set(offs), key=offs.count) if offs else None
    if n < 2:
        verdict = 'short'
    elif n_bad * 5 >= n * 4:
        verdict = 'SHIFTED'
    elif n_ok == n:
        verdict = 'ok'
    elif n_sil * 2 >= n:
        verdict = 'silent'
    else:
        verdict = 'mixed'
    rows.append({'after': name, 'boundary': b, 'from': round(a, 2), 'to': round(e, 2),
                 'windows': n, 'ok': n_ok, 'bad': n_bad, 'silent': n_sil,
                 'offset': off, 'verdict': verdict,
                 'timeline': ''.join({'ok': '.', 'bad': 'X', 'silent': '_'}[st]
                                     for st, _ in sel)})

def kinds(b):
    return '+'.join(k if n == 1 else '%s*%d' % (k, n) for k, n in b['kinds'].items())

print('%-6s %9s %-24s %-8s %-7s %s' % ('after', 't_rec/s', 'event', 'verdict',
                                       'offset', 'timeline (0.5 s windows)'))
for row in rows:
    b = row['boundary']
    print('%-6s %9s %-24s %-8s %-7s %s' % (
        row['after'], '-' if not b else '%.2f' % b['t'], '-' if not b else kinds(b),
        row['verdict'], '' if row['offset'] is None else '%+d' % row['offset'],
        row['timeline']))

judged = [row for row in rows[1:] if row['verdict'] != 'short']
shifted = [row for row in judged if row['verdict'] == 'SHIFTED']
count = lambda k: sum(1 for _, kind in events if kind == k)
print()
print('%s: %d, underruns: %d, overruns: %d; judged spans: %d, shifted: %d' % (
    provoke, count(provoke), count('underrun'), count('overrun'),
    len(judged), len(shifted)))
if ignored:
    print('xruns in one direction only, no boundary with snd-usb-motu: %d' % len(ignored))
if os.path.exists(stats_log):
    print('stream gaps the driver detected besides the provoked ones: %d%s' % (
        sum(g['count'] for g in driver_gaps),
        ''.join(' at %.0f s' % g['t'] for g in driver_gaps[:10])))

# Shift episodes: runs of shifted windows with one offset, split where a
# boundary falls into a window. A run that ends within g_after of the
# boundary before it is the restart transient (or the gap itself); any
# other run of 2 or more windows is a shift, also one that starts in the
# middle of a span or is cut short by a spurious boundary.
def span_of(t):
    k = None
    for i, e in enumerate(edges):
        if e < t + WIN - 0.01:
            k = i
    return k

episodes = []
cur = None
for (t, st), off in zip(detail, offsets):
    k = span_of(t)
    if (st == 'bad' and cur and cur['k'] == k and cur['offset'] == off and
            abs(t - cur['to']) < 0.01):
        cur['to'] = round(t + WIN, 3)
        cur['windows'] += 1
        continue
    if cur:
        episodes.append(cur)
        cur = None
    if st == 'bad':
        cur = {'k': k, 'offset': off, 'from': t, 'to': round(t + WIN, 3), 'windows': 1}
if cur:
    episodes.append(cur)

for ep in episodes:
    k = ep.pop('k')
    b = bounds[k] if k is not None else None
    nxt = bounds[k + 1] if (k if k is not None else -1) + 1 < len(bounds) else None
    ep['after'] = b and {'n': k + 1, 't': b['t'], 'event': kinds(b),
                         'delay': round(ep['from'] - b['t'], 2)}
    ep['until'] = nxt and {'n': (k if k is not None else -1) + 2, 't': nxt['t'],
                           'event': kinds(nxt), 'gap': round(nxt['t'] - ep['to'], 2)}
    transient = b is not None and ep['to'] <= b['t'] + g_after + 0.01
    ep['shift'] = ep['windows'] >= 2 and not transient
    # unprovoked driver-detected gaps between the boundary and the onset
    ep['driver_gaps_before'] = sum(
        g['count'] for g in driver_gaps
        if (b is None or g['t'] >= b['t']) and g['t'] <= ep['from'] + WIN)

shifts = [ep for ep in episodes if ep['shift']]
offs = sorted({ep['offset'] for ep in shifts if ep['offset'] is not None})
print('shift episodes (1 s or more, outside the %g s after a boundary): %d, offsets: %s' % (
    g_after, len(shifts), offs or '-'))
if shifts:
    print('  %-8s %-8s %-6s %-6s  %-34s %s' % ('from/s', 'to/s', 'len/s', 'offset',
                                             'after boundary (onset delay)', 'until boundary'))
for ep in shifts:
    a, u = ep['after'], ep['until']
    print('  %-8.2f %-8.2f %-6.1f %-6s  %-34s %s' % (
        ep['from'], ep['to'], ep['to'] - ep['from'],
        '?' if ep['offset'] is None else '%+d' % ep['offset'],
        '-' if not a else '%d %s at %.2f (%+.2f s)' % (a['n'], a['event'], a['t'], a['delay']),
        'end of signal' if not u else '%d %s at %.2f' % (u['n'], u['event'], u['t'])))
for v in ('mixed', 'silent'):
    k = sum(row['verdict'] == v for row in judged)
    if k:
        print('%d span(s) %s: look at them by hand (--keep-audio to keep the audio)' % (k, v))

json.dump({'boundaries': bounds, 'ignored_xruns': ignored,
           'driver_gaps': driver_gaps, 'spans': rows, 'shifted': len(shifted),
           'judged': len(judged), 'episodes': episodes,
           'shift_episodes': len(shifts), 'offsets': offs}, open(out, 'w'), indent=1)

if rows[0]['verdict'] not in ('ok', 'short') or any(not ep['after'] for ep in shifts):
    print('mapping not intact before the first provocation: the run cannot tell')
    sys.exit(2)
if not judged:
    print('no provocation with enough signal after it: the run cannot tell')
    sys.exit(2)
sys.exit(0 if shifts else 1)
EOF
RC=${PIPESTATUS[0]}

# A shift caused by the device's state must go away when the streams are
# closed (alternate setting 0) and reopened. snd-usb-motu stops its streams
# 2 s after the last substream stopped.
sleep 3
log "recheck: 8 s loopback after reopening the streams"
check_clean recheck
RECHECK=$?
case $RECHECK in
    0) note "mapping intact after reopening" ;;
    *) note "mapping NOT intact after reopening (rc $RECHECK), see recheck.analysis.json" ;;
esac

klog=$(journalctl -k --no-pager -o short-monotonic --since "@$START" 2>/dev/null || sudo dmesg)
echo "$klog" > "$RES/kernel.log"
if [ $DRIVER = snd-usb-motu ]; then
    note "kernel log: $(grep -c 'soft_reset triggered' "$RES/kernel.log") soft_reset, $(grep -c 'Test hook: playback gap' "$RES/kernel.log") injected gaps, $(grep -c 'stop_streaming_endpoints' "$RES/kernel.log") stream stops, see kernel.log"
else
    note "kernel log: $(grep -Eci 'usb|xhci|snd' "$RES/kernel.log") USB/sound lines, see kernel.log"
fi

COUNTS=$(python3 -I -c 'import json,sys; r = json.load(open(sys.argv[1]))
print("%d shift episodes, %d of %d judged spans shifted" % (r["shift_episodes"], r["shifted"], r["judged"]))' \
    "$RES/intervals.json" 2>/dev/null || echo '?')
case $RC in
    0) RESULT_LINE="RESULT: SHIFT with $DRIVER after $PROVOKE: $COUNTS (see intervals.txt)" ;;
    1) RESULT_LINE="RESULT: no shift with $DRIVER after $PROVOKE: $COUNTS" ;;
    *) RESULT_LINE="RESULT: INVALID (see intervals.txt)"; RC=2 ;;
esac
[ $RECHECK = 0 ] || RESULT_LINE="$RESULT_LINE; recheck after reopening failed"
exit $RC
