#include "ApiClient.h"
#include "Config.h"
#include <iostream>
#include <fstream>
#include <curl/curl.h>
#include <chrono>

// Helper function for curl to write response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    s->append((char*)contents, newLength);
    return newLength;
}

namespace SurfCam {

ApiClient::ApiClient(const std::string& apiEndpoint, const std::string& apiKey)
    : apiEndpoint_(apiEndpoint), apiKey_(apiKey), lastStreamRequestTime_(0) {
    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ApiClient::~ApiClient() {
    curl_global_cleanup();
}

// In the uploadSnapshot function:

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
        
        // Read image file
        std::ifstream imageFile(imagePath, std::ios::binary);
        if (!imageFile) {
            std::cerr << "Failed to open image file: " << imagePath << std::endl;
            return false;
        }
        
        // Read the entire file
        std::string imageData((std::istreambuf_iterator<char>(imageFile)),
                             std::istreambuf_iterator<char>());
        imageFile.close();
        
        // Set up curl to upload the file
        std::string url = apiEndpoint_ + "/upload-snapshot";
        
        // Set up HTTP form
        mime = curl_mime_init(curl);
        curl_mimepart* part;
        
        // Add file part
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_filename(part, "snapshot.jpg");
        curl_mime_data(part, imageData.c_str(), imageData.size());
        curl_mime_type(part, "image/jpeg");
        
        // Add timestamp
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%dT%H:%M:%S");
        std::string timestamp = ss.str();
        
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "timestamp");
        curl_mime_data(part, timestamp.c_str(), CURL_ZERO_TERMINATED);
        
        // Add spot_id - use the configured value
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "spot_id");
        curl_mime_data(part, spotId.c_str(), CURL_ZERO_TERMINATED);
        
        // Set headers
        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
        
        // Set curl options
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, Config::REQUEST_TIMEOUT);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // 5 seconds connect timeout
        
        // Low speed limits for Pi Zero's slower network
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L); // 10 bytes/sec minimum
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L); // for 10 seconds = timeout
        
        // Response handling
        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        // Perform the request
        CURLcode res = curl_easy_perform(curl);
        
        // Check for errors
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            // Get response code
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
    } 
    catch (const std::exception& e) {
        std::cerr << "Exception uploading snapshot: " << e.what() << std::endl;
    }

    // Clean up resources properly
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
        // Set up the URL
        std::string url = apiEndpoint_ + "/check-streaming-requested?spot_id=" + spotId;
        
        // Set headers
        headers = nullptr;
        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
        
        // Response handling
        std::string response;
        
        // Set curl options
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 seconds timeout
        
        // Perform request
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            // Get response code
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (http_code == 200) {
                // Parse JSON response
                auto json_response = nlohmann::json::parse(response);
                if (json_response.contains("stream_requested") && 
                    json_response["stream_requested"].is_boolean()) {
                    
                    streaming_requested = json_response["stream_requested"].get<bool>();
                    
                    if (streaming_requested) {
                        std::lock_guard<std::mutex> lock(apiMutex_);
                        lastStreamRequestTime_ = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
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
                std::chrono::system_clock::now().time_since_epoch()).count();
                
            if (now - lastStreamRequestTime_ < Config::STREAM_TIMEOUT.count()) {
                streaming_requested = true;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception checking streaming status: " << e.what() << std::endl;
        curl_easy_cleanup(curl);
    }
    
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);

    return streaming_requested;
}

std::optional<AwsCredentials> ApiClient::getStreamingCredentials() {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return std::nullopt;
    }
    
    std::optional<AwsCredentials> credentials;
    struct curl_slist* headers = nullptr;

    try {
        // Set up the URL
        std::string url = apiEndpoint_ + "/get-streaming-credentials";
        
        // Set headers
        headers = nullptr;
        std::string authHeader = "Authorization: ApiKey " + apiKey_;
        headers = curl_slist_append(headers, authHeader.c_str());
        
        // Response handling
        std::string response;
        
        // Set curl options
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 seconds timeout
        
        // Perform request
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            // Get response code
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (http_code == 200) {
                // Parse JSON response
                auto json_response = nlohmann::json::parse(response);
                
                // Create credentials structure
                AwsCredentials creds;
                creds.accessKey = json_response["accessKey"].get<std::string>();
                creds.secretKey = json_response["secretKey"].get<std::string>();
                creds.sessionToken = json_response["sessionToken"].get<std::string>();
                
                credentials = creds;
                std::cout << "Received streaming credentials" << std::endl;
            } else {
                std::cerr << "Failed to get streaming credentials. Status code: " << http_code << std::endl;
                std::cerr << "Response: " << response << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Exception getting streaming credentials: " << e.what() << std::endl;
    }
    
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);

    return credentials;
}

}  // namespace SurfCam