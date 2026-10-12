#include "gamebanana.hpp"

#include <stdexcept>

#include "nlohmann/json.hpp"

namespace mm::gb {

using json = nlohmann::json;

namespace {

constexpr const char* kApi = "https://gamebanana.com/apiv11";

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int64_t num(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int64_t>() : 0;
}

bool flag(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

// _aPreviewMedia._aImages[i] -> URL at the requested width ("220" or "530"), or full size.
std::string imageUrl(const json& img, const char* width) {
    auto base = str(img, "_sBaseUrl");
    auto file = str(img, (std::string("_sFile") + width).c_str());
    if (file.empty()) file = str(img, "_sFile");
    return (base.empty() || file.empty()) ? std::string() : base + "/" + file;
}

const json* images(const json& record) {
    auto pm = record.find("_aPreviewMedia");
    if (pm == record.end() || !pm->is_object()) return nullptr;
    auto imgs = pm->find("_aImages");
    return (imgs != pm->end() && imgs->is_array()) ? &*imgs : nullptr;
}

ModSummary parseSummary(const json& r) {
    ModSummary m;
    m.id = num(r, "_idRow");
    m.sourceKey = "gb:" + std::to_string(m.id);
    m.name = str(r, "_sName");
    m.profileUrl = str(r, "_sProfileUrl");
    m.dateAdded = num(r, "_tsDateAdded");
    m.dateModified = num(r, "_tsDateModified");
    m.likes = num(r, "_nLikeCount");
    m.views = num(r, "_nViewCount");
    m.hasContentRatings = flag(r, "_bHasContentRatings");
    auto visibility = str(r, "_sInitialVisibility");
    m.nsfw = m.hasContentRatings && (visibility == "warn" || visibility == "hide");
    m.obsolete = flag(r, "_bIsObsolete");
    if (auto g = r.find("_aGame"); g != r.end() && g->is_object()) m.gameId = num(*g, "_idRow");
    if (auto s = r.find("_aSubmitter"); s != r.end() && s->is_object()) m.author = str(*s, "_sName");
    for (const char* key : {"_aRootCategory", "_aCategory"})
        if (auto c = r.find(key); c != r.end() && c->is_object() && m.category.empty()) m.category = str(*c, "_sName");
    if (auto imgs = images(r); imgs && !imgs->empty()) m.thumbnailUrl = imageUrl((*imgs)[0], "220");
    return m;
}

json parse(const std::string& body) {
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("GameBanana returned an invalid response");
    if (j.is_object() && j.contains("_sErrorCode"))
        throw std::runtime_error("GameBanana error: " + str(j, "_sErrorMessage"));
    return j;
}

const char* sortParam(Sort s) {
    switch (s) {
        case Sort::MostDownloaded: return "Generic_MostDownloaded";
        case Sort::MostLiked: return "Generic_MostLiked";
        case Sort::Newest: return "Generic_Newest";
        case Sort::LatestUpdated: return "Generic_LatestUpdated";
    }
    return "Generic_Newest";
}

}  // namespace

Page parseModList(const std::string& body) {
    auto j = parse(body);
    Page p;
    if (auto meta = j.find("_aMetadata"); meta != j.end()) {
        p.totalCount = num(*meta, "_nRecordCount");
        p.isLast = flag(*meta, "_bIsComplete");
    }
    for (const auto& r : j.value("_aRecords", json::array()))
        if (str(r, "_sModelName") == "Mod") p.mods.push_back(parseSummary(r));
    return p;
}

ModDetails parseModDetails(const std::string& body, int64_t modId) {
    auto j = parse(body);
    ModDetails d;
    d.summary = parseSummary(j);
    d.summary.id = modId;
    d.summary.sourceKey = "gb:" + std::to_string(modId);
    if (d.summary.profileUrl.empty()) d.summary.profileUrl = "https://gamebanana.com/mods/" + std::to_string(modId);
    d.version = str(j, "_sVersion");
    d.descriptionHtml = str(j, "_sText");
    d.downloads = num(j, "_nDownloadCount");
    if (auto imgs = images(j))
        for (const auto& img : *imgs)
            if (auto u = imageUrl(img, "530"); !u.empty()) d.screenshotUrls.push_back(u);
    for (const auto& f : j.value("_aFiles", json::array())) {
        ModFile mf;
        mf.id = num(f, "_idRow");
        mf.filename = str(f, "_sFile");
        mf.size = num(f, "_nFilesize");
        mf.downloadUrl = str(f, "_sDownloadUrl");
        mf.md5 = str(f, "_sMd5Checksum");
        mf.version = str(f, "_sVersion");
        mf.description = str(f, "_sDescription");
        mf.dateAdded = num(f, "_tsDateAdded");
        auto av = str(f, "_sAvResult");
        mf.avClean = av.empty() || av == "clean";
        if (flag(f, "_bIsArchived")) continue;
        d.files.push_back(std::move(mf));
    }
    return d;
}

std::vector<Category> parseCategories(const std::string& body) {
    std::vector<Category> out;
    for (const auto& c : parse(body)) out.push_back({num(c, "_idRow"), str(c, "_sName")});
    return out;
}

std::string Client::listUrl(Game game, Sort sort, int page, int perPage, int64_t categoryId) {
    auto url = std::string(kApi) + "/Mod/Index?_nPage=" + std::to_string(page) + "&_nPerpage=" + std::to_string(perPage) +
               "&_aFilters%5BGeneric_Game%5D=" + std::to_string(gameInfo(game).gamebananaId) + "&_sSort=" + sortParam(sort);
    if (categoryId) url += "&_aFilters%5BGeneric_Category%5D=" + std::to_string(categoryId);
    return url;
}

std::string Client::searchUrl(Game game, const std::string& query, int page) {
    return std::string(kApi) + "/Util/Search/Results?_sModelName=Mod&_sSearchString=" + urlEncode(query) +
           "&_idGameRow=" + std::to_string(gameInfo(game).gamebananaId) + "&_nPage=" + std::to_string(page);
}

std::string Client::detailsUrl(int64_t modId) {
    return std::string(kApi) + "/Mod/" + std::to_string(modId) +
           "?_csvProperties=_sName,_aGame,_aSubmitter,_aCategory,_aPreviewMedia,_sText,_sVersion,_aFiles,"
           "_nLikeCount,_nViewCount,_nDownloadCount,_tsDateAdded,_tsDateModified,_bIsObsolete,_sProfileUrl";
}

Page Client::list(Game game, Sort sort, int page, int perPage, int64_t categoryId) {
    return parseModList(http_.get(listUrl(game, sort, page, perPage, categoryId)));
}

Page Client::search(Game game, const std::string& query, int page) {
    auto p = parseModList(http_.get(searchUrl(game, query, page)));
    // Belt and braces: never show another game's mods even if _idGameRow is ignored.
    auto want = gameInfo(game).gamebananaId;
    std::erase_if(p.mods, [&](const ModSummary& m) { return m.gameId && m.gameId != want; });
    return p;
}

ModDetails Client::details(int64_t modId) { return parseModDetails(http_.get(detailsUrl(modId)), modId); }

std::vector<Category> Client::categories(Game game) {
    return parseCategories(http_.get(std::string(kApi) + "/Mod/Categories?_idGameRow=" +
                                     std::to_string(gameInfo(game).gamebananaId) + "&_sSort=a_to_z&_bShowEmpty=false"));
}

}  // namespace mm::gb
