#include "gomoku/TimeGovernor.hpp"

#include <algorithm>
#include <cmath>

namespace gomoku {

namespace {

enum class Phase { Opening, Midgame, Endgame };

Phase classifyPhase(const ClockState& clock, const TimeGovernorConfig& cfg) {
    const int ply = static_cast<int>(clock.moveNumber);
    if (ply < cfg.openingPlyThreshold) return Phase::Opening;
    if (ply < cfg.endgamePlyThreshold) return Phase::Midgame;
    return Phase::Endgame;
}

int expectedMovesLeft(Phase phase, const TimeGovernorConfig& cfg) {
    switch (phase) {
        case Phase::Opening: return std::max(1, cfg.expectedMovesOpening);
        case Phase::Midgame: return std::max(1, cfg.expectedMovesMidgame);
        case Phase::Endgame: return std::max(1, cfg.expectedMovesEndgame);
    }
    return std::max(1, cfg.expectedMovesMidgame);
}

double phaseScale(Phase phase, const TimeGovernorConfig& cfg) {
    switch (phase) {
        case Phase::Opening: return cfg.openingScale;
        case Phase::Midgame: return cfg.midgameScale;
        case Phase::Endgame: return cfg.endgameScale;
    }
    return cfg.midgameScale;
}

std::int64_t clamp64(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    if (hi < lo) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

std::optional<MoveBudget> TimeGovernor::computeBaselineBudget(
    const ClockState& clock,
    const TimeGovernorConfig& cfg,
    const ThreatAssessment& assessment) const
{
    // Short-circuit: without a game clock we have nothing global to
    // govern. The caller keeps its existing per-turn scheduling.
    if (!clock.hasGameClock()) {
        return std::nullopt;
    }

    const std::int64_t timeLeft = clock.timeLeftMs;
    const std::int64_t relativeReserve = static_cast<std::int64_t>(
        std::llround(cfg.relativeReserveFrac * static_cast<double>(std::max<std::int64_t>(timeLeft, 0))));
    std::int64_t usable = timeLeft - cfg.absoluteReserveMs - relativeReserve;
    if (usable < cfg.minSearchMs) {
        usable = cfg.minSearchMs;
    }

    const Phase phase = classifyPhase(clock, cfg);
    const int movesLeft = expectedMovesLeft(phase, cfg);
    const double scale = phaseScale(phase, cfg);

    const double baseline = (static_cast<double>(usable) / static_cast<double>(movesLeft)) * scale;

    const std::int64_t softCeil = static_cast<std::int64_t>(cfg.softCapFrac * static_cast<double>(usable));
    const std::int64_t hardCeil = static_cast<std::int64_t>(cfg.hardCapFrac * static_cast<double>(usable));

    std::int64_t targetMs = clamp64(static_cast<std::int64_t>(baseline),
                                    cfg.minSearchMs,
                                    std::max<std::int64_t>(softCeil, cfg.minSearchMs));

    std::int64_t hardCapMs = static_cast<std::int64_t>(baseline * cfg.hardOverTargetRatio);
    hardCapMs = clamp64(hardCapMs,
                        targetMs,
                        std::max<std::int64_t>(hardCeil, targetMs));

    // Threat asymmetry: the caller can signal that this position is
    // tactically sharp. Defence and attack bonuses never stack — mixed
    // positions take max(defenseBonus, attackBonus) to avoid runaway
    // budgets from compounded modifiers. The bonus is allowed to push
    // past the softCeil/hardCeil fractions (those are conservative
    // defaults, not safety rails), but never past `usable` or the
    // external turn cap.
    const double defenseBonus = (assessment.defense != DefenseThreatLevel::None)
        ? cfg.defenseThreatBonusFrac : 0.0;
    const double attackBonus  = (assessment.attack  != AttackThreatLevel::None)
        ? cfg.attackThreatBonusFrac  : 0.0;
    const double threatScale  = 1.0 + std::max(defenseBonus, attackBonus);
    if (threatScale > 1.0) {
        targetMs  = static_cast<std::int64_t>(std::llround(static_cast<double>(targetMs)  * threatScale));
        hardCapMs = static_cast<std::int64_t>(std::llround(static_cast<double>(hardCapMs) * threatScale));
        if (targetMs  > usable) targetMs  = usable;
        if (hardCapMs > usable) hardCapMs = usable;
        if (hardCapMs < targetMs) hardCapMs = targetMs;
    }

    // Protocol-supplied turn cap wins if present. The governor is not
    // sovereign: external ceilings always dominate, including over any
    // threat bonus applied above.
    if (clock.hasTurnCap() && clock.timeoutTurnMs > 0) {
        if (hardCapMs > clock.timeoutTurnMs) hardCapMs = clock.timeoutTurnMs;
        if (targetMs > hardCapMs) targetMs = hardCapMs;
    }

    MoveBudget budget;
    budget.targetMs = targetMs;
    budget.hardCapMs = hardCapMs;
    budget.emergencyThresholdMs = cfg.emergencyThresholdMs;
    budget.finalizationSlackMs = cfg.finalizationSlackMs;
    budget.emergency = (timeLeft <= cfg.emergencyThresholdMs);
    budget.nextIterBranchingEstimate = cfg.nextIterBranchingEstimate;
    budget.bestMoveUnstableScale  = cfg.bestMoveUnstableScale;
    budget.bestMoveStableScale    = cfg.bestMoveStableScale;
    budget.stableIterationsNeeded = cfg.stableIterationsNeeded;
    return budget;
}

}  // namespace gomoku
