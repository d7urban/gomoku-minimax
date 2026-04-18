#pragma once

#include "gomoku/Search.hpp"

namespace gomoku {

class ExpertAI {
public:
    // ExpertAI enforces expert-mode minimums even if the caller passes lower settings.
    // It also pushes the soft stop close to the hard limit so Expert uses more of the configured move time.
    // Use SearchEngine directly when tests or benchmarks need exact config control.
    static SearchResult chooseMove(const GameState& state, Player player, SearchConfig config = {});
    static SwapChoice chooseSwapChoice(const GameState& state);
};

}  // namespace gomoku
