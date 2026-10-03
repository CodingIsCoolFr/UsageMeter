#pragma once

#include <map>
#include <string>
#include <vector>

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;  // transport error; empty on a completed HTTP exchange
    std::map<std::string, std::string> headers;  // lower-cased names
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    std::string body;
    std::string cookie_jar;  // path; empty disables
    long timeout_s = 25;
};

HttpResponse http_request(const HttpRequest& req);

// RFC 4648 base64url, no padding.
std::string b64url(const unsigned char* data, size_t len);
std::string random_b64url(size_t bytes);
std::string sha256_b64url(const std::string& input);
