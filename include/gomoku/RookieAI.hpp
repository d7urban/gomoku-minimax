#pragma once

#include <optional>

#include "gomoku/GameState.hpp"

namespace gomoku {

class RookieAI {
public:
    static std::optional<Move> chooseMove(const GameState& state, Player player);
    static SwapChoice chooseSwapChoice(const GameState& state);
};

}  // namespace gomoku
