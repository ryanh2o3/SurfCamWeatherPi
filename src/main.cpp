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

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
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

std::atomic<bool> keepRunning{true};
std::atomic<bool> streamShouldRun{false};
std::unique_ptr<std::thread> streamThread;
std::atomic<long> lastStreamRequestTime{0};

void signalHandler(int /*signum*/) {
    std::cout << "\nShutting down gracefully..." << std::endl;
    keepRunning = false;
}

std::string getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void snapshotWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    while (keepRunning) {
        std::cout << "[" << getCurrentTimeString() << "] Taking snapshot..." << std::endl;

        if (camera.takePicture(SurfCam::Config::IMAGE_PATH)) {
            if (api.uploadSnapshot(SurfCam::Config::IMAGE_PATH, SurfCam::Config::SPOT_ID)) {
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
        return;
    }

    std::cout << "[" << getCurrentTimeString() << "] HLS encode started → " << SurfCam::Config::HLS_OUTPUT_DIR
              << std::endl;

    while (streamShouldRun.load() && keepRunning.load()) {
        uploader.pollAndUpload(api, SurfCam::Config::SPOT_ID, SurfCam::Config::HLS_OUTPUT_DIR);
        std::this_thread::sleep_for(SurfCam::Config::HLS_UPLOAD_POLL);
    }

    camera.stopVideoMode();

    for (int i = 0; i < 8; ++i) {
        uploader.pollAndUpload(api, SurfCam::Config::SPOT_ID, SurfCam::Config::HLS_OUTPUT_DIR);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "[" << getCurrentTimeString() << "] HLS stream worker finished" << std::endl;
}

void streamCheckWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    int failureCount = 0;

    while (keepRunning) {
        try {
            bool streamRequested = api.isStreamingRequested(SurfCam::Config::SPOT_ID);

            auto now = std::chrono::system_clock::now();
            auto nowSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

            if (streamRequested) {
                lastStreamRequestTime.store(nowSeconds);
                failureCount = 0;
            }

            bool shouldStream =
                streamRequested ||
                (nowSeconds - lastStreamRequestTime.load() < SurfCam::Config::STREAM_TIMEOUT.count());

            if (shouldStream && (!streamThread || !streamThread->joinable())) {
                std::cout << "[" << getCurrentTimeString() << "] Starting HLS upload session..." << std::endl;
                streamShouldRun.store(true);
                streamThread = std::make_unique<std::thread>(streamHlsWorker, std::ref(camera), std::ref(api));
                failureCount = 0;
            } else if (!shouldStream && streamThread && streamThread->joinable()) {
                std::cout << "[" << getCurrentTimeString() << "] Stopping HLS session..." << std::endl;
                streamShouldRun.store(false);
                streamThread->join();
                streamThread.reset();
                failureCount = 0;
            }

        } catch (const std::exception& e) {
            std::cerr << "[" << getCurrentTimeString() << "] Exception in stream check: " << e.what() << std::endl;
            failureCount++;
        }

        if (failureCount > 3) {
            std::cerr << "[" << getCurrentTimeString() << "] Multiple failures, attempting recovery" << std::endl;

            if (streamThread && streamThread->joinable()) {
                streamShouldRun.store(false);
                streamThread->join();
                streamThread.reset();
            }

            if (!camera.reinitialize()) {
                std::cerr << "[" << getCurrentTimeString() << "] Camera reinitialization failed" << std::endl;
                std::this_thread::sleep_for(std::chrono::minutes(1));
            } else {
                failureCount = 0;
            }
        }

        std::this_thread::sleep_for(SurfCam::Config::STREAM_CHECK_INTERVAL);
    }
}

void monitorSystemResources() {
    while (keepRunning) {
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        int totalMem = 0;
        int freeMem = 0;
        int buffers = 0;
        int cached = 0;

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

        int availableMem = (freeMem + buffers + cached) / 1024;
        int totalMemMB = totalMem / 1024;

        std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
        int temp = 0;
        if (tempFile >> temp) {
            float cpuTemp = temp / 1000.0f;

            std::cout << "[" << getCurrentTimeString() << "] System monitor: "
                      << "Memory: " << availableMem << "/" << totalMemMB << " MB free, "
                      << "CPU temp: " << cpuTemp << "°C" << std::endl;

            if (availableMem < 40) {
                std::cerr << "[" << getCurrentTimeString() << "] LOW MEMORY WARNING: " << availableMem << " MB"
                          << std::endl;
                if (streamThread && streamThread->joinable()) {
                    std::cout << "[" << getCurrentTimeString() << "] Emergency HLS shutdown (low memory)"
                              << std::endl;
                    streamShouldRun.store(false);
                    streamThread->join();
                    streamThread.reset();
                }
            }

            if (cpuTemp > 75.0f) {
                std::cerr << "[" << getCurrentTimeString() << "] HIGH TEMPERATURE WARNING: " << cpuTemp << "°C"
                          << std::endl;
                if (streamThread && streamThread->joinable()) {
                    std::cout << "[" << getCurrentTimeString() << "] Emergency HLS shutdown (temperature)"
                              << std::endl;
                    streamShouldRun.store(false);
                    streamThread->join();
                    streamThread.reset();
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "[" << getCurrentTimeString() << "] Starting SurfCam Weather Pi" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Snapshots every " << SurfCam::Config::SNAPSHOT_INTERVAL.count()
              << " s; stream check every " << SurfCam::Config::STREAM_CHECK_INTERVAL.count() << " s" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Live path: HLS → S3 via " << SurfCam::Config::API_ENDPOINT
              << SurfCam::Config::HLS_PRESIGN_PATH << std::endl;

    SurfCam::CameraManager camera;
    if (!camera.initialize()) {
        std::cerr << "Failed to initialize camera" << std::endl;
        return 1;
    }

    SurfCam::ApiClient apiClient(SurfCam::Config::API_ENDPOINT, SurfCam::Config::API_KEY);

    std::thread snapshotThread(snapshotWorker, std::ref(camera), std::ref(apiClient));
    std::thread checkStreamThread(streamCheckWorker, std::ref(camera), std::ref(apiClient));
    std::thread monitorThread(monitorSystemResources);

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    streamShouldRun.store(false);
    snapshotThread.join();
    checkStreamThread.join();
    monitorThread.join();

    if (streamThread && streamThread->joinable()) {
        streamThread->join();
    }

    std::cout << "Exiting..." << std::endl;
    return 0;
}
