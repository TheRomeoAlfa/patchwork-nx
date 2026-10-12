#include "ui.hpp"

#include <algorithm>

namespace app::ui {

ProgressBar::ProgressBar() : brls::Box(brls::Axis::ROW) {
    setHeight(10);
    setCornerRadius(5);
    setBackgroundColor(line());
    fill_ = new brls::Box();
    fill_->setCornerRadius(5);
    fill_->setBackgroundColor(accent());
    fill_->setWidthPercentage(0);
    addView(fill_);
}

void ProgressBar::setProgress(float fraction) {
    fill_->setWidthPercentage(std::clamp(fraction, 0.0f, 1.0f) * 100.0f);
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
