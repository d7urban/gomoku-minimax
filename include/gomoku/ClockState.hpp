#pragma once

#include <cstdint>

namespace gomoku {

// External clock facts observed from the match protocol or harness. This
// is observed state, not a derived budget: the governor will later turn
// a ClockState into a MoveBudget, but it must never mutate ClockState in
// place. Sentinel -1 means "unknown/unset" for the corresponding field,
// which lets fixed-turn mode coexist with global-clock mode using the
// same type.
struct ClockState {
    std::int64_t timeLeftMs {-1};      // per-game remaining time for side to move
    std::int64_t timeoutMatchMs {-1};  // total match limit, if the protocol supplied one
    std::int64_t timeoutTurnMs {-1};   // hard per-turn ceiling, if supplied
    int movesToReset {-1};             // plies remaining until the current time-control period resets, if known
    std::uint32_t moveNumber {0};      // 0-based index of the move being searched

    bool hasGameClock() const { return timeLeftMs >= 0; }
    bool hasTurnCap()   const { return timeoutTurnMs >= 0; }
};

}  // namespace gomoku
