#include "gomoku/PatternAnalysis.hpp"

namespace gomoku {

namespace {

constexpr int kBoundaryVariants = 4;

struct PatternTable {
    int length {0};
    int tableSize {0};
    std::vector<ThreatType> values;
};

int pow3(int exponent) {
    int result = 1;
    for (int index = 0; index < exponent; ++index) {
        result *= 3;
    }
    return result;
}

int encodeCells(const std::vector<PatternCell>& cells) {
    int code = 0;
    for (PatternCell cell : cells) {
        code = code * 3 + static_cast<int>(cell);
    }
    return code;
}

int encodeBoundaryState(bool ownBefore, bool ownAfter) {
    return (ownBefore ? 2 : 0) | (ownAfter ? 1 : 0);
}

ThreatType classifyPatternInternal(
    const std::vector<PatternCell>& cells, int targetIndex, int recursionDepth, bool exactFiveRequired, bool ownBefore, bool ownAfter) {
    if (cells[targetIndex] != PatternCell::Own) {
        return ThreatType::None;
    }

    if (containsFiveIncludingTarget(cells, targetIndex, exactFiveRequired, ownBefore, ownAfter)) {
        return ThreatType::Five;
    }

    const int immediateWins = countWinningContinuations(cells, targetIndex, exactFiveRequired, ownBefore, ownAfter);
    if (immediateWins >= 2) {
        return ThreatType::OpenFour;
    }
    if (immediateWins == 1) {
        return ThreatType::SimpleFour;
    }

    int openFourBuilders = 0;
    int threeBuilders = 0;
    for (int index = 0; index < static_cast<int>(cells.size()); ++index) {
        if (cells[index] != PatternCell::Empty) {
            continue;
        }

        auto extended = cells;
        extended[index] = PatternCell::Own;

        if (countWinningContinuations(extended, targetIndex, exactFiveRequired, ownBefore, ownAfter) >= 2) {
            ++openFourBuilders;
        }

        if (recursionDepth == 0) {
            continue;
        }

        const ThreatType nextThreat = classifyPatternInternal(extended, targetIndex, recursionDepth - 1, exactFiveRequired, ownBefore, ownAfter);
        if (nextThreat == ThreatType::OpenThree || nextThreat == ThreatType::BrokenThree
            || nextThreat == ThreatType::OpenFour || nextThreat == ThreatType::SimpleFour) {
            ++threeBuilders;
        }
    }

    if (openFourBuilders >= 2) {
        return ThreatType::OpenThree;
    }
    if (openFourBuilders == 1) {
        return ThreatType::BrokenThree;
    }
    if (threeBuilders >= 2) {
        return ThreatType::Two;
    }
    if (threeBuilders == 1) {
        return ThreatType::One;
    }
    return ThreatType::None;
}

PatternTable buildPatternTable(int length, bool exactFiveRequired) {
    PatternTable table;
    table.length = length;
    table.tableSize = pow3(length);
    table.values.resize(static_cast<std::size_t>(table.tableSize * kBoundaryVariants * length), ThreatType::None);

    std::vector<PatternCell> cells(static_cast<std::size_t>(length), PatternCell::Empty);
    for (int code = 0; code < table.tableSize; ++code) {
        int remaining = code;
        for (int index = length - 1; index >= 0; --index) {
            cells[static_cast<std::size_t>(index)] = static_cast<PatternCell>(remaining % 3);
            remaining /= 3;
        }

        for (int boundaryState = 0; boundaryState < kBoundaryVariants; ++boundaryState) {
            const bool ownBefore = (boundaryState & 2) != 0;
            const bool ownAfter = (boundaryState & 1) != 0;
            for (int targetIndex = 0; targetIndex < length; ++targetIndex) {
                const std::size_t valueIndex = static_cast<std::size_t>((code * kBoundaryVariants + boundaryState) * length + targetIndex);
                table.values[valueIndex] = classifyPatternInternal(cells, targetIndex, 1, exactFiveRequired, ownBefore, ownAfter);
            }
        }
    }

    return table;
}

const PatternTable& tableForLength(int length, bool exactFiveRequired) {
    static const PatternTable kFreestyleFive = buildPatternTable(5, false);
    static const PatternTable kFreestyleSix = buildPatternTable(6, false);
    static const PatternTable kFreestyleSeven = buildPatternTable(7, false);
    static const PatternTable kExactFive = buildPatternTable(5, true);
    static const PatternTable kExactSix = buildPatternTable(6, true);
    static const PatternTable kExactSeven = buildPatternTable(7, true);

    if (exactFiveRequired) {
        switch (length) {
            case 5:
                return kExactFive;
            case 6:
                return kExactSix;
            case 7:
            default:
                return kExactSeven;
        }
    }

    switch (length) {
        case 5:
            return kFreestyleFive;
        case 6:
            return kFreestyleSix;
        case 7:
        default:
            return kFreestyleSeven;
    }
}

}  // namespace

bool containsFiveIncludingTarget(
    const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired, bool ownBefore, bool ownAfter) {
    if (cells[targetIndex] != PatternCell::Own || static_cast<int>(cells.size()) < 5) {
        return false;
    }

    for (int start = 0; start + 5 <= static_cast<int>(cells.size()); ++start) {
        if (targetIndex < start || targetIndex >= start + 5) {
            continue;
        }

        bool allOwn = true;
        for (int index = start; index < start + 5; ++index) {
            if (cells[index] != PatternCell::Own) {
                allOwn = false;
                break;
            }
        }

        if (!allOwn) {
            continue;
        }

        if (!exactFiveRequired) {
            return true;
        }

        const bool extendsBefore = (start > 0) ? (cells[static_cast<std::size_t>(start - 1)] == PatternCell::Own) : ownBefore;
        const bool extendsAfter = (start + 5 < static_cast<int>(cells.size()))
            ? (cells[static_cast<std::size_t>(start + 5)] == PatternCell::Own)
            : ownAfter;
        if (!extendsBefore && !extendsAfter) {
            return true;
        }
    }

    return false;
}

int countWinningContinuations(
    const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired, bool ownBefore, bool ownAfter) {
    int count = 0;
    for (int index = 0; index < static_cast<int>(cells.size()); ++index) {
        if (cells[index] != PatternCell::Empty) {
            continue;
        }

        auto extended = cells;
        extended[index] = PatternCell::Own;
        if (containsFiveIncludingTarget(extended, targetIndex, exactFiveRequired, ownBefore, ownAfter)) {
            ++count;
        }
    }

    return count;
}

ThreatType classifyPatternWindow(
    const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired, bool ownBefore, bool ownAfter) {
    const PatternTable& table = tableForLength(static_cast<int>(cells.size()), exactFiveRequired);
    const int code = encodeCells(cells);
    const int boundaryState = encodeBoundaryState(ownBefore, ownAfter);
    return table.values[static_cast<std::size_t>((code * kBoundaryVariants + boundaryState) * table.length + targetIndex)];
}

}  // namespace gomoku
