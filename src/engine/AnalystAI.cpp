#include "gomoku/AnalystAI.hpp"

#include "gomoku/ExpertAI.hpp"

namespace gomoku {

SearchResult AnalystAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    return ExpertAI::chooseMove(state, player, config);
}

SwapChoice AnalystAI::chooseSwapChoice(const GameState& state) {
    return ExpertAI::chooseSwapChoice(state);
}

}  // namespace gomoku
