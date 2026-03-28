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
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
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

class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    bool initialize();
    bool takePicture(const std::string& outputPath);
    bool startVideoMode(int width, int height, int fps);
    bool stopVideoMode();
    bool reinitialize();

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

    // GStreamer pipeline: appsrc -> H.264 -> hlssink
    GstElement* pipeline_{nullptr};
    GstElement* appsrc_{nullptr};
    GstElement* hlssink_{nullptr};
    GstBus* bus_{nullptr};
    GMainLoop* loop_{nullptr};
    std::thread gstThread_;
    bool gstRunning_{false};

    bool initializeGstreamerPipeline(int width, int height, int fps);
    void shutdownGstreamerPipeline();
    static gboolean onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data);

    // Flags and state
    size_t nextBufferIndex_{0};
    bool isInitialized_{false};
    bool isVideoMode_{false};
    std::atomic<bool> captureActive_{false};
};

}  // namespace SurfCam
