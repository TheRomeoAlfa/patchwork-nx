# Three ways to build:
#
#   Core + unit tests + mmcli dev tool (host):
#     cmake -S . -B build && cmake --build build && ./build/mmtests
#
#   The app on macOS/Linux (for UI development):
#     cmake -S . -B build-desktop -DPLATFORM_DESKTOP=ON && cmake --build build-desktop
#     ./build-desktop/patchwork          (MM_SD_ROOT=<folder> to choose the fake SD card)
#
#   The app for Switch (.nro):
#     DEVKITPRO=/opt/devkitpro cmake -S . -B build-switch -DPLATFORM_SWITCH=ON
#     cmake --build build-switch --target patchwork.nro
cmake_minimum_required(VERSION 3.15)

set(APP_BUILD OFF)
if(PLATFORM_DESKTOP OR PLATFORM_SWITCH)
    set(APP_BUILD ON)
    set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library)
    include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
    option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
    cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
    cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
    cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)
    include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)
endif()

project(patchwork VERSION 0.3.1 LANGUAGES C CXX)
set(APP_TITLE "Patchwork")
set(APP_AUTHOR "TheRomeoAlfa")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- core library
file(GLOB CORE_SOURCES source/core/*.cpp)
add_library(mmcore STATIC ${CORE_SOURCES})
target_include_directories(mmcore PUBLIC source/core source/third_party)
target_compile_options(mmcore PRIVATE -Wall -Wextra)

if(NINTENDO_SWITCH OR PLATFORM_SWITCH)
    # Static portlibs; list private deps explicitly (pkg-config may see host .pc files).
    target_include_directories(mmcore PUBLIC $ENV{DEVKITPRO}/portlibs/switch/include)
    target_link_directories(mmcore PUBLIC $ENV{DEVKITPRO}/portlibs/switch/lib)
    target_link_libraries(mmcore PUBLIC curl mbedtls mbedx509 mbedcrypto archive zstd lz4 lzma bz2 expat z)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(mmcore PUBLIC PkgConfig::LIBARCHIVE CURL::libcurl ZLIB::ZLIB)
endif()

# ---------------------------------------------------------------- host tools
if(NOT APP_BUILD)
    if(NINTENDO_SWITCH)
        # Console smoke test (aarch64-none-elf-cmake without PLATFORM_SWITCH).
        add_executable(mm_smoke tools/switch_smoke.cpp)
        target_link_libraries(mm_smoke PRIVATE mmcore)
        nx_generate_nacp(OUTPUT mm_smoke.nacp NAME "Patchwork Smoke Test" AUTHOR ${APP_AUTHOR} VERSION ${PROJECT_VERSION})
        nx_create_nro(mm_smoke NACP mm_smoke.nacp)
    else()
        file(GLOB TEST_SOURCES tests/*.cpp)
        add_executable(mmtests ${TEST_SOURCES})
        target_link_libraries(mmtests PRIVATE mmcore)
        target_compile_definitions(mmtests PRIVATE MM_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
        enable_testing()
        add_test(NAME mmtests COMMAND mmtests)

        add_executable(mmcli tools/mmcli.cpp)
        target_link_libraries(mmcli PRIVATE mmcore)
        add_executable(survey tools/survey.cpp)
        target_link_libraries(survey PRIVATE mmcore)
    endif()
    return()
endif()

# ---------------------------------------------------------------- the app
add_subdirectory(library/borealis/library)
find_package(Threads REQUIRED)
# Yoga is built with -fno-rtti, which on macOS gives it a private copy of std::exception's
# typeinfo that the linker then uses for our catch clauses too: errors thrown by the
# system C++ library (e.g. std::filesystem) became uncatchable and crashed the app.
if(TARGET yogacore)
    target_compile_options(yogacore PRIVATE -frtti)
endif()

# Resources: Borealis' icon font (+ the UI font on desktop) plus our own files.
set(APP_RESOURCES ${CMAKE_BINARY_DIR}/resources)
set(BRLS_RES ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/resources)
add_custom_target(app_resources
    COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_RESOURCES}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/material ${APP_RESOURCES}/material
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/i18n/en-US ${APP_RESOURCES}/i18n/en-US
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/img/sys ${APP_RESOURCES}/img/sys
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources ${APP_RESOURCES}
)
if(PLATFORM_DESKTOP)
    # The Switch uses its built-in system font; desktop needs a copy.
    add_custom_command(TARGET app_resources POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/font ${APP_RESOURCES}/font)
endif()

file(GLOB_RECURSE APP_SOURCES source/app/*.cpp)
list(APPEND APP_SOURCES source/third_party/qrcodegen/qrcodegen.cpp)
set(APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})
if(PLATFORM_DESKTOP)
    pkg_check_modules(LIBWEBP REQUIRED IMPORTED_TARGET libwebp)  # GameBanana serves some previews as WebP
    list(APPEND APP_PLATFORM_LIB PkgConfig::LIBWEBP)
endif()
if(APPLE AND PLATFORM_DESKTOP)
    list(APPEND APP_PLATFORM_LIB "-framework CoreWLAN" z)  # Borealis' Wi-Fi status on macOS; zlib for debug screenshots
endif()
if(PLATFORM_SWITCH)
    list(APPEND APP_SOURCES ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
    list(APPEND APP_PLATFORM_LIB webp glfw3 EGL glapi drm_nouveau nx m)
endif()

program_target(patchwork "${APP_SOURCES}")
add_dependencies(patchwork app_resources)
target_include_directories(patchwork PRIVATE source/app)
target_compile_options(patchwork PRIVATE -ffunction-sections -fdata-sections -Wall)
target_compile_definitions(patchwork PRIVATE APP_VERSION="${PROJECT_VERSION}" APP_TITLE="${APP_TITLE}")
target_link_libraries(patchwork PRIVATE mmcore borealis ${APP_PLATFORM_LIB})

if(PLATFORM_SWITCH)
    set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon.jpg)
    add_custom_target(patchwork.nro DEPENDS patchwork app_resources
        COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${APP_AUTHOR}" "${PROJECT_VERSION}" patchwork.nacp
        COMMAND ${NX_ELF2NRO_EXE} patchwork.elf patchwork.nro --icon=${APP_ICON} --nacp=patchwork.nacp --romfsdir=${APP_RESOURCES}
    )
endif()
