#include "games.hpp"

#include "util.hpp"

namespace mm {

const GameInfo& gameInfo(Game g) {
    for (const auto& info : kGames)
        if (info.game == g) return info;
    return kGames[0];
}

std::optional<Game> gameFromKey(std::string_view key) {
    for (const auto& info : kGames)
        if (info.key == key) return info.game;
    return std::nullopt;
}

std::vector<std::string> titleIdsFor(Game game) {
    std::vector<std::string> ids{std::string(gameInfo(game).titleId)};
    if (game == Game::Botw) ids.emplace_back("01007EF00011F001");
    return ids;
}

std::optional<Game> gameFromTitleId(std::string_view titleId) {
    for (const auto& info : kGames)
        if (iequals(info.titleId, titleId)) return info.game;
    return std::nullopt;
}

}  // namespace mm
