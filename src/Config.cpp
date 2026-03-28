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

#include "Config.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace SurfCam {

std::string Config::API_KEY;
std::string Config::SPOT_ID;
std::string Config::SNAPSHOT_PATH;

bool Config::loadFromEnvironment(std::ostream& err) {
    const char* k = std::getenv("API_KEY");
    if (!k || std::strlen(k) == 0) {
        err << "ERROR: API_KEY environment variable is not set.\n";
        err << "Set API_KEY before running the application.\n";
        return false;
    }
    API_KEY = k;

    const char* s = std::getenv("SPOT_ID");
    if (!s || std::strlen(s) == 0) {
        err << "ERROR: SPOT_ID environment variable is not set.\n";
        err << "Set SPOT_ID before running the application.\n";
        return false;
    }
    SPOT_ID = s;

    const char* p = std::getenv("SNAPSHOT_PATH");
    SNAPSHOT_PATH = (p && std::strlen(p) > 0) ? std::string(p) : std::string("/tmp/surfcam-snapshot.jpg");
    return true;
}

}  // namespace SurfCam
