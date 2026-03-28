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

#include "CameraManager.h"
#include "Config.h"
#include "LibcameraCompat.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <utility>
#include <libcamera/libcamera.h>
#include <gst/gst.h>
#include <gst/gstpluginfeature.h>
#include <gst/app/gstappsrc.h>

namespace SurfCam {

namespace {

/// Unlocks a held unique_lock for rollback/stop that take videoTeardownMutex_ (avoids lock order deadlock).
struct ScopedUnlockUniqueLock {
    explicit ScopedUnlockUniqueLock(std::unique_lock<std::mutex>& lock) : lock_(lock) { lock_.unlock(); }
    ~ScopedUnlockUniqueLock() {
        if (!lock_.owns_lock()) {
            lock_.lock();
        }
    }

    ScopedUnlockUniqueLock(const ScopedUnlockUniqueLock&) = delete;
    ScopedUnlockUniqueLock& operator=(const ScopedUnlockUniqueLock&) = delete;

private:
    std::unique_lock<std::mutex>& lock_;
};

void stopCameraIfRunning(libcamera::Camera* cam) {
    if (!cam) {
        return;
    }
#if SURFCAM_LIBCAMERA_LEGACY
    cam->stop();
#else
    if (cam->isRunning()) {
        cam->stop();
    }
#endif
}

/// Disconnects still-capture requestCompleted slot (R4). libcamera < 0.2 has no Connection type.
#if SURFCAM_LIBCAMERA_LEGACY
struct ScopedPhotoRequestCompletedConnection {
    libcamera::Camera* cam_;
    CameraManager* self_;

    template <typename F>
    ScopedPhotoRequestCompletedConnection(libcamera::Camera* c, CameraManager* s, F&& f) : cam_(c), self_(s) {
        cam_->requestCompleted.connect(s, std::forward<F>(f));
    }

    ~ScopedPhotoRequestCompletedConnection() {
        if (cam_) {
            cam_->requestCompleted.disconnect(self_);
        }
    }

    ScopedPhotoRequestCompletedConnection(const ScopedPhotoRequestCompletedConnection&) = delete;
    ScopedPhotoRequestCompletedConnection& operator=(const ScopedPhotoRequestCompletedConnection&) = delete;
};
#else
struct ScopedPhotoRequestCompletedConnection {
    libcamera::Connection conn_;

    template <typename F>
    ScopedPhotoRequestCompletedConnection(libcamera::Camera* c, CameraManager* s, F&& f)
        : conn_(c->requestCompleted.connect(s, std::forward<F>(f))) {}

    ~ScopedPhotoRequestCompletedConnection() { conn_.disconnect(); }

    ScopedPhotoRequestCompletedConnection(const ScopedPhotoRequestCompletedConnection&) = delete;
    ScopedPhotoRequestCompletedConnection& operator=(const ScopedPhotoRequestCompletedConnection&) = delete;
};
#endif

}  // namespace

CameraManager::CameraManager() : isInitialized_(false), isVideoMode_(false) {
    gst_init(nullptr, nullptr);
}

bool CameraManager::consumeEncoderPipelineFailure() {
    return gstPipelineError_.exchange(false, std::memory_order_acq_rel);
}

CameraManager::~CameraManager() {
    try {
        if (isVideoMode_) {
            stopVideoMode();
        }

        if (camera_) {
            stopCameraIfRunning(camera_.get());
            camera_->release();
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
        }

        if (cameraManager_) {
            cameraManager_->stop();
        }
        cameraManager_.reset();

        shutdownGstreamerPipeline();
    } catch (const std::exception& e) {
        std::cerr << "Exception in CameraManager destructor: " << e.what() << std::endl;
    }
}

bool CameraManager::initialize() {
    std::lock_guard<std::mutex> lock(cameraOpsMutex_);
    try {
        cameraManager_ = std::make_unique<libcamera::CameraManager>();
        cameraManager_->start();

        auto cameras = cameraManager_->cameras();
        if (cameras.empty()) {
            std::cerr << "No cameras available" << std::endl;
            return false;
        }

        camera_ = cameras[0];

        if (camera_->acquire() != 0) {
            std::cerr << "Failed to acquire camera" << std::endl;
            return false;
        }

        isInitialized_ = true;
        snapshotsPaused_.store(false, std::memory_order_release);
        std::cout << "Camera initialized: " << camera_->id() << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception initializing camera: " << e.what() << std::endl;
        return false;
    }
}

bool CameraManager::takePicture(const std::string& outputPath) {
    std::unique_lock<std::mutex> cameraLock(cameraOpsMutex_);
    if (snapshotsPaused_.load(std::memory_order_acquire)) {
        std::cerr << "Snapshots paused (camera recovery failed); skipping capture" << std::endl;
        return false;
    }
    if (!isInitialized_ || !camera_) {
        std::cerr << "Camera not initialized" << std::endl;
        return false;
    }

    if (isVideoMode_) {
        cameraLock.unlock();
        stopVideoMode();
        cameraLock.lock();
        if (!isInitialized_ || !camera_) {
            std::cerr << "Camera not initialized" << std::endl;
            return false;
        }
    }

    try {
        libcamera::StreamRoles roles = {libcamera::StreamRole::StillCapture};
        auto config = camera_->generateConfiguration(roles);

        auto& streamConfig = config->at(0);
        streamConfig.size = {2592, 1944};
        streamConfig.pixelFormat = libcamera::formats::MJPEG;
        streamConfig.bufferCount = 1;

        auto configStatus = camera_->configure(config.get());
        if (configStatus != 0) {
            std::cerr << "Failed to configure camera: " << configStatus << std::endl;
            return false;
        }

        libcamera::FrameBufferAllocator allocator(camera_);
        if (allocator.allocate(streamConfig.stream()) < 0) {
            std::cerr << "Failed to allocate buffers" << std::endl;
            return false;
        }

        auto request = camera_->createRequest();
        if (!request) {
            std::cerr << "Failed to create request" << std::endl;
            return false;
        }

        const auto& buffers = allocator.buffers(streamConfig.stream());
        if (buffers.empty()) {
            std::cerr << "No buffers allocated" << std::endl;
            return false;
        }

        if (request->addBuffer(streamConfig.stream(), buffers[0].get()) < 0) {
            std::cerr << "Failed to add buffer to request" << std::endl;
            return false;
        }

        std::mutex completionMutex;
        std::condition_variable completionCV;
        bool completed = false;

        ScopedPhotoRequestCompletedConnection photoSlot(camera_.get(), this, [&](libcamera::Request* /*req*/) {
            std::lock_guard<std::mutex> lock(completionMutex);
            completed = true;
            completionCV.notify_one();
        });

        if (camera_->start() != 0) {
            std::cerr << "Failed to start camera" << std::endl;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (camera_->queueRequest(request.get()) != 0) {
            std::cerr << "Failed to queue request" << std::endl;
            camera_->stop();
            return false;
        }

        {
            std::unique_lock<std::mutex> lock(completionMutex);
            if (!completionCV.wait_for(lock, std::chrono::seconds(5), [&] { return completed; })) {
                std::cerr << "Timeout waiting for image capture" << std::endl;
                camera_->stop();
                return false;
            }
        }

        auto buffer = request->buffers().begin()->second;

#if SURFCAM_LIBCAMERA_LEGACY
        LegacyMappedPlane mapped;
        if (!mapped.mapRead(buffer)) {
            std::cerr << "Failed to map buffer" << std::endl;
            camera_->stop();
            return false;
        }

        std::ofstream output(outputPath, std::ios::binary);
        if (!output.is_open()) {
            std::cerr << "Failed to open output file" << std::endl;
            camera_->stop();
            return false;
        }

        output.write(static_cast<const char*>(mapped.data()), mapped.size());
        output.close();
#else
        auto span = buffer->planes()[0].map();
        if (!span) {
            std::cerr << "Failed to map buffer" << std::endl;
            camera_->stop();
            return false;
        }

        std::ofstream output(outputPath, std::ios::binary);
        if (!output.is_open()) {
            std::cerr << "Failed to open output file" << std::endl;
            camera_->stop();
            return false;
        }

        output.write(static_cast<const char*>(span.data()), span.size());
        output.close();

        buffer->planes()[0].unmap();
#endif

        camera_->stop();

        std::cout << "Image captured and saved to: " << outputPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception taking picture: " << e.what() << std::endl;
        stopCameraIfRunning(camera_.get());
        return false;
    }
}

void CameraManager::shutdownGstreamerPipeline() {
    std::lock_guard<std::mutex> pushLock(pipelinePushMutex_);

    if (!pipeline_) {
        return;
    }

    gst_element_set_state(pipeline_, GST_STATE_NULL);

    if (loop_ && g_main_loop_is_running(loop_)) {
        g_main_loop_quit(loop_);
    }

    if (gstThread_.joinable()) {
        gstThread_.join();
    }

    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }

    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    if (loop_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }

    appsrc_ = nullptr;
    hlssink_ = nullptr;
}

void CameraManager::rollbackFailedVideoStart() {
    std::lock_guard<std::mutex> tdLock(videoTeardownMutex_);
    disconnectVideoRequestSlot();
    stopCameraIfRunning(camera_.get());
    {
        std::lock_guard<std::mutex> rq(requestMutex_);
        pendingRequests_.clear();
        completedRequests_.clear();
    }
    shutdownGstreamerPipeline();
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        allocator_.reset();
    }
    captureActive_.store(false, std::memory_order_release);
    shuttingDown_.store(false, std::memory_order_release);
}

gboolean CameraManager::onGstMessage(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
    CameraManager* manager = static_cast<CameraManager*>(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug_info = nullptr;
            gst_message_parse_error(msg, &err, &debug_info);

            std::cerr << "GStreamer error: " << err->message << std::endl;
            if (debug_info) {
                std::cerr << "Debug info: " << debug_info << std::endl;
            }

            g_clear_error(&err);
            g_free(debug_info);

            manager->gstPipelineError_.store(true, std::memory_order_release);
            if (manager->loop_ && g_main_loop_is_running(manager->loop_)) {
                g_main_loop_quit(manager->loop_);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            std::cout << "End of stream reached" << std::endl;
            break;
        default:
            break;
    }

    return TRUE;
}

static void configureH264Encoder(GstElement* h264enc, int fps) {
    GstElementFactory* factory = gst_element_get_factory(h264enc);
    const gchar* factoryName =
        factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "";

    if (g_strcmp0(factoryName, "x264enc") == 0) {
        g_object_set(G_OBJECT(h264enc), "tune", 4, "speed-preset", 1, "bitrate",
                     Config::GSTREAMER_BITRATE / 1000, "key-int-max", fps, "byte-stream", TRUE, "threads", 1,
                     nullptr);
    } else {
        g_object_set(G_OBJECT(h264enc), "bitrate", Config::GSTREAMER_BITRATE, nullptr);
    }
}

bool CameraManager::initializeGstreamerPipeline(int width, int height, int fps) {
    try {
        std::filesystem::create_directories(Config::HLS_OUTPUT_DIR);

        pipeline_ = gst_pipeline_new("camera-hls-pipeline");
        if (!pipeline_) {
            std::cerr << "Failed to create GStreamer pipeline" << std::endl;
            return false;
        }

        appsrc_ = gst_element_factory_make("appsrc", "camera-source");
        GstElement* convert = gst_element_factory_make("videoconvert", "converter");

        GstElement* h264enc = nullptr;
        const char* encoders[] = {"omxh264enc", "v4l2h264enc", "x264enc", nullptr};
        for (int i = 0; encoders[i] != nullptr; i++) {
            h264enc = gst_element_factory_make(encoders[i], "h264-encoder");
            if (h264enc) {
                std::cout << "Using encoder: " << encoders[i] << std::endl;
                break;
            }
        }

        if (!h264enc) {
            std::cerr << "Failed to create any H264 encoder" << std::endl;
            if (appsrc_)
                gst_object_unref(appsrc_);
            if (convert)
                gst_object_unref(convert);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            appsrc_ = nullptr;
            return false;
        }

        GstElement* h264parse = gst_element_factory_make("h264parse", "parser");
        GstElement* queue = gst_element_factory_make("queue", "queue");
        hlssink_ = gst_element_factory_make("hlssink", "hls-sink");
        if (!hlssink_) {
            hlssink_ = gst_element_factory_make("hlssink2", "hls-sink");
        }

        if (!appsrc_ || !convert || !h264enc || !h264parse || !queue || !hlssink_) {
            std::cerr << "Failed to create GStreamer elements (install gstreamer1.0-plugins-bad for hlssink)"
                      << std::endl;
            if (appsrc_)
                gst_object_unref(appsrc_);
            if (convert)
                gst_object_unref(convert);
            if (h264enc)
                gst_object_unref(h264enc);
            if (h264parse)
                gst_object_unref(h264parse);
            if (queue)
                gst_object_unref(queue);
            if (hlssink_)
                gst_object_unref(hlssink_);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            appsrc_ = nullptr;
            hlssink_ = nullptr;
            return false;
        }

        gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, h264enc, h264parse, queue, hlssink_, nullptr);

        if (!gst_element_link_many(appsrc_, convert, h264enc, h264parse, queue, hlssink_, nullptr)) {
            std::cerr << "Failed to link GStreamer elements" << std::endl;
            shutdownGstreamerPipeline();
            return false;
        }

        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT,
                                            width, "height", G_TYPE_INT, height, "framerate", GST_TYPE_FRACTION, fps,
                                            1, nullptr);
        g_object_set(G_OBJECT(appsrc_), "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE, "do-timestamp",
                     TRUE, nullptr);
        gst_caps_unref(caps);

        configureH264Encoder(h264enc, fps);

        g_object_set(G_OBJECT(queue), "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", 0, nullptr);

        const std::string playlistPath = Config::HLS_OUTPUT_DIR + "/" + Config::HLS_PLAYLIST_NAME;
        const std::string segmentPattern = Config::HLS_OUTPUT_DIR + "/segment-%05d.ts";

        g_object_set(G_OBJECT(hlssink_), "playlist-location", playlistPath.c_str(), "location",
                     segmentPattern.c_str(), "target-duration", Config::HLS_SEGMENT_TARGET_DURATION_SEC, "max-files",
                     Config::HLS_PLAYLIST_MAX_FILES, nullptr);

        bus_ = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
        gst_bus_add_watch(bus_, (GstBusFunc)onGstMessage, this);

        loop_ = g_main_loop_new(nullptr, FALSE);
        gstThread_ = std::thread([this]() {
            gstRunning_.store(true, std::memory_order_release);
            g_main_loop_run(loop_);
            gstRunning_.store(false, std::memory_order_release);
        });

        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "Failed to start GStreamer pipeline" << std::endl;
            shutdownGstreamerPipeline();
            return false;
        }

        gstPipelineError_.store(false, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception in initializeGstreamerPipeline: " << e.what() << std::endl;
        shutdownGstreamerPipeline();
        return false;
    }
}

bool CameraManager::startVideoMode(int width, int height, int fps) {
    std::unique_lock<std::mutex> cameraLock(cameraOpsMutex_);
    if (!isInitialized_ || !camera_) {
        std::cerr << "Camera not initialized" << std::endl;
        return false;
    }

    try {
        if (isVideoMode_) {
            cameraLock.unlock();
            stopVideoMode();
            cameraLock.lock();
            if (!isInitialized_ || !camera_) {
                std::cerr << "Camera not initialized" << std::endl;
                return false;
            }
        }

        libcamera::StreamRoles roles = {libcamera::StreamRole::VideoRecording};
        currentConfig_ = camera_->generateConfiguration(roles);

        auto& streamConfig = currentConfig_->at(0);
        streamConfig.size = {static_cast<unsigned int>(width), static_cast<unsigned int>(height)};
        streamConfig.pixelFormat = libcamera::formats::RGB888;
        streamConfig.bufferCount = 4;

        auto configStatus = camera_->configure(currentConfig_.get());
        if (configStatus != 0) {
            std::cerr << "Failed to configure camera for video: " << configStatus << std::endl;
            return false;
        }

        allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);

        for (const auto& sc : *currentConfig_) {
            int ret = allocator_->allocate(sc.stream());
            if (ret < 0) {
                std::cerr << "Failed to allocate buffers" << std::endl;
                {
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    allocator_.reset();
                }
                return false;
            }
        }

        if (!initializeGstreamerPipeline(width, height, fps)) {
            std::cerr << "Failed to initialize GStreamer pipeline" << std::endl;
            ScopedUnlockUniqueLock un(cameraLock);
            rollbackFailedVideoStart();
            return false;
        }

        disconnectVideoRequestSlot();
#if SURFCAM_LIBCAMERA_LEGACY
        camera_->requestCompleted.connect(this, &CameraManager::requestComplete);
        videoRequestSlotConnected_ = true;
#else
        videoRequestCompletedConn_.emplace(
            camera_->requestCompleted.connect(this, &CameraManager::requestComplete));
#endif

        if (camera_->start() != 0) {
            std::cerr << "Failed to start camera for video" << std::endl;
            ScopedUnlockUniqueLock un(cameraLock);
            rollbackFailedVideoStart();
            return false;
        }

        libcamera::Stream* primaryStream = currentConfig_->at(0).stream();
        const auto& videoBuffers = allocator_->buffers(primaryStream);
        for (const auto& bufferHolder : videoBuffers) {
            if (!bufferHolder) {
                continue;
            }
            libcamera::FrameBuffer* buffer = bufferHolder.get();

            auto request = camera_->createRequest();
            if (!request) {
                std::cerr << "Failed to create request" << std::endl;
                ScopedUnlockUniqueLock un(cameraLock);
                rollbackFailedVideoStart();
                return false;
            }

            if (request->addBuffer(primaryStream, buffer) < 0) {
                std::cerr << "Failed to add buffer to request" << std::endl;
                ScopedUnlockUniqueLock un(cameraLock);
                rollbackFailedVideoStart();
                return false;
            }

            if (camera_->queueRequest(request.get()) != 0) {
                std::cerr << "Failed to queue request" << std::endl;
                ScopedUnlockUniqueLock un(cameraLock);
                rollbackFailedVideoStart();
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(requestMutex_);
                pendingRequests_.push_back(std::move(request));
            }
        }

        isVideoMode_ = true;
        captureActive_.store(true, std::memory_order_release);
        shuttingDown_.store(false, std::memory_order_release);

        std::cout << "Video mode started (HLS) at " << width << "x" << height << " @ " << fps << " fps" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception starting video mode: " << e.what() << std::endl;
        ScopedUnlockUniqueLock un(cameraLock);
        rollbackFailedVideoStart();
        return false;
    }
}

void CameraManager::bestEffortVideoTeardownWhileHoldingTeardownLock() {
    captureActive_.store(false, std::memory_order_release);
    shuttingDown_.store(false, std::memory_order_release);
    try {
        disconnectVideoRequestSlot();
        stopCameraIfRunning(camera_.get());
        {
            std::lock_guard<std::mutex> lock(cameraOpsMutex_);
            {
                std::lock_guard<std::mutex> rq(requestMutex_);
                pendingRequests_.clear();
                completedRequests_.clear();
            }
            shutdownGstreamerPipeline();
        }
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
        }
    } catch (const std::exception& e) {
        std::cerr << "Secondary error during video teardown recovery: " << e.what() << std::endl;
    }
    isVideoMode_ = false;
}

bool CameraManager::stopVideoMode() {
    std::lock_guard<std::mutex> tdLock(videoTeardownMutex_);
    if (!isVideoMode_) {
        return true;
    }

    try {
        captureActive_.store(false, std::memory_order_release);
        shuttingDown_.store(true, std::memory_order_release);

        disconnectVideoRequestSlot();

        stopCameraIfRunning(camera_.get());

        {
            std::lock_guard<std::mutex> lock(cameraOpsMutex_);
            {
                std::lock_guard<std::mutex> rq(requestMutex_);
                pendingRequests_.clear();
                completedRequests_.clear();
            }
            shutdownGstreamerPipeline();
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
        }

        shuttingDown_.store(false, std::memory_order_release);
        isVideoMode_ = false;
        std::cout << "Video mode stopped" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception stopping video mode: " << e.what() << std::endl;
        bestEffortVideoTeardownWhileHoldingTeardownLock();
        return false;
    }
}

void CameraManager::recycleVideoRequest(libcamera::Request* request) {
    request->reuse();
    if (!camera_) {
        return;
    }
#if !SURFCAM_LIBCAMERA_LEGACY
    if (!camera_->isRunning()) {
        return;
    }
#endif
    if (camera_->queueRequest(request) != 0) {
        std::cerr << "Failed to re-queue request after reuse" << std::endl;
    }
}

void CameraManager::requestComplete(libcamera::Request* request) {
    if (request->status() == libcamera::Request::RequestCancelled) {
        return;
    }

    const bool allowPipeline =
        captureActive_.load(std::memory_order_acquire) && !shuttingDown_.load(std::memory_order_acquire);
    if (!allowPipeline) {
        recycleVideoRequest(request);
        return;
    }

    std::lock_guard<std::mutex> pushLock(pipelinePushMutex_);

    const bool stillAllow =
        captureActive_.load(std::memory_order_acquire) && !shuttingDown_.load(std::memory_order_acquire);
    GstElement* appsrc = appsrc_;
    if (!stillAllow || !appsrc) {
        recycleVideoRequest(request);
        return;
    }

    auto buffers = request->buffers();
    for (auto& [stream, buffer] : buffers) {
        GstElement* asrc = appsrc_;
        if (!asrc) {
            continue;
        }

#if SURFCAM_LIBCAMERA_LEGACY
        LegacyMappedPlane mapped;
        if (!mapped.mapRead(buffer)) {
            continue;
        }
        const std::size_t nbytes = mapped.size();
        GstBuffer* gstBuffer = gst_buffer_new_allocate(nullptr, nbytes, nullptr);
        if (!gstBuffer) {
            continue;
        }
        GstMapInfo map;
        if (gst_buffer_map(gstBuffer, &map, GST_MAP_WRITE)) {
            memcpy(map.data, mapped.data(), nbytes);
            gst_buffer_unmap(gstBuffer, &map);
            GST_BUFFER_PTS(gstBuffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DTS(gstBuffer) = GST_CLOCK_TIME_NONE;
            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(asrc), gstBuffer);
            if (ret != GST_FLOW_OK) {
                std::cerr << "Failed to push buffer to GStreamer: " << ret << std::endl;
                gst_buffer_unref(gstBuffer);
                gstPipelineError_.store(true, std::memory_order_release);
            }
        } else {
            gst_buffer_unref(gstBuffer);
        }
#else
        auto planes = buffer->planes();
        if (planes.empty()) {
            continue;
        }
        auto span = planes[0].map();

        if (span) {
            GstBuffer* gstBuffer = gst_buffer_new_allocate(nullptr, span.size(), nullptr);

            if (gstBuffer) {
                GstMapInfo map;
                if (gst_buffer_map(gstBuffer, &map, GST_MAP_WRITE)) {
                    memcpy(map.data, span.data(), span.size());
                    gst_buffer_unmap(gstBuffer, &map);

                    GST_BUFFER_PTS(gstBuffer) = GST_CLOCK_TIME_NONE;
                    GST_BUFFER_DTS(gstBuffer) = GST_CLOCK_TIME_NONE;

                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(asrc), gstBuffer);
                    if (ret != GST_FLOW_OK) {
                        std::cerr << "Failed to push buffer to GStreamer: " << ret << std::endl;
                        gst_buffer_unref(gstBuffer);
                        gstPipelineError_.store(true, std::memory_order_release);
                    }
                } else {
                    gst_buffer_unref(gstBuffer);
                }
            }
            planes[0].unmap();
        }
#endif
    }

    recycleVideoRequest(request);
}

bool CameraManager::reinitialize() {
    std::cout << "Attempting to reinitialize camera..." << std::endl;

    std::unique_lock<std::mutex> cameraLock(cameraOpsMutex_);
    if (isVideoMode_) {
        cameraLock.unlock();
        stopVideoMode();
        cameraLock.lock();
    }

    try {
        if (camera_) {
            stopCameraIfRunning(camera_.get());
            camera_->release();
            camera_.reset();
        }

        if (cameraManager_) {
            cameraManager_->stop();
            cameraManager_.reset();
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
        }

        shutdownGstreamerPipeline();

        cameraManager_ = std::make_unique<libcamera::CameraManager>();
        cameraManager_->start();

        auto cameras = cameraManager_->cameras();
        if (cameras.empty()) {
            std::cerr << "No cameras available" << std::endl;
            snapshotsPaused_.store(true, std::memory_order_release);
            return false;
        }

        camera_ = cameras[0];

        if (camera_->acquire() != 0) {
            std::cerr << "Failed to acquire camera" << std::endl;
            snapshotsPaused_.store(true, std::memory_order_release);
            return false;
        }

        isInitialized_ = true;
        isVideoMode_ = false;
        captureActive_.store(false, std::memory_order_release);
        snapshotsPaused_.store(false, std::memory_order_release);
        std::cout << "Camera reinitialized: " << camera_->id() << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception reinitializing camera: " << e.what() << std::endl;
        snapshotsPaused_.store(true, std::memory_order_release);
        return false;
    }
}

void CameraManager::disconnectVideoRequestSlot() {
#if SURFCAM_LIBCAMERA_LEGACY
    if (camera_ && videoRequestSlotConnected_) {
        camera_->requestCompleted.disconnect(this);
        videoRequestSlotConnected_ = false;
    }
#else
    if (videoRequestCompletedConn_) {
        videoRequestCompletedConn_->disconnect();
        videoRequestCompletedConn_.reset();
    }
#endif
}

}  // namespace SurfCam
