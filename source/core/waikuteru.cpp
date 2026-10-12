#include "waikuteru.hpp"

#include <algorithm>
#include <regex>

#include "util.hpp"

namespace mm::wk {

namespace {

std::string unescapeHtml(std::string s) {
    static const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#x27;", "'"}, {"&#39;", "'"}, {"&#039;", "'"}, {"&nbsp;", " "}};
    for (auto [from, to] : entities) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, std::string(from).size(), to);
            pos += std::string(to).size();
        }
    }
    return s;
}

// Visible text of a page, as trimmed pieces in document order.
std::vector<std::string> textTokens(const std::string& html) {
    static const std::regex hidden(R"(<(script|style)\b[\s\S]*?</\1>)", std::regex::icase);
    auto body = std::regex_replace(html, hidden, "");
    std::vector<std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        auto lt = body.find('<', i);
        auto text = body.substr(i, lt == std::string::npos ? std::string::npos : lt - i);
        auto first = text.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            auto last = text.find_last_not_of(" \t\r\n");
            out.push_back(unescapeHtml(text.substr(first, last - first + 1)));
        }
        if (lt == std::string::npos) break;
        auto gt = body.find('>', lt);
        if (gt == std::string::npos) break;
        i = gt + 1;
    }
    return out;
}

std::string alnumKey(const std::string& s) {
    std::string out;
    for (unsigned char c : toLower(s))
        if (std::isalnum(c)) out += static_cast<char>(c);
    return out;
}

std::vector<std::string> hrefs(const std::string& html) {
    static const std::regex re(R"#(href="([^"]+)")#");
    std::vector<std::string> out;
    for (std::sregex_iterator it(html.begin(), html.end(), re), end; it != end; ++it) out.push_back(unescapeHtml((*it)[1].str()));
    return out;
}

}  // namespace

bool isModPageUrl(const std::string& url) {
    static const std::regex re(R"(^https?://(www\.)?waikuteru-zelda-mods\.com/downloads/(botw|totk)/[^/?#]+)", std::regex::icase);
    return std::regex_search(url, re);
}

std::string sourceKeyForUrl(const std::string& pageUrl) {
    auto pos = pageUrl.find("/downloads/");
    if (pos == std::string::npos) return "wk:" + pageUrl;
    auto rest = pageUrl.substr(pos + 11);
    rest = rest.substr(0, rest.find_first_of("?#"));
    return "wk:" + toLower(urlDecode(rest));
}

std::vector<ListEntry> parseDownloadsPage(const std::string& html) {
    // Mod pages linked from the page, by normalised name -> url.
    std::vector<std::pair<std::string, std::string>> pages;
    for (const auto& h : hrefs(html)) {
        if (!isModPageUrl(h)) continue;
        if (std::none_of(pages.begin(), pages.end(), [&](const auto& p) { return p.second == h; })) {
            auto slug = h.substr(h.rfind('/') + 1);
            pages.emplace_back(alnumKey(urlDecode(slug)), h);
        }
    }

    // Cards read as: <name> / "Platform: ..." or "Game Version: ..." / "Download Now".
    std::vector<ListEntry> out;
    auto toks = textTokens(html);
    for (size_t i = 2; i < toks.size(); ++i) {
        if (toks[i] != "Download Now") continue;
        const auto& info = toks[i - 1];
        const auto& name = toks[i - 2];
        bool platform = info.rfind("Platform:", 0) == 0, version = info.rfind("Game Version:", 0) == 0;
        if (!platform && !version) continue;
        // Some mods exist for both games under the same name; the card type says which:
        // BOTW cards list the platform, TOTK cards the game version.
        auto key = alnumKey(name);
        const char* section = version ? "/downloads/totk/" : "/downloads/botw/";
        auto page = std::find_if(pages.begin(), pages.end(), [&](const auto& p) {
            return p.first == key && p.second.find(section) != std::string::npos;
        });
        if (page == pages.end()) continue;
        ListEntry e;
        e.game = page->second.find("/downloads/totk/") != std::string::npos ? Game::Totk : Game::Botw;
        e.onSwitch = version || info.find("Switch") != std::string::npos;
        auto detail = info.substr(info.find(':') + 1);
        detail.erase(0, detail.find_first_not_of(' '));
        e.summary.name = name;
        e.summary.author = "Waikuteru";
        e.summary.category = version ? "Game " + detail : detail;
        e.summary.profileUrl = page->second;
        e.summary.sourceKey = sourceKeyForUrl(page->second);
        e.summary.id = stableId(e.summary.sourceKey);
        out.push_back(std::move(e));
    }
    return out;
}

gb::ModDetails parseModPage(const std::string& html, const gb::ModSummary& summary) {
    gb::ModDetails d;
    d.summary = summary;

    // Description: the text between "About" and the download buttons.
    auto toks = textTokens(html);
    auto about = std::find(toks.begin(), toks.end(), "About");
    std::string desc;
    for (auto it = about == toks.end() ? toks.end() : about + 1; it != toks.end(); ++it) {
        if (it->rfind("Download", 0) == 0 || *it == "bottom of page") break;
        if (it->find_first_not_of("─-") == std::string::npos) continue;  // the divider line
        desc += (desc.empty() ? "" : "\n\n") + *it;
    }
    d.descriptionHtml = desc;

    // Screenshots: the images inside the page's gallery (title card first). Their ids sit in
    // each gallery item's data-image-info ("uri":"696de8_…~mv2.png", HTML-escaped).
    static const std::regex galleryUri(R"(uri(?:&quot;|")\s*:\s*(?:&quot;|")([0-9a-f]+_[0-9a-f]+~mv2\.(?:png|jpe?g)))");
    auto gallery = html.find("wixui-gallery");
    if (gallery != std::string::npos) {
        auto rest = html.substr(gallery);
        std::vector<std::string> seen;
        for (std::sregex_iterator it(rest.begin(), rest.end(), galleryUri), end; it != end; ++it) {
            auto id = (*it)[1].str();
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
            seen.push_back(id);
            d.screenshotUrls.push_back("https://static.wixstatic.com/media/" + id + "/v1/fit/w_530,h_300,al_c/preview.png");
        }
    }

    for (const auto& h : hrefs(html)) {
        if (h.find("/_files/archives/") == std::string::npos && h.find("filesusr.com/archives/") == std::string::npos) continue;
        if (std::any_of(d.files.begin(), d.files.end(), [&](const gb::ModFile& f) { return f.downloadUrl == h; })) continue;
        gb::ModFile f;
        f.downloadUrl = h;
        f.id = stableId(h);
        auto dn = h.find("dn=");
        auto name = dn != std::string::npos ? urlDecode(urlDecode(h.substr(dn + 3))) : h.substr(h.rfind('/') + 1);
        name = name.substr(0, name.find('&'));
        f.filename = name;
        bool wiiu = name.find("(Wii U)") != std::string::npos, sw = name.find("(Switch)") != std::string::npos;
        f.description = sw ? "Switch" : wiiu ? "Wii U only" : (d.summary.sourceKey.rfind("wk:totk/", 0) == 0 ? "Switch" : "");
        d.files.push_back(std::move(f));
    }
    return d;
}

bool isWiiUOnly(const gb::ModFile& f) {
    return f.filename.find("(Wii U)") != std::string::npos && f.filename.find("(Switch)") == std::string::npos;
}

}  // namespace mm::wk
