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

namespace SurfCam {

/// HTTP client; libcurl is initialized once in main (R8).
class ApiClient {
public:
    explicit ApiClient(std::string apiEndpoint, std::string apiKey);
    ~ApiClient() = default;

    bool uploadSnapshot(const std::string& imagePath, const std::string& spotId);
    /// True only when the API JSON says stream is requested (timeout/grace handled in main — R9).
    bool isStreamingRequested(const std::string& spotId);

    /// POST /hls/presign then PUT file bytes to the returned URL (S3 presigned PUT).
    bool uploadLocalFileWithPresign(const std::string& objectKey, const std::string& contentType,
                                    const std::string& filePath);

private:
    std::string apiEndpoint_;
    std::string apiKey_;
};

}  // namespace SurfCam
