#include "my_mods_page.hpp"

#include <ctime>

#include "app_state.hpp"
#include "compat.hpp"
#include "ui.hpp"

namespace app {

MyModsPage::MyModsPage() : brls::Box(brls::Axis::ROW) {
    setGrow(1);

    auto* left = ui::vbox();
    left->setWidth(500);
    left->setShrink(0);
    left->setPadding(20, 14, 0, 16);
    left->setLineRight(1);
    left->setLineColor(ui::line());
    auto* header = ui::hbox();
    header->setMargins(0, 6, 10, 6);
    header->addView(ui::label("Load order", 22, ui::text()));
    header->addView(ui::grow());
    count_ = ui::label("top of the list wins", 15, ui::muted());
    header->addView(count_);
    left->addView(header);

    list_ = ui::vbox();
    scroll_ = new brls::ScrollingFrame();
    scroll_->setGrow(1);
    scroll_->setContentView(list_);
    left->addView(scroll_);
    addView(left);

    pane_ = ui::vbox();
    pane_->setGrow(1);
    pane_->setPadding(24, 30, 20, 30);
    pane_->setBackgroundColor(ui::panelBg());
    addView(pane_);

    libSub_ = AppState::get().libraryChanged()->subscribe([this] {
        if (!busy_) rebuild();
    });
    rebuild();
}

MyModsPage::~MyModsPage() {
    *alive_ = false;
    AppState::get().libraryChanged()->unsubscribe(libSub_);
}

brls::View* MyModsPage::getDefaultFocus() { return list_->getDefaultFocus(); }

void MyModsPage::rebuild() {
    auto& st = AppState::get();
    auto mods = st.mods(st.game());
    bool hadFocus = false;
    for (auto* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
        if (v == this) hadFocus = true;

    list_->clearViews();
    pane_->clearViews();

    if (mods.empty()) {
        auto* row = ui::focusRow();
        row->addView(ui::label(ui::icon::browse, 24, ui::accent()));
        auto* l = ui::label("No mods yet. Open Browse to find some.", 17, ui::text());
        l->setMarginLeft(12);
        row->addView(l);
        list_->addView(row);
        pane_->addView(ui::label("Mods you install show up here. You can turn each one on or off, and choose which wins when two mods change the same file.", 18, ui::muted(), false));
        if (hadFocus) brls::Application::giveFocus(row);
        return;
    }

    std::set<std::string> conflicted;
    for (const auto& c : st.snapshot().conflicts.at(st.game()))
        for (const auto& id : c.modIds) conflicted.insert(id);
    for (const auto& o : st.snapshot().overlaps.at(st.game()))
        for (const auto& id : o.modIds) conflicted.insert(id);

    brls::View* focusTarget = nullptr;
    int n = 1;
    for (const auto* m : mods) {
        std::string id = m->id;
        auto* row = ui::focusRow();
        row->addView(ui::label(std::to_string(n++), 15, ui::muted()));
        auto* name = ui::label(m->name, 18, m->enabled ? ui::text() : ui::muted());
        name->setMarginLeft(14);
        name->setGrow(1);
        name->setShrink(1);
        row->addView(name);
        if (conflicted.count(id) && m->enabled) row->addView(ui::label(ui::icon::warning, 22, ui::warn()));
        // On/off pill
        auto* pill = ui::hbox();
        pill->setJustifyContent(brls::JustifyContent::CENTER);
        pill->setWidth(64);
        pill->setHeight(30);
        pill->setCornerRadius(15);
        pill->setMarginLeft(10);
        pill->setBackgroundColor(m->enabled ? ui::accent() : ui::line());
        pill->addView(ui::label(m->enabled ? "On" : "Off", 15, m->enabled ? nvgRGB(0x10, 0x28, 0x2d) : ui::muted()));
        row->addView(pill);

        row->getFocusEvent()->subscribe([this, id](brls::View*) {
            focusedId_ = id;
            showDetails(id);
        });
        row->registerClickAction([this, id](brls::View*) {
            toggle(id);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, m->enabled ? "Turn off" : "Turn on");
        row->registerAction("Move up", brls::BUTTON_LB, [this, id](brls::View*) {
            move(id, +1);
            return true;
        });
        row->registerAction("Move down", brls::BUTTON_RB, [this, id](brls::View*) {
            move(id, -1);
            return true;
        });
        row->registerAction("Remove", brls::BUTTON_X, [this, id](brls::View*) {
            remove(id);
            return true;
        });
        list_->addView(row);
        if (id == focusedId_ || !focusTarget) focusTarget = row;
    }
    if (hadFocus && focusTarget) brls::Application::giveFocus(focusTarget);
    showDetails(focusedId_.empty() ? mods.front()->id : focusedId_);
}

void MyModsPage::showDetails(const std::string& id) {
    auto& st = AppState::get();
    const auto* m = st.findMod(id);
    pane_->clearViews();
    if (!m) return;

    pane_->addView(ui::label(m->name, 26, ui::text()));
    std::string meta = (m->author.empty() ? "" : "by " + m->author + " · ") + std::to_string(m->files.size()) + " files · " +
                       (m->enabled ? "on" : "off");
    auto* metaL = ui::label(meta, 16, ui::muted());
    metaL->setMargins(4, 0, 16, 0);
    pane_->addView(metaL);

    auto addNote = [this](const std::string& text, NVGcolor color, NVGcolor bg) {
        auto* box = ui::hbox();
        box->setBackgroundColor(bg);
        box->setCornerRadius(4);
        box->setPadding(10, 12, 10, 12);
        box->setMarginBottom(8);
        auto* l = ui::label(text, 16, color, false);
        l->setGrow(1);
        l->setShrink(1);
        box->addView(l);
        pane_->addView(box);
    };
    auto nameOf = [&st](const std::string& modId) {
        auto* other = st.findMod(modId);
        return other ? other->name : modId;
    };

    // Conflicts: who this mod overrides and who overrides it.
    std::map<std::string, int> overrides, overriddenBy;
    for (const auto& c : st.snapshot().conflicts.at(m->game)) {
        auto pos = std::find(c.modIds.begin(), c.modIds.end(), id);
        if (pos == c.modIds.end()) continue;
        if (pos == c.modIds.begin())
            for (auto it = c.modIds.begin() + 1; it != c.modIds.end(); ++it) overrides[*it]++;
        else
            overriddenBy[c.modIds.front()]++;
    }
    auto files = [](int n) { return std::to_string(n) + (n == 1 ? " file" : " files"); };
    for (auto& [other, n] : overrides)
        addNote(std::string(ui::icon::warning) + "  Overrides " + files(n) + " from " + nameOf(other) + ". Move " + nameOf(other) + " above it if you want that mod to win.", ui::warn(), ui::warnBg());
    for (auto& [other, n] : overriddenBy)
        addNote(std::string(ui::icon::warning) + "  " + files(n) + " replaced by " + nameOf(other) + ", which is higher in the list.", ui::warn(), ui::warnBg());
    for (const auto& o : st.snapshot().overlaps.at(m->game)) {
        if (std::find(o.modIds.begin(), o.modIds.end(), id) == o.modIds.end()) continue;
        std::string names;
        for (const auto& other : o.modIds)
            if (other != id) names += (names.empty() ? "" : ", ") + nameOf(other);
        addNote(std::string(ui::icon::warning) + "  Also patches the game code like " + names + ". Using both may crash the game.", ui::warn(), ui::warnBg());
    }

    const auto& installed = st.gameVersion(m->game);
    if (mm::checkCompat(installed, m->gameVersionsNamed) == mm::Compat::Mismatch) {
        std::string named;
        for (const auto& v : m->gameVersionsNamed) named += (named.empty() ? "" : ", ") + v;
        addNote(std::string(ui::icon::warning) + "  Made for game " + named + ". You have " + installed + ".", ui::warn(), ui::warnBg());
    }

    char date[32] = "";
    std::time_t t = static_cast<std::time_t>(m->installedAt);
    if (t) std::strftime(date, sizeof date, "%b %d, %Y", std::localtime(&t));
    std::string info = "Installed " + std::string(date);
    if (!m->sourceFile.empty()) info += "\nFile: " + m->sourceFile;
    if (!m->version.empty()) info += "\nMod version: " + m->version;
    if (m->variants.size() > 1 || (m->variants.size() == 1 && m->variants[0] != "Main")) {
        info += "\nOptions:";
        for (const auto& v : m->variants) info += " " + v + ";";
        info.pop_back();
    }
    auto* infoL = ui::label(info, 16, ui::muted(), false);
    infoL->setMarginTop(8);
    pane_->addView(infoL);
}

void MyModsPage::runJob(const std::string& status, std::function<void(mm::Library&)> job) {
    if (busy_) return;
    busy_ = true;
    brls::Application::blockInputs();
    pane_->clearViews();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->addView(new brls::ProgressSpinner());
    auto* l = ui::label(status, 20, ui::text());
    l->setMarginTop(16);
    box->addView(l);
    pane_->addView(box);

    auto alive = alive_;
    AppState::get().runFileJob(std::move(job), [this, alive](const std::string& error) {
        brls::Application::unblockInputs();
        if (!*alive) return;
        busy_ = false;
        if (!error.empty()) brls::Application::notify("Something went wrong: " + error);
        rebuild();
    });
}

void MyModsPage::toggle(const std::string& id) {
    const auto* m = AppState::get().findMod(id);
    if (!m) return;
    bool enable = !m->enabled;
    runJob(enable ? "Turning on " + m->name + "…" : "Turning off " + m->name + "…",
           [id, enable](mm::Library& lib) { lib.setEnabled(id, enable); });
}

void MyModsPage::move(const std::string& id, int delta) {
    runJob("Updating load order…", [id, delta](mm::Library& lib) { lib.move(id, delta); });
}

void MyModsPage::remove(const std::string& id) {
    const auto* m = AppState::get().findMod(id);
    if (!m) return;
    auto* dialog = new brls::Dialog("Remove " + m->name + "? Its files are deleted from the SD card. You can download it again from Browse.");
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Remove", [this, id, name = m->name] {
        if (focusedId_ == id) focusedId_.clear();
        runJob("Removing " + name + "…", [id](mm::Library& lib) { lib.remove(id); });
    });
    dialog->open();
}

}  // namespace app
