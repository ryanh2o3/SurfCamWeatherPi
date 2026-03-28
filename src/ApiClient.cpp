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

#include "ApiClient.h"
#include "Config.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <curl/curl.h>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    s->append(static_cast<char*>(contents), newLength);
    return newLength;
}

static size_t ReadCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* stream = static_cast<std::ifstream*>(userdata);
    stream->read(ptr, static_cast<std::streamsize>(size * nmemb));
    return static_cast<size_t>(stream->gcount());
}

namespace {

/// mmap(2) the file for curl_mime_data (avoids a second full-file heap copy in a std::string).
struct FileMmap {
    void* addr{MAP_FAILED};
    size_t len{0};

    FileMmap() = default;

    bool map(const std::string& path) {
        clear();
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            std::cerr << "Failed to open image file: " << path << " (" << std::strerror(errno) << ")" << std::endl;
            return false;
        }
        struct stat st {};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            std::cerr << "Invalid snapshot file size: " << path << std::endl;
            close(fd);
            return false;
        }
        len = static_cast<size_t>(st.st_size);
        addr = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (addr == MAP_FAILED) {
            std::cerr << "mmap failed for: " << path << " (" << std::strerror(errno) << ")" << std::endl;
            len = 0;
            return false;
        }
        return true;
    }

    void clear() {
        if (addr != MAP_FAILED) {
            munmap(addr, len);
            addr = MAP_FAILED;
            len = 0;
        }
    }

    ~FileMmap() { clear(); }

    FileMmap(const FileMmap&) = delete;
    FileMmap& operator=(const FileMmap&) = delete;
};

[[nodiscard]] bool curlErrorIsTransient(CURLcode c) {
    return c == CURLE_OPERATION_TIMEDOUT || c == CURLE_COULDNT_CONNECT || c == CURLE_SEND_ERROR ||
           c == CURLE_RECV_ERROR || c == CURLE_GOT_NOTHING || c == CURLE_SSL_CONNECT_ERROR;
}

[[nodiscard]] bool httpStatusIsTransient(long code) {
    return code == 429 || code == 502 || code == 503 || code == 504;
}

constexpr int kPresignedTransferMaxAttempts = 4;

}  // namespace

namespace SurfCam {

ApiClient::ApiClient(std::string apiEndpoint, std::string apiKey)
    : apiEndpoint_(std::move(apiEndpoint)), apiKey_(std::move(apiKey)) {}

bool ApiClient::uploadSnapshot(const std::string& imagePath, const std::string& spotId) {
    CURL* curl = nullptr;
    curl_mime* mime = nullptr;
    struct curl_slist* headers = nullptr;
    bool success = false;

    try {
        curl = curl_easy_init();
        if (!curl) {
            std::cerr << "Failed to initialize curl" << std::endl;
            return false;
        }

        std::string url = apiEndpoint_ + "/upload-snapshot";

        FileMmap imageMap;
        if (!imageMap.map(imagePath)) {
            curl_easy_cleanup(curl);
            curl = nullptr;
            return false;
        }

        mime = curl_mime_init(curl);
        if (!mime) {
            std::cerr << "curl_mime_init failed" << std::endl;
            curl_easy_cleanup(curl);
            curl = nullptr;
            return false;
        }

        curl_mimepart* part;

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filename(part, "snapshot.jpg");
        curl_mime_type(part, "image/jpeg");
        if (curl_mime_data(part, static_cast<const char*>(imageMap.addr), imageMap.len) != CURLE_OK) {
            std::cerr << "curl_mime_data failed for snapshot file" << std::endl;
            curl_mime_free(mime);
            mime = nullptr;
            curl_easy_cleanup(curl);
            curl = nullptr;
            return false;
        }

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm tmBuf {};
        struct tm* tmPtr = localtime_r(&now_time_t, &tmBuf);
        std::stringstream ss;
        if (tmPtr) {
            ss << std::put_time(tmPtr, "%Y-%m-%dT%H:%M:%S");
        }
        std::string timestamp = ss.str();

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "timestamp");
        curl_mime_data(part, timestamp.c_str(), CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "spot_id");
        curl_mime_data(part, spotId.c_str(), CURL_ZERO_TERMINATED);

        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, Config::REQUEST_TIMEOUT);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code == 200) {
                std::cout << "Snapshot uploaded successfully!" << std::endl;
                success = true;
            } else {
                std::cerr << "Failed to upload snapshot. Status code: " << http_code << std::endl;
                const std::size_t kMax = 200;
                std::cerr << "Response: " << response.substr(0, kMax)
                          << (response.size() > kMax ? "..." : "") << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception uploading snapshot: " << e.what() << std::endl;
    }

    if (mime) curl_mime_free(mime);
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);

    return success;
}

std::optional<bool> ApiClient::isStreamingRequested(const std::string& spotId) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return std::nullopt;
    }

    struct curl_slist* headers = nullptr;
    std::optional<bool> result = std::nullopt;

    char* escapedSpot = curl_easy_escape(curl, spotId.c_str(), 0);
    if (!escapedSpot) {
        std::cerr << "curl_easy_escape failed for spot_id" << std::endl;
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
    std::string url = apiEndpoint_ + "/check-streaming-requested?spot_id=" + escapedSpot;
    curl_free(escapedSpot);

    std::string authHeader = "Authorization: ApiKey " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200) {
            try {
                auto json_response = nlohmann::json::parse(response);
                if (json_response.contains("stream_requested") && json_response["stream_requested"].is_boolean()) {
                    result = json_response["stream_requested"].get<bool>();
                    if (*result) {
                        std::cout << "Streaming requested!" << std::endl;
                    }
                } else {
                    std::cerr << "check-streaming: 200 response missing boolean stream_requested" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "check-streaming: JSON parse error: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "Failed to check streaming status. Status code: " << http_code << std::endl;
            const std::size_t kMax = 500;
            std::cerr << "Response: " << response.substr(0, kMax) << (response.size() > kMax ? "..." : "")
                      << std::endl;
        }
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result;
}

bool ApiClient::uploadLocalFileWithPresign(const std::string& objectKey, const std::string& contentType,
                                           const std::string& filePath) {
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(filePath, ec);
    if (ec || fsize == 0) {
        return false;
    }

    for (int attempt = 0; attempt < kPresignedTransferMaxAttempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
        }

        CURL* presignCurl = curl_easy_init();
        if (!presignCurl) {
            return false;
        }

        struct curl_slist* presignHeaders = nullptr;
        std::string presignUrl;
        std::vector<std::pair<std::string, std::string>> presignResponseHeaders;
        bool presignOk = false;
        CURLcode presignCurlCode = CURLE_OK;
        long presignHttp = 0;

        try {
            std::string postUrl = apiEndpoint_ + Config::HLS_PRESIGN_PATH;
            nlohmann::json body;
            body["key"] = objectKey;
            body["content_type"] = contentType;
            std::string payload = body.dump();

            std::string authHeader = "Authorization: ApiKey " + apiKey_;
            presignHeaders = curl_slist_append(presignHeaders, authHeader.c_str());
            presignHeaders = curl_slist_append(presignHeaders, "Content-Type: application/json");

            std::string response;
            curl_easy_setopt(presignCurl, CURLOPT_URL, postUrl.c_str());
            curl_easy_setopt(presignCurl, CURLOPT_HTTPHEADER, presignHeaders);
            curl_easy_setopt(presignCurl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(presignCurl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
            curl_easy_setopt(presignCurl, CURLOPT_TIMEOUT, Config::REQUEST_TIMEOUT);
            curl_easy_setopt(presignCurl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(presignCurl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(presignCurl, CURLOPT_WRITEDATA, &response);

            presignCurlCode = curl_easy_perform(presignCurl);
            if (presignCurlCode == CURLE_OK) {
                curl_easy_getinfo(presignCurl, CURLINFO_RESPONSE_CODE, &presignHttp);
                if (presignHttp == 200) {
                    auto j = nlohmann::json::parse(response);
                    if (j.contains("url") && j["url"].is_string()) {
                        presignUrl = j["url"].get<std::string>();
                        if (j.contains("headers") && j["headers"].is_object()) {
                            for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it) {
                                if (it.value().is_string()) {
                                    presignResponseHeaders.push_back({it.key(), it.value().get<std::string>()});
                                }
                            }
                        }
                        presignOk = !presignUrl.empty();
                    }
                } else {
                    std::cerr << "presign HTTP " << presignHttp << " " << response << std::endl;
                }
            } else {
                std::cerr << "presign curl: " << curl_easy_strerror(presignCurlCode) << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "presign: " << e.what() << std::endl;
        }

        if (presignHeaders) curl_slist_free_all(presignHeaders);
        curl_easy_cleanup(presignCurl);

        if (!presignOk) {
            const bool retry = curlErrorIsTransient(presignCurlCode) ||
                               (presignCurlCode == CURLE_OK && httpStatusIsTransient(presignHttp));
            if (!retry) {
                return false;
            }
            continue;
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return false;
        }

        CURL* put = curl_easy_init();
        if (!put) {
            return false;
        }

        struct curl_slist* putHeaders = nullptr;
        for (const auto& h : presignResponseHeaders) {
            std::string line = h.first + ": " + h.second;
            putHeaders = curl_slist_append(putHeaders, line.c_str());
        }

        CURLcode putCurlCode = CURLE_OK;
        long putHttp = 0;
        bool putOk = false;

        curl_easy_setopt(put, CURLOPT_URL, presignUrl.c_str());
        curl_easy_setopt(put, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(put, CURLOPT_READFUNCTION, ReadCallback);
        curl_easy_setopt(put, CURLOPT_READDATA, &file);
        curl_easy_setopt(put, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(fsize));
        if (putHeaders) {
            curl_easy_setopt(put, CURLOPT_HTTPHEADER, putHeaders);
        }
        curl_easy_setopt(put, CURLOPT_TIMEOUT, Config::REQUEST_TIMEOUT);
        curl_easy_setopt(put, CURLOPT_CONNECTTIMEOUT, 10L);

        std::string putResp;
        curl_easy_setopt(put, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(put, CURLOPT_WRITEDATA, &putResp);

        putCurlCode = curl_easy_perform(put);
        if (putCurlCode != CURLE_OK) {
            std::cerr << "S3 PUT failed: " << curl_easy_strerror(putCurlCode) << std::endl;
        } else {
            curl_easy_getinfo(put, CURLINFO_RESPONSE_CODE, &putHttp);
            if (putHttp >= 200 && putHttp < 300) {
                putOk = true;
            } else {
                std::cerr << "S3 PUT HTTP " << putHttp << " " << putResp << std::endl;
            }
        }

        if (putHeaders) curl_slist_free_all(putHeaders);
        curl_easy_cleanup(put);

        if (putOk) {
            return true;
        }

        const bool retryPut =
            curlErrorIsTransient(putCurlCode) || (putCurlCode == CURLE_OK && httpStatusIsTransient(putHttp));
        if (!retryPut) {
            return false;
        }
    }

    return false;
}

}  // namespace SurfCam
