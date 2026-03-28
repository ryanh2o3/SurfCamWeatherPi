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

#include <chrono>
#include <iosfwd>
#include <string>

namespace SurfCam {

/// Runtime configuration (Phase 3): load once from the environment in main — no exit() in static init (R11).
struct Config {
    static std::string API_KEY;
    static std::string SPOT_ID;
    /// Local JPEG path; env `SNAPSHOT_PATH`, default `/tmp/surfcam-snapshot.jpg`.
    static std::string SNAPSHOT_PATH;

    static bool loadFromEnvironment(std::ostream& err);

    static constexpr const char* API_ENDPOINT = "https://treblesurf.com/api";
    static constexpr int REQUEST_TIMEOUT = 10;

    // HLS (local staging before S3 via presigned PUT)
    inline static const std::string HLS_OUTPUT_DIR{"/tmp/surfcam-hls"};
    inline static const std::string HLS_PLAYLIST_NAME{"index.m3u8"};
    static constexpr int HLS_SEGMENT_TARGET_DURATION_SEC = 5;
    static constexpr int HLS_PLAYLIST_MAX_FILES = 8;
    static constexpr const char* HLS_PRESIGN_PATH = "/hls/presign";

    // Timing
    static constexpr std::chrono::seconds SNAPSHOT_INTERVAL{30};
    static constexpr std::chrono::seconds STREAM_CHECK_INTERVAL{5};
    static constexpr std::chrono::seconds STREAM_TIMEOUT{30};
    static constexpr std::chrono::milliseconds HLS_UPLOAD_POLL{400};

    // Camera
    static constexpr int CAMERA_WIDTH = 1280;
    static constexpr int CAMERA_HEIGHT = 720;
    static constexpr int STREAM_FPS = 15;

    // Pi Zero tuning (GStreamer encoder)
    static constexpr int GSTREAMER_BITRATE = 400000;  // 400Kbps
};

}  // namespace SurfCam
