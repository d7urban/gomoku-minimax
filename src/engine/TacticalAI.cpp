#include "gomoku/TacticalAI.hpp"

#include "gomoku/ClubAI.hpp"

namespace gomoku {

SearchResult TacticalAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    const int timeMs = std::max(config.timeLimitMs, 500);
    if (timeMs >= 5000) {
        config.maxDepth = std::max(config.maxDepth, 30);
    } else if (timeMs >= 2000) {
        config.maxDepth = std::max(config.maxDepth, 20);
    } else {
        config.maxDepth = std::max(config.maxDepth, 10);
    }
    config.maxNodes = std::max<std::uint64_t>(config.maxNodes, 180000);
    config.timeLimitMs = timeMs;
    config.maxCandidateMoves = std::max<std::size_t>(config.maxCandidateMoves, 20);

    SearchEngine engine(config);
    return engine.search(state);
}

SwapChoice TacticalAI::chooseSwapChoice(const GameState& state) {
    return ClubAI::chooseSwapChoice(state);
}

}  // namespace gomoku
