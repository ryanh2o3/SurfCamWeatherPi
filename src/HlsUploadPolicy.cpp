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

#include "HlsUploadPolicy.h"

#include <istream>
#include <string>

namespace SurfCam::HlsUploadPolicy {

void trimTrailingCarriageReturn(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

bool playlistAllTsLinesUploaded(std::istream& in, const std::unordered_set<std::string>& uploaded) {
    std::string line;
    while (std::getline(in, line)) {
        trimTrailingCarriageReturn(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.size() < 4 || line.compare(line.size() - 3, 3, ".ts") != 0) {
            continue;
        }
        if (uploaded.count(line) == 0) {
            return false;
        }
    }
    return true;
}

void recordSegmentUploaded(const std::string& segmentFilename, std::unordered_set<std::string>& uploaded,
                           std::list<std::string>& order) {
    if (uploaded.count(segmentFilename) != 0) {
        return;
    }
    while (order.size() >= kMaxUploadedSegmentNames) {
        const std::string& oldest = order.front();
        uploaded.erase(oldest);
        order.pop_front();
    }
    order.push_back(segmentFilename);
    uploaded.insert(segmentFilename);
}

std::string s3KeyForFile(const std::string& spotId, const std::string& filename) {
    return "spots/" + spotId + "/live/" + filename;
}

}  // namespace SurfCam::HlsUploadPolicy
