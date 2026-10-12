#pragma once
// Shared look: colors, sizes and small view-building helpers.

#include <borealis.hpp>

#include <string>

namespace app::ui {

// Palette (dark, Switch-like with the system cyan as the accent).
inline NVGcolor bg() { return nvgRGB(0x2b, 0x2b, 0x2b); }
inline NVGcolor sidebarBg() { return nvgRGB(0x24, 0x24, 0x24); }
inline NVGcolor panelBg() { return nvgRGB(0x32, 0x32, 0x32); }
inline NVGcolor rowBg() { return nvgRGB(0x30, 0x30, 0x30); }
inline NVGcolor line() { return nvgRGB(0x3b, 0x3b, 0x3b); }
inline NVGcolor text() { return nvgRGB(0xea, 0xea, 0xea); }
inline NVGcolor muted() { return nvgRGB(0x9a, 0x9a, 0x9a); }
inline NVGcolor accent() { return nvgRGB(0x00, 0xc3, 0xe3); }
inline NVGcolor accentDim() { return nvgRGB(0x1f, 0x3a, 0x40); }
inline NVGcolor ok() { return nvgRGB(0x5d, 0xca, 0xa5); }
inline NVGcolor warn() { return nvgRGB(0xfa, 0xc7, 0x75); }
inline NVGcolor warnBg() { return nvgRGB(0x3a, 0x30, 0x20); }
inline NVGcolor thumbBg() { return nvgRGB(0x3a, 0x3a, 0x3a); }

// Material Icons code points (rendered through Borealis' icon fallback font).
namespace icon {
inline constexpr const char* browse = "\uE80B";    // public
inline constexpr const char* search = "\uE8B6";    // search
inline constexpr const char* myMods = "\uE896";    // list
inline constexpr const char* settings = "\uE8B8";  // settings
inline constexpr const char* game = "\uE338";      // sports_esports
inline constexpr const char* check = "\uE86C";     // check_circle
inline constexpr const char* warning = "\uE002";   // warning
inline constexpr const char* pc = "\uE30A";        // computer
inline constexpr const char* heart = "\uE87D";     // favorite
inline constexpr const char* file = "\uE24D";      // insert_drive_file
inline constexpr const char* boxOn = "\uE834";     // check_box
inline constexpr const char* boxOff = "\uE835";    // check_box_outline_blank
inline constexpr const char* image = "\uE3F4";     // image
inline constexpr const char* folder = "\uE2C7";    // folder
inline constexpr const char* add = "\uE2C4";       // file_download
}  // namespace icon

// Borealis keeps pointing at the focused view after it's deleted, and crashes drawing its
// highlight on the next frame. Before clearing anything that may contain focus, park focus
// on a FocusSink that outlives the cleared views; move it back once the new views exist.
// A sink only accepts focus while parked, so navigation never lands on it.
class FocusSink : public brls::Box {
public:
    FocusSink();
    brls::View* getDefaultFocus() override { return parked_ ? this : nullptr; }
    void onFocusLost() override;
    void park();

private:
    bool parked_ = false;
};

bool focusInside(brls::View* container);
bool focusParked();  // focus is on some FocusSink, waiting for content
// Park focus on `sink` if it is currently inside `container`.
void parkFocusIfInside(brls::View* container, FocusSink* sink);
// Give focus to `target` if it can take it; returns false otherwise.
bool tryFocus(brls::View* target);

// Thin horizontal bar showing 0..1 progress.
class ProgressBar : public brls::Box {
public:
    explicit ProgressBar(float width);
    void setProgress(float fraction);

private:
    float width_;
    brls::Box* fill_;
};

// A full-pane "please wait" view that holds focus and swallows every button (B and + say
// `pleaseWait`) until it is removed, so nothing else can start while files are changing.
struct BusyBox {
    brls::Box* box;
    brls::Label* status;
    ProgressBar* bar;     // hidden until the first update
    brls::Label* count;   // "3 / 10 files"
    void update(size_t done, size_t total);
};
BusyBox busyBox(const std::string& status, const std::string& hint, const std::string& pleaseWait);

brls::Label* label(const std::string& text, float size, NVGcolor color, bool singleLine = true);
brls::Box* hbox();
brls::Box* vbox();
// Spacer that eats the remaining room in a flex row/column.
brls::View* grow();
// A focusable row with the app's highlight style.
brls::Box* focusRow();

// Touch support: Borealis views only react to taps if they carry a tap recognizer.
// Lists work like the HOME menu: the first tap selects (shows the preview), a second tap
// on the selected item presses A. Toggles and navigation act on the first tap.
// Taps are ignored while an install or file job holds the screen.
enum class Tap { SelectThenAct, Act };
void tappable(brls::View* view, Tap mode = Tap::SelectThenAct);

}  // namespace app::ui
