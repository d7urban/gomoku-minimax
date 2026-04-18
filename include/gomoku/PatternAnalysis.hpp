#pragma once

#include <cstdint>
#include <vector>

#include "gomoku/ThreatTypes.hpp"

namespace gomoku {

enum class PatternCell : std::uint8_t {
    Empty = 0,
    Own = 1,
    Opponent = 2,
};

bool containsFiveIncludingTarget(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);
int countWinningContinuations(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);
ThreatType classifyPatternWindow(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);

}  // namespace gomoku
