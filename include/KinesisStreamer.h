#pragma once

#include <string>
#include <vector>
#include <memory>
#include <KinesisVideoProducer.h>
#include <DefaultDeviceInfoProvider.h>
#include <DefaultCallbackProvider.h>

#include "ApiClient.h" // For AwsCredentials

namespace SurfCam {

class KinesisStreamer {
public:
    KinesisStreamer(const std::string& streamName, const std::string& region);
    ~KinesisStreamer();

    bool initialize(const AwsCredentials& credentials);
    bool sendFrameFragment(const std::vector<uint8_t>& frameData, 
                           long long timestamp);
    void shutdown();
    size_t getMaxRecommendedFragmentSize() const {
    // Limit fragment size based on bandwidth and memory constraints
    // Return a reasonable size limit (e.g., 1MB)
    return 1024 * 1024; // 1MB
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