#include "gomoku/AnalystAI.hpp"

#include <algorithm>
#include <cstdint>

#include "gomoku/ExpertAI.hpp"
#include "gomoku/ProofSearch.hpp"

namespace gomoku {

namespace {

constexpr int kAnalystMateScore = 10'000'000;

bool isSharpPosition(const GameState& state, Player player) {
    return state.hasThreatAtLeast(player, ThreatType::OpenThree)
        || state.hasThreatAtLeast(otherPlayer(player), ThreatType::OpenThree);
}

ProofAnalysisConfig makeProofConfig(SearchConfig config) {
    ProofAnalysisConfig proof;
    proof.maxDepth = std::max(6, std::min(config.maxDepth, 8));
    proof.maxNodes = std::max<std::uint64_t>(150000, config.maxNodes / 2);
    proof.timeLimitMs = config.timeLimitMs > 0 ? std::max(1, config.timeLimitMs / 2) : 150;
    proof.maxCandidateMoves = std::max<std::size_t>(8, config.maxCandidateMoves / 2);
    proof.maxThreatMoves = std::max<std::size_t>(8, config.maxCandidateMoves / 2);
    return proof;
}

SearchConfig remainingExpertConfig(SearchConfig config, const ProofAnalysisResult& proof) {
    if (config.timeLimitMs > 0) {
        config.timeLimitMs = std::max(1, config.timeLimitMs - proof.elapsedMs);
    }

    if (config.softTimeLimitMs > 0) {
        config.softTimeLimitMs = std::max(1, config.softTimeLimitMs - proof.elapsedMs);
    } else if (config.timeLimitMs > 0) {
        config.softTimeLimitMs = config.timeLimitMs;
    }

    if (config.maxNodes > 0) {
        config.maxNodes = std::max<std::uint64_t>(1, config.maxNodes - proof.nodes);
    }

    return config;
}

}  // namespace

SearchResult AnalystAI::chooseMove(const GameState& state, Player player, SearchConfig config) {
    SearchResult result;
    if (state.sideToMove() != player) {
        return result;
    }

    if (isSharpPosition(state, player)) {
        ProofAnalyzer analyzer(makeProofConfig(config));
        result.proofAnalysis = analyzer.analyze(state, player);
        if (result.proofAnalysis->outcome == ProofOutcome::ProvenWin && result.proofAnalysis->bestMove.has_value()) {
            result.bestMove = result.proofAnalysis->bestMove;
            result.summary.score = kAnalystMateScore;
            result.summary.elapsedMs = result.proofAnalysis->elapsedMs;
            result.summary.nodes = result.proofAnalysis->nodes;
            result.summary.depthReached = result.proofAnalysis->maxDepthReached;
            result.summary.principalVariation = result.proofAnalysis->principalVariation;
            result.threatSequence = result.proofAnalysis->threatSequence;
            return result;
        }

        config = remainingExpertConfig(config, *result.proofAnalysis);
    }

    SearchResult expert = ExpertAI::chooseMove(state, player, config);
    if (result.proofAnalysis.has_value()) {
        expert.proofAnalysis = result.proofAnalysis;
        expert.summary.elapsedMs += result.proofAnalysis->elapsedMs;
    }
    return expert;
}

SwapChoice AnalystAI::chooseSwapChoice(const GameState& state) {
    return ExpertAI::chooseSwapChoice(state);
}

}  // namespace gomoku
