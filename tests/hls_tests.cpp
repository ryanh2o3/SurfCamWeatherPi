/*
 * SurfCam Weather Pi
 * Copyright (C) 2025  Ryan Patton
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Config.h"
#include "HlsUploader.h"
#include "HlsUploadPolicy.h"
#include "IHlsPresignClient.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <list>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct FakePresign : SurfCam::IHlsPresignClient {
    bool failNextTs = false;
    std::vector<std::string> objectKeys;

    bool uploadLocalFileWithPresign(const std::string& objectKey, const std::string& /*contentType*/,
                                    const std::string& /*filePath*/) override {
        const bool isTs = objectKey.size() >= 3 && objectKey.compare(objectKey.size() - 3, 3, ".ts") == 0;
        if (isTs && failNextTs) {
            failNextTs = false;
            return false;
        }
        objectKeys.push_back(objectKey);
        return true;
    }
};

class TempDir {
public:
    TempDir() {
        static std::random_device rd;
        path_ = fs::temp_directory_path() / ("surfcam-hls-test-" + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }

    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    fs::path path_;
};

void writeFile(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    REQUIRE(out);
    out << data;
}

void waitForStablePoll() { std::this_thread::sleep_for(std::chrono::milliseconds(750)); }

}  // namespace

TEST_CASE("HlsUploadPolicy trims CR and detects playlist gaps") {
    using SurfCam::HlsUploadPolicy::playlistAllTsLinesUploaded;
    using SurfCam::HlsUploadPolicy::trimTrailingCarriageReturn;

    std::string line = "seg.ts\r";
    trimTrailingCarriageReturn(line);
    REQUIRE(line == "seg.ts");

    std::unordered_set<std::string> have{"a.ts"};
    std::istringstream good("#EXTM3U\na.ts\n");
    REQUIRE(playlistAllTsLinesUploaded(good, have));

    std::istringstream missing("#EXTM3U\na.ts\nb.ts\n");
    REQUIRE_FALSE(playlistAllTsLinesUploaded(missing, have));
}

TEST_CASE("HlsUploadPolicy eviction keeps a bounded uploaded set") {
    using SurfCam::HlsUploadPolicy::kMaxUploadedSegmentNames;
    using SurfCam::HlsUploadPolicy::recordSegmentUploaded;

    std::unordered_set<std::string> uploaded;
    std::list<std::string> order;
    const int extra = 8;
    for (int i = 0; i < static_cast<int>(kMaxUploadedSegmentNames) + extra; ++i) {
        recordSegmentUploaded("s" + std::to_string(i), uploaded, order);
    }
    REQUIRE(uploaded.size() == kMaxUploadedSegmentNames);
    for (int i = 0; i < extra; ++i) {
        REQUIRE(uploaded.count("s" + std::to_string(i)) == 0);
    }
    REQUIRE(uploaded.count("s" + std::to_string(kMaxUploadedSegmentNames + extra - 1)) == 1);
}

TEST_CASE("HlsUploader uploads playlist only after segments succeed") {
    TempDir dir;
    const std::string spot = "spot1";
    const fs::path root = dir.string();
    writeFile(root / "segment-00001.ts", std::string(4000, 'x'));
    writeFile(root / "index.m3u8", "#EXTM3U\nsegment-00001.ts\n");
    waitForStablePoll();

    SurfCam::HlsUploader up;
    FakePresign fake;
    fake.failNextTs = true;
    REQUIRE(up.pollAndUpload(fake, spot, root.string()));
    REQUIRE(fake.objectKeys.empty());

    REQUIRE(up.pollAndUpload(fake, spot, root.string()));
    REQUIRE(fake.objectKeys.size() == 2);
    REQUIRE(fake.objectKeys[0] == SurfCam::HlsUploadPolicy::s3KeyForFile(spot, "segment-00001.ts"));
    REQUIRE(fake.objectKeys[1] ==
            SurfCam::HlsUploadPolicy::s3KeyForFile(spot, SurfCam::Config::HLS_PLAYLIST_NAME));
}
