#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "util.hpp"

namespace mm {

// Return false to cancel the transfer.
using DownloadProgressFn = std::function<bool(uint64_t done, uint64_t total)>;

class HttpClient {
public:
    virtual ~HttpClient() = default;
    // GET a URL and return the body. Throws on network error or non-2xx status.
    virtual std::string get(const std::string& url) = 0;
    // Stream a URL to a file (following redirects). Throws on error.
    virtual void download(const std::string& url, const fs::path& dest, const DownloadProgressFn& progress = {}) = 0;
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

private:
    std::string userAgent_;
    std::string caBundle_;
};

std::string urlEncode(const std::string& s);

}  // namespace mm
