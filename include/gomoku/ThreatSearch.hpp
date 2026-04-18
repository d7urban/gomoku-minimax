#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "gomoku/Threats.hpp"

namespace gomoku {

enum class ThreatGraphNodeKind : std::uint8_t {
    Threat,
    Combination,
};

struct ThreatSequenceConfig {
    int maxDepth {6};
    std::uint64_t maxNodes {25000};
    int timeLimitMs {0};
    std::size_t maxThreatMoves {12};
};

struct ThreatStep {
    int nodeId {-1};
    Move move {};
    ThreatType type {ThreatType::None};
    int direction {-1};
    std::vector<Move> defenseMoves;
    std::vector<Move> continuationMoves;
    std::vector<Move> requiredEmpty;
    std::vector<Move> supportMoves;
};

struct ThreatGraphNode {
    int id {-1};
    ThreatGraphNodeKind kind {ThreatGraphNodeKind::Threat};
    Move move {};
    ThreatType type {ThreatType::None};
    std::vector<int> dependencies;
    std::vector<Move> defenseMoves;
    std::vector<Move> continuationMoves;
    std::vector<Move> requiredEmpty;
};

struct ThreatSearchResult {
    bool foundWin {false};
    std::vector<ThreatStep> sequence;
    std::vector<ThreatGraphNode> graph;
    std::vector<Move> refutations;
    std::uint64_t nodes {0};
};

struct ThreatEnumerationResult {
    std::vector<ThreatStep> threats;
    std::uint64_t nodes {0};
};

std::string_view toString(ThreatGraphNodeKind kind);

class ThreatSequenceSearcher {
public:
    explicit ThreatSequenceSearcher(ThreatSequenceConfig config = {});

    std::vector<ThreatStep> enumerateThreats(const GameState& state, Player attacker) const;
    ThreatEnumerationResult enumerateThreatsWithStats(const GameState& state, Player attacker) const;
    ThreatSearchResult searchWinningSequence(const GameState& state, Player attacker) const;

private:
    ThreatSequenceConfig config_ {};
};

}  // namespace gomoku
