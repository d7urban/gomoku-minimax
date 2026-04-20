#include "gomoku/Threats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "gomoku/PatternAnalysis.hpp"

namespace gomoku {

namespace {

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
    int score = 0;
    for (int dRow = -2; dRow <= 2; ++dRow) {
        for (int dCol = -2; dCol <= 2; ++dCol) {
            if (dRow == 0 && dCol == 0) {
                continue;
            }

            const int row = move.row + dRow;
            const int col = move.col + dCol;
            if (!state.isInside(row, col)) {
                continue;
            }

            const Player cell = state.cellAt(row, col);
            if (cell == player) {
                score += (std::abs(dRow) <= 1 && std::abs(dCol) <= 1) ? 18 : 7;
            } else if (cell == otherPlayer(player)) {
                score += 5;
            }
        }
    }
    return score;
}

bool isNearExistingStone(const GameState& state, Move move) {
    for (int dRow = -2; dRow <= 2; ++dRow) {
        for (int dCol = -2; dCol <= 2; ++dCol) {
            if (dRow == 0 && dCol == 0) {
                continue;
            }

            const int row = move.row + dRow;
            const int col = move.col + dCol;
            if (!state.isInside(row, col)) {
                continue;
            }
            if (state.cellAt(row, col) != Player::None) {
                return true;
            }
        }
    }
    return false;
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

int threatSeverity(ThreatType type) {
    return static_cast<int>(type);
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

int threatWeight(ThreatType type) {
    switch (type) {
        case ThreatType::None:
            return 0;
        case ThreatType::One:
            return 2;
        case ThreatType::Two:
            return 6;
        case ThreatType::BrokenThree:
            return 18;
        case ThreatType::OpenThree:
            return 40;
        case ThreatType::SimpleFour:
            return 160;
        case ThreatType::OpenFour:
            return 600;
        case ThreatType::Five:
            return 20000;
        default:
            return 0;
    }
}

MoveThreatInfo computeMoveThreatInfo(const GameState& state, Move move, Player player) {
    MoveThreatInfo info;
    if (!state.isInside(move.row, move.col) || state.cellAt(move.row, move.col) != Player::None) {
        return info;
    }

    const bool exactFiveRequired = state.rules().exactFiveRequired;

    for (int directionIndex = 0; directionIndex < 4; ++directionIndex) {
        ThreatType best = ThreatType::None;
        const GameState::LineLocation line = state.lineLocation(move, directionIndex);
        const std::uint16_t ownBits = state.lineBits(player, directionIndex, line.lineIndex);
        const std::uint16_t opponentBits = state.lineBits(otherPlayer(player), directionIndex, line.lineIndex);

        for (int length = 5; length <= 7; ++length) {
            for (int targetOffset = 0; targetOffset < length; ++targetOffset) {
                const int startOffset = line.offset - targetOffset;
                if (startOffset < 0 || startOffset + length > line.length) {
                    continue;
                }

                std::vector<PatternCell> cells(static_cast<std::size_t>(length), PatternCell::Empty);
                for (int index = 0; index < length; ++index) {
                    const int bitIndex = startOffset + index;
                    if (bitIndex < 0 || bitIndex >= 16) {
                        continue;
                    }
                    const std::uint16_t bit = static_cast<std::uint16_t>(1U << bitIndex);
                    if (index == targetOffset) {
                        cells[static_cast<std::size_t>(index)] = PatternCell::Own;
                    } else if ((ownBits & bit) != 0U) {
                        cells[static_cast<std::size_t>(index)] = PatternCell::Own;
                    } else if ((opponentBits & bit) != 0U) {
                        cells[static_cast<std::size_t>(index)] = PatternCell::Opponent;
                    }
                }

                const bool ownBefore = startOffset > 0
                    && (ownBits & static_cast<std::uint16_t>(1U << (startOffset - 1))) != 0U;
                const bool ownAfter = startOffset + length < line.length
                    && (ownBits & static_cast<std::uint16_t>(1U << (startOffset + length))) != 0U;

                best = std::max(best, classifyPatternWindow(cells, targetOffset, exactFiveRequired, ownBefore, ownAfter),
                    [](ThreatType left, ThreatType right) {
                    return threatSeverity(left) < threatSeverity(right);
                });
            }
        }

        info.lineThreats[static_cast<std::size_t>(directionIndex)] = best;
    }

    const auto [first, second] = topTwoThreats(info.lineThreats);
    info.best = first;
    info.second = second;
    info.totalScore = lineThreatScore(first, second);
    return info;
}

MoveThreatInfo StaticEvaluator::analyzeMove(const GameState& state, Move move, Player player) {
    return state.threatInfoAt(move, player);
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
    std::vector<CandidateMove> moves;
    if (state.isGameOver() || state.isSwapDecisionPending() || state.sideToMove() != player) {
        return moves;
    }

    if (!hasAnyStone(state)) {
        const Move move = centerMove(state);
        moves.push_back({move, analyzeMove(state, move, player), 100000});
        return moves;
    }

    std::vector<Move> legalMoves = state.legalMoves();
    moves.reserve(legalMoves.size());

    for (const Move& move : legalMoves) {
        if (!isNearExistingStone(state, move)) {
            continue;
        }

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

    if (moves.empty()) {
        for (const Move& move : legalMoves) {
            CandidateMove candidate;
            candidate.move = move;
            candidate.threatInfo = analyzeMove(state, move, player);
            candidate.score = candidate.threatInfo.totalScore + centralityScore(state, move);
            moves.push_back(candidate);
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
