# Cross-compilation for Pi Zero:
#   cmake -B build-pi -DCMAKE_TOOLCHAIN_FILE=toolchain-pi-zero.cmake -DPI_SYSROOT=$HOME/pi-sysroot .
#   cmake --build build-pi -j$(nproc)
#   scp build-pi/surfcam pi@<pi-ip>:/usr/local/bin/
#
# macOS / hosts without libcamera: use -DBUILD_TESTING=ON to build unit + HTTP tests only.

cmake_minimum_required(VERSION 3.14)
project(SurfCamWeatherPi VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
option(ENABLE_TSAN "Build with ThreadSanitizer (-fsanitize=thread)" OFF)
option(BUILD_TESTING "Build unit tests, HTTP smoke binary, and register ctest targets" OFF)
include(CTest)
if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")  # Add optimization flag for Pi Zero
endif()

find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBCAMERA libcamera)
pkg_check_modules(GSTREAMER gstreamer-1.0 gstreamer-app-1.0)

if(LIBCAMERA_FOUND AND GSTREAMER_FOUND)
    set(SURFCAM_HAVE_CAMERA ON)
else()
    set(SURFCAM_HAVE_CAMERA OFF)
endif()

if(NOT SURFCAM_HAVE_CAMERA AND NOT BUILD_TESTING)
    message(FATAL_ERROR
        "libcamera and GStreamer (pkg-config) are required for the surfcam binary.\n"
        "On macOS they are usually unavailable. Options:\n"
        "  - Docker: docker build -f Dockerfile.build .\n"
        "  - Tests only: cmake -DBUILD_TESTING=ON (unit + HTTP smoke; no camera)")
endif()

if(NOT SURFCAM_HAVE_CAMERA AND BUILD_TESTING)
    message(STATUS "libcamera/GStreamer not found: skipping surfcam; building tests only (BUILD_TESTING=ON).")
endif()

# ApiClient uses nlohmann/json. Prefer a system CMake package (Linux/Homebrew); otherwise fetch (e.g. Xcode-only SDK).
find_package(nlohmann_json 3.2.0 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_Declare(
        surfcam_nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(surfcam_nlohmann_json)
endif()

# Debian bookworm ships libcamera 0.0.x (no Connection, no Plane::map(), no Camera::isRunning()).
set(SURFCAM_LIBCAMERA_LEGACY 0)
if(SURFCAM_HAVE_CAMERA AND PKG_CONFIG_EXECUTABLE)
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

if(SURFCAM_HAVE_CAMERA)
    add_executable(surfcam
        src/main.cpp
        src/CameraManager.cpp
        src/ApiClient.cpp
        src/HlsUploader.cpp
        src/HlsUploadPolicy.cpp
        src/Config.cpp
    )
    target_include_directories(surfcam PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LIBCAMERA_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${GSTREAMER_INCLUDE_DIRS}
    )
    target_link_directories(surfcam PRIVATE
        ${LIBCAMERA_LIBRARY_DIRS}
        ${GSTREAMER_LIBRARY_DIRS}
    )
    target_link_libraries(surfcam PRIVATE
        ${LIBCAMERA_LIBRARIES}
        ${CURL_LIBRARIES}
        pthread
        ${GSTREAMER_LIBRARIES}
        nlohmann_json::nlohmann_json
    )
    target_compile_definitions(surfcam PRIVATE
        PI_ZERO=1
        SURFCAM_LIBCAMERA_LEGACY=${SURFCAM_LIBCAMERA_LEGACY}
    )
    target_compile_options(surfcam PRIVATE ${LIBCAMERA_CFLAGS_OTHER} ${GSTREAMER_CFLAGS_OTHER})

    install(TARGETS surfcam DESTINATION /usr/local/bin)
endif()

if(BUILD_TESTING AND NOT ENABLE_TSAN)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.3
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(surfcam_unit_tests
        tests/hls_tests.cpp
        src/HlsUploader.cpp
        src/HlsUploadPolicy.cpp
    )
    target_include_directories(surfcam_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(surfcam_unit_tests PRIVATE Catch2::Catch2WithMain)

    add_executable(surfcam_test_http
        tests/http_smoke.cpp
        src/ApiClient.cpp
        src/Config.cpp
    )
    target_include_directories(surfcam_test_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CURL_INCLUDE_DIRS})
    target_link_libraries(surfcam_test_http PRIVATE ${CURL_LIBRARIES} pthread nlohmann_json::nlohmann_json)

    add_test(NAME surfcam_unit_tests COMMAND surfcam_unit_tests)
endif()
