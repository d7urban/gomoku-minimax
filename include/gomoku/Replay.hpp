#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/ProofSearch.hpp"

namespace gomoku {

struct PositionAnnotation {
    std::string label;
    Player analysisPlayer {Player::None};
    ProofOutcome proofOutcome {ProofOutcome::Unknown};
    std::uint64_t proofNodes {0};
    std::vector<Move> principalVariation;
    std::vector<Move> provenWinningMoves;
    std::vector<Move> provenLosingMoves;
};

std::string serializeReplay(const GameState& state);
bool deserializeReplay(const RulesSpec& rules, std::string_view text, GameState& state, std::string& error);
std::string serializeAnnotatedPosition(const GameState& state, const PositionAnnotation& annotation);
bool deserializeAnnotatedPosition(std::string_view text, GameState& state, PositionAnnotation& annotation, std::string& error);

}  // namespace gomoku
