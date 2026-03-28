# Cross-compilation for Pi Zero:
#   cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=toolchain-pi-zero.cmake -DPI_SYSROOT=$HOME/pi-sysroot .
#   cmake --build build-pi -j$(nproc)
#   scp build-pi/surfcam pi@<pi-ip>:/usr/local/bin/

cmake_minimum_required(VERSION 3.10)
project(SurfCamWeatherPi VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")  # Add optimization flag for Pi Zero

find_package(CURL REQUIRED)
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)


include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}

)

link_directories(
    ${LIBCAMERA_LIBRARY_DIRS}
    ${GSTREAMER_LIBRARY_DIRS}
)

add_definitions(
    ${LIBCAMERA_CFLAGS_OTHER}
    ${GSTREAMER_CFLAGS_OTHER}
)

add_executable(surfcam
    src/main.cpp
    src/CameraManager.cpp
    src/ApiClient.cpp
    src/HlsUploader.cpp
    src/Config.cpp
)

target_link_libraries(surfcam
    ${LIBCAMERA_LIBRARIES}
    ${CURL_LIBRARIES}
    ${OpenCV_LIBS}
    pthread
    ${GSTREAMER_LIBRARIES}
)

# Add system monitoring capabilities
target_compile_definitions(surfcam PRIVATE PI_ZERO=1)

# Install target
install(TARGETS surfcam DESTINATION /usr/local/bin)
