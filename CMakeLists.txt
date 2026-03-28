# Cross-compilation for Pi Zero:
#   cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=toolchain-pi-zero.cmake -DPI_SYSROOT=$HOME/pi-sysroot .
#   cmake --build build-pi -j$(nproc)
#   scp build-pi/surfcam pi@<pi-ip>:/usr/local/bin/

cmake_minimum_required(VERSION 3.10)
project(SurfCamWeatherPi VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
option(ENABLE_TSAN "Build with ThreadSanitizer (-fsanitize=thread)" OFF)
if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")  # Add optimization flag for Pi Zero
endif()

find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)

# Debian bookworm ships libcamera 0.0.x (no Connection, no Plane::map(), no Camera::isRunning()).
set(SURFCAM_LIBCAMERA_LEGACY 0)
if(PKG_CONFIG_EXECUTABLE)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion libcamera
        OUTPUT_VARIABLE SURFCAM_LIBCAMERA_PKG_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _SURFCAM_LC_VER_RC
    )
    if(_SURFCAM_LC_VER_RC EQUAL 0 AND SURFCAM_LIBCAMERA_PKG_VERSION VERSION_LESS "0.2.0")
        set(SURFCAM_LIBCAMERA_LEGACY 1)
    endif()
endif()


include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
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
    pthread
    ${GSTREAMER_LIBRARIES}
)

target_compile_definitions(surfcam PRIVATE
    PI_ZERO=1
    SURFCAM_LIBCAMERA_LEGACY=${SURFCAM_LIBCAMERA_LEGACY}
)

# Install target
install(TARGETS surfcam DESTINATION /usr/local/bin)
