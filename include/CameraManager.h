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
#include <optional>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <gst/gst.h>
#include <libcamera/libcamera.h>

namespace SurfCam {

/// Camera + GStreamer lifecycle (Phase 0 remediation):
/// 1. Set captureActive_ / shuttingDown_ (memory_order_seq_cst).
/// 2. Disconnect video requestCompleted slot, then camera_->stop() (RequestCancelled in requestComplete).
/// 3. shutdownGstreamerPipeline().
/// 4. Release allocator_ / clear buffers_ under bufferMutex_.
/// Only streamCheckWorker and main join the HLS worker thread (see main.cpp — strategy A).
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

    /// Serializes initialize, takePicture, startVideoMode, reinitialize. Not held across camera_->stop()
    /// (see stopVideoMode) so requestComplete never deadlocks with libcamera callbacks.
    std::mutex cameraOpsMutex_;

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
    /// Tear down partial/failed video startup (no isVideoMode_).
    void rollbackFailedVideoStart();
    static gboolean onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data);

    // Flags and state
    size_t nextBufferIndex_{0};
    bool isInitialized_{false};
    bool isVideoMode_{false};
    std::atomic<bool> captureActive_{false};
    std::atomic<bool> shuttingDown_{false};
    std::optional<libcamera::Connection> videoRequestCompletedConn_;
};

}  // namespace SurfCam
