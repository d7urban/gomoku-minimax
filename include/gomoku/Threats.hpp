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
int threatSeverity(ThreatType type);
int threatWeight(ThreatType type);
int threatSeverityEnhanced(const MoveThreatInfo& info);
MoveThreatInfo computeMoveThreatInfo(const GameState& state, Move move, Player player);

// Summarises root-level tactical pressure for the governor. Uses the
// existing StaticEvaluator candidate machinery; maps the strongest
// available threat for each side into the coarse ThreatAssessment
// levels so the governor does not need to know pattern-analysis
// internals.
ThreatAssessment assessRootThreats(const GameState& state);

class StaticEvaluator {
public:
    static MoveThreatInfo analyzeMove(const GameState& state, Move move, Player player);
    static int evaluate(const GameState& state, Player perspective);
    static std::vector<CandidateMove> generateCandidateMoves(const GameState& state, Player player, std::size_t maxMoves);

private:
    static int evaluatePlayerPotential(const GameState& state, Player player);
};

}  // namespace gomoku
