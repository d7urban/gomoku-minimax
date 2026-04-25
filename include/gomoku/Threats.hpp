#pragma once

#include <cstdint>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/ThreatAssessment.hpp"
#include "gomoku/ThreatTypes.hpp"

namespace gomoku {

struct CandidateMove {
    Move move {};
    MoveThreatInfo threatInfo {};
    int score {0};
};

std::string_view toString(ThreatType type);
inline int threatSeverity(ThreatType type) {
    return static_cast<int>(type);
}
inline int threatWeight(ThreatType type) {
    switch (type) {
        case ThreatType::None: return 0;
        case ThreatType::One: return 2;
        case ThreatType::Two: return 6;
        case ThreatType::BrokenThree: return 18;
        case ThreatType::OpenThree: return 40;
        case ThreatType::SimpleFour: return 160;
        case ThreatType::OpenFour: return 600;
        case ThreatType::Five: return 20000;
        default: return 0;
    }
}
int threatSeverityEnhanced(const MoveThreatInfo& info);
int combinedThreatScore(ThreatType first, ThreatType second);
MoveThreatInfo computeMoveThreatInfo(const GameState& state, Move move, Player player);

// Recomputes the threat for a single direction index (0=row, 1=col,
// 2=diag, 3=antidiag) only. Used on the incremental-update hot path:
// after a stone is placed, every affected empty cell needs at most ONE
// of its 4 per-direction threats recomputed — the three orthogonal
// lines never passed through the placed stone and so cannot have
// changed.
ThreatType computeLineThreatForDirection(const GameState& state, Move move, Player player, int directionIndex);

// Summarises root-level tactical pressure for the governor. Uses the
// existing StaticEvaluator candidate machinery; maps the strongest
// available threat for each side into the coarse ThreatAssessment
// levels so the governor does not need to know pattern-analysis
// internals.
ThreatAssessment assessRootThreats(const GameState& state);

// Optional instrumentation counters for profiling candidate-generation cost.
// Search is single-threaded so plain counters suffice. In normal builds these
// are compiled out; enable them with `-DGOMOKU_ENABLE_SEARCH_PROFILING=ON`.
struct ProfilingCounters {
    std::uint64_t generateCandidateCalls {0};
    std::uint64_t nearStoneChecks {0};
    std::uint64_t analyzeMoveCalls {0};
    std::uint64_t legalMovesCalls {0};
    std::uint64_t computeThreatInfoCalls {0};
    std::uint64_t patternWindowsScanned {0};
};

bool profilingCountersEnabled();
void resetProfilingCounters();
ProfilingCounters readProfilingCounters();
void bumpLegalMovesCounter();

class StaticEvaluator {
public:
    static MoveThreatInfo analyzeMove(const GameState& state, Move move, Player player);
    static int evaluate(const GameState& state, Player perspective);
    static std::vector<CandidateMove> generateCandidateMoves(const GameState& state, Player player, std::size_t maxMoves);

private:
    static int evaluatePlayerPotential(const GameState& state, Player player);
};

}  // namespace gomoku
