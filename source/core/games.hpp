#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace mm {

enum class Game { Botw, Totk };

struct GameInfo {
    Game game;
    std::string_view key;          // short id used in folder names and state files
    std::string_view displayName;
    std::string_view titleId;      // upper-case hex, as Atmosphère expects
    int gamebananaId;
};

inline constexpr std::array<GameInfo, 2> kGames = {{
    {Game::Botw, "botw", "Breath of the Wild", "01007EF00011E000", 6386},
    {Game::Totk, "totk", "Tears of the Kingdom", "0100F2C0115B6000", 7617},
}};

const GameInfo& gameInfo(Game g);
std::optional<Game> gameFromKey(std::string_view key);
std::optional<Game> gameFromTitleId(std::string_view titleId);  // case-insensitive

}  // namespace mm
