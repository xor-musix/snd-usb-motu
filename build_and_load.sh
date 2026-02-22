#!/bin/bash

# Parse named parameters (order-independent)
sample_rate=48000
uframes_per_urb=8

for arg in "$@"; do
    case "$arg" in
        sample_rate=*) sample_rate="${arg#sample_rate=}" ;;
        uframes_per_urb=*) uframes_per_urb="${arg#uframes_per_urb=}" ;;
        *) echo "Unknown parameter: $arg"; exit 1 ;;
    esac
done

echo "Using sample rate '$sample_rate'"
echo "Using uframes_per_urb '$uframes_per_urb'"

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
    
    echo "Inserting MOTU Pro Audio Driver..."
    sudo insmod snd-usb-motu.ko sample_rate=$sample_rate uframes_per_urb=$uframes_per_urb
    
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