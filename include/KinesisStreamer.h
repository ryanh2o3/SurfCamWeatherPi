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

#include <string>
#include <vector>
#include <memory>
#include <KinesisVideoProducer.h>
#include <DefaultDeviceInfoProvider.h>
#include <DefaultCallbackProvider.h>

#include "ApiClient.h" // For AwsCredentials
#include "Config.h"

namespace SurfCam {

class KinesisStreamer {
public:
    KinesisStreamer(const std::string& streamName, const std::string& region);
    ~KinesisStreamer();

    bool initialize(const AwsCredentials& credentials);
    bool sendFrameFragment(const std::vector<uint8_t>& frameData,
                           long long timestamp, bool isKeyFrame);
    void shutdown();
    size_t getMaxRecommendedFragmentSize() const {
        return Config::MAX_FRAGMENT_SIZE;
    }

private:
    std::string streamName_;
    std::string region_;
    std::unique_ptr<com::amazonaws::kinesis::video::DefaultCallbackProvider> callbackProvider_;
    std::unique_ptr<com::amazonaws::kinesis::video::DeviceInfoProvider> deviceInfoProvider_;
    std::shared_ptr<com::amazonaws::kinesis::video::KinesisVideoProducer> kinesisVideoProducer_;
    std::shared_ptr<com::amazonaws::kinesis::video::Stream> videoStream_;
    bool isInitialized_{false};
    uint64_t currentTimestamp_{0};
};

}  // namespace SurfCam