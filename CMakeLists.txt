cmake_minimum_required(VERSION 3.10)
project(SurfCamWeatherPi VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")  # Add optimization flag for Pi Zero

find_package(libcamera REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenCV REQUIRED)
find_package(aws-cpp-sdk-core REQUIRED)
find_package(aws-cpp-sdk-kinesisvideo REQUIRED)
find_package(aws-cpp-sdk-kinesis-video-media REQUIRED)
find_package(KinesisVideoProducerCpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)


include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBCAMERA_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}

)

add_executable(surfcam
    src/main.cpp
    src/CameraManager.cpp
    src/ApiClient.cpp
    src/KinesisStreamer.cpp
)

target_link_libraries(surfcam
    ${LIBCAMERA_LIBRARIES}
    ${CURL_LIBRARIES}
    ${OpenCV_LIBS}
    aws-cpp-sdk-core
    aws-cpp-sdk-kinesisvideo
    aws-cpp-sdk-kinesis-video-media
    pthread
    KinesisVideoProducerCpp
    ${GSTREAMER_LIBRARIES}
)

# Add system monitoring capabilities
target_compile_definitions(surfcam PRIVATE PI_ZERO=1)

# Install target
install(TARGETS surfcam DESTINATION /usr/local/bin)