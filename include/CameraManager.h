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

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <gst/gst.h>

namespace libcamera {
class Camera;
class CameraManager;
class FrameBufferAllocator;
class FrameBuffer;
class CameraConfiguration;
class Request;
}

namespace SurfCam {

struct FrameData {
std::vector<uint8_t> data;
uint64_t timestamp;
bool keyFrame;
};

class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    bool initialize();
    bool takePicture(const std::string& outputPath);
    bool startVideoMode(int width, int height, int fps);
    bool stopVideoMode();
    bool getEncodedFrame(FrameData& frameData);

private:
    // LibCamera members
    std::unique_ptr<libcamera::CameraManager> cameraManager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> currentConfig_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::shared_ptr<libcamera::FrameBuffer>> buffers_;
    std::mutex bufferMutex_;
    std::condition_variable bufferCV_;

    // Request completion handling
    void requestComplete(libcamera::Request* request);
    std::vector<std::shared_ptr<libcamera::Request>> pendingRequests_;
    std::vector<libcamera::Request*> completedRequests_;
    std::mutex requestMutex_;
    std::condition_variable requestCV_;

    // GStreamer pipeline for H.264 encoding
    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    GstElement* appsink_{nullptr};
    GstBus* bus_{nullptr};
    GMainLoop* loop_{nullptr};
    std::thread gstThread_;
    bool gstRunning_{false};
    
    // Initialize and manage GStreamer pipeline
    bool initializeGstreamerPipeline(int width, int height, int fps);
    void shutdownGstreamerPipeline();
    static void onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data);
    static void onNewEncodedBuffer(GstElement* sink, gpointer user_data);
    
    // Buffer management
    std::queue<FrameData> encodedFrames_;
    std::mutex encodedFramesMutex_;
    std::condition_variable encodedFramesCV_;
    size_t maxBufferedFrames_{10}; // Limit number of buffered frames
    
    // Flags and state
    size_t nextBufferIndex_{0};
    bool isInitialized_{false};
    bool isVideoMode_{false};
    std::atomic<bool> captureActive_{false};
};

}   // namespace SurfCam