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

#include "KinesisStreamer.h"
#include "Config.h"
#include <iostream>
#include <thread>
#include <StreamDefinition.h>
#include <Logger.h>

using namespace com::amazonaws::kinesis::video;

namespace SurfCam {

KinesisStreamer::KinesisStreamer(const std::string& streamName, const std::string& region)
    : streamName_(streamName), region_(region), isInitialized_(false), currentTimestamp_(0) {
    // Set default log level
    SET_LOGGER_LOG_LEVEL(Logger::LOG_LEVEL_INFO);
}

KinesisStreamer::~KinesisStreamer() {
    shutdown();
}

bool KinesisStreamer::initialize(const AwsCredentials& credentials) {
    try {
        // Create device info provider
        auto deviceInfo = DefaultDeviceInfoProvider::createDefault();
        deviceInfoProvider_ = std::move(deviceInfo);
        
        // Set storage size - reduced for Pi Zero
        auto storageInfo = deviceInfoProvider_->getStorageInfo();
        storageInfo.storageSize = Config::KVS_STORAGE_SIZE; // 32MB instead of 128MB
        deviceInfoProvider_->setStorageInfo(storageInfo);

        // Create credential provider
        auto clientConfig = std::make_unique<ClientConfiguration>();
        clientConfig->region = region_;
        
        // Increase timeout for slower Pi Zero network performance
        clientConfig->connectTimeoutMs = 5000;  // 5 second connect timeout
        clientConfig->requestTimeoutMs = 10000; // 10 second request timeout
        
        callbackProvider_ = std::unique_ptr<DefaultCallbackProvider>(
            new DefaultCallbackProvider(
                std::move(clientConfig),
                credentials.accessKey,
                credentials.secretKey,
                credentials.sessionToken,
                std::chrono::seconds(180)
            )
        );

        // Create Kinesis Video client
        kinesisVideoProducer_ = KinesisVideoProducer::createSync(
            std::move(deviceInfoProvider_),
            std::move(callbackProvider_)
        );
        
        // Define stream with reduced parameters for Pi Zero
        auto streamDefinition = std::make_unique<StreamDefinition>(
            streamName_.c_str(),
            hours(2),               // Retention period
            nullptr,                // No tags
            "",                     // No kms key ID
            STREAMING_TYPE_REALTIME,
            "video/h264",           // Content type
            milliseconds(4000),     // Max latency - increased for Pi Zero
            seconds(2),             // Fragment duration
            milliseconds(1000),     // Timecode scale
            true,                   // Key frame fragmentation
            true,                   // Frame timecodes
            true,                   // Absolute fragment times
            true,                   // Fragment acks
            true,                   // Restart on error
            false,                  // Don't recalculate metrics for Pi Zero
            0,                      // No framerate
            milliseconds(5000),     // Increased connection staleness detection
            milliseconds(2000),     // Increased connection staleness timeout
            25,                     // Codec ID
            "V_MPEG4/ISO/AVC",      // Track name
            nullptr,                // No codec private data
            0,                      // No codec private data size
            MKV_TRACK_INFO_TYPE_VIDEO // Track type
        );
        
        // Create stream
        videoStream_ = kinesisVideoProducer_->createStreamSync(std::move(streamDefinition));
        
        isInitialized_ = true;
        std::cout << "KinesisStreamer initialized with stream: " << streamName_ << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception initializing KinesisStreamer: " << e.what() << std::endl;
        return false;
    }
}

bool KinesisStreamer::sendFrameFragment(const std::vector<uint8_t>& frameData, long long timestamp) {
    if (!isInitialized_ || !videoStream_) {
        std::cerr << "KinesisStreamer not initialized" << std::endl;
        return false;
    }
    
    try {
        // Update timestamp if provided, otherwise use incremental
        if (timestamp > 0) {
            currentTimestamp_ = timestamp;
        } else {
            // If no timestamp provided, increment by reasonable default
            currentTimestamp_ += 33 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND; // ~30fps
        }
        
        std::vector<uint8_t> frameCopy(frameData);
        Frame frame;
        frame.frameData = frameCopy.data();
        frame.size = frameCopy.size();
        frame.trackId = DEFAULT_TRACK_ID;
        frame.duration = 0;
        frame.decodingTs = currentTimestamp_; 
        frame.presentationTs = currentTimestamp_;
        frame.flags = FRAME_FLAG_KEY_FRAME;

        const int MAX_RETRIES = 3;
        int retryCount = 0;
        bool success = false;

        while (!success && retryCount < MAX_RETRIES) {
            try {
                videoStream_->putFrame(frame);
                success = true;
            }
            catch (const std::exception& e) {
                retryCount++;
                std::cerr << "Error sending frame (attempt " << retryCount << "): " << e.what() << std::endl;
                if (retryCount < MAX_RETRIES) {
                    // Exponential backoff: 100ms, 200ms, 400ms, etc.
                    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << (retryCount - 1))));
                } else {
                    throw; // Re-throw after max retries
                }
            }
        }
        
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Exception sending frame: " << e.what() << std::endl;
        return false;
    }
}

void KinesisStreamer::shutdown() {
    if (!isInitialized_) {
        return;
    }
    
    if (videoStream_) {
        // Stop the stream
        videoStream_->stopSync();
        videoStream_.reset();
    }
    
    if (kinesisVideoProducer_) {
        // Free the producer
        kinesisVideoProducer_.reset();
    }
    
    isInitialized_ = false;
}

}  // namespace SurfCam