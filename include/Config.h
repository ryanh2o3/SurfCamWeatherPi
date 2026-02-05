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

    // Timing
    inline static const std::chrono::seconds SNAPSHOT_INTERVAL{30};
    inline static const std::chrono::seconds STREAM_CHECK_INTERVAL{5};
    inline static const std::chrono::seconds STREAM_TIMEOUT{30};
    inline static const int CREDENTIAL_REFRESH_SECONDS = 150; // Refresh before 180s KVS token expiry

    // AWS
    inline static const std::string AWS_REGION = "eu-west-1";
    inline static const std::string KINESIS_STREAM_NAME = "treblesurf-webcam";

    // Camera
    inline static const int CAMERA_WIDTH = 1280;
    inline static const int CAMERA_HEIGHT = 720;
    inline static const int STREAM_FPS = 15;
    inline static const std::string IMAGE_PATH = "/home/ryanpatton/image.jpg";

    // Pi Zero tuning
    inline static const int MAX_FRAGMENT_SIZE = 256 * 1024;       // 256KB fragments
    inline static const int KVS_STORAGE_SIZE  = 32 * 1024 * 1024; // 32MB internal buffer
    inline static const int GSTREAMER_BITRATE = 400000;            // 400Kbps
};

}  // namespace SurfCam