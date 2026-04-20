#include "gomoku/Threats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "gomoku/PatternAnalysis.hpp"

namespace gomoku {

namespace {

#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
ProfilingCounters g_profilingCounters {};
#endif

void bumpNearStoneChecks() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.nearStoneChecks;
#endif
}

void bumpGenerateCandidateCalls() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.generateCandidateCalls;
#endif
}

void bumpAnalyzeMoveCalls() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.analyzeMoveCalls;
#endif
}

void bumpComputeThreatInfoCalls() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.computeThreatInfoCalls;
#endif
}

void bumpPatternWindowsScanned() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.patternWindowsScanned;
#endif
}

int lineThreatScore(ThreatType first, ThreatType second) {
    return threatWeight(first) * 3 / 2 + threatWeight(second);
}

std::array<ThreatType, 2> topTwoThreats(const std::array<ThreatType, 4>& threats) {
    std::array<ThreatType, 4> sorted = threats;
    std::sort(sorted.begin(), sorted.end(), [](ThreatType left, ThreatType right) {
        return threatSeverity(left) > threatSeverity(right);
    });
    return {sorted[0], sorted[1]};
}

Move centerMove(const GameState& state) {
    const int center = state.boardSize() / 2;
    return {center, center};
}

bool hasAnyStone(const GameState& state) {
    return state.moveCount() > 0;
}

int centralityScore(const GameState& state, Move move) {
    const int center = state.boardSize() / 2;
    return 200 - 12 * (std::abs(move.row - center) + std::abs(move.col - center));
}

int neighborhoodPressure(const GameState& state, Move move, Player player) {
    const std::vector<Player>& board = state.board();
    const int boardSize = state.boardSize();
    const Player opponent = otherPlayer(player);
    // Clamp the 5x5 window to the board bounds once, then scan the
    // resulting rectangle directly against the board vector — avoids the
    // 50 redundant isInside/cellAt calls the naive loop incurred.
    const int rowMin = std::max(0, move.row - 2);
    const int rowMax = std::min(boardSize - 1, move.row + 2);
    const int colMin = std::max(0, move.col - 2);
    const int colMax = std::min(boardSize - 1, move.col + 2);

    int score = 0;
    for (int row = rowMin; row <= rowMax; ++row) {
        const int dRow = row - move.row;
        const bool rowInner = (dRow >= -1 && dRow <= 1);
        const std::size_t rowBase = static_cast<std::size_t>(row) * static_cast<std::size_t>(boardSize);
        for (int col = colMin; col <= colMax; ++col) {
            const int dCol = col - move.col;
            if (dRow == 0 && dCol == 0) {
                continue;
            }
            const Player cell = board[rowBase + static_cast<std::size_t>(col)];
            if (cell == player) {
                score += (rowInner && dCol >= -1 && dCol <= 1) ? 18 : 7;
            } else if (cell == opponent) {
                score += 5;
            }
        }
    }
    return score;
}

bool isNearExistingStone(const GameState& state, Move move) {
    bumpNearStoneChecks();
    return state.isNearStone(move);
}

}  // namespace

std::string_view toString(ThreatType type) {
    switch (type) {
        case ThreatType::None:
            return "none";
        case ThreatType::One:
            return "one";
        case ThreatType::Two:
            return "two";
        case ThreatType::BrokenThree:
            return "broken_three";
        case ThreatType::OpenThree:
            return "open_three";
        case ThreatType::SimpleFour:
            return "simple_four";
        case ThreatType::OpenFour:
            return "open_four";
        case ThreatType::Five:
            return "five";
        default:
            return "unknown";
    }
}

// Ordering key only: rewards combined threats so double-threats sort above the
// severity of their primary component. Do NOT use this scale for evaluation —
// the magnitudes are step-functions chosen to break ordering ties, not to
// reflect winning probability.
int threatSeverityEnhanced(const MoveThreatInfo& info) {
    if (info.best == ThreatType::OpenFour) {
        return 1000 + threatSeverity(info.second);
    }
    if (info.best == ThreatType::SimpleFour && info.second == ThreatType::OpenThree) {
        return 800;
    }
    if (info.best == ThreatType::OpenThree && info.second == ThreatType::OpenThree) {
        return 600;
    }
    if (info.best == ThreatType::OpenThree && info.second == ThreatType::BrokenThree) {
        return 400;
    }
    return threatSeverity(info.best) + (threatSeverity(info.second) / 4);
}

namespace {

ThreatType computeDirectionThreatImpl(
    const GameState& state, Move move, Player player, int directionIndex,
    const ThreatType* table5, const ThreatType* table6, const ThreatType* table7)
{
    ThreatType best = ThreatType::None;
    const GameState::LineLocation line = state.lineLocation(move, directionIndex);
    const std::uint16_t ownBits = state.lineBits(player, directionIndex, line.lineIndex);
    const std::uint16_t opponentBits = state.lineBits(otherPlayer(player), directionIndex, line.lineIndex);

    // If the line has no own stones anywhere, the target is the only own
    // cell in every window through it — and a single own stone in a 5-7
    // cell window can never form a 3+ pattern. Bail out.
    if (ownBits == 0) {
        return ThreatType::None;
    }

    // Fold the hypothetical target stone into ownBits once; the inner
    // classifier can now treat all positions uniformly.
    const std::uint16_t ownBitsWithTarget = static_cast<std::uint16_t>(ownBits | (1U << line.offset));

    auto severityLess = [](ThreatType left, ThreatType right) {
        return threatSeverity(left) < threatSeverity(right);
    };

    auto scanWindows = [&]<int Length>(const ThreatType* tableValues, std::integral_constant<int, Length>) {
        constexpr std::uint16_t lengthMask = static_cast<std::uint16_t>((1U << Length) - 1);
        for (int targetOffset = 0; targetOffset < Length; ++targetOffset) {
            const int startOffset = line.offset - targetOffset;
            if (startOffset < 0 || startOffset + Length > line.length) {
                continue;
            }
            const std::uint16_t windowMask = static_cast<std::uint16_t>(lengthMask << startOffset);
            if ((ownBits & windowMask) == 0U) {
                continue;
            }
            bumpPatternWindowsScanned();
            const bool ownBefore = startOffset > 0
                && (ownBits & static_cast<std::uint16_t>(1U << (startOffset - 1))) != 0U;
            const bool ownAfter = startOffset + Length < line.length
                && (ownBits & static_cast<std::uint16_t>(1U << (startOffset + Length))) != 0U;
            const ThreatType window = classifyPatternBits<Length>(
                ownBitsWithTarget, opponentBits, startOffset, targetOffset, ownBefore, ownAfter, tableValues);
            best = std::max(best, window, severityLess);
        }
    };

    scanWindows(table5, std::integral_constant<int, 5>{});
    scanWindows(table6, std::integral_constant<int, 6>{});
    scanWindows(table7, std::integral_constant<int, 7>{});

    return best;
}

}  // namespace

ThreatType computeLineThreatForDirection(const GameState& state, Move move, Player player, int directionIndex) {
    if (!state.isInside(move.row, move.col) || state.cellAt(move.row, move.col) != Player::None) {
        return ThreatType::None;
    }
    const bool exactFiveRequired = state.rules().exactFiveRequired;
    return computeDirectionThreatImpl(state, move, player, directionIndex,
        patternTableValues(5, exactFiveRequired),
        patternTableValues(6, exactFiveRequired),
        patternTableValues(7, exactFiveRequired));
}

MoveThreatInfo computeMoveThreatInfo(const GameState& state, Move move, Player player) {
    bumpComputeThreatInfoCalls();
    MoveThreatInfo info;
    if (!state.isInside(move.row, move.col) || state.cellAt(move.row, move.col) != Player::None) {
        return info;
    }

    const bool exactFiveRequired = state.rules().exactFiveRequired;
    const ThreatType* const table5 = patternTableValues(5, exactFiveRequired);
    const ThreatType* const table6 = patternTableValues(6, exactFiveRequired);
    const ThreatType* const table7 = patternTableValues(7, exactFiveRequired);

    for (int directionIndex = 0; directionIndex < 4; ++directionIndex) {
        info.lineThreats[static_cast<std::size_t>(directionIndex)] =
            computeDirectionThreatImpl(state, move, player, directionIndex, table5, table6, table7);
    }

    const auto [first, second] = topTwoThreats(info.lineThreats);
    info.best = first;
    info.second = second;
    info.totalScore = lineThreatScore(first, second);
    return info;
}

MoveThreatInfo StaticEvaluator::analyzeMove(const GameState& state, Move move, Player player) {
    bumpAnalyzeMoveCalls();
    return state.threatInfoAt(move, player);
}

bool profilingCountersEnabled() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    return true;
#else
    return false;
#endif
}

void resetProfilingCounters() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    g_profilingCounters = {};
#endif
}

ProfilingCounters readProfilingCounters() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    return g_profilingCounters;
#else
    return {};
#endif
}

void bumpLegalMovesCounter() {
#if defined(GOMOKU_ENABLE_SEARCH_PROFILING)
    ++g_profilingCounters.legalMovesCalls;
#endif
}

int StaticEvaluator::evaluatePlayerPotential(const GameState& state, Player player) {
    return state.totalPotential(player);
}

int StaticEvaluator::evaluate(const GameState& state, Player perspective) {
    if (state.result() == GameResult::Draw) {
        return 0;
    }
    if ((perspective == Player::Black && state.result() == GameResult::BlackWin)
        || (perspective == Player::White && state.result() == GameResult::WhiteWin)) {
        return 1000000;
    }
    if ((perspective == Player::Black && state.result() == GameResult::WhiteWin)
        || (perspective == Player::White && state.result() == GameResult::BlackWin)) {
        return -1000000;
    }

    const Player opponent = otherPlayer(perspective);
    int score = evaluatePlayerPotential(state, perspective) - evaluatePlayerPotential(state, opponent);

    for (int row = 0; row < state.boardSize(); ++row) {
        for (int col = 0; col < state.boardSize(); ++col) {
            const Player cell = state.cellAt(row, col);
            if (cell == Player::None) {
                continue;
            }

            const int delta = centralityScore(state, {row, col});
            if (cell == perspective) {
                score += delta / 4;
            } else {
                score -= delta / 4;
            }
        }
    }

    if (state.sideToMove() == perspective) {
        score += 12;
    }

    return score;
}

std::vector<CandidateMove> StaticEvaluator::generateCandidateMoves(const GameState& state, Player player, std::size_t maxMoves) {
    bumpGenerateCandidateCalls();
    std::vector<CandidateMove> moves;
    if (state.isGameOver() || state.isSwapDecisionPending() || state.sideToMove() != player) {
        return moves;
    }

    if (!hasAnyStone(state)) {
        const Move move = centerMove(state);
        moves.push_back({move, analyzeMove(state, move, player), 100000});
        return moves;
    }

    const std::vector<Player>& board = state.board();
    const std::vector<std::uint8_t>& nearCounts = state.nearStoneCounts();
    const int boardSize = state.boardSize();
    moves.reserve(64);

    for (int row = 0; row < boardSize; ++row) {
        for (int col = 0; col < boardSize; ++col) {
            const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
            if (board[index] != Player::None) {
                continue;
            }
            if (nearCounts[index] == 0) {
                continue;
            }
            const Move move {row, col};

            CandidateMove candidate;
            candidate.move = move;
            candidate.threatInfo = analyzeMove(state, move, player);
            candidate.score = candidate.threatInfo.totalScore + centralityScore(state, move) + neighborhoodPressure(state, move, player);
            const MoveThreatInfo defensiveInfo = analyzeMove(state, move, otherPlayer(player));
            candidate.score += defensiveInfo.totalScore;

            if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
                candidate.score += 500'000;
            } else if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::OpenThree)) {
                candidate.score += 50'000;
            } else if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::BrokenThree)) {
                candidate.score += 5'000;
            }

            if (candidate.threatInfo.best == ThreatType::Five) {
                candidate.score += 10000000;
            }

            moves.push_back(candidate);
        }
    }

    if (moves.empty()) {
        for (int row = 0; row < boardSize; ++row) {
            for (int col = 0; col < boardSize; ++col) {
                const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
                if (board[index] != Player::None) {
                    continue;
                }
                const Move move {row, col};
                CandidateMove candidate;
                candidate.move = move;
                candidate.threatInfo = analyzeMove(state, move, player);
                candidate.score = candidate.threatInfo.totalScore + centralityScore(state, move);
                moves.push_back(candidate);
            }
        }
    }

    std::sort(moves.begin(), moves.end(), [](const CandidateMove& left, const CandidateMove& right) {
        return left.score > right.score;
    });

    if (moves.size() > maxMoves) {
        moves.resize(maxMoves);
    }

    return moves;
}

namespace {

// Best threat the given player could create with one move from the
// current position. Scans empty squares near existing stones and
// returns the highest severity seen. Runs once per search, so a full
// neighbourhood scan is acceptable.
ThreatType bestThreatFor(const GameState& state, Player player) {
    ThreatType best = ThreatType::None;
    for (const Move& move : state.legalMoves()) {
        if (!isNearExistingStone(state, move)) {
            continue;
        }
        const MoveThreatInfo info = StaticEvaluator::analyzeMove(state, move, player);
        if (threatSeverity(info.best) > threatSeverity(best)) {
            best = info.best;
            if (best == ThreatType::Five) {
                return best;  // cannot be exceeded
            }
        }
    }
    return best;
}

}  // namespace

ThreatAssessment assessRootThreats(const GameState& state) {
    ThreatAssessment result;
    if (state.isGameOver() || state.isSwapDecisionPending() || !hasAnyStone(state)) {
        return result;
    }

    const Player us   = state.sideToMove();
    const Player them = otherPlayer(us);

    const ThreatType ours = bestThreatFor(state, us);
    if (threatSeverity(ours) >= threatSeverity(ThreatType::OpenFour)) {
        result.attack = AttackThreatLevel::Immediate;
    } else if (threatSeverity(ours) >= threatSeverity(ThreatType::OpenThree)) {
        result.attack = AttackThreatLevel::Strong;
    }

    const ThreatType theirs = bestThreatFor(state, them);
    if (threatSeverity(theirs) >= threatSeverity(ThreatType::OpenFour)) {
        result.defense = DefenseThreatLevel::Immediate;
    } else if (threatSeverity(theirs) >= threatSeverity(ThreatType::OpenThree)) {
        result.defense = DefenseThreatLevel::Forcing;
    }

    return result;
}

}  // namespace gomoku
