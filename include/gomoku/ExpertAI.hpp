#pragma once

#include "gomoku/Search.hpp"

namespace gomoku {

class ExpertAI {
public:
    // ExpertAI enforces expert-mode minimums even if the caller passes lower settings.
    // The depth floor scales with the configured move time so longer budgets are not
    // bottlenecked by the previous shallow expert cap.
    // It also pushes the soft stop close to the hard limit so Expert uses more of the configured move time.
    // Use SearchEngine directly when tests or benchmarks need exact config control.
    static SearchResult chooseMove(const GameState& state, Player player, SearchConfig config = {});
    static SwapChoice chooseSwapChoice(const GameState& state);
};

}  // namespace gomoku
