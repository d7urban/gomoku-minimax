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

    switch (ruleset) {
        case Ruleset::Freestyle15:
            return kFreestyle15;
        case Ruleset::Standard15:
            return kStandard15;
        default:
            return kFreestyle15;
    }
}

}  // namespace gomoku
