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

CurlPtr makeHandle(const std::string& url, const std::string& userAgent, const std::string& caBundle) {
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
    // Only web protocols, also after redirects.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h.get(), CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(h.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS | CURLPROTO_HTTP));
    curl_easy_setopt(h.get(), CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    if (!caBundle.empty()) {
        curl_easy_setopt(h.get(), CURLOPT_CAINFO, caBundle.c_str());
        curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
#ifdef __SWITCH__
        curl_easy_setopt(h.get(), CURLOPT_SSL_VERIFYPEER, 0L);
#endif
    }
    return h;
}

void perform(CURL* h, const std::string& url) {
    auto rc = curl_easy_perform(h);
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
        throw std::runtime_error(std::string("secure connection failed: ") + curl_easy_strerror(rc) +
                                 ". Check the console's date and time.");
    if (rc != CURLE_OK) throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc));
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("the server answered with error " + std::to_string(status));
    (void)url;
}

}  // namespace

CurlHttpClient::CurlHttpClient(std::string userAgent, std::string caBundle)
    : userAgent_(std::move(userAgent)), caBundle_(std::move(caBundle)) {
    static CurlGlobal global;
}

CurlHttpClient::~CurlHttpClient() = default;

std::string CurlHttpClient::get(const std::string& url) {
    auto h = makeHandle(url, userAgent_, caBundle_);
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
    auto cleanup = [&] {
        std::fclose(f);
        std::error_code ec;
        fs::remove(part, ec);
    };
    CurlPtr h(nullptr, curl_easy_cleanup);
    try {
        h = makeHandle(url, userAgent_, caBundle_);
    } catch (...) {
        cleanup();
        throw;
    }
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, f);
    curl_easy_setopt(h.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(h.get(), CURLOPT_XFERINFODATA, &progress);
    try {
        perform(h.get(), url);
    } catch (...) {
        cleanup();
        throw;
    }
    if (std::fclose(f) != 0) {
        std::error_code ec;
        fs::remove(part, ec);
        throw std::runtime_error("write failed (SD card full?)");
    }
    std::error_code ec;
    fs::remove(dest, ec);
    fs::rename(part, dest);
}

int64_t CurlHttpClient::contentLength(const std::string& url) {
    // Ask for the first byte only: "Content-Range: bytes 0-0/<total>" gives the size even from
    // servers whose HEAD replies leave it out (Wix file hosting does). If a server ignores the
    // range, stop reading after the first chunk and use Content-Length.
    struct State {
        int64_t total = -1, length = -1;
        size_t received = 0;
    } st;
    auto h = makeHandle(url, userAgent_, caBundle_);
    curl_easy_setopt(h.get(), CURLOPT_RANGE, "0-0");
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(h.get(), CURLOPT_HEADERDATA, &st);
    curl_easy_setopt(h.get(), CURLOPT_HEADERFUNCTION, +[](char* buf, size_t size, size_t n, void* ud) -> size_t {
        auto* s = static_cast<State*>(ud);
        std::string line(buf, size * n);
        auto lower = line;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("http/", 0) == 0) *s = State{};  // a new response (after a redirect)
        if (lower.rfind("content-range:", 0) == 0) {
            auto slash = lower.rfind('/');
            if (slash != std::string::npos && std::isdigit(static_cast<unsigned char>(lower[slash + 1]))) s->total = std::stoll(lower.substr(slash + 1));
        }
        if (lower.rfind("content-length:", 0) == 0) s->length = std::stoll(lower.substr(15));
        return size * n;
    });
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t n, void* ud) -> size_t {
        auto* s = static_cast<State*>(ud);
        s->received += size * n;
        return s->received > 4096 ? 0 : size * n;  // abort: the server is sending the whole file
    });
    curl_easy_perform(h.get());  // an abort above is expected; use whatever headers arrived
    return st.total > 0 ? st.total : st.length > 1 ? st.length : -1;
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
