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
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <opencv2/opencv.hpp>
#include <libcamera/libcamera.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

namespace SurfCam {

CameraManager::CameraManager() : isInitialized_(false), isVideoMode_(false) {
        gst_init(nullptr, nullptr);
}

CameraManager::~CameraManager() {
    try {
        // Stop video mode if active
        if (isVideoMode_) {
            stopVideoMode();
        }
        
        // Clean up camera
        if (camera_) {
            if (camera_->isRunning()) {
                camera_->stop();
            }
            
            // Release camera
            camera_->release();
        }
        
        // Clean up allocator (which will free buffers)
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
            buffers_.clear();
        }
        
        // Stop and reset camera manager
        if (cameraManager_) {
            cameraManager_->stop();
        }
        cameraManager_.reset();
        
        // Clean up GStreamer
        shutdownGstreamerPipeline();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in CameraManager destructor: " << e.what() << std::endl;
    }
}

bool CameraManager::initialize() {
    try {
        // Create camera manager
        cameraManager_ = std::make_unique<libcamera::CameraManager>();
        
        // Start the camera manager
        cameraManager_->start();
        
        // Get list of cameras
        auto cameras = cameraManager_->cameras();
        if (cameras.empty()) {
            std::cerr << "No cameras available" << std::endl;
            return false;
        }
        
        // Use the first camera
        camera_ = cameras[0];
        
        // Acquire the camera
        if (camera_->acquire() != 0) {
            std::cerr << "Failed to acquire camera" << std::endl;
            return false;
        }
        
        isInitialized_ = true;
        std::cout << "Camera initialized: " << camera_->id() << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception initializing camera: " << e.what() << std::endl;
        return false;
    }
}

bool CameraManager::takePicture(const std::string& outputPath) {
    if (!isInitialized_ || !camera_) {
        std::cerr << "Camera not initialized" << std::endl;
        return false;
    }
    
    try {
        // If in video mode, switch to still mode
        if (isVideoMode_) {
            stopVideoMode();
        }
        
        // Configure camera for still capture
        libcamera::StreamRoles roles = {libcamera::StreamRole::StillCapture};
        auto config = camera_->generateConfiguration(roles);
        
        // Configure stream format (4:3 aspect ratio for stills)
        auto& streamConfig = config->at(0);
        streamConfig.size = {2592, 1944};
        streamConfig.pixelFormat = libcamera::formats::MJPEG;
        streamConfig.bufferCount = 1;
        
        // Apply configuration
        auto configStatus = camera_->configure(config.get());
        if (configStatus != 0) {
            std::cerr << "Failed to configure camera: " << configStatus << std::endl;
            return false;
        }
        
        // Set up a buffer for the request
        libcamera::FrameBufferAllocator allocator(camera_);
        if (allocator.allocate(streamConfig.stream()) < 0) {
            std::cerr << "Failed to allocate buffers" << std::endl;
            return false;
        }

        // Create request
        auto request = camera_->createRequest();
        if (!request) {
            std::cerr << "Failed to create request" << std::endl;
            return false;
        }
        
        const auto &buffers = allocator.buffers(streamConfig.stream());
        if (buffers.empty()) {
            std::cerr << "No buffers allocated" << std::endl;
            return false;
        }
        
        // Add the buffer to the request
        if (request->addBuffer(streamConfig.stream(), buffers[0]) < 0) {
            std::cerr << "Failed to add buffer to request" << std::endl;
            return false;
        }
        
        // Setup completion callback with mutex/condition variable
        std::mutex completionMutex;
        std::condition_variable completionCV;
        bool completed = false;
        
        // Set up request completion handler
        camera_->requestCompleted.connect([&](libcamera::Request *request) {
            std::lock_guard<std::mutex> lock(completionMutex);
            completed = true;
            completionCV.notify_one();
        });
        
        // Start camera
        if (camera_->start() != 0) {
            std::cerr << "Failed to start camera" << std::endl;
            return false;
        }
        
        // Wait for camera to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Queue request
        if (camera_->queueRequest(request.get()) != 0) {
            std::cerr << "Failed to queue request" << std::endl;
            camera_->stop();
            return false;
        }
        
        // Wait for completion using proper synchronization
        {
            std::unique_lock<std::mutex> lock(completionMutex);
            if (!completionCV.wait_for(lock, std::chrono::seconds(5), [&]{ return completed; })) {
                std::cerr << "Timeout waiting for image capture" << std::endl;
                camera_->stop();
                return false;
            }
        }
        
        // Get the buffer
        auto buffer = request->buffers().begin()->second;
        auto span = buffer->planes()[0].map();
        if (!span) {
            std::cerr << "Failed to map buffer" << std::endl;
            camera_->stop();
            return false;
        }
        
        // Save the actual image data to file
        std::ofstream output(outputPath, std::ios::binary);
        if (!output.is_open()) {
            std::cerr << "Failed to open output file" << std::endl;
            camera_->stop();
            return false;
        }
        
        // Write the actual buffer data
        output.write(static_cast<const char*>(span.data()), span.size());
        output.close();
        
        // Unmap buffer
        buffer->planes()[0].unmap();
        
        // Stop camera
        camera_->stop();
        
        std::cout << "Image captured and saved to: " << outputPath << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception taking picture: " << e.what() << std::endl;
        if (camera_ && camera_->isRunning())
            camera_->stop();
        return false;
    }
}


void CameraManager::shutdownGstreamerPipeline() {
    if (pipeline_) {
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
        appsink_ = nullptr;
    }
}

// Static callback for GStreamer bus messages
gboolean CameraManager::onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data) {
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

bool CameraManager::initializeGstreamerPipeline(int width, int height, int fps) {
    try {
        // Create a new pipeline
        pipeline_ = gst_pipeline_new("camera-pipeline");
        if (!pipeline_) {
            std::cerr << "Failed to create GStreamer pipeline" << std::endl;
            return false;
        }

        // Create the elements
        appsrc_ = gst_element_factory_make("appsrc", "camera-source");
        auto convert = gst_element_factory_make("videoconvert", "converter");
        
        // Try multiple encoders in order of preference for Pi Zero
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
            return false;
        }
        
        auto h264parse = gst_element_factory_make("h264parse", "parser");
        auto queue = gst_element_factory_make("queue", "queue");
        appsink_ = gst_element_factory_make("appsink", "app-sink");

        // Check elements
        if (!appsrc_ || !convert || !h264enc || !h264parse || !queue || !appsink_) {
            std::cerr << "Failed to create GStreamer elements" << std::endl;
            return false;
        }

        // Add elements to the pipeline
        gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, h264enc, h264parse, queue, appsink_, nullptr);

        // Link elements
        if (!gst_element_link_many(appsrc_, convert, h264enc, h264parse, queue, appsink_, nullptr)) {
            std::cerr << "Failed to link GStreamer elements" << std::endl;
            return false;
        }

        // Configure appsrc
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                          "format", G_TYPE_STRING, "RGB",
                                          "width", G_TYPE_INT, width,
                                          "height", G_TYPE_INT, height,
                                          "framerate", GST_TYPE_FRACTION, fps, 1,
                                          nullptr);
        g_object_set(G_OBJECT(appsrc_),
                     "caps", caps,
                     "format", GST_FORMAT_TIME,
                     "is-live", TRUE,
                     "do-timestamp", TRUE,
                     nullptr);
        gst_caps_unref(caps);

        // Configure encoder with lower bitrate for Pi Zero
        g_object_set(G_OBJECT(h264enc),
           "tune", 4, // zerolatency
           "speed-preset", 1, // ultrafast
           "bitrate", Config::GSTREAMER_BITRATE / 1000,
           "key-int-max", fps,
           "byte-stream", TRUE,
           "threads", 1, // Single thread for Pi Zero
           nullptr);

        // Configure queue with smaller size for Pi Zero
        g_object_set(G_OBJECT(queue),
                     "max-size-buffers", 2,
                     "max-size-bytes", 0,
                     "max-size-time", 0,
                     nullptr);

        // Configure appsink
        g_object_set(G_OBJECT(appsink_),
                     "emit-signals", TRUE,
                     "sync", FALSE,
                     nullptr);

        // Set callbacks
        g_signal_connect(appsink_, "new-sample", G_CALLBACK(onNewEncodedBuffer), this);

        // Setup bus watch
        bus_ = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
        gst_bus_add_watch(bus_, (GstBusFunc)onGstMessage, this);

        // Start GStreamer main loop
        loop_ = g_main_loop_new(nullptr, FALSE);
        gstThread_ = std::thread([this]() {
            gstRunning_ = true;
            g_main_loop_run(loop_);
            gstRunning_ = false;
        });

        // Start the pipeline
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "Failed to start GStreamer pipeline" << std::endl;
            return false;
        }

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in initializeGstreamerPipeline: " << e.what() << std::endl;
        return false;
    }
}


// Static callback for GStreamer bus messages
gboolean CameraManager::onGstMessage(GstBus* bus, GstMessage* msg, gpointer user_data) {
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


void CameraManager::onNewEncodedBuffer(GstElement* sink, gpointer user_data) {
    CameraManager* manager = static_cast<CameraManager*>(user_data);
    GstSample* sample = nullptr;
    
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) {
        return;
    }
    
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return;
    }
    
    // Check if this is a keyframe
    bool isKeyframe = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT) ? false : true;
    
    // Map the buffer
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        // Create frame data with proper timestamp
        FrameData frame;
        frame.data.resize(map.size);
        memcpy(frame.data.data(), map.data, map.size);
        frame.timestamp = GST_BUFFER_PTS(buffer);
        frame.keyFrame = isKeyframe;
        
        // Add to frame queue with size limit to prevent memory issues
        {
            std::lock_guard<std::mutex> lock(manager->encodedFramesMutex_);
            
            // If queue is full, remove oldest frame
            if (manager->encodedFrames_.size() >= manager->maxBufferedFrames_) {
                manager->encodedFrames_.pop();
            }
            
            // Add new frame
            manager->encodedFrames_.push(frame);
            manager->encodedFramesCV_.notify_one();
        }
        
        // Unmap buffer
        gst_buffer_unmap(buffer, &map);
    }
    
    // Always clean up the sample
    gst_sample_unref(sample);
}

bool CameraManager::startVideoMode(int width, int height, int fps) {
    if (!isInitialized_ || !camera_) {
        std::cerr << "Camera not initialized" << std::endl;
        return false;
    }
    
    try {
        // If already in video mode, stop first
        if (isVideoMode_) {
            stopVideoMode();
        }
        
        // Configure camera for video capture
        libcamera::StreamRoles roles = {libcamera::StreamRole::VideoRecording};
        currentConfig_ = camera_->generateConfiguration(roles);
        
        // Configure stream format
        auto& streamConfig = currentConfig_->at(0);
        streamConfig.size = {width, height};
        streamConfig.pixelFormat = libcamera::formats::RGB888; // Use RGB for GStreamer
        streamConfig.bufferCount = 4; // Use multiple buffers for smooth streaming
        
        // Apply configuration
        auto configStatus = camera_->configure(currentConfig_.get());
        if (configStatus != 0) {
            std::cerr << "Failed to configure camera for video: " << configStatus << std::endl;
            return false;
        }
        
        // Create buffer allocator
        allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
        
        // Allocate buffers for the stream
        for (const auto &streamConfig : *currentConfig_) {
            int ret = allocator_->allocate(streamConfig.stream());
            if (ret < 0) {
                std::cerr << "Failed to allocate buffers" << std::endl;
                return false;
            }
            
            // Save allocated buffers
            const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers = 
                allocator_->buffers(streamConfig.stream());
            
            std::lock_guard<std::mutex> lock(bufferMutex_);
            for (unsigned int i = 0; i < buffers.size(); ++i) {
                buffers_.push_back(std::move(buffers[i]));
            }
        }
        
        // Initialize GStreamer pipeline
        if (!initializeGstreamerPipeline(width, height, fps)) {
            std::cerr << "Failed to initialize GStreamer pipeline" << std::endl;
            return false;
        }
        
        // Set up request completion handler
        camera_->requestCompleted.connect(this, &CameraManager::requestComplete);
        
        // Start camera
        if (camera_->start() != 0) {
            std::cerr << "Failed to start camera for video" << std::endl;
            return false;
        }
        
        // Create and queue requests for each buffer
        for (const auto& buffer : buffers_) {
            auto request = camera_->createRequest();
            if (!request) {
                std::cerr << "Failed to create request" << std::endl;
                camera_->stop();
                return false;
            }
            
            // Add buffer to the request
            const auto stream = currentConfig_->at(0).stream();
            if (request->addBuffer(stream, buffer.get()) < 0) {
                std::cerr << "Failed to add buffer to request" << std::endl;
                camera_->stop();
                return false;
            }
            
            // Queue the request
            if (camera_->queueRequest(request.get()) != 0) {
                std::cerr << "Failed to queue request" << std::endl;
                camera_->stop();
                return false;
            }
            
            // Store the request
            {
                std::lock_guard<std::mutex> lock(requestMutex_);
                pendingRequests_.push_back(std::move(request));
            }
        }
        
        isVideoMode_ = true;
        captureActive_ = true;
        
        std::cout << "Video mode started at " << width << "x" << height 
                  << " @ " << fps << " fps" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception starting video mode: " << e.what() << std::endl;
        return false;
    }
}

bool CameraManager::stopVideoMode() {
    if (!isVideoMode_) {
        return true;
    }
    
    try {
        // Stop capture
        captureActive_ = false;
        
        // Stop camera
        if (camera_ && camera_->isRunning()) {
            camera_->stop();
        }
        
        // Clean up requests
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            pendingRequests_.clear();
            completedRequests_.clear();
        }
        
        // Shut down GStreamer pipeline
        shutdownGstreamerPipeline();
        
        // Clean up allocator and buffers
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
            buffers_.clear();
        }
        
        // Clear frame queue
        {
            std::lock_guard<std::mutex> lock(encodedFramesMutex_);
            std::queue<FrameData> empty;
            std::swap(encodedFrames_, empty);
        }
        
        isVideoMode_ = false;
        std::cout << "Video mode stopped" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception stopping video mode: " << e.what() << std::endl;
        return false;
    }
}

bool CameraManager::getEncodedFrame(FrameData& frameData) {
    std::unique_lock<std::mutex> lock(encodedFramesMutex_);
    
    // Wait for up to 1 second for a frame to be available
    bool frameAvailable = encodedFramesCV_.wait_for(lock, std::chrono::seconds(1),
        [this]() { return !encodedFrames_.empty(); });
    
    if (!frameAvailable) {
        return false; // Timeout waiting for frame
    }
    
    // Get the next frame
    frameData = encodedFrames_.front();
    encodedFrames_.pop();
    
    return true;
}

void CameraManager::requestComplete(libcamera::Request* request) {
    if (request->status() == libcamera::Request::RequestCancelled) {
        return;
    }
    
    // Process the request - feed frame to GStreamer
    auto buffers = request->buffers();
    for (auto& [stream, buffer] : buffers) {
        if (captureActive_) {
            // Map the buffer
            auto planes = buffer->planes();
            auto span = planes[0].map();
            
            if (span) {
                // Create GStreamer buffer and push to appsrc
                if (appsrc_) {
                    GstBuffer* gstBuffer = gst_buffer_new_allocate(nullptr, span.size(), nullptr);
                    
                    if (gstBuffer) {
                        GstMapInfo map;
                        if (gst_buffer_map(gstBuffer, &map, GST_MAP_WRITE)) {
                            // Copy the frame data
                            memcpy(map.data, span.data(), span.size());
                            gst_buffer_unmap(gstBuffer, &map);
                            
                            // Add timestamp
                            GST_BUFFER_PTS(gstBuffer) = GST_CLOCK_TIME_NONE;
                            GST_BUFFER_DTS(gstBuffer) = GST_CLOCK_TIME_NONE;
                            
                            // Push the buffer to GStreamer
                            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), gstBuffer);
                            if (ret != GST_FLOW_OK) {
                                std::cerr << "Failed to push buffer to GStreamer: " << ret << std::endl;
                            }
                        } else {
                            gst_buffer_unref(gstBuffer);
                        }
                    }
                }
                
                // Unmap the buffer
                planes[0].unmap();
            }
            
            // Re-queue the request with the same buffer
            request->reuse();
            camera_->queueRequest(request);
        }
    }
}

// Add a new method for resilient camera reinitialization

bool CameraManager::reinitialize() {
    std::cout << "Attempting to reinitialize camera..." << std::endl;
    
    try {
        // First, clean up existing camera if any
        if (camera_) {
            if (camera_->isRunning()) {
                camera_->stop();
            }
            camera_->release();
            camera_.reset();
        }
        
        // Reset camera manager
        if (cameraManager_) {
            cameraManager_->stop();
            cameraManager_.reset();
        }
        
        // Clean up other resources
        {
            std::lock_guard<std::mutex> lock(bufferMutex_);
            allocator_.reset();
            buffers_.clear();
        }
        
        // Clean up GStreamer
        shutdownGstreamerPipeline();
        
        // Create new camera manager
        cameraManager_ = std::make_unique<libcamera::CameraManager>();
        
        // Start the camera manager
        cameraManager_->start();
        
        // Get list of cameras
        auto cameras = cameraManager_->cameras();
        if (cameras.empty()) {
            std::cerr << "No cameras available" << std::endl;
            return false;
        }
        
        // Use the first camera
        camera_ = cameras[0];
        
        // Acquire the camera
        if (camera_->acquire() != 0) {
            std::cerr << "Failed to acquire camera" << std::endl;
            return false;
        }
        
        isInitialized_ = true;
        isVideoMode_ = false;
        captureActive_ = false;
        std::cout << "Camera reinitialized: " << camera_->id() << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception reinitializing camera: " << e.what() << std::endl;
        return false;
    }
}

}  // namespace SurfCam