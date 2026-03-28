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
#include <filesystem>
#include <vector>
#include <nlohmann/json.hpp>

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

namespace SurfCam {

ApiClient::ApiClient(const std::string& apiEndpoint, const std::string& apiKey)
    : apiEndpoint_(apiEndpoint), apiKey_(apiKey), lastStreamRequestTime_(0) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ApiClient::~ApiClient() {
    curl_global_cleanup();
}

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

        std::ifstream imageFile(imagePath, std::ios::binary);
        if (!imageFile) {
            std::cerr << "Failed to open image file: " << imagePath << std::endl;
            return false;
        }

        std::string imageData((std::istreambuf_iterator<char>(imageFile)), std::istreambuf_iterator<char>());
        imageFile.close();

        std::string url = apiEndpoint_ + "/upload-snapshot";

        mime = curl_mime_init(curl);
        curl_mimepart* part;

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filename(part, "snapshot.jpg");
        curl_mime_data(part, imageData.c_str(), imageData.size());
        curl_mime_type(part, "image/jpeg");

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%dT%H:%M:%S");
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
                std::cerr << "Response: " << response.substr(0, 200) << "..." << std::endl;
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

bool ApiClient::isStreamingRequested(const std::string& spotId) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return false;
    }

    bool streaming_requested = false;
    struct curl_slist* headers = nullptr;

    try {
        std::string url = apiEndpoint_ + "/check-streaming-requested?spot_id=" + spotId;

        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());

        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code == 200) {
                auto json_response = nlohmann::json::parse(response);
                if (json_response.contains("stream_requested") && json_response["stream_requested"].is_boolean()) {
                    streaming_requested = json_response["stream_requested"].get<bool>();

                    if (streaming_requested) {
                        std::lock_guard<std::mutex> lock(apiMutex_);
                        lastStreamRequestTime_ = std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                     .count();
                        std::cout << "Streaming requested!" << std::endl;
                    }
                }
            } else {
                std::cerr << "Failed to check streaming status. Status code: " << http_code << std::endl;
                std::cerr << "Response: " << response << std::endl;
            }
        }

        {
            std::lock_guard<std::mutex> lock(apiMutex_);
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

            if (now - lastStreamRequestTime_ < Config::STREAM_TIMEOUT.count()) {
                streaming_requested = true;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception checking streaming status: " << e.what() << std::endl;
    }

    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);

    return streaming_requested;
}

bool ApiClient::uploadLocalFileWithPresign(const std::string& objectKey, const std::string& contentType,
                                           const std::string& filePath) {
    CURL* presignCurl = curl_easy_init();
    if (!presignCurl) {
        return false;
    }

    struct curl_slist* presignHeaders = nullptr;
    std::string presignUrl;
    std::vector<std::pair<std::string, std::string>> presignResponseHeaders;
    bool presignOk = false;

    try {
        std::string url = apiEndpoint_ + Config::HLS_PRESIGN_PATH;
        nlohmann::json body;
        body["key"] = objectKey;
        body["content_type"] = contentType;
        std::string payload = body.dump();

        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        presignHeaders = curl_slist_append(presignHeaders, authHeader.c_str());
        presignHeaders = curl_slist_append(presignHeaders, "Content-Type: application/json");

        std::string response;
        curl_easy_setopt(presignCurl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(presignCurl, CURLOPT_HTTPHEADER, presignHeaders);
        curl_easy_setopt(presignCurl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(presignCurl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(presignCurl, CURLOPT_TIMEOUT, Config::REQUEST_TIMEOUT);
        curl_easy_setopt(presignCurl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(presignCurl, CURLOPT_WRITEDATA, &response);

        if (curl_easy_perform(presignCurl) == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(presignCurl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code == 200) {
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
                    presignOk = true;
                }
            } else {
                std::cerr << "presign HTTP " << http_code << " " << response << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "presign: " << e.what() << std::endl;
    }

    if (presignHeaders) curl_slist_free_all(presignHeaders);
    curl_easy_cleanup(presignCurl);

    if (!presignOk || presignUrl.empty()) {
        return false;
    }

    std::error_code ec;
    const auto fsize = std::filesystem::file_size(filePath, ec);
    if (ec || fsize == 0) {
        return false;
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

    bool ok = false;
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

    CURLcode pr = curl_easy_perform(put);
    if (pr != CURLE_OK) {
        std::cerr << "S3 PUT failed: " << curl_easy_strerror(pr) << std::endl;
    } else {
        long code = 0;
        curl_easy_getinfo(put, CURLINFO_RESPONSE_CODE, &code);
        if (code >= 200 && code < 300) {
            ok = true;
        } else {
            std::cerr << "S3 PUT HTTP " << code << " " << putResp << std::endl;
        }
    }

    if (putHeaders) curl_slist_free_all(putHeaders);
    curl_easy_cleanup(put);
    return ok;
}

}  // namespace SurfCam
