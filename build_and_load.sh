#!/bin/bash

# Parse named arguments
sample_rate=""
uframes_per_urb=""
num_urbs=""
pb_safety_offset=""
rec_safety_offset=""
bpf_factor=""
use_cfc=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sample_rate=*)       sample_rate="${1#*=}" ;;
        --uframes_per_urb=*)   uframes_per_urb="${1#*=}" ;;
        --num_urbs=*)          num_urbs="${1#*=}" ;;
        --pb_safety_offset=*)  pb_safety_offset="${1#*=}" ;;
        --rec_safety_offset=*) rec_safety_offset="${1#*=}" ;;
        --bpf_factor=*)        bpf_factor="${1#*=}" ;;
        --use_cfc=*)           use_cfc="${1#*=}" ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

echo "Removing UAC Driver..."
sudo rmmod snd-usb-audio

# Check if rmmod was successful
if [ $? -eq 0 ]; then
    echo "UAC Driver removed successfully"
fi

echo "Removing Drumfix Driver..."
sudo rmmod motu

# Check if rmmod was successful
if [ $? -eq 0 ]; then
    echo "Drumfix Driver removed successfully"
fi

echo "Removing MOTU Pro Audio Driver..."
sudo rmmod snd-usb-motu

# Check if rmmod was successful
if [ $? -eq 0 ]; then
    echo "MOTU Pro Audio Driver removed successfully"
fi

make

# Check if make was successful
if [ $? -eq 0 ]; then
    echo "Compilation successful"
    
    # Build insmod parameter string
    params=""
    [ -n "$sample_rate" ]       && params="$params sample_rate=$sample_rate"
    [ -n "$uframes_per_urb" ]   && params="$params uframes_per_urb=$uframes_per_urb"
    [ -n "$num_urbs" ]          && params="$params num_urbs=$num_urbs"
    [ -n "$pb_safety_offset" ]  && params="$params pb_safety_offset=$pb_safety_offset"
    [ -n "$rec_safety_offset" ] && params="$params rec_safety_offset=$rec_safety_offset"
    [ -n "$bpf_factor" ]        && params="$params bpf_factor=$bpf_factor"
    [ -n "$use_cfc" ]           && params="$params use_cfc=$use_cfc"

    echo "Inserting MOTU Pro Audio Driver with: $params"
    sudo insmod snd-usb-motu.ko $params
    
    # Check if insmod was successful
    if [ $? -eq 0 ]; then
        echo "MOTU Pro Audio Driver inserted successfully"
    else
        echo "Failed to insert MOTU Pro Audio Driver"
        exit 1
    fi
else
    echo "Compilation failed"
    exit 1
fi

exit 0