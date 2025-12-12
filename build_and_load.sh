#!/bin/bash

# First argument is sample_rate, default to 48000 if not provided
sample_rate=${1:-48000}
echo "Using sample rate '$sample_rate'"

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
    sudo insmod snd-usb-motu.ko sample_rate=$sample_rate
    
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