#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "http.hpp"

namespace mm {

namespace {

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t writeToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata)) * size;
}

int progressCb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* fn = static_cast<const DownloadProgressFn*>(userdata);
    if (*fn && !(*fn)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal))) return 1;  // abort
    return 0;
}

using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

CurlPtr makeHandle(const std::string& url, const std::string& userAgent) {
    CurlPtr h(curl_easy_init(), curl_easy_cleanup);
    if (!h) throw std::runtime_error("curl_easy_init failed");
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);  // abort if stalled for 60s
    curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h.get(), CURLOPT_ACCEPT_ENCODING, "");
#ifdef __SWITCH__
    // The Switch has no system CA bundle reachable from homebrew.
    curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYPEER, 0L);
#endif
    return h;
}

void perform(CURL* h, const std::string& url) {
    auto rc = curl_easy_perform(h);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc) + " (" + url + ")");
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("HTTP " + std::to_string(status) + " from " + url);
}

}  // namespace

CurlHttpClient::CurlHttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {
    static CurlGlobal global;
}

CurlHttpClient::~CurlHttpClient() = default;

std::string CurlHttpClient::get(const std::string& url) {
    auto h = makeHandle(url, userAgent_);
    std::string body;
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 60L);
    perform(h.get(), url);
    return body;
}

void CurlHttpClient::download(const std::string& url, const fs::path& dest, const DownloadProgressFn& progress) {
    fs::create_directories(dest.parent_path());
    auto part = dest;
    part += ".part";
    FILE* f = std::fopen(part.string().c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + part.string());
    std::setvbuf(f, nullptr, _IOFBF, 1 << 18);  // big buffer: SD card writes are slow in small chunks
    auto h = makeHandle(url, userAgent_);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, f);
    curl_easy_setopt(h.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFODATA, &progress);
    try {
        perform(h.get(), url);
    } catch (...) {
        std::fclose(f);
        std::error_code ec;
        fs::remove(part, ec);
        throw;
    }
    if (std::fclose(f) != 0) throw std::runtime_error("write failed (SD card full?)");
    std::error_code ec;
    fs::remove(dest, ec);
    fs::rename(part, dest);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace mm
