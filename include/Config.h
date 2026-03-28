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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace SurfCam {

struct Config {
    // API
    inline static const std::string API_ENDPOINT = "https://treblesurf.com/api";
    inline static const std::string API_KEY = []() {
        const char* env_key = std::getenv("API_KEY");
        if (!env_key || strlen(env_key) == 0) {
            std::cerr << "ERROR: API_KEY environment variable is not set!" << std::endl;
            std::cerr << "Please set it before running the application." << std::endl;
            exit(1);
        }
        return std::string(env_key);
    }();
    inline static const std::string SPOT_ID = []() {
        const char* env_spot_id = std::getenv("SPOT_ID");
        if (!env_spot_id || strlen(env_spot_id) == 0) {
            std::cerr << "ERROR: SPOT_ID environment variable is not set!" << std::endl;
            std::cerr << "Please set it before running the application." << std::endl;
            exit(1);
        }
        return std::string(env_spot_id);
    }();
    inline static const int REQUEST_TIMEOUT = 10;

    // HLS (local staging before S3 via presigned PUT)
    inline static const std::string HLS_OUTPUT_DIR = "/tmp/surfcam-hls";
    inline static const std::string HLS_PLAYLIST_NAME = "index.m3u8";
    inline static const int HLS_SEGMENT_TARGET_DURATION_SEC = 5;
    inline static const int HLS_PLAYLIST_MAX_FILES = 8;
    inline static const std::string HLS_PRESIGN_PATH = "/hls/presign";

    // Timing
    inline static const std::chrono::seconds SNAPSHOT_INTERVAL{30};
    inline static const std::chrono::seconds STREAM_CHECK_INTERVAL{5};
    inline static const std::chrono::seconds STREAM_TIMEOUT{30};
    inline static const std::chrono::milliseconds HLS_UPLOAD_POLL{400};

    // Camera
    inline static const int CAMERA_WIDTH = 1280;
    inline static const int CAMERA_HEIGHT = 720;
    inline static const int STREAM_FPS = 15;
    inline static const std::string IMAGE_PATH = "/home/ryanpatton/image.jpg";

    // Pi Zero tuning (GStreamer encoder)
    inline static const int GSTREAMER_BITRATE = 400000;  // 400Kbps
};

}  // namespace SurfCam
