#pragma once

#include <cstdint>
#include <vector>

#include "gomoku/ThreatTypes.hpp"

namespace gomoku {

enum class PatternCell : std::uint8_t {
    Empty = 0,
    Own = 1,
    Opponent = 2,
};

bool containsFiveIncludingTarget(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);
int countWinningContinuations(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);
ThreatType classifyPatternWindow(const std::vector<PatternCell>& cells, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);
// Hot-path overload: avoids constructing a std::vector per window. The
// caller can keep a std::array<PatternCell, N> on the stack and pass
// its data pointer + length.
ThreatType classifyPatternWindow(const PatternCell* cells, int length, int targetIndex, bool exactFiveRequired = false,
    bool ownBefore = false, bool ownAfter = false);

// Raw-bit classifier: for the tightest inner loop in computeMoveThreatInfo.
// Skips PatternCell materialization and fuses encoding with the table
// lookup. Caller hoists the table pointer outside the innermost loop so
// there is exactly one cross-TU function call per (length, exactFiveRequired)
// pair, not per window.
//
// Pre-conditions the caller in Threats.cpp already enforces:
//   - startOffset >= 0
//   - startOffset + Length <= line.length <= 15
// so bit indices always fit in the uint16 and no bounds check is needed.
const ThreatType* patternTableValues(int length, bool exactFiveRequired);

// Precomputed per-length encoding: bitEncode<Length>[x] returns the
// ternary value of placing digit 1 at every set bit of x (and 0
// elsewhere), i.e. sum over set bits i of 3^(Length-1-i). Multiplying
// this table's value by 2 gives the digit-2 contribution.
namespace pattern_detail {

template<int Length>
struct BitEncodeTable {
    std::uint16_t values[1U << Length];

    constexpr BitEncodeTable() : values{} {
        for (int x = 0; x < (1 << Length); ++x) {
            int code = 0;
            for (int i = 0; i < Length; ++i) {
                const int digit = ((x >> i) & 1);
                code = code * 3 + digit;
            }
            values[x] = static_cast<std::uint16_t>(code);
        }
    }
};

template<int Length>
inline constexpr BitEncodeTable<Length> kBitEncode = BitEncodeTable<Length>();

}  // namespace pattern_detail

// The caller must OR the target bit into `ownBitsWithTarget` BEFORE
// calling — that is, `ownBitsWithTarget = rawOwnBits | (1U << lineOffset)`
// where lineOffset is the absolute position of the empty target cell on
// the line. This fold moves the target special-case out of the inner
// loop so classification is a branchless encode + table lookup.
template<int Length>
inline ThreatType classifyPatternBits(
    std::uint16_t ownBitsWithTarget, std::uint16_t opponentBits,
    int startOffset, int targetOffset,
    bool ownBefore, bool ownAfter,
    const ThreatType* tableValues)
{
    constexpr std::uint16_t kMask = static_cast<std::uint16_t>((1U << Length) - 1);
    const std::uint16_t own = static_cast<std::uint16_t>((ownBitsWithTarget >> startOffset) & kMask);
    const std::uint16_t opp = static_cast<std::uint16_t>((opponentBits >> startOffset) & kMask);
    const auto& encode = pattern_detail::kBitEncode<Length>;
    const int code = static_cast<int>(encode.values[own]) + 2 * static_cast<int>(encode.values[opp]);
    const int boundaryState = (ownBefore ? 2 : 0) | (ownAfter ? 1 : 0);
    return tableValues[(code * 4 + boundaryState) * Length + targetOffset];
}

}  // namespace gomoku
