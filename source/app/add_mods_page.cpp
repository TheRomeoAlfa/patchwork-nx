#include "add_mods_page.hpp"

#include <algorithm>
#include <ctime>

#include "app_state.hpp"
#include "archive.hpp"
#include "install_flow.hpp"
#include "qrcodegen/qrcodegen.hpp"

namespace app {

namespace {

// Draws a QR code for a short text (the link page's address).
class QrView : public brls::Box {
public:
    explicit QrView(const std::string& text) : qr_(qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM)) {
        setDimensions(176, 176);
        setShrink(0);
    }
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 8);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);
        int n = qr_.getSize();
        float quiet = 10, cell = (std::min(width, height) - 2 * quiet) / n;
        nvgBeginPath(vg);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                if (qr_.getModule(c, r)) nvgRect(vg, x + quiet + c * cell, y + quiet + r * cell, cell + 0.5f, cell + 0.5f);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        nvgFill(vg);
    }

private:
    qrcodegen::QrCode qr_;
};

std::vector<mm::fs::path> inboxFiles() {
    std::vector<mm::fs::path> out;
    std::error_code ec;
    for (mm::fs::directory_iterator it(AppState::get().inboxDir(), ec), end; it != end; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (!it->is_regular_file(ec) || name.rfind("._", 0) == 0) continue;
        if (mm::isSupportedArchiveName(name) || mm::endsWithI(name, ".bnp") || mm::endsWithI(name, ".tkcl")) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string modified(const mm::fs::path& p) {
    std::error_code ec;
    auto t = mm::fs::last_write_time(p, ec);
    if (ec) return "";
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - decltype(t)::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    char buf[32];
    std::strftime(buf, sizeof buf, "%b %d", std::localtime(&tt));
    return buf;
}

}  // namespace

AddModsPage::AddModsPage() : brls::Box(brls::Axis::ROW) {
    setGrow(1);

    auto* left = ui::vbox();
    left->setWidth(500);
    left->setShrink(0);
    left->setPadding(20, 14, 0, 16);
    left->setLineRight(1);
    left->setLineColor(ui::line());
    auto* title = ui::label("Add mods", 22, ui::text());
    title->setWidth(400);
    title->setMargins(0, 6, 10, 6);
    left->addView(title);
    list_ = ui::vbox();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(list_);
    left->addView(scroll);
    addView(left);

    pane_ = ui::vbox();
    pane_->setGrow(1);
    pane_->setPadding(24, 30, 20, 30);
    pane_->setBackgroundColor(ui::panelBg());
    addView(pane_);

    sink_ = new ui::FocusSink();
    addView(sink_);
    flow_ = std::make_unique<InstallFlow>(pane_, sink_);
    flow_->onExit = [this] {
        if (server_.running())
            showLinkScreen();
        else
            rebuild();
    };
    rebuild();
}

AddModsPage::~AddModsPage() {
    *alive_ = false;
    server_.stop();
}

brls::View* AddModsPage::getDefaultFocus() { return list_->getDefaultFocus(); }

void AddModsPage::clearPane() {
    ui::parkFocusIfInside(pane_, sink_);
    pane_->clearViews();
    linkStatus_ = nullptr;
}

bool AddModsPage::paneFree() {
    if (server_.running() && !flow_->active()) closeLinkScreen();
    if (!flow_->active()) return true;
    if (flow_->busy()) return false;
    flow_->onExit = nullptr;  // abandoning a flow that's waiting for a choice
    flow_ = std::make_unique<InstallFlow>(pane_, sink_);
    flow_->onExit = [this] {
        if (server_.running())
            showLinkScreen();
        else
            rebuild();
    };
    return true;
}

void AddModsPage::rebuild() {
    bool hadFocus = ui::focusInside(this) || ui::focusParked();
    ui::parkFocusIfInside(this, sink_);
    list_->clearViews();
    clearPane();

    auto section = [this](const std::string& text) {
        auto* l = ui::label(text, 15, ui::muted());
        l->setMargins(10, 0, 8, 6);
        list_->addView(l);
    };
    section("From SD card · switch/patchwork/inbox");
    brls::View* first = nullptr;
    auto files = inboxFiles();
    const auto& installed = AppState::get().snapshot().installedKeys;
    for (const auto& path : files) {
        auto* row = ui::focusRow();
        row->addView(ui::label(ui::icon::file, 24, ui::accent()));
        auto* text = ui::vbox();
        text->setWidth(330);
        text->setMarginLeft(12);
        text->addView(ui::label(path.filename().string(), 17, ui::text()));
        std::error_code ec;
        auto size = mm::fs::file_size(path, ec);
        auto* sub = ui::label(formatSize(size) + " · " + modified(path), 14, ui::muted());
        sub->setMarginTop(3);
        text->addView(sub);
        row->addView(text);
        if (installed.count("file:" + path.filename().string())) row->addView(ui::label(ui::icon::check, 22, ui::ok()));
        auto p = path;
        row->getFocusEvent()->subscribe([this, p](brls::View*) {
            if (paneFree()) showInboxFile(p);
        });
        row->registerClickAction([this, p](brls::View*) {
            installInboxFile(p);
            return true;
        });
        row->updateActionHint(brls::BUTTON_A, "Install");
        row->registerAction("Delete file", brls::BUTTON_X, [this, p](brls::View*) {
            deleteInboxFile(p);
            return true;
        });
        list_->addView(row);
        if (!first || p.string() == focusedPath_) first = row;
    }
    if (files.empty()) {
        auto* none = ui::label("No files yet. Copy .zip, .7z or .rar files into switch/patchwork/inbox on the SD card.", 15, ui::muted(), false);
        none->setMargins(0, 6, 6, 6);
        list_->addView(none);
    }

    section("From your phone");
    auto* row = ui::focusRow();
    row->addView(ui::label("", 24, ui::accent()));  // smartphone
    auto* text = ui::vbox();
    text->setWidth(330);
    text->setMarginLeft(12);
    text->addView(ui::label("Send a link to this Switch", 17, ui::text()));
    auto* sub = ui::label("Paste a download link on your phone", 14, ui::muted());
    sub->setMarginTop(3);
    text->addView(sub);
    row->addView(text);
    row->getFocusEvent()->subscribe([this](brls::View*) {
        if (paneFree()) showLinkIntro();
    });
    row->registerClickAction([this](brls::View*) {
        openLinkScreen();
        return true;
    });
    row->updateActionHint(brls::BUTTON_A, "Start");
    list_->addView(row);
    linkRow_ = row;
    if (!first) first = row;

    if (hadFocus) brls::Application::giveFocus(first);
    if (!files.empty())
        showInboxFile(focusedPath_.empty() ? files.front() : mm::fs::path(focusedPath_));
    else
        showLinkIntro();
}

void AddModsPage::showInboxFile(const mm::fs::path& path) {
    focusedPath_ = path.string();
    clearPane();
    std::error_code ec;
    if (!mm::fs::exists(path, ec)) return;
    pane_->addView(ui::label(path.filename().string(), 24, ui::text()));
    auto* meta = ui::label(formatSize(mm::fs::file_size(path, ec)) + " · copied to the SD card " + modified(path), 16, ui::muted());
    meta->setMargins(4, 0, 16, 0);
    pane_->addView(meta);
    const auto& game = mm::gameInfo(AppState::get().game()).displayName;
    pane_->addView(ui::label("Press A to unpack it and see what's inside. It installs for " + std::string(game) +
                                 " (switch game with ZL/ZR); you'll pick options and see any clashes next, like any other install.",
                             16, nvgRGB(0xc8, 0xc8, 0xc8), false));
    auto* more = ui::label("To add more, copy zip, 7z or rar files into switch/patchwork/inbox on the SD card, from a PC or over USB/FTP "
                           "(for example with DBI or sys-ftpd). The file stays here after installing; X deletes it.",
                           15, ui::muted(), false);
    more->setMarginTop(14);
    pane_->addView(more);
}

void AddModsPage::installInboxFile(const mm::fs::path& path) {
    if (!paneFree()) return;
    std::error_code ec;
    if (!mm::fs::exists(path, ec)) {
        rebuild();
        return;
    }
    mm::gb::ModDetails d;
    auto name = path.filename().string();
    d.summary.sourceKey = "file:" + name;
    d.summary.name = path.stem().string();
    d.summary.author = "SD card";
    mm::gb::ModFile f;
    f.filename = name;
    f.downloadUrl = "file://" + path.string();
    f.size = static_cast<int64_t>(mm::fs::file_size(path, ec));
    f.id = mm::stableId(path.string() + ":" + std::to_string(f.size));
    f.description = "Copied to the SD card";
    d.files.push_back(f);
    flow_->install(d, f);
}

void AddModsPage::deleteInboxFile(const mm::fs::path& path) {
    auto* dialog = new brls::Dialog("Delete " + path.filename().string() + " from the SD card? Mods already installed from it stay installed.");
    dialog->addButton("Cancel", [] {});
    dialog->addButton("Delete", [this, path] {
        mm::removeTree(path);
        focusedPath_.clear();
        auto alive = alive_;
        brls::sync([this, alive] {  // after the dialog has handed focus back
            if (*alive) rebuild();
        });
    });
    dialog->open();
}

void AddModsPage::showLinkIntro() {
    clearPane();
    pane_->addView(ui::label("Send a link from your phone", 24, ui::text()));
    auto* body = ui::label(
        "Download pages are easier to find and paste on a phone than to type on the Switch. Press A to start: the Switch shows "
        "an address and a QR code to open on your phone (on the same Wi-Fi). Paste a link there and tap Send.\n\n"
        "Works with direct links to .zip, .7z or .rar files, Waikuteru mod pages and GameBanana mod pages. "
        "Nothing is installed until you confirm on the Switch.",
        16, nvgRGB(0xc8, 0xc8, 0xc8), false);
    body->setMarginTop(12);
    pane_->addView(body);
}

void AddModsPage::openLinkScreen() {
    if (!paneFree()) return;
    auto alive = alive_;
    receivedLink_.clear();
    bool ok = server_.start([this, alive](const std::string& url) {
        brls::sync([this, alive, url] {
            if (!*alive || !server_.running()) return;
            receivedLink_ = url;
            if (!flow_->active()) showLinkScreen();
        });
    });
    if (!ok) {
        flow_->showMessage("Couldn't start", server_.error(), true);
        return;
    }
    showLinkScreen();
}

void AddModsPage::showLinkScreen() {
    clearPane();
    auto* box = ui::vbox();
    box->setGrow(1);
    box->setFocusable(true);
    box->setHideHighlight(true);
    auto* top = ui::hbox();
    top->setAlignItems(brls::AlignItems::FLEX_START);
    top->addView(new QrView("http://" + server_.address()));
    auto* steps = ui::vbox();
    steps->setMarginLeft(24);
    steps->setGrow(1);
    steps->setShrink(1);
    steps->addView(ui::label("Send a link from your phone", 24, ui::text()));
    auto* s1 = ui::label("1. Connect your phone to the same Wi-Fi.\n2. Scan the code, or open this address:", 16, nvgRGB(0xc8, 0xc8, 0xc8), false);
    s1->setMarginTop(10);
    steps->addView(s1);
    auto* addr = ui::label(server_.address(), 30, ui::accent());
    addr->setMargins(6, 0, 6, 0);
    steps->addView(addr);
    steps->addView(ui::label("3. Paste a mod link and tap Send.", 16, nvgRGB(0xc8, 0xc8, 0xc8), false));
    top->addView(steps);
    box->addView(top);

    auto* note = ui::label("The address works only while this screen is open. Press B to stop.", 14, ui::muted(), false);
    note->setMarginTop(16);
    box->addView(note);

    auto* card = ui::vbox();
    card->setMarginTop(18);
    card->setPadding(12, 14, 12, 14);
    card->setBackgroundColor(ui::accentDim());
    card->setLineLeft(4);
    card->setLineColor(ui::accent());
    if (receivedLink_.empty()) {
        linkStatus_ = ui::label("Waiting for a link…", 17, ui::muted());
        card->addView(linkStatus_);
    } else {
        card->addView(ui::label("Link received", 18, ui::text()));
        auto* url = ui::label(receivedLink_, 14, ui::muted(), false);
        url->setMargins(4, 0, 4, 0);
        card->addView(url);
        card->addView(ui::label("Press A to look at it", 15, ui::accent()));
    }
    box->addView(card);

    box->registerAction("Open link", brls::BUTTON_A, [this](brls::View*) {
        if (receivedLink_.empty()) return true;
        openLink(receivedLink_);
        return true;
    });
    box->registerAction("Stop", brls::BUTTON_B, [this](brls::View*) {
        closeLinkScreen();
        ui::tryFocus(linkRow_);
        return true;
    });
    pane_->addView(box);
    brls::Application::giveFocus(box);
}

void AddModsPage::closeLinkScreen() {
    server_.stop();
    receivedLink_.clear();
    showLinkIntro();
}

void AddModsPage::openLink(const std::string& url) {
    flow_->showLoading("Looking at the link…");
    auto alive = alive_;
    auto game = AppState::get().game();
    InstallFlow* flow = flow_.get();
    AppState::get().runNetwork([this, alive, url, game, flow] {
        try {
            auto d = AppState::get().resolveLink(url, game);
            brls::sync([this, alive, d, flow] {
                if (!*alive || flow_.get() != flow || !flow_->active()) return;
                AppState::get().cacheDetails(d);
                flow_->showFilePicker(d);
            });
        } catch (const std::exception& e) {
            std::string err = e.what();
            brls::sync([this, alive, err, flow] {
                if (*alive && flow_.get() == flow && flow_->active()) flow_->showMessage("Can't use that link", err, true);
            });
        }
    });
}

}  // namespace app
