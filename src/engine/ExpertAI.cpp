#include "gomoku/ExpertAI.hpp"

#include <algorithm>

#include "gomoku/ClubAI.hpp"

namespace gomoku {

SearchResult ExpertAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    config.maxDepth = std::max(config.maxDepth, 11);
    config.maxNodes = std::max<std::uint64_t>(config.maxNodes, 900000);
    config.timeLimitMs = std::max(config.timeLimitMs, 1);
    config.softTimeLimitMs = std::min(config.timeLimitMs,
        std::max(config.softTimeLimitMs, std::max(1, config.timeLimitMs * 19 / 20)));
    config.maxCandidateMoves = std::max<std::size_t>(config.maxCandidateMoves, 28);
    config.useAspirationWindows = true;
    config.useNullMovePruning = true;
    config.useDefensiveFiltering = true;
    config.useOpeningBook = true;

    SearchEngine engine(config);
    return engine.search(state);
}

SwapChoice ExpertAI::chooseSwapChoice(const GameState& state) {
    return ClubAI::chooseSwapChoice(state);
}

}  // namespace gomoku
