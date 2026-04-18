#pragma once

#include "gomoku/Search.hpp"

namespace gomoku {

class ClubAI {
public:
    static SearchResult chooseMove(const GameState& state, Player player, SearchConfig config = {});
    static SwapChoice chooseSwapChoice(const GameState& state);
};

}  // namespace gomoku
