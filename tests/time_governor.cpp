#include <cassert>

#include "gomoku/ClockState.hpp"
#include "gomoku/TimeGovernor.hpp"

namespace {

using gomoku::ClockState;
using gomoku::MoveBudget;
using gomoku::TimeGovernor;
using gomoku::TimeGovernorConfig;

void testNoClockShortCircuits() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;  // defaults: timeLeftMs = -1
    assert(!clock.hasGameClock());
    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(!budget.has_value());
}

void testBaselineAllocatesInOpening() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;
    clock.moveNumber = 0;  // opening

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->targetMs >= cfg.minSearchMs);
    assert(budget->hardCapMs >= budget->targetMs);
    // Baseline opening: usable ~= 30000 - 500 - 1500 = 28000; /30 = 933; *0.7 = ~653
    // Target should be in the ballpark, not pegged at min or softCap.
    assert(budget->targetMs > 200);
    assert(budget->targetMs < 3000);
    // softCapFrac 0.25 of 28000 = 7000
    assert(budget->targetMs <= 7000);
    assert(!budget->emergency);
}

void testBaselineSpendsMoreInMidgame() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;

    clock.moveNumber = 0;
    const auto opening = governor.computeBaselineBudget(clock, cfg);
    clock.moveNumber = 20;
    const auto midgame = governor.computeBaselineBudget(clock, cfg);

    // Midgame scale 1.2 > opening 0.7 and expectedMoves are similar-ish,
    // so midgame target should exceed opening target.
    assert(opening && midgame);
    assert(midgame->targetMs > opening->targetMs);
}

void testTurnCapDominatesGovernor() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;
    clock.moveNumber = 20;     // midgame — governor would want more time
    clock.timeoutTurnMs = 100; // but protocol says 100ms max

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->hardCapMs <= 100);
    assert(budget->targetMs <= budget->hardCapMs);
}

void testEmergencyFlagSetWhenTimeIsLow() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 800;  // well below default emergencyThresholdMs=1500
    clock.moveNumber = 30;

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->emergency);
    assert(budget->targetMs >= cfg.minSearchMs);
    assert(budget->hardCapMs >= budget->targetMs);
}

void testTargetAndHardCapNeverBelowFloor() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 50;  // pathologically low — almost nothing usable
    clock.moveNumber = 40;

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->targetMs >= cfg.minSearchMs);
    assert(budget->hardCapMs >= budget->targetMs);
}

}  // namespace

int main() {
    testNoClockShortCircuits();
    testBaselineAllocatesInOpening();
    testBaselineSpendsMoreInMidgame();
    testTurnCapDominatesGovernor();
    testEmergencyFlagSetWhenTimeIsLow();
    testTargetAndHardCapNeverBelowFloor();
    return 0;
}
