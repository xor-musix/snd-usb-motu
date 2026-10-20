#!/bin/bash

echo "Kill Jack..."
killall -9 jackdbus

# Check if rmmod was successful
if [ $? -eq 0 ]; then
    echo "Jack killed successfully"
fi

echo "Removing MOTU Pro Audio Driver..."
sudo rmmod snd-usb-motu

# Check if rmmod was successful
if [ $? -eq 0 ]; then
    echo "MOTU Pro Audio Driver removed successfully"
fi

echo "Inserting Default USB Audio Driver"
sudo modprobe snd-usb-audio

# Check if insmod was successful
if [ $? -eq 0 ]; then
    echo "Default USB Audio Driver inserted successfully"
else
    echo "Failed to insert Default USB Audio Driver"
    exit 1
fi

exit 0
