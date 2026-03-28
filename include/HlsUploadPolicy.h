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

#include <cstddef>
#include <iosfwd>
#include <list>
#include <string>
#include <unordered_set>

namespace SurfCam::HlsUploadPolicy {

inline constexpr std::size_t kMaxUploadedSegmentNames = 512;

void trimTrailingCarriageReturn(std::string& line);

/// Returns true when every non-comment line ending in `.ts` names a file present in `uploaded`.
[[nodiscard]] bool playlistAllTsLinesUploaded(std::istream& in, const std::unordered_set<std::string>& uploaded);

/// Insert `segmentFilename` into `uploaded` and `order`, evicting oldest names when over the cap.
void recordSegmentUploaded(const std::string& segmentFilename, std::unordered_set<std::string>& uploaded,
                           std::list<std::string>& order);

[[nodiscard]] std::string s3KeyForFile(const std::string& spotId, const std::string& filename);

}  // namespace SurfCam::HlsUploadPolicy
