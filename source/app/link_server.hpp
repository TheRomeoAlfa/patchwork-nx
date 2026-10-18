#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace app {

// A tiny web page served from the Switch while the "Send a link" screen is open: a phone
// on the same Wi-Fi opens http://<switch-ip>:<port>, pastes a link and taps Send.
// It only accepts a single form field (the link, max 2 KB) and never acts on it by itself:
// the link is shown on the Switch and nothing happens until the user presses A there.
class LinkServer {
public:
    ~LinkServer();

    // onLink and onVisit run on the server thread; hop to the UI thread with brls::sync.
    // onVisit fires each time a phone opens the page.
    bool start(std::function<void(const std::string& url)> onLink, std::function<void()> onVisit = {});
    void stop();
    bool running() const { return running_; }
    // "192.168.1.20:8080/k7q2xa": the page only answers under its random code, so only
    // someone who can see the Switch's screen can send a link.
    const std::string& address() const { return address_; }
    const std::string& error() const { return error_; }  // why start() failed
    int visits() const { return visits_; }
    std::string status() const;  // last network error while running, or ""

private:
    void loop();
    void handle(int client, const std::string& request, size_t headerEnd);
    void noteError(const char* what, int err);

    std::function<void(const std::string&)> onLink_;
    std::function<void()> onVisit_;
    std::atomic<int> visits_{0};
    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    int fd_ = -1;
    std::string address_, error_, token_;
};

// The console's (or computer's) address on the local network, or "" if not connected.
std::string localIpAddress();

}  // namespace app
