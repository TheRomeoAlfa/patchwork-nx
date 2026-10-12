#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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

    // onLink runs on the server thread; hop to the UI thread with brls::sync.
    bool start(std::function<void(const std::string& url)> onLink);
    void stop();
    bool running() const { return running_; }
    const std::string& address() const { return address_; }  // "192.168.1.20:8080"
    const std::string& error() const { return error_; }

private:
    void loop();
    void handle(int client);

    std::function<void(const std::string&)> onLink_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    int fd_ = -1;
    std::string address_, error_;
};

// The console's (or computer's) address on the local network, or "" if not connected.
std::string localIpAddress();

}  // namespace app
