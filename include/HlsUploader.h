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

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

namespace SurfCam {

class ApiClient;

/// Watches the local HLS directory and uploads new .ts segments and the playlist via presigned PUT.
class HlsUploader {
public:
    void resetSession();

    /// Upload any new stable segments, then the playlist if it changed. Returns false if dir missing.
    bool pollAndUpload(ApiClient& api, const std::string& spotId, const std::string& dirPath);

private:
    std::unordered_set<std::string> uploadedSegments_;
    std::optional<std::filesystem::file_time_type> lastPlaylistWrite_;
};

}  // namespace SurfCam
