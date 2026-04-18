#pragma once

#include <array>
#include <cstdint>

namespace gomoku {

enum class ThreatType : std::uint8_t {
    None = 0,
    One = 1,
    Two = 2,
    BrokenThree = 3,
    OpenThree = 4,
    SimpleFour = 5,
    OpenFour = 6,
    Five = 7,
};

struct MoveThreatInfo {
    std::array<ThreatType, 4> lineThreats {
        ThreatType::None,
        ThreatType::None,
        ThreatType::None,
        ThreatType::None,
    };
    ThreatType best {ThreatType::None};
    ThreatType second {ThreatType::None};
    int totalScore {0};

    bool operator==(const MoveThreatInfo&) const = default;
};

}  // namespace gomoku
