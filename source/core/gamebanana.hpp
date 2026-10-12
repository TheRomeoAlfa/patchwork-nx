#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "games.hpp"
#include "http.hpp"

namespace mm::gb {

enum class Sort { MostDownloaded, MostLiked, Newest, LatestUpdated };

struct Category {
    int64_t id = 0;
    std::string name;
};

struct ModSummary {
    int64_t id = 0;
    // Where the mod comes from: "gb:<id>" (GameBanana), "wk:<game>/<slug>" (Waikuteru),
    // "url:<link>" or "file:<name>". Identifies the mod across sources.
    std::string sourceKey;
    int64_t gameId = 0;
    std::string name;
    std::string author;
    std::string category;
    std::string thumbnailUrl;  // ~220px wide preview, may be empty
    std::string profileUrl;
    int64_t dateAdded = 0;
    int64_t dateModified = 0;
    int64_t likes = 0;
    int64_t views = 0;
    bool hasContentRatings = false;  // any rating at all (includes e.g. "Blood and Gore")
    // GameBanana warns about or hides the mod for signed-out visitors: its own NSFW signal
    // (sexual themes, nudity), set together with a content rating.
    bool nsfw = false;
    bool obsolete = false;
};

struct ModFile {
    int64_t id = 0;
    std::string filename;
    int64_t size = 0;
    std::string downloadUrl;
    std::string md5;
    std::string version;
    std::string description;
    int64_t dateAdded = 0;
    bool avClean = true;  // GameBanana's antivirus result
};

struct ModDetails {
    ModSummary summary;
    std::string version;
    std::string descriptionHtml;
    int64_t downloads = 0;
    std::vector<std::string> screenshotUrls;  // ~530px wide
    std::vector<ModFile> files;
};

struct Page {
    std::vector<ModSummary> mods;
    int64_t totalCount = 0;
    bool isLast = true;
};

// Parsing is separate from fetching so it can be tested against saved responses.
Page parseModList(const std::string& body);
ModDetails parseModDetails(const std::string& body, int64_t modId);
std::vector<Category> parseCategories(const std::string& body);

class Client {
public:
    explicit Client(HttpClient& http) : http_(http) {}

    Page list(Game game, Sort sort, int page, int perPage = 20, int64_t categoryId = 0);
    Page search(Game game, const std::string& query, int page);  // GameBanana returns 15 per page
    ModDetails details(int64_t modId);
    std::vector<Category> categories(Game game);

    static std::string listUrl(Game game, Sort sort, int page, int perPage, int64_t categoryId);
    static std::string searchUrl(Game game, const std::string& query, int page);
    static std::string detailsUrl(int64_t modId);

private:
    HttpClient& http_;
};

}  // namespace mm::gb
