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

#include "HlsUploader.h"
#include "Config.h"
#include "HlsUploadPolicy.h"
#include "IHlsPresignClient.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace SurfCam {

namespace {

bool fileSizeStable(const std::filesystem::path& p) {
    std::error_code ec;
    const auto a = std::filesystem::file_size(p, ec);
    if (ec || a == 0) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto b = std::filesystem::file_size(p, ec);
    if (ec || b != a) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto c = std::filesystem::file_size(p, ec);
    return !ec && c == b;
}

}  // namespace

void HlsUploader::resetSession() {
    uploadedSegments_.clear();
    uploadedSegmentOrder_.clear();
    lastPlaylistWrite_.reset();
}

void HlsUploader::onSegmentUploaded(const std::string& segmentFilename) {
    HlsUploadPolicy::recordSegmentUploaded(segmentFilename, uploadedSegments_, uploadedSegmentOrder_);
}

bool HlsUploader::allPlaylistSegmentsUploaded(const std::filesystem::path& playlistPath) const {
    std::ifstream in(playlistPath);
    if (!in) {
        return false;
    }
    return HlsUploadPolicy::playlistAllTsLinesUploaded(in, uploadedSegments_);
}

bool HlsUploader::pollAndUpload(IHlsPresignClient& api, const std::string& spotId, const std::string& dirPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dirPath, ec)) {
        return false;
    }

    std::vector<fs::path> tsFiles;
    for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".ts") {
            tsFiles.push_back(entry.path());
        }
    }
    std::sort(tsFiles.begin(), tsFiles.end());

    for (const auto& p : tsFiles) {
        const std::string name = p.filename().string();
        if (uploadedSegments_.count(name) != 0) {
            continue;
        }
        if (!fileSizeStable(p)) {
            continue;
        }
        const std::string key = HlsUploadPolicy::s3KeyForFile(spotId, name);
        if (api.uploadLocalFileWithPresign(key, "video/mp2t", p.string())) {
            onSegmentUploaded(name);
        }
    }

    const fs::path playlist = fs::path(dirPath) / Config::HLS_PLAYLIST_NAME;
    if (!fs::exists(playlist)) {
        return true;
    }

    const auto mtime = fs::last_write_time(playlist, ec);
    if (ec) {
        return true;
    }
    if (lastPlaylistWrite_.has_value() && *lastPlaylistWrite_ == mtime) {
        return true;
    }
    if (!fileSizeStable(playlist)) {
        return true;
    }

    if (!allPlaylistSegmentsUploaded(playlist)) {
        return true;
    }

    const std::string pk = HlsUploadPolicy::s3KeyForFile(spotId, Config::HLS_PLAYLIST_NAME);
    if (api.uploadLocalFileWithPresign(pk, "application/vnd.apple.mpegurl", playlist.string())) {
        lastPlaylistWrite_ = mtime;
    }

    return true;
}

}  // namespace SurfCam
