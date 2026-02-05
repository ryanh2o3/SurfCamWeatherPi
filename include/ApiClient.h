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
#include <mutex>
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