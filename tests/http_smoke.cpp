/*
 * SurfCam Weather Pi
 * Copyright (C) 2025  Ryan Patton
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ApiClient.h"
#include "Config.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << std::endl;
    return 1;
}

std::string makeTempFile(const std::string& contents) {
    std::string path = std::string("/tmp/surfcam-http-smoke-XXXXXX");
    std::vector<char> tmpl(path.begin(), path.end());
    tmpl.push_back('\0');
    const int fd = mkstemp(tmpl.data());
    if (fd < 0) {
        return {};
    }
    const ssize_t n = static_cast<ssize_t>(contents.size());
    if (write(fd, contents.data(), static_cast<size_t>(n)) != n) {
        close(fd);
        return {};
    }
    close(fd);
    return std::string(tmpl.data());
}

}  // namespace

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char* baseEnv = std::getenv("SURFCAM_TEST_API_BASE");
    if (!baseEnv || std::string(baseEnv).empty()) {
        curl_global_cleanup();
        return fail("SURFCAM_TEST_API_BASE is not set");
    }
    const std::string base = baseEnv;

    const char* keyEnv = std::getenv("API_KEY");
    const std::string apiKey = (keyEnv && std::strlen(keyEnv) > 0) ? std::string(keyEnv) : std::string("test-api-key");

    SurfCam::ApiClient api(base, apiKey);

    {
        auto r = api.isStreamingRequested("ok_spot");
        if (!r.has_value() || *r != false) {
            curl_global_cleanup();
            return fail("check-streaming ok_spot: expected optional(false)");
        }
    }

    {
        auto r = api.isStreamingRequested("badjson_spot");
        if (r.has_value()) {
            curl_global_cleanup();
            return fail("check-streaming badjson_spot: expected nullopt on invalid JSON");
        }
    }

    {
        auto r = api.isStreamingRequested("foo bar");
        if (!r.has_value() || *r != false) {
            curl_global_cleanup();
            return fail("check-streaming encoded spot_id: expected optional(false)");
        }
    }

    {
        auto r = api.isStreamingRequested("slow_spot");
        if (r.has_value()) {
            curl_global_cleanup();
            return fail("check-streaming slow_spot: expected timeout -> nullopt");
        }
    }

    {
        SurfCam::ApiClient dead("http://127.0.0.1:1", apiKey);
        auto r = dead.isStreamingRequested("x");
        if (r.has_value()) {
            curl_global_cleanup();
            return fail("connection refused: expected nullopt");
        }
    }

    {
        const std::string path = makeTempFile("presign-body");
        if (path.empty()) {
            curl_global_cleanup();
            return fail("temp file for presign");
        }
        const std::string key = "spots/fail/PRESIGN_FAIL_MARKER/live/x.ts";
        if (api.uploadLocalFileWithPresign(key, "video/mp2t", path)) {
            std::remove(path.c_str());
            curl_global_cleanup();
            return fail("presign failure: expected false");
        }
        std::remove(path.c_str());
    }

    {
        const std::string path = makeTempFile("upload-bytes");
        if (path.empty()) {
            curl_global_cleanup();
            return fail("temp file for put");
        }
        const std::string key = "spots/ok/live/segment.ts";
        if (!api.uploadLocalFileWithPresign(key, "video/mp2t", path)) {
            std::remove(path.c_str());
            curl_global_cleanup();
            return fail("presign+put happy path: expected true");
        }
        std::remove(path.c_str());
    }

    curl_global_cleanup();
    std::cout << "surfcam_test_http: all checks passed" << std::endl;
    return 0;
}
