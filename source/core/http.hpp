#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "util.hpp"

namespace mm {

// The server answered, but not with success (404 and so on).
class HttpStatusError : public std::runtime_error {
public:
    explicit HttpStatusError(long status)
        : std::runtime_error(status == 429 ? "the server is busy (too many requests). Wait a minute and try again"
                                           : "the server answered with error " + std::to_string(status)),
          status_(status) {}
    long status() const { return status_; }

private:
    long status_;
};

// Return false to cancel the transfer.
using DownloadProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // GET a URL and return the body. Throws on network error or non-2xx status.
    virtual std::string get(const std::string& url) = 0;
    // Stream a URL to a file (following redirects). Throws on error.
    virtual void download(const std::string& url, const fs::path& dest, const DownloadProgressFn& progress = {}) = 0;
    // Size of the file at a URL (HEAD request, following redirects); -1 if unknown.
    virtual int64_t contentLength(const std::string& url) = 0;
    // Bytes [from, to] of a URL. Throws if the server doesn't support ranges. `finalUrl`
    // receives where redirects led, so follow-up ranges can skip them.
    virtual std::string getRange(const std::string& url, uint64_t from, uint64_t to, std::string* finalUrl = nullptr) = 0;
    // Make every running and later request fail soon (the app is quitting). Any thread.
    virtual void cancelAll() {}
};

// libcurl implementation (works on macOS and on Switch via devkitPro's switch-curl).
class CurlHttpClient : public HttpClient {
public:
    // caBundle: PEM file of trusted certificates (the Switch has no system store we can use).
    // Empty uses the system store; on Switch that means no verification.
    CurlHttpClient(std::string userAgent, std::string caBundle = {});
    ~CurlHttpClient() override;
    std::string get(const std::string& url) override;
    void download(const std::string& url, const fs::path& dest, const DownloadProgressFn& progress = {}) override;
    int64_t contentLength(const std::string& url) override;
    std::string getRange(const std::string& url, uint64_t from, uint64_t to, std::string* finalUrl = nullptr) override;
    void cancelAll() override { cancelled_ = true; }

private:
    std::string userAgent_;
    std::string caBundle_;
    std::atomic<bool> cancelled_{false};
};

std::string urlEncode(const std::string& s);

}  // namespace mm
