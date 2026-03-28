/*
 * SurfCam Weather Pi
 * Copyright (C) 2025  Ryan Patton
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "ApiClient.h"
#include "CameraManager.h"
#include "Config.h"
#include "HlsUploader.h"

// Stream thread ownership (Phase 0 / R1 — strategy A): only streamCheckWorker and main (after
// checkStreamThread joins) call join/reset on streamThread. The monitor sets emergencyStopHls;
// streamCheckWorker performs stop/join/reset on the next loop iteration.

std::atomic<bool> keepRunning{true};
std::atomic<bool> streamShouldRun{false};
std::unique_ptr<std::thread> streamThread;
std::atomic<long> lastStreamRequestTime{0};
std::atomic<bool> emergencyStopHls{false};
std::atomic<bool> cameraRecoveryNeeded{false};

namespace {

void stopHlsStreamSession() {
    streamShouldRun.store(false);
    if (streamThread && streamThread->joinable()) {
        streamThread->join();
        streamThread.reset();
    }
}

/// Ensures streamShouldRun is cleared when the HLS worker thread exits so streamCheckWorker can reap the thread.
struct StreamShouldRunClear {
    ~StreamShouldRunClear() { streamShouldRun.store(false); }
};

void reapFinishedHlsWorker() {
    if (streamThread && streamThread->joinable() && !streamShouldRun.load(std::memory_order_acquire)) {
        streamThread->join();
        streamThread.reset();
    }
}

}  // namespace

void signalHandler(int /*signum*/) {
    keepRunning.store(false);
}

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

std::string getCurrentTimeString() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tmBuf {};
    struct tm* tmPtr = localtime_r(&t, &tmBuf);
    std::stringstream ss;
    if (tmPtr) {
        ss << std::put_time(tmPtr, "%Y-%m-%d %H:%M:%S");
    }
    return ss.str();
}

void snapshotWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    while (keepRunning) {
        std::cout << "[" << getCurrentTimeString() << "] Taking snapshot..." << std::endl;

        if (camera.takePicture(SurfCam::Config::SNAPSHOT_PATH)) {
            if (api.uploadSnapshot(SurfCam::Config::SNAPSHOT_PATH, SurfCam::Config::SPOT_ID)) {
                std::cout << "[" << getCurrentTimeString() << "] Snapshot uploaded successfully!" << std::endl;
            } else {
                std::cout << "[" << getCurrentTimeString() << "] Failed to upload snapshot." << std::endl;
            }
        } else {
            std::cout << "[" << getCurrentTimeString() << "] Failed to capture snapshot." << std::endl;
        }

        std::this_thread::sleep_for(SurfCam::Config::SNAPSHOT_INTERVAL);
    }
}

void streamHlsWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    StreamShouldRunClear clearStreamFlag;
    SurfCam::HlsUploader uploader;
    uploader.resetSession();

    try {
        std::filesystem::create_directories(SurfCam::Config::HLS_OUTPUT_DIR);
        for (const auto& entry : std::filesystem::directory_iterator(SurfCam::Config::HLS_OUTPUT_DIR)) {
            std::filesystem::remove(entry.path());
        }
    } catch (const std::exception& e) {
        std::cerr << "[" << getCurrentTimeString() << "] HLS dir cleanup failed: " << e.what() << std::endl;
        return;
    }

    if (!camera.startVideoMode(SurfCam::Config::CAMERA_WIDTH, SurfCam::Config::CAMERA_HEIGHT,
                               SurfCam::Config::STREAM_FPS)) {
        std::cerr << "[" << getCurrentTimeString() << "] Failed to start video mode" << std::endl;
        cameraRecoveryNeeded.store(true, std::memory_order_release);
        return;
    }

    std::cout << "[" << getCurrentTimeString() << "] HLS encode started → " << SurfCam::Config::HLS_OUTPUT_DIR
              << std::endl;

    try {
        while (streamShouldRun.load(std::memory_order_acquire) && keepRunning.load(std::memory_order_acquire) &&
               !emergencyStopHls.load(std::memory_order_acquire)) {
            if (camera.consumeEncoderPipelineFailure()) {
                std::cerr << "[" << getCurrentTimeString()
                          << "] GStreamer encoder error; stopping HLS encode session..." << std::endl;
                break;
            }
            uploader.pollAndUpload(api, SurfCam::Config::SPOT_ID, SurfCam::Config::HLS_OUTPUT_DIR);
            std::this_thread::sleep_for(SurfCam::Config::HLS_UPLOAD_POLL);
        }
    } catch (const std::exception& e) {
        std::cerr << "[" << getCurrentTimeString() << "] Exception in HLS worker loop: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[" << getCurrentTimeString() << "] Unknown exception in HLS worker loop" << std::endl;
    }

    camera.stopVideoMode();

    try {
        for (int i = 0; i < 8; ++i) {
            uploader.pollAndUpload(api, SurfCam::Config::SPOT_ID, SurfCam::Config::HLS_OUTPUT_DIR);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (const std::exception& e) {
        std::cerr << "[" << getCurrentTimeString() << "] HLS drain upload failed: " << e.what() << std::endl;
    }

    std::cout << "[" << getCurrentTimeString() << "] HLS stream worker finished" << std::endl;
}

void streamCheckWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    int apiFailureStreak = 0;

    while (keepRunning.load(std::memory_order_acquire)) {
        reapFinishedHlsWorker();

        if (emergencyStopHls.exchange(false, std::memory_order_acq_rel)) {
            std::cerr << "[" << getCurrentTimeString()
                      << "] Emergency HLS stop requested (monitor); stopping session..." << std::endl;
            stopHlsStreamSession();
            apiFailureStreak = 0;
        }

        if (cameraRecoveryNeeded.exchange(false, std::memory_order_acq_rel) && keepRunning.load(std::memory_order_acquire)) {
            std::cerr << "[" << getCurrentTimeString()
                      << "] Camera recovery after HLS start failure; stopping session and reinitializing..."
                      << std::endl;
            stopHlsStreamSession();
            if (!camera.reinitialize()) {
                std::cerr << "[" << getCurrentTimeString() << "] Camera reinitialization failed" << std::endl;
                std::this_thread::sleep_for(std::chrono::minutes(1));
            }
        }

        try {
            auto streamState = api.isStreamingRequested(SurfCam::Config::SPOT_ID);

            // Re-check after blocking API work so shutdown cannot start a new HLS session mid-iteration.
            if (!keepRunning.load(std::memory_order_acquire)) {
                continue;
            }

            const auto now = std::chrono::system_clock::now();
            const auto nowSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

            if (streamState.has_value()) {
                apiFailureStreak = 0;
                if (*streamState) {
                    lastStreamRequestTime.store(nowSeconds, std::memory_order_release);
                }
            } else {
                apiFailureStreak++;
            }

            const bool apiSaysStreamOn = streamState.has_value() && *streamState;
            const long lastReqSec = lastStreamRequestTime.load(std::memory_order_acquire);
            const bool withinGrace =
                lastReqSec != 0 &&
                (nowSeconds - lastReqSec) < SurfCam::Config::STREAM_TIMEOUT.count();
            const bool shouldStream = apiSaysStreamOn || withinGrace;

            if (keepRunning.load(std::memory_order_acquire)) {
                if (shouldStream && (!streamThread || !streamThread->joinable())) {
                    std::cout << "[" << getCurrentTimeString() << "] Starting HLS upload session..." << std::endl;
                    streamShouldRun.store(true);
                    streamThread = std::make_unique<std::thread>(streamHlsWorker, std::ref(camera), std::ref(api));
                } else if (!shouldStream && streamThread && streamThread->joinable()) {
                    std::cout << "[" << getCurrentTimeString() << "] Stopping HLS session..." << std::endl;
                    stopHlsStreamSession();
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "[" << getCurrentTimeString() << "] Exception in stream check (API): " << e.what()
                      << std::endl;
            apiFailureStreak++;
        }

        std::this_thread::sleep_for(SurfCam::Config::STREAM_CHECK_INTERVAL);
        if (apiFailureStreak > 5 && keepRunning.load(std::memory_order_acquire)) {
            const int extraSec = std::min(180, (apiFailureStreak - 5) * 20);
            std::this_thread::sleep_for(std::chrono::seconds(extraSec));
        }
    }
}

void monitorSystemResources() {
    using clock = std::chrono::steady_clock;
    constexpr auto kWarnInterval = std::chrono::minutes(2);
    auto lastProcWarn = clock::time_point::min();
    auto lastLowMemWarn = clock::time_point::min();
    auto lastHighTempWarn = clock::time_point::min();

    while (keepRunning.load(std::memory_order_acquire)) {
        const auto now = clock::now();
        bool memOk = false;
        int totalMem = 0;
        int freeMem = 0;
        int buffers = 0;
        int cached = 0;

        {
            std::ifstream meminfo("/proc/meminfo");
            if (meminfo) {
                memOk = true;
                std::string line;
                while (std::getline(meminfo, line)) {
                    if (line.find("MemTotal:") != std::string::npos) {
                        sscanf(line.c_str(), "MemTotal: %d", &totalMem);
                    } else if (line.find("MemFree:") != std::string::npos) {
                        sscanf(line.c_str(), "MemFree: %d", &freeMem);
                    } else if (line.find("Buffers:") != std::string::npos) {
                        sscanf(line.c_str(), "Buffers: %d", &buffers);
                    } else if (line.find("Cached:") != std::string::npos &&
                               line.find("SwapCached:") == std::string::npos) {
                        sscanf(line.c_str(), "Cached: %d", &cached);
                    }
                }
            }
        }

        int availableMemMb = 0;
        int totalMemMb = 0;
        if (memOk && totalMem > 0) {
            availableMemMb = (freeMem + buffers + cached) / 1024;
            totalMemMb = totalMem / 1024;
        }

        bool tempOk = false;
        int tempMilliC = 0;
        {
            std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
            if (tempFile >> tempMilliC) {
                tempOk = true;
            }
        }

        if (memOk && tempOk) {
            const float cpuTemp = tempMilliC / 1000.0f;
            std::cout << "[" << getCurrentTimeString() << "] System monitor: "
                      << "Memory: " << availableMemMb << "/" << totalMemMb << " MB free, "
                      << "CPU temp: " << cpuTemp << "°C" << std::endl;

            if (availableMemMb < 40) {
                emergencyStopHls.store(true, std::memory_order_release);
                if (now - lastLowMemWarn > kWarnInterval) {
                    std::cerr << "[" << getCurrentTimeString() << "] LOW MEMORY WARNING: " << availableMemMb
                              << " MB (HLS stop requested)" << std::endl;
                    lastLowMemWarn = now;
                }
            }

            if (cpuTemp > 75.0f) {
                emergencyStopHls.store(true, std::memory_order_release);
                if (now - lastHighTempWarn > kWarnInterval) {
                    std::cerr << "[" << getCurrentTimeString() << "] HIGH TEMPERATURE WARNING: " << cpuTemp
                              << "°C (HLS stop requested)" << std::endl;
                    lastHighTempWarn = now;
                }
            }
        } else {
            if (now - lastProcWarn > kWarnInterval) {
                if (!memOk) {
                    std::cerr << "[" << getCurrentTimeString()
                              << "] System monitor: cannot read /proc/meminfo; skipping memory metrics this cycle"
                              << std::endl;
                }
                if (!tempOk) {
                    std::cerr << "[" << getCurrentTimeString()
                              << "] System monitor: cannot read thermal zone; skipping temperature metrics this cycle"
                              << std::endl;
                }
                lastProcWarn = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

int main() {
    installSignalHandlers();

    if (!SurfCam::Config::loadFromEnvironment(std::cerr)) {
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "curl_global_init failed." << std::endl;
        return 1;
    }

    std::cout << "[" << getCurrentTimeString() << "] Starting SurfCam Weather Pi" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Snapshots every " << SurfCam::Config::SNAPSHOT_INTERVAL.count()
              << " s; stream check every " << SurfCam::Config::STREAM_CHECK_INTERVAL.count() << " s" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Live path: HLS → S3 via " << SurfCam::Config::API_ENDPOINT
              << SurfCam::Config::HLS_PRESIGN_PATH << std::endl;

    SurfCam::CameraManager camera;
    if (!camera.initialize()) {
        std::cerr << "Failed to initialize camera" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    SurfCam::ApiClient apiClient(SurfCam::Config::API_ENDPOINT, SurfCam::Config::API_KEY);

    std::thread snapshotThread(snapshotWorker, std::ref(camera), std::ref(apiClient));
    std::thread checkStreamThread(streamCheckWorker, std::ref(camera), std::ref(apiClient));
    std::thread monitorThread(monitorSystemResources);

    while (keepRunning.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\nShutting down gracefully..." << std::endl;

    streamShouldRun.store(false);
    snapshotThread.join();
    checkStreamThread.join();
    monitorThread.join();

    stopHlsStreamSession();

    curl_global_cleanup();
    std::cout << "Exiting..." << std::endl;
    return 0;
}
