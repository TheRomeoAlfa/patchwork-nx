#include "debug_script.hpp"

#ifndef __SWITCH__

#include <borealis.hpp>
#include <glad/glad.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace app::debug {

namespace {

std::vector<std::string> steps;
size_t next = 0;
std::string pendingShot;

void writePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba) {
    std::string raw;
    raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
    for (int y = h - 1; y >= 0; --y) {  // GL rows are bottom-up
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const auto* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            raw.append(reinterpret_cast<const char*>(p), 3);
        }
    }
    uLongf len = compressBound(raw.size());
    std::string z(len, '\0');
    compress2(reinterpret_cast<Bytef*>(z.data()), &len, reinterpret_cast<const Bytef*>(raw.data()), raw.size(), 6);
    z.resize(len);
    auto be = [](uint32_t v) {
        std::string s(4, '\0');
        for (int i = 0; i < 4; ++i) s[i] = static_cast<char>(v >> (24 - 8 * i));
        return s;
    };
    auto chunk = [&](const std::string& type, const std::string& data) {
        std::string td = type + data;
        return be(static_cast<uint32_t>(data.size())) + td + be(static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(td.data()), td.size())));
    };
    std::string ihdr = be(w) + be(h) + std::string("\x08\x02\x00\x00\x00", 5);
    std::ofstream out(path, std::ios::binary);
    out << std::string("\x89PNG\r\n\x1a\n", 8) << chunk("IHDR", ihdr) << chunk("IDAT", z) << chunk("IEND", "");
}

void runNext() {
    while (next < steps.size()) {
        auto step = steps[next++];
        char op = step[0];
        auto arg = step.substr(1);
        if (op == 'w') {
            brls::delay(std::stol(arg), runNext);
            return;
        }
        if (op == 's') {
            pendingShot = arg;
            return;  // afterFrame() continues the script once the shot is taken
        }
        if (op == 'q') {
            brls::Application::quit();
            return;
        }
        if (op == 't') {
            // Tap at x,y (1280x720 layout coordinates): touch down, then up 80 ms later.
            auto comma = arg.find(',');
            brls::Point pos{std::stof(arg.substr(0, comma)), std::stof(arg.substr(comma + 1))};
            auto stack = brls::Application::getActivitiesStack();
            if (stack.empty()) return runNext();
            auto* view = stack.back()->getContentView()->hitTest(pos);
            if (!view) {
                brls::Logger::info("tap {}: nothing there", arg);
                return runNext();
            }
            brls::TouchState touch;
            touch.position = pos;
            touch.view = view;
            touch.phase = brls::TouchPhase::START;
            view->gestureRecognizerRequest(touch, brls::MouseState(), view);
            brls::delay(80, [arg, view, touch]() mutable {
                touch.phase = brls::TouchPhase::END;
                view->gestureRecognizerRequest(touch, brls::MouseState(), view);
                brls::delay(150, [arg] {
                    std::string chain;
                    for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
                        chain += v->describe() + " < ";
                    brls::Logger::info("after tap {}: focus = {}", arg, chain);
                    runNext();
                });
            });
            return;
        }
        if (op == 'k') {
            static const std::map<std::string, brls::ControllerButton> buttons = {
                {"up", brls::BUTTON_NAV_UP},       {"down", brls::BUTTON_NAV_DOWN}, {"left", brls::BUTTON_NAV_LEFT},
                {"right", brls::BUTTON_NAV_RIGHT}, {"a", brls::BUTTON_A},           {"b", brls::BUTTON_B},
                {"x", brls::BUTTON_X},             {"y", brls::BUTTON_Y},           {"l", brls::BUTTON_LB},
                {"r", brls::BUTTON_RB},            {"zl", brls::BUTTON_LT},         {"zr", brls::BUTTON_RT},
                {"plus", brls::BUTTON_START},      {"minus", brls::BUTTON_BACK},
            };
            auto it = buttons.find(arg);
            if (it != buttons.end()) brls::Application::onControllerButtonPressed(it->second, false);
            brls::delay(150, [arg] {
                std::string chain;
                for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
                    chain += v->describe() + " < ";
                brls::Logger::info("after key {}: focus = {}", arg, chain);
                runNext();
            });
            return;
        }
    }
}

}  // namespace

void startScriptFromEnv() {
    const char* s = std::getenv("MM_DEBUG_SCRIPT");
    if (!s) return;
    std::stringstream ss(s);
    std::string step;
    while (ss >> step) steps.push_back(step);
    brls::delay(100, runNext);
}

void afterFrame() {
    if (pendingShot.empty()) return;
    // The frame was just presented; read it back from the front buffer.
    int w = 0, h = 0;
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    w = vp[2];
    h = vp[3];
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glReadBuffer(GL_BACK);
    const char* dir = std::getenv("MM_SHOT_DIR");
    writePng(std::string(dir ? dir : ".") + "/" + pendingShot + ".png", w, h, rgba);
    pendingShot.clear();
    runNext();
}

}  // namespace app::debug

#else

namespace app::debug {
void startScriptFromEnv() {}
void afterFrame() {}
}  // namespace app::debug

#endif
