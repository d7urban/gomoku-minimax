#include "gomoku/ExpertAI.hpp"

#include <algorithm>

#include "gomoku/ClubAI.hpp"

namespace gomoku {

namespace {

int expertDepthFloorForTimeMs(int timeLimitMs) {
    if (timeLimitMs >= 10000) {
        return 50;
    }
    if (timeLimitMs >= 5000) {
        return 40;
    }
    if (timeLimitMs >= 2000) {
        return 30;
    }
    if (timeLimitMs >= 1000) {
        return 24;
    }
    if (timeLimitMs >= 500) {
        return 18;
    }
    return 12;
}

std::uint64_t expertNodeFloorForTimeMs(int timeLimitMs) {
    const std::uint64_t base = static_cast<std::uint64_t>(timeLimitMs) * 1500ULL;
    return base < 500'000ULL ? 500'000ULL : base;
}

}  // namespace

SearchResult ExpertAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    config.timeLimitMs = std::max(config.timeLimitMs, 1);
    config.maxDepth = std::max(config.maxDepth, expertDepthFloorForTimeMs(config.timeLimitMs));
    config.maxNodes = std::max(config.maxNodes, expertNodeFloorForTimeMs(config.timeLimitMs));
    config.softTimeLimitMs = std::min(config.timeLimitMs,
        std::max(config.softTimeLimitMs, std::max(1, config.timeLimitMs * 19 / 20)));
    config.maxCandidateMoves = std::max<std::size_t>(config.maxCandidateMoves, 28);
    config.useAspirationWindows = true;
    config.useNullMovePruning = !config.disableNullMovePruning;
    config.useDefensiveFiltering = !config.disableDefensiveFiltering;
    config.useStrictDefenseFiltering = config.useStrictDefenseFiltering && config.useDefensiveFiltering;
    config.useOpeningBook = !config.disableOpeningBook;

    SearchEngine engine(config);
    return engine.search(state);
}

SwapChoice ExpertAI::chooseSwapChoice(const GameState& state) {
    return ClubAI::chooseSwapChoice(state);
}

}  // namespace gomoku
