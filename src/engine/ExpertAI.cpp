#include "gomoku/ExpertAI.hpp"

#include <algorithm>

#include "gomoku/ClubAI.hpp"

namespace gomoku {

namespace {

int expertDepthFloorForTimeMs(int timeLimitMs) {
    if (timeLimitMs >= 20000) {
        return 24;
    }
    if (timeLimitMs >= 10000) {
        return 22;
    }
    if (timeLimitMs >= 5000) {
        return 20;
    }
    if (timeLimitMs >= 2000) {
        return 18;
    }
    if (timeLimitMs >= 1000) {
        return 16;
    }
    if (timeLimitMs >= 500) {
        return 14;
    }
    return 12;
}

}  // namespace

SearchResult ExpertAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    config.timeLimitMs = std::max(config.timeLimitMs, 1);
    config.maxDepth = std::max(config.maxDepth, expertDepthFloorForTimeMs(config.timeLimitMs));
    config.maxNodes = std::max<std::uint64_t>(config.maxNodes, 900000);
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
