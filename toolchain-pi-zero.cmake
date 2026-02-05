# Cross-compilation toolchain for Raspberry Pi Zero W (armv6, armhf)
#
# Prerequisites (on your build machine — Ubuntu/Debian x86_64):
#
#   sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
#
# Then build the sysroot from your Pi (one-time setup):
#
#   # On your Pi, export the package list:
#   dpkg --get-selections > ~/selections.txt
#
#   # On your build machine, rsync the Pi's libraries and headers:
#   mkdir -p ~/pi-sysroot
#   rsync -avz pi@<pi-ip>:/usr/lib/arm-linux-gnueabihf/ ~/pi-sysroot/usr/lib/arm-linux-gnueabihf/
#   rsync -avz pi@<pi-ip>:/usr/include/                  ~/pi-sysroot/usr/include/
#   rsync -avz pi@<pi-ip>:/usr/local/lib/                ~/pi-sysroot/usr/local/lib/
#   rsync -avz pi@<pi-ip>:/usr/local/include/            ~/pi-sysroot/usr/local/include/
#   rsync -avz pi@<pi-ip>:/opt/vc/                       ~/pi-sysroot/opt/vc/
#   rsync -avz pi@<pi-ip>:/lib/arm-linux-gnueabihf/      ~/pi-sysroot/lib/arm-linux-gnueabihf/
#
#   # Fix absolute symlinks inside the sysroot:
#   cd ~/pi-sysroot && find . -type l | while read l; do
#     target=$(readlink "$l")
#     if [ "${target:0:1}" = "/" ]; then
#       ln -sfn "$(pwd)$target" "$l"
#     fi
#   done
#
# Usage:
#   cmake -B build-pi \
#         -DCMAKE_TOOLCHAIN_FILE=toolchain-pi-zero.cmake \
#         -DPI_SYSROOT=$HOME/pi-sysroot \
#         ..
#   cmake --build build-pi -j$(nproc)
#   scp build-pi/surfcam pi@<pi-ip>:/usr/local/bin/
#

cmake_minimum_required(VERSION 3.10)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv6)

# Cross compiler (armhf for Pi Zero W)
set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Sysroot — pass -DPI_SYSROOT=/path/to/sysroot on the cmake command line
if(NOT PI_SYSROOT)
    set(PI_SYSROOT "$ENV{HOME}/pi-sysroot")
endif()
set(CMAKE_SYSROOT ${PI_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${PI_SYSROOT})

# Tell pkg-config to look inside the sysroot
set(ENV{PKG_CONFIG_PATH} "${PI_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${PI_SYSROOT}/usr/local/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PI_SYSROOT}")

# Search paths: prefer sysroot for libs/headers, host for programs (cmake, etc.)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Pi Zero is ARMv6 with hard-float VFP
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -march=armv6zk -mfpu=vfp -mfloat-abi=hard" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv6zk -mfpu=vfp -mfloat-abi=hard" CACHE STRING "" FORCE)
