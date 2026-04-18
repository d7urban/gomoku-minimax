#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "gomoku/ThreatSearch.hpp"

namespace gomoku {

enum class ProofOutcome : std::uint8_t {
    Unknown,
    ProvenWin,
    ProvenLoss,
};

struct ProofAnalysisConfig {
    int maxDepth {6};
    std::uint64_t maxNodes {120000};
    int timeLimitMs {250};
    std::size_t maxCandidateMoves {8};
    std::size_t maxThreatMoves {8};
};

struct ProofMoveSummary {
    Move move {};
    ThreatType threatType {ThreatType::None};
    ProofOutcome outcome {ProofOutcome::Unknown};
    std::uint64_t proofNumber {1};
    std::uint64_t disproofNumber {1};
};

struct ProofAnalysisResult {
    Player attacker {Player::None};
    ProofOutcome outcome {ProofOutcome::Unknown};
    std::optional<Move> bestMove;
    std::uint64_t nodes {0};
    int elapsedMs {0};
    int maxDepthReached {0};
    std::uint64_t rootProofNumber {1};
    std::uint64_t rootDisproofNumber {1};
    bool usedThreatShortcut {false};
    std::vector<Move> principalVariation;
    std::vector<ProofMoveSummary> rootMoves;
    std::optional<ThreatSearchResult> threatSequence;
};

std::string_view toString(ProofOutcome outcome);

class ProofAnalyzer {
public:
    explicit ProofAnalyzer(ProofAnalysisConfig config = {});

    ProofAnalysisResult analyze(const GameState& state, Player attacker) const;

private:
    ProofAnalysisConfig config_ {};
};

}  // namespace gomoku
