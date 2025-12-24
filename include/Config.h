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
    static const std::string API_ENDPOINT;
    static const std::string API_KEY;
    static const std::chrono::seconds SNAPSHOT_INTERVAL;
    static const std::chrono::seconds STREAM_CHECK_INTERVAL;
    static const std::string AWS_REGION;
    static const std::string KINESIS_STREAM_NAME;
    static const std::string IMAGE_PATH;
    static const std::chrono::seconds STREAM_TIMEOUT;
    
    static const int CAMERA_WIDTH;
    static const int CAMERA_HEIGHT;
    static const int STREAM_FPS;

    static const int MAX_FRAGMENT_SIZE;
    static const int KVS_STORAGE_SIZE;
    static const int GSTREAMER_BITRATE;
    static const std::string SPOT_ID;
    static const int REQUEST_TIMEOUT;
};

const std::string Config::API_ENDPOINT = "https://treblesurf.com/api";
const std::string Config::API_KEY = []() {
    const char* env_key = std::getenv("API_KEY");
    if (!env_key || strlen(env_key) == 0) {
        std::cerr << "ERROR: API_KEY environment variable is not set!" << std::endl;
        std::cerr << "Please set it before running the application." << std::endl;
        exit(1);
    }
    return std::string(env_key);
}();
const std::chrono::seconds Config::SNAPSHOT_INTERVAL{30};
const std::chrono::seconds Config::STREAM_CHECK_INTERVAL{5};
const std::string Config::AWS_REGION = "eu-west-1";
const std::string Config::KINESIS_STREAM_NAME = "treblesurf-webcam";
const std::string Config::IMAGE_PATH = "/home/ryanpatton/image.jpg";
const std::chrono::seconds Config::STREAM_TIMEOUT{30};
const int Config::CAMERA_WIDTH = 1280;
const int Config::CAMERA_HEIGHT = 720;
const int Config::STREAM_FPS = 15;
const int Config::MAX_FRAGMENT_SIZE = 256 * 1024; // 256KB fragments for Pi Zero
const int Config::KVS_STORAGE_SIZE = 32 * 1024 * 1024; // 32MB instead of 128MB
const int Config::GSTREAMER_BITRATE = 400000; // 500Kbps for Pi Zero
const std::string Config::SPOT_ID = []() {
    const char* env_spot_id = std::getenv("SPOT_ID");
    if (!env_spot_id || strlen(env_spot_id) == 0) {
        std::cerr << "ERROR: SPOT_ID environment variable is not set!" << std::endl;
        std::cerr << "Please set it before running the application." << std::endl;
        exit(1);
    }
    return std::string(env_spot_id);
}();
const int Config::REQUEST_TIMEOUT = 10; // 10 seconds timeout for network requests

}  // namespace SurfCam