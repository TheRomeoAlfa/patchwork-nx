#include "link_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "util.hpp"

namespace app {

namespace {

constexpr size_t kMaxRequest = 8192;

const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Patchwork</title>
<style>body{font-family:system-ui,sans-serif;background:#f5f5f5;color:#1e1e1e;margin:0;padding:24px;max-width:480px}
h1{font-size:20px;margin:0 0 4px}p{color:#555;font-size:14px}
input{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border:1px solid #bbb;border-radius:8px}
button{width:100%;margin-top:12px;font-size:16px;padding:12px;border:0;border-radius:8px;background:#00a3bf;color:#fff}
.ok{background:#e1f5ee;color:#085041;padding:10px;border-radius:8px;font-size:14px;margin-top:16px;word-break:break-all}
.err{background:#fcebeb;color:#791f1f;padding:10px;border-radius:8px;font-size:14px;margin-top:16px}
ul{color:#666;font-size:13px;padding-left:18px}</style></head><body>
<h1>Patchwork</h1><p>Send a mod to your Switch</p>
<form method="post" action="/send"><input name="url" type="url" placeholder="Paste a link…" required autofocus>
<button type="submit">Send to Switch</button></form>%STATUS%
<ul><li>Direct links to .zip, .7z or .rar files</li><li>Waikuteru mod pages</li><li>GameBanana mod pages</li></ul>
<p>Then look at your Switch: nothing is installed until you confirm there.</p></body></html>)HTML";

std::string page(const std::string& status) {
    std::string html = kPage;
    auto pos = html.find("%STATUS%");
    html.replace(pos, 8, status);
    return html;
}

std::string escapeHtml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

void sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void respond(int fd, const char* status, const std::string& body) {
    std::string head = std::string("HTTP/1.1 ") + status +
                       "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n";
    sendAll(fd, head + body);
}

bool waitReadable(int fd, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
}

}  // namespace

std::string localIpAddress() {
#ifdef __SWITCH__
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) return "";
    in_addr a{};
    a.s_addr = ip;
    return inet_ntoa(a);
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "";
    std::string found;
    for (auto* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
        found = buf;
        break;
    }
    freeifaddrs(list);
    return found;
#endif
}

LinkServer::~LinkServer() { stop(); }

bool LinkServer::start(std::function<void(const std::string&)> onLink) {
    stop();
    onLink_ = std::move(onLink);
    auto ip = localIpAddress();
    if (ip.empty()) {
        error_ = "This Switch isn't connected to a network.";
        return false;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        error_ = "Couldn't open a network socket.";
        return false;
    }
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    uint16_t port = 0;
    for (uint16_t p = 8080; p < 8090 && !port; ++p) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(p);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) port = p;
    }
    if (!port || ::listen(fd_, 4) != 0) {
        ::close(fd_);
        fd_ = -1;
        error_ = "Couldn't start the link page (ports 8080-8089 are busy).";
        return false;
    }
    address_ = ip + ":" + std::to_string(port);
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void LinkServer::stop() {
    if (!running_) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    running_ = false;
}

void LinkServer::loop() {
    while (!stop_) {
        if (!waitReadable(fd_, 250)) continue;  // wake up regularly to notice stop()
        int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0) continue;
        handle(client);
        ::close(client);
    }
}

void LinkServer::handle(int client) {
    // Read headers (and a small body) with a deadline, so a stuck connection can't block us.
    std::string req;
    char buf[1024];
    size_t contentLength = 0;
    size_t headerEnd = std::string::npos;
    while (req.size() < kMaxRequest && waitReadable(client, 3000)) {
        auto n = ::recv(client, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
        if (headerEnd == std::string::npos && (headerEnd = req.find("\r\n\r\n")) != std::string::npos) {
            auto lower = mm::toLower(req.substr(0, headerEnd));
            auto cl = lower.find("content-length:");
            if (cl != std::string::npos) contentLength = std::strtoul(lower.c_str() + cl + 15, nullptr, 10);
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLength) break;
    }
    if (headerEnd == std::string::npos) return;

    if (req.rfind("POST /send", 0) == 0) {
        auto body = req.substr(headerEnd + 4);
        std::string url;
        for (size_t start = 0; start <= body.size();) {
            auto end = body.find('&', start);
            auto pair = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (pair.rfind("url=", 0) == 0) url = mm::urlDecode(pair.substr(4));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        while (!url.empty() && (url.back() == ' ' || url.back() == '\n' || url.back() == '\r')) url.pop_back();
        while (!url.empty() && url.front() == ' ') url.erase(0, 1);
        bool web = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
        if (!web || url.size() > 2048) {
            respond(client, "200 OK", page("<div class=\"err\">That doesn't look like a web link (it should start with https://).</div>"));
            return;
        }
        if (onLink_) onLink_(url);
        respond(client, "200 OK", page("<div class=\"ok\">Sent: " + escapeHtml(url) + "<br>Check your Switch.</div>"));
        return;
    }
    if (req.rfind("GET / ", 0) == 0 || req.rfind("GET /?", 0) == 0) {
        respond(client, "200 OK", page(""));
        return;
    }
    respond(client, "404 Not Found", page(""));
}

}  // namespace app
