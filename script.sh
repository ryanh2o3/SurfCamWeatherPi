#!/bin/bash

# Set environment variables
export API_KEY="your_api_key_here"
export LD_LIBRARY_PATH=/usr/local/lib
export GST_DEBUG=2

# Kill any existing instances
killall -q surfcam

# Wait for network
while ! ping -c 1 -W 1 1.1.1.1 &> /dev/null; do
    echo "Waiting for network..."
    sleep 5
done

# Wait for camera
while ! ls /dev/video* &> /dev/null; do
    echo "Waiting for camera..."
    sleep 5
done

# Start the application with log redirection
cd /home/ryanpatton
/usr/local/bin/surfcam > /home/ryanpatton/surfcam.log 2>&1