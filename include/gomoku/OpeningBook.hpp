#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gomoku/GameState.hpp"

namespace gomoku {

struct OpeningBookHit {
    Move move {};
    std::string_view lineName {};
};

std::optional<OpeningBookHit> lookupOpeningBookMove(const GameState& state);
std::vector<std::string> openingBookLines();

}  // namespace gomoku
