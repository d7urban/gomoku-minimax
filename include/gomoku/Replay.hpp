#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/Match.hpp"

namespace gomoku {

struct PositionAnnotation {
    std::string label;
    Player analysisPlayer {Player::None};
    std::vector<Move> principalVariation;
};

std::string serializeReplay(const GameState& state);
bool deserializeReplay(const RulesSpec& rules, std::string_view text, GameState& state, std::string& error);
std::string serializeMatchSession(const Match& match);
bool deserializeMatchSession(std::string_view text, Match& match, std::string& error);
std::string serializeAnnotatedPosition(const GameState& state, const PositionAnnotation& annotation);
bool deserializeAnnotatedPosition(std::string_view text, GameState& state, PositionAnnotation& annotation, std::string& error);

}  // namespace gomoku
