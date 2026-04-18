#include "gomoku/ClubAI.hpp"

namespace gomoku {

SearchResult ClubAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    SearchEngine engine(config);
    return engine.search(state);
}

SwapChoice ClubAI::chooseSwapChoice(const GameState& state) {
    const int blackScore = StaticEvaluator::evaluate(state, Player::Black);
    const int whiteScore = StaticEvaluator::evaluate(state, Player::White);
    return blackScore >= whiteScore ? SwapChoice::SwapColors : SwapChoice::KeepColors;
}

}  // namespace gomoku
