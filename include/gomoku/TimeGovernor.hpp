#pragma once

#include <cstdint>
#include <optional>

#include "gomoku/ClockState.hpp"
#include "gomoku/ThreatAssessment.hpp"

namespace gomoku {

// Derived per-move budget produced by the time governor. Distinct from
// ClockState: ClockState is observed fact, MoveBudget is policy output.
// All fields are absolute durations for the current move, already
// clamped by any protocol/caller ceilings the governor knew about.
struct MoveBudget {
    std::int64_t targetMs {-1};             // soft target for normal stopping
    std::int64_t hardCapMs {-1};             // absolute search ceiling
    std::int64_t emergencyThresholdMs {-1};  // informational: below this, search is in emergency territory
    std::int64_t finalizationSlackMs {0};    // reserved for move emission / stop-propagation
    bool emergency {false};                  // true if timeLeft is already below the emergency threshold
    // Predicted cost multiplier for the *next* iterative-deepening
    // iteration relative to the last one. The root ID loop skips
    // starting the next iteration when elapsed + lastCost * this + slack
    // would exceed hardCapMs. 0.0 disables the check.
    double nextIterBranchingEstimate {0.0};
};

// Static knobs for the baseline governor (v1 step 3). Only the fields
// used by computeBaselineBudget are consumed today; the rest are
// placeholders the difficulty/affordability steps will read. Keeping
// them here locks the vocabulary in one place.
struct TimeGovernorConfig {
    // Reserves applied to the game clock before baseline allocation.
    std::int64_t absoluteReserveMs {500};
    double relativeReserveFrac {0.05};

    // Phase classification thresholds (in completed plies). Boundaries
    // are inclusive-on-upper for readability: ply < openingThreshold is
    // opening; ply < endgameThreshold is midgame; else endgame.
    int openingPlyThreshold {10};
    int endgamePlyThreshold {50};

    // Expected moves remaining for each phase.
    int expectedMovesOpening {30};
    int expectedMovesMidgame {20};
    int expectedMovesEndgame {8};

    // Phase-dependent scaling applied to the baseline allocation.
    double openingScale {0.7};
    double midgameScale {1.2};
    double endgameScale {1.0};

    // Cap fractions of usable remaining time.
    double softCapFrac {0.25};
    double hardCapFrac {0.40};

    // Below this much time_left we flag emergency on the MoveBudget.
    std::int64_t emergencyThresholdMs {1500};

    // Absolute floor so search always has time to do something.
    std::int64_t minSearchMs {30};

    // Ratio between target and hard cap. Hard cap is clamped above
    // target by this factor, then further clamped by hardCapFrac *
    // usable and by any turn cap.
    double hardOverTargetRatio {2.0};

    // Reserved for finalization/stop slack reported in MoveBudget.
    std::int64_t finalizationSlackMs {50};

    // Predicted cost of the next ID iteration relative to the last. 3.0
    // is a conservative default for alpha-beta + PVS with good move
    // ordering; smaller values let the search start an extra iteration
    // more often, larger values are safer against overshoot.
    double nextIterBranchingEstimate {3.0};

    // Threat asymmetry: defence bonus intentionally larger than attack
    // bonus. Rationale — a wrong defensive move usually loses on the
    // next ply, while a missed attack only costs a tempo and can be
    // recovered. The governor applies max(defenseBonus, attackBonus) so
    // that mixed positions do not stack bonuses multiplicatively.
    double defenseThreatBonusFrac {0.30};
    double attackThreatBonusFrac  {0.15};
};

class TimeGovernor {
public:
    // Returns std::nullopt when the governor is not engaged (no game
    // clock in the snapshot). In that case the caller must leave its
    // existing per-turn scheduling untouched.
    //
    // The optional ThreatAssessment lets the caller inform the governor
    // that the position is tactically sharp: the governor scales target
    // and hard cap up by defense/attack bonuses when set, then re-clamps
    // under the turn cap. A default-constructed assessment (both None)
    // leaves the baseline behaviour unchanged.
    std::optional<MoveBudget> computeBaselineBudget(
        const ClockState& clock,
        const TimeGovernorConfig& cfg,
        const ThreatAssessment& assessment = {}) const;
};

}  // namespace gomoku
