#pragma once
#include <string>
#include <vector>
#include <utility>

namespace h7::net {

struct HttpRequest {
    std::string method = "GET";
    std::string url;                                          // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers; // sent as given
    std::string body;
    int timeoutMs = 10000;                                    // resolve, connect, send and receive each
};

struct HttpResponse {
    bool ok = false;     // a response was received (any status code)
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
    std::string error;   // set when ok == false
};

// Blocking: call from a worker thread, never the game thread. Thread-safe (one shared WinHTTP session).
HttpResponse HttpSend(const HttpRequest& req, const std::string& userAgent);

} // namespace h7::net
