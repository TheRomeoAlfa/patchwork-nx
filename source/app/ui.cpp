#include "ui.hpp"

#include "app_state.hpp"

#include <algorithm>

namespace app::ui {

FocusSink::FocusSink() {
    setFocusable(true);
    setHideHighlight(true);
    setHideClickAnimation(true);
    setDimensions(0, 0);
}

void FocusSink::onFocusLost() {
    brls::Box::onFocusLost();
    parked_ = false;
}

void FocusSink::park() {
    parked_ = true;
    brls::Application::giveFocus(this);
}

bool focusInside(brls::View* container) {
    for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
        if (v == container) return true;
    return false;
}

bool focusParked() { return dynamic_cast<FocusSink*>(brls::Application::getCurrentFocus()) != nullptr; }

void parkFocusIfInside(brls::View* container, FocusSink* sink) {
    if (sink && focusInside(container)) sink->park();
}

bool tryFocus(brls::View* target) {
    if (!target || !target->getDefaultFocus()) return false;
    brls::Application::giveFocus(target);
    return true;
}

ProgressBar::ProgressBar(float width) : brls::Box(brls::Axis::ROW), width_(width) {
    setWidth(width);
    setMaxWidthPercentage(100);
    setHeight(10);
    setCornerRadius(5);
    setBackgroundColor(line());
    fill_ = new brls::Box();
    fill_->setCornerRadius(5);
    fill_->setBackgroundColor(accent());
    fill_->setWidth(0);
    fill_->setShrink(1);
    addView(fill_);
}

void ProgressBar::setProgress(float fraction) {
    // Pixel widths, not percentages: the fill must never be wider than the track.
    if (!(fraction >= 0.0f)) fraction = 0.0f;  // also catches NaN
    float track = getWidth() > 0 ? std::min(getWidth(), width_) : width_;
    fill_->setWidth(std::clamp(fraction, 0.0f, 1.0f) * track);
}

brls::Label* label(const std::string& text, float size, NVGcolor color, bool singleLine) {
    auto* l = new brls::Label();
    l->setFontSize(size);
    l->setTextColor(color);
    l->setSingleLine(singleLine);
    if (!singleLine) l->setIsWrapping(true);
    l->setText(text);
    return l;
}

brls::Box* hbox() {
    auto* b = new brls::Box(brls::Axis::ROW);
    b->setAlignItems(brls::AlignItems::CENTER);
    return b;
}

brls::Box* vbox() {
    auto* b = new brls::Box(brls::Axis::COLUMN);
    return b;
}

brls::View* grow() {
    auto* v = new brls::Box();
    v->setGrow(1.0f);
    return v;
}

void tappable(brls::View* view, Tap mode) {
    auto wasFocused = std::make_shared<bool>(false);
    auto tracking = std::make_shared<bool>(false);
    view->addGestureRecognizer(new brls::TapGestureRecognizer([view, mode, wasFocused, tracking](brls::TapGestureStatus status, brls::Sound* sound) {
        if (AppState::get().installBusy()) {
            *tracking = false;
            return;
        }
        switch (status.state) {
            case brls::GestureState::UNSURE:
            case brls::GestureState::START:  // touch down (START while the list is still scrolling)
                if (!*tracking) {
                    *tracking = true;
                    *wasFocused = brls::Application::getCurrentFocus() == view;
                }
                break;
            case brls::GestureState::END: {
                *tracking = false;
                bool act = mode == Tap::Act || *wasFocused;
                brls::Application::giveFocus(view);
                if (!act) {
                    *sound = brls::SOUND_FOCUS_CHANGE;
                    break;
                }
                // Like pressing A: the nearest view up the chain that handles A (some pages
                // register it on the list rather than on each row).
                for (auto* v = view; v; v = v->getParent()) {
                    for (auto& action : v->getActions()) {
                        if (action->getType() != brls::ACTION_GAMEPAD || action->getButton() != brls::BUTTON_A || !action->isAvailable()) continue;
                        view->playClickAnimation();
                        if (action->getActionListener()(v)) *sound = action->getSound();
                        return;
                    }
                }
                break;
            }
            default:
                *tracking = false;
                break;
        }
    }));
}

brls::Box* focusRow() {
    auto* b = hbox();
    b->setFocusable(true);
    b->setBackgroundColor(rowBg());
    b->setCornerRadius(4);
    b->setHighlightCornerRadius(6);
    b->setPadding(8, 12, 8, 12);
    b->setMarginBottom(6);
    return b;
}

}  // namespace app::ui
