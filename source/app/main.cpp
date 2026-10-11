#include <borealis.hpp>

#include <cstdlib>
#include <cstring>

#include "app_state.hpp"
#include "debug_script.hpp"
#include "main_activity.hpp"
#include "platform.hpp"

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        if (std::strcmp(argv[i], "-v") == 0) brls::Application::enableDebuggingView(true);
    }

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }
    brls::Application::createWindow(APP_TITLE);
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);  // + quits

    app::platform::init();
    app::AppState::get().init();

    brls::Application::pushActivity(new app::MainActivity());
    app::debug::startScriptFromEnv();

    while (brls::Application::mainLoop()) app::debug::afterFrame();

    app::AppState::get().shutdown();
    app::platform::exit();
    return EXIT_SUCCESS;
}
