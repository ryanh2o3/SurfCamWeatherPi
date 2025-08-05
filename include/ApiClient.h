#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace SurfCam {

struct AwsCredentials {
    std::string accessKey;
    std::string secretKey;
    std::string sessionToken;
};

class ApiClient {
public:
    ApiClient(const std::string& apiEndpoint, const std::string& apiKey);
    ~ApiClient();

    bool uploadSnapshot(const std::string& imagePath, const std::string& spotId);
    bool isStreamingRequested(const std::string& spotId);
    std::optional<AwsCredentials> getStreamingCredentials();

private:
    std::string apiEndpoint_;
    std::string apiKey_;
    long lastStreamRequestTime_{0};
    std::mutex apiMutex_;
};

}  // namespace SurfCam