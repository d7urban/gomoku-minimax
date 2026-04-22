#include "TestAssert.hpp"
#include "gomoku/ClockState.hpp"
#include "gomoku/ThreatAssessment.hpp"
#include "gomoku/TimeGovernor.hpp"

namespace {

using gomoku::AttackThreatLevel;
using gomoku::ClockState;
using gomoku::DefenseThreatLevel;
using gomoku::MoveBudget;
using gomoku::ThreatAssessment;
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

void testBudgetReportsBranchingEstimate() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    cfg.nextIterBranchingEstimate = 4.5;
    ClockState clock;
    clock.timeLeftMs = 10'000;
    clock.moveNumber = 5;

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->nextIterBranchingEstimate == 4.5);
    assert(budget->finalizationSlackMs == cfg.finalizationSlackMs);
}

void testBudgetReportsInstabilityKnobs() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    cfg.bestMoveUnstableScale = 1.5;
    cfg.bestMoveStableScale   = 0.6;
    cfg.stableIterationsNeeded = 3;
    ClockState clock;
    clock.timeLeftMs = 10'000;
    clock.moveNumber = 5;

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());
    assert(budget->bestMoveUnstableScale  == 1.5);
    assert(budget->bestMoveStableScale    == 0.6);
    assert(budget->stableIterationsNeeded == 3);
}

void testThreatBonusScalesBudgetUp() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;
    clock.moveNumber = 20;  // midgame

    const auto baseline = governor.computeBaselineBudget(clock, cfg, {});

    ThreatAssessment defend;
    defend.defense = DefenseThreatLevel::Immediate;
    const auto defending = governor.computeBaselineBudget(clock, cfg, defend);

    assert(baseline && defending);
    assert(defending->targetMs  > baseline->targetMs);
    assert(defending->hardCapMs > baseline->hardCapMs);
    assert(defending->hardCapMs >= defending->targetMs);
}

void testDefenseBonusDominatesAttackBonus() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;
    clock.moveNumber = 20;

    ThreatAssessment attack;
    attack.attack = AttackThreatLevel::Immediate;
    const auto attacking = governor.computeBaselineBudget(clock, cfg, attack);

    ThreatAssessment defend;
    defend.defense = DefenseThreatLevel::Immediate;
    const auto defending = governor.computeBaselineBudget(clock, cfg, defend);

    // Default cfg: defenseThreatBonusFrac 0.30 > attackThreatBonusFrac 0.15,
    // so defending gets more time for the same clock state.
    assert(attacking && defending);
    assert(defending->targetMs > attacking->targetMs);
}

void testThreatBonusDoesNotStack() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30'000;
    clock.moveNumber = 20;

    ThreatAssessment defend;
    defend.defense = DefenseThreatLevel::Immediate;
    const auto onlyDefend = governor.computeBaselineBudget(clock, cfg, defend);

    ThreatAssessment both;
    both.defense = DefenseThreatLevel::Immediate;
    both.attack  = AttackThreatLevel::Immediate;
    const auto mixed = governor.computeBaselineBudget(clock, cfg, both);

    // Governor takes max(defenseBonus, attackBonus); mixed must match
    // defence-only (the larger bonus), not be compounded higher.
    assert(onlyDefend && mixed);
    assert(mixed->targetMs  == onlyDefend->targetMs);
    assert(mixed->hardCapMs == onlyDefend->hardCapMs);
}

void testThreatBonusReclampedByTurnCap() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs     = 30'000;
    clock.moveNumber     = 20;
    clock.timeoutTurnMs  = 200;  // protocol says 200ms regardless of bonuses

    ThreatAssessment defend;
    defend.defense = DefenseThreatLevel::Immediate;
    const auto budget = governor.computeBaselineBudget(clock, cfg, defend);

    assert(budget.has_value());
    assert(budget->hardCapMs <= 200);
    assert(budget->targetMs  <= budget->hardCapMs);
}

void testResetBasedClockUsesActualPeriodLength() {
    TimeGovernor governor;
    TimeGovernorConfig cfg;
    ClockState clock;
    clock.timeLeftMs = 30LL * 60LL * 1000LL;
    clock.moveNumber = 0;   // opening
    clock.movesToReset = 80;

    const auto budget = governor.computeBaselineBudget(clock, cfg);
    assert(budget.has_value());

    // With a repeating 30m/80 control, the governor should spend
    // against the real period length, not the generic opening 30-move
    // heuristic that would inflate this to roughly 39.9s.
    const std::int64_t usable = clock.timeLeftMs - cfg.absoluteReserveMs
        - static_cast<std::int64_t>(cfg.relativeReserveFrac * static_cast<double>(clock.timeLeftMs));
    const std::int64_t expectedTarget = static_cast<std::int64_t>(
        (static_cast<double>(usable) / static_cast<double>(clock.movesToReset)) * cfg.openingScale);

    assert(budget->targetMs == expectedTarget);
    assert(budget->targetMs < 20'000);
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
    testBudgetReportsBranchingEstimate();
    testBudgetReportsInstabilityKnobs();
    testThreatBonusScalesBudgetUp();
    testDefenseBonusDominatesAttackBonus();
    testThreatBonusDoesNotStack();
    testThreatBonusReclampedByTurnCap();
    testResetBasedClockUsesActualPeriodLength();
    testTargetAndHardCapNeverBelowFloor();
    return 0;
}
