#pragma once

#include <string_view>

#include "gomoku/Types.hpp"

namespace gomoku {

struct RulesSpec {
    Ruleset ruleset {};
    int boardSize {15};
    bool exactFiveRequired {false};
    bool swapOpening {false};
    std::string_view name {};
};

const RulesSpec& rulesFor(Ruleset ruleset);

}  // namespace gomoku
