#pragma once

namespace gomoku {

// Compact, governor-facing summary of root-level tactical pressure.
// Produced by a small helper that translates engine pattern analysis
// into coarse levels so the time governor does not depend on the
// internals of Threats / PatternAnalysis.
enum class DefenseThreatLevel {
    None,      // opponent has no forcing reply
    Forcing,   // opponent can make OpenThree / SimpleFour that we must address
    Immediate, // opponent has an unblockable OpenFour or immediate Five
};

enum class AttackThreatLevel {
    None,      // no strong forcing move available to us
    Strong,    // we can play OpenThree / SimpleFour
    Immediate, // we can force OpenFour or Five this move
};

struct ThreatAssessment {
    DefenseThreatLevel defense {DefenseThreatLevel::None};
    AttackThreatLevel  attack  {AttackThreatLevel::None};
};

}  // namespace gomoku
