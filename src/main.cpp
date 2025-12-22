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

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <ctime>
#include <sstream>
#include <iomanip>

#include "Config.h"
#include "CameraManager.h"
#include "ApiClient.h"
#include "KinesisStreamer.h"

// Global variables
std::atomic<bool> keepRunning{true};
std::atomic<bool> streamShouldRun{false}; // New atomic flag to control stream thread
std::unique_ptr<std::thread> streamThread;
std::atomic<long> lastStreamRequestTime{0}; // Make this atomic to prevent race conditions


// Signal handler
void signalHandler(int signum) {
    std::cout << "\nShutting down gracefully..." << std::endl;
    keepRunning = false;
}

// Get current timestamp as string
std::string getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// Snapshot worker function
void snapshotWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    while (keepRunning) {
        std::cout << "[" << getCurrentTimeString() << "] Taking snapshot..." << std::endl;
        
        if (camera.takePicture(SurfCam::Config::IMAGE_PATH)) {
            if (api.uploadSnapshot(SurfCam::Config::IMAGE_PATH, "Ireland_Donegal_Ballymastocker")) {
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

void streamToKinesis(SurfCam::CameraManager& camera, 
                     const SurfCam::AwsCredentials& credentials) {
    try {
        SurfCam::KinesisStreamer streamer(SurfCam::Config::KINESIS_STREAM_NAME, 
                                         SurfCam::Config::AWS_REGION);
                                         
        if (!streamer.initialize(credentials)) {
            std::cerr << "[" << getCurrentTimeString() << "] Failed to initialize KVS streamer" << std::endl;
            return;
        }
        
        // Start video capture with lower resolution for Pi Zero
        if (!camera.startVideoMode(SurfCam::Config::CAMERA_WIDTH, 
                                  SurfCam::Config::CAMERA_HEIGHT, 
                                  SurfCam::Config::STREAM_FPS)) {
            std::cerr << "[" << getCurrentTimeString() << "] Failed to start video mode" << std::endl;
            return;
        }
        
        std::cout << "[" << getCurrentTimeString() << "] Stream started" << std::endl;
        
        int frameCount = 0;
        int fragmentNumber = 0;
        const int framesPerFragment = SurfCam::Config::STREAM_FPS; // 1 second fragments for Pi Zero
        
        long long streamStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        std::vector<uint8_t> frameBuffer;
        std::vector<uint8_t> fragmentBuffer;
        SurfCam::FrameData frameData;

        
        // Add watchdog timing
        auto lastSuccessTime = std::chrono::steady_clock::now();
        
        while (streamShouldRun.load() && keepRunning.load()) {
            // Capture frame with timeout
            if (!camera.getEncodedFrame(frameData)) {
                std::cerr << "[" << getCurrentTimeString() << "] Failed to capture frame" << std::endl;
                
                // Check if we're stuck
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastSuccessTime).count() > 10) {
                    std::cerr << "[" << getCurrentTimeString() << "] Frame capture stalled, restarting camera" << std::endl;
                    camera.stopVideoMode();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!camera.startVideoMode(SurfCam::Config::CAMERA_WIDTH,
                                             SurfCam::Config::CAMERA_HEIGHT,
                                             SurfCam::Config::STREAM_FPS)) {
                        std::cerr << "[" << getCurrentTimeString() << "] Failed to restart camera" << std::endl;
                        return;
                    }
                    lastSuccessTime = std::chrono::steady_clock::now();
                }
                continue;
            }
            
            lastSuccessTime = std::chrono::steady_clock::now();
            
            // Check if buffer would exceed size limit before adding frame
            if (fragmentBuffer.size() + frameData.data.size() > SurfCam::Config::MAX_FRAGMENT_SIZE) {
                // Send current fragment and reset buffer
                long long fragmentTimecode = fragmentNumber * 1000; // 1 second in milliseconds
                
                if (streamer.sendFrameFragment(fragmentBuffer, fragmentTimecode)) {
                    std::cout << "[" << getCurrentTimeString() << "] Sent fragment " 
                              << fragmentNumber << " (size limit)" << std::endl;
                } else {
                    std::cerr << "[" << getCurrentTimeString() << "] Failed to send fragment" << std::endl;
                }
                
                fragmentBuffer.clear();
                frameCount = 0;
                fragmentNumber++;
            }
            
            // Add frame to fragment buffer
            fragmentBuffer.insert(fragmentBuffer.end(), frameData.data.begin(), frameData.data.end());
            frameCount++;
            
            // When we have enough frames for a fragment, send it to Kinesis
            if (frameCount >= framesPerFragment) {
                // Calculate timestamp
                long long fragmentTimecode = fragmentNumber * 1000; // 1 second in milliseconds
                
                if (streamer.sendFrameFragment(fragmentBuffer, fragmentTimecode)) {
                    std::cout << "[" << getCurrentTimeString() << "] Sent fragment " 
                              << fragmentNumber << " (frame count)" << std::endl;
                } else {
                    std::cerr << "[" << getCurrentTimeString() << "] Failed to send fragment" << std::endl;
                }
                
                fragmentBuffer.clear();
                frameCount = 0;
                fragmentNumber++;
            }
            
            // Sleep briefly to give other processes CPU time on Pi Zero
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        
        // Clean up
        camera.stopVideoMode();
        streamer.shutdown();
        std::cout << "[" << getCurrentTimeString() << "] Stream stopped" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[" << getCurrentTimeString() << "] Error in streaming thread: " << e.what() << std::endl;
        camera.stopVideoMode();
    }
}

// In the streamCheckWorker function:

void streamCheckWorker(SurfCam::CameraManager& camera, SurfCam::ApiClient& api) {
    int failureCount = 0;
    
    while (keepRunning) {
        try {
            bool streamRequested = api.isStreamingRequested(SurfCam::Config::SPOT_ID);
            
            // Check if streaming should continue based on timeout
            auto now = std::chrono::system_clock::now();
            auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
                
            if (streamRequested) {
                lastStreamRequestTime.store(nowSeconds);
                failureCount = 0; // Reset failure count on successful API request
            }
            
            bool shouldStream = (streamRequested || 
                               (nowSeconds - lastStreamRequestTime.load() < SurfCam::Config::STREAM_TIMEOUT.count()));
            
            // Start streaming if requested and not already streaming
            if (shouldStream && (!streamThread || !streamThread->joinable())) {
                // Get streaming credentials
                auto credentials = api.getStreamingCredentials();
                if (credentials) {
                    std::cout << "[" << getCurrentTimeString() << "] Starting stream..." << std::endl;
                    streamShouldRun.store(true);
                    streamThread = std::make_unique<std::thread>(streamToKinesis, 
                                                              std::ref(camera), 
                                                              credentials.value());
                    failureCount = 0; // Reset failure count on successful stream start
                } else {
                    std::cerr << "[" << getCurrentTimeString() << "] Failed to get streaming credentials" << std::endl;
                    failureCount++;
                }
            } 
            // Stop streaming if not requested and currently streaming
            else if (!shouldStream && streamThread && streamThread->joinable()) {
                std::cout << "[" << getCurrentTimeString() << "] Stopping stream..." << std::endl;
                streamShouldRun.store(false); // Signal stream thread to stop
                
                // Join with timeout
                auto stopStart = std::chrono::steady_clock::now();
                while (streamThread->joinable()) {
                    // Try to join with a short timeout
                    std::thread watchdog([&]() {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                    });
                    watchdog.join(); // Wait up to 5 seconds
                    
                    // Check if thread is still running
                    if (streamThread->joinable()) {
                        auto stopDuration = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - stopStart).count();
                            
                        if (stopDuration > 10) {
                            // Thread hasn't stopped after 10 seconds
                            std::cerr << "[" << getCurrentTimeString() << "] Stream thread not responding, forcing termination" << std::endl;
                            // Note: In a production system, we'd need a safer way to handle this
                            // For now, we'll just detach and let the thread die on its own
                            streamThread->detach();
                            break;
                        }
                    } else {
                        break; // Thread successfully joined
                    }
                }
                
                streamThread.reset();
                failureCount = 0;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[" << getCurrentTimeString() << "] Exception in stream check: " << e.what() << std::endl;
            failureCount++;
        }
        
        // Handle repeated failures
        if (failureCount > 3) {
            std::cerr << "[" << getCurrentTimeString() << "] Multiple failures detected, attempting recovery" << std::endl;
            
            // Try to stop streaming if active
            if (streamThread && streamThread->joinable()) {
                streamShouldRun.store(false);
                streamThread->join();
                streamThread.reset();
            }
            
            // Try to reinitialize camera
            if (!camera.reinitialize()) {
                std::cerr << "[" << getCurrentTimeString() << "] Camera reinitialization failed" << std::endl;
                
                // Sleep a bit longer to avoid rapid reinit attempts
                std::this_thread::sleep_for(std::chrono::minutes(1));
            } else {
                failureCount = 0; // Reset on successful recovery
            }
        }
        
        std::this_thread::sleep_for(SurfCam::Config::STREAM_CHECK_INTERVAL);
    }
}


// Add this function:

void monitorSystemResources() {
    while (keepRunning) {
        // Check system memory using procfs
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        int totalMem = 0;
        int freeMem = 0;
        int buffers = 0;
        int cached = 0;
        
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") != std::string::npos) {
                sscanf(line.c_str(), "MemTotal: %d", &totalMem);
            }
            else if (line.find("MemFree:") != std::string::npos) {
                sscanf(line.c_str(), "MemFree: %d", &freeMem);
            }
            else if (line.find("Buffers:") != std::string::npos) {
                sscanf(line.c_str(), "Buffers: %d", &buffers);
            }
            else if (line.find("Cached:") != std::string::npos && 
                     line.find("SwapCached:") == std::string::npos) {
                sscanf(line.c_str(), "Cached: %d", &cached);
            }
        }
        
        // Calculate available memory in MB
        int availableMem = (freeMem + buffers + cached) / 1024;
        int totalMemMB = totalMem / 1024;
        
        // Check CPU temperature
        std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
        int temp = 0;
        if (tempFile >> temp) {
            float cpuTemp = temp / 1000.0f;
            
            std::cout << "[" << getCurrentTimeString() << "] System monitor: " 
                      << "Memory: " << availableMem << "/" << totalMemMB << " MB free, "
                      << "CPU temp: " << cpuTemp << "°C" << std::endl;
                      
            // Check if resources are critically low
            if (availableMem < 40) { // Less than 40MB free
                std::cerr << "[" << getCurrentTimeString() << "] LOW MEMORY WARNING: " 
                          << availableMem << " MB available" << std::endl;
                          
                // If streaming, stop it to recover memory
                if (streamThread && streamThread->joinable()) {
                    std::cout << "[" << getCurrentTimeString() << "] Emergency stream shutdown due to low memory" << std::endl;
                    streamShouldRun.store(false);
                    streamThread->join();
                    streamThread.reset();
                }
            }
            
            // Check for overheating
            if (cpuTemp > 75.0) { // Pi Zero throttles at 80°C
                std::cerr << "[" << getCurrentTimeString() << "] HIGH TEMPERATURE WARNING: " 
                          << cpuTemp << "°C" << std::endl;
                          
                // If streaming, stop it to reduce CPU load
                if (streamThread && streamThread->joinable()) {
                    std::cout << "[" << getCurrentTimeString() << "] Emergency stream shutdown due to high temperature" << std::endl;
                    streamShouldRun.store(false);
                    streamThread->join();
                    streamThread.reset();
                }
            }
        }
        
        // Sleep for 30 seconds
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

int main() {
    // Register signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << "[" << getCurrentTimeString() << "] Starting SurfCam Weather Pi" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Taking snapshots every " 
              << SurfCam::Config::SNAPSHOT_INTERVAL.count() << " seconds" << std::endl;
    std::cout << "[" << getCurrentTimeString() << "] Checking for streaming requests every " 
              << SurfCam::Config::STREAM_CHECK_INTERVAL.count() << " seconds" << std::endl;
    
    // Initialize camera
    SurfCam::CameraManager camera;
    if (!camera.initialize()) {
        std::cerr << "Failed to initialize camera" << std::endl;
        return 1;
    }
    
    // Initialize API client
    SurfCam::ApiClient apiClient(SurfCam::Config::API_ENDPOINT, SurfCam::Config::API_KEY);
    
    // Start worker threads
    std::thread snapshotThread(snapshotWorker, std::ref(camera), std::ref(apiClient));
    std::thread checkStreamThread(streamCheckWorker, std::ref(camera), std::ref(apiClient));
    std::thread monitorThread(monitorSystemResources); // Add this line

    // Keep main thread alive
    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Clean up
    snapshotThread.join();
    checkStreamThread.join();
    monitorThread.join(); // Add this line

    if (streamThread && streamThread->joinable()) {
        streamThread->join();
    }
    
    std::cout << "Exiting..." << std::endl;
    return 0;
}