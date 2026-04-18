#pragma once

#include <cstdint>
#include <vector>

#include "gomoku/GameState.hpp"
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
MoveThreatInfo computeMoveThreatInfo(const GameState& state, Move move, Player player);

class StaticEvaluator {
public:
    static MoveThreatInfo analyzeMove(const GameState& state, Move move, Player player);
    static int evaluate(const GameState& state, Player perspective);
    static std::vector<CandidateMove> generateCandidateMoves(const GameState& state, Player player, std::size_t maxMoves);

private:
    static int evaluatePlayerPotential(const GameState& state, Player player);
};

}  // namespace gomoku
