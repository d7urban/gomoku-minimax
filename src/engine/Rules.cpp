#include "gomoku/Rules.hpp"

namespace gomoku {

const RulesSpec& rulesFor(Ruleset ruleset) {
    static const RulesSpec kFreestyle15 {
        Ruleset::Freestyle15,
        15,
        false,
        false,
        "15x15 free-style",
    };

    static const RulesSpec kStandard15 {
        Ruleset::Standard15,
        15,
        true,
        false,
        "15x15 standard",
    };

    static const RulesSpec kSwap16 {
        Ruleset::Swap16,
        16,
        false,
        true,
        "16x16 swap-rule",
    };

    switch (ruleset) {
        case Ruleset::Freestyle15:
            return kFreestyle15;
        case Ruleset::Standard15:
            return kStandard15;
        case Ruleset::Swap16:
            return kSwap16;
        default:
            return kFreestyle15;
    }
}

}  // namespace gomoku
