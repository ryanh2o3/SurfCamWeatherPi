#!/bin/bash
#
# SurfCam Weather Pi
# Copyright (C) 2025  Ryan Patton
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

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