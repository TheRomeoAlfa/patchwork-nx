#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
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

// Called about once a second, also while connecting, so cancelAll() stops every request quickly.
struct Progress {
    const std::atomic<bool>* cancelled;
    const DownloadProgressFn* fn = nullptr;
};

int progressCb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* p = static_cast<const Progress*>(userdata);
    if (*p->cancelled) return 1;  // abort
    if (p->fn && *p->fn && !(*p->fn)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal))) return 1;
    return 0;
}

// Shared between all requests (from any thread): DNS answers and TLS sessions, so a burst of
// small requests to the same host skips most of the handshake.
CURLSH* sharedState() {
    // Never destroyed: download threads may still be finishing while the app exits.
    static auto* locks = new std::mutex[CURL_LOCK_DATA_LAST];
    static CURLSH* share = [] {
        CURLSH* s = curl_share_init();
        curl_share_setopt(s, CURLSHOPT_LOCKFUNC, +[](CURL*, curl_lock_data data, curl_lock_access, void*) { locks[data].lock(); });
        curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, +[](CURL*, curl_lock_data data, void*) { locks[data].unlock(); });
        curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        // Not CURL_LOCK_DATA_CONNECT: libcurl doesn't support sharing connections across threads.
        return s;
    }();
    return share;
}

using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

// One handle per thread, kept from request to request: it keeps its connections open, so the
// next request to the same host (most go to GameBanana) skips the TCP and TLS handshakes. On
// the Switch every new TLS connection goes through the system's SSL service and loads the
// certificate list; a page of thumbnails used to start dozens of them at once.
// `progress` must outlive the request.
CURL* makeHandle(const std::string& url, const std::string& userAgent, const std::string& caBundle, const Progress& progress) {
    if (*progress.cancelled) throw std::runtime_error("cancelled");
    thread_local CurlPtr handle(nullptr, curl_easy_cleanup);
    if (handle)
        curl_easy_reset(handle.get());  // options only: open connections and caches stay
    else
        handle.reset(curl_easy_init());
    CURL* h = handle.get();
    if (!h) throw std::runtime_error("curl_easy_init failed");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  // required with threads (DNS timeouts use signals otherwise)
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &progress);
    curl_easy_setopt(h, CURLOPT_SHARE, sharedState());
    curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L);  // the default minute meant fresh lookups all the time
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);  // abort if stalled for 60s
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    // Only web protocols, also after redirects.
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS | CURLPROTO_HTTP));
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    if (!caBundle.empty()) {
        curl_easy_setopt(h, CURLOPT_CAINFO, caBundle.c_str());
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
#ifdef __SWITCH__
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
#endif
    }
    return h;
}

// Failures the network causes rather than the server: worth one more try, which usually works
// (a kept connection the server had closed, a slow lookup, a dropped packet).
bool transient(CURLcode rc) {
    switch (rc) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

// `attempt` sets up and runs the request, returning curl's result; it runs again once after a
// transient failure.
template <typename Attempt>
CURLcode withRetry(const std::atomic<bool>& cancelled, Attempt attempt) {
    auto rc = attempt(0);
    if (rc != CURLE_OK && transient(rc) && !cancelled) rc = attempt(1);
    return rc;
}

void check(CURL* h, CURLcode rc) {
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE)
        throw std::runtime_error(std::string("secure connection failed: ") + curl_easy_strerror(rc) +
                                 ". Check the console's date and time.");
    if (rc != CURLE_OK) throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc));
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw HttpStatusError(status);
}

}  // namespace

CurlHttpClient::CurlHttpClient(std::string userAgent, std::string caBundle)
    : userAgent_(std::move(userAgent)), caBundle_(std::move(caBundle)) {
    static CurlGlobal global;
}

CurlHttpClient::~CurlHttpClient() = default;

std::string CurlHttpClient::get(const std::string& url) {
    Progress prog{&cancelled_};
    std::string body;
    CURL* h = nullptr;
    auto rc = withRetry(cancelled_, [&](int) {
        h = makeHandle(url, userAgent_, caBundle_, prog);
        body.clear();
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, appendToString);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
        return curl_easy_perform(h);
    });
    check(h, rc);
    return body;
}

void CurlHttpClient::download(const std::string& url, const fs::path& dest, const DownloadProgressFn& progress) {
    fs::create_directories(dest.parent_path());
    auto part = dest;
    part += ".part";
    Progress prog{&cancelled_, &progress};
    FILE* f = nullptr;
    auto discard = [&] {
        if (f) std::fclose(f);
        f = nullptr;
        std::error_code ec;
        fs::remove(part, ec);
    };
    CURL* h = nullptr;
    CURLcode rc;
    try {
        // Tried again only when nothing arrived: a half-done big download isn't restarted.
        bool started = false;
        auto attempt = [&](int) {
            discard();
            f = std::fopen(part.string().c_str(), "wb");
            if (!f) throw std::runtime_error("cannot write " + part.string());
            std::setvbuf(f, nullptr, _IOFBF, 1 << 18);  // big buffer: SD card writes are slow in small chunks
            h = makeHandle(url, userAgent_, caBundle_, prog);
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeToFile);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, f);
            auto r = curl_easy_perform(h);
            curl_off_t got = 0;
            curl_easy_getinfo(h, CURLINFO_SIZE_DOWNLOAD_T, &got);
            started = got > 0;
            return r;
        };
        rc = attempt(0);
        if (rc != CURLE_OK && transient(rc) && !started && !cancelled_) rc = attempt(1);
        check(h, rc);
    } catch (...) {
        discard();
        throw;
    }
    if (std::fclose(f) != 0) {
        f = nullptr;
        discard();
        throw std::runtime_error("write failed (SD card full?)");
    }
    std::error_code ec;
    fs::remove(dest, ec);
    fs::rename(part, dest);
}

std::string CurlHttpClient::getRange(const std::string& url, uint64_t from, uint64_t to, std::string* finalUrl) {
    struct State {
        std::string body;
        size_t limit;
    } st{{}, static_cast<size_t>(to - from + 1)};
    Progress prog{&cancelled_};
    auto range = std::to_string(from) + "-" + std::to_string(to);
    CURL* h = nullptr;
    auto rc = withRetry(cancelled_, [&](int) {
        h = makeHandle(url, userAgent_, caBundle_, prog);
        st.body.clear();
        curl_easy_setopt(h, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &st);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char* buf, size_t size, size_t n, void* ud) -> size_t {
            auto* s = static_cast<State*>(ud);
            if (s->body.size() + size * n > s->limit) return 0;  // the whole file is coming: stop
            s->body.append(buf, size * n);
            return size * n;
        });
        return curl_easy_perform(h);
    });
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (rc == CURLE_WRITE_ERROR || (rc == CURLE_OK && status == 200)) throw std::runtime_error("the server doesn't support partial downloads");
    if (rc != CURLE_OK) throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc));
    if (status != 206) throw HttpStatusError(status);
    if (finalUrl) {
        char* effective = nullptr;
        curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective);
        *finalUrl = effective ? effective : url;
    }
    return st.body;
}

int64_t CurlHttpClient::contentLength(const std::string& url) {
    // Ask for the first byte only: "Content-Range: bytes 0-0/<total>" gives the size even from
    // servers whose HEAD replies leave it out (Wix file hosting does). If a server ignores the
    // range, stop reading after the first chunk and use Content-Length.
    struct State {
        int64_t total = -1, length = -1;
        size_t received = 0;
    } st;
    Progress prog{&cancelled_};
    withRetry(cancelled_, [&](int) {
        st = State{};
        CURL* h = makeHandle(url, userAgent_, caBundle_, prog);
        curl_easy_setopt(h, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &st);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, +[](char* buf, size_t size, size_t n, void* ud) -> size_t {
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
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &st);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t n, void* ud) -> size_t {
            auto* s = static_cast<State*>(ud);
            s->received += size * n;
            return s->received > 4096 ? 0 : size * n;  // abort: the server is sending the whole file
        });
        return curl_easy_perform(h);  // an abort above is expected; use whatever headers arrived
    });
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
