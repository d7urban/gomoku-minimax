#include "gomoku/Threats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "gomoku/PatternAnalysis.hpp"

namespace gomoku {

namespace {

constexpr std::size_t kEvaluatedThreatCount = 4;
constexpr int kStaticMateScore = 10'000'000;
constexpr int kImmediateWinScore = 8'000'000;

int lineThreatScore(ThreatType first, ThreatType second) {
    return threatWeight(first) * 3 / 2 + threatWeight(second);
}

void insertTopThreat(std::array<int, kEvaluatedThreatCount>& topThreats, int score) {
    for (std::size_t index = 0; index < topThreats.size(); ++index) {
        if (score <= topThreats[index]) {
            continue;
        }

        for (std::size_t shifted = topThreats.size() - 1; shifted > index; --shifted) {
            topThreats[shifted] = topThreats[shifted - 1];
        }
        topThreats[index] = score;
        return;
    }
}

struct PlayerFeatures {
    std::array<int, kEvaluatedThreatCount> topThreats {};
    int quietPotential {0};
    int centrality {0};
    ThreatType bestThreat {ThreatType::None};
};

struct PositionFeatures {
    PlayerFeatures black;
    PlayerFeatures white;
};

void addThreatFeatures(PlayerFeatures& features, const MoveThreatInfo& info) {
    insertTopThreat(features.topThreats, info.totalScore);
    if (threatSeverity(info.best) < threatSeverity(ThreatType::OpenThree)) {
        features.quietPotential += info.totalScore;
    }
    if (threatSeverity(info.best) > threatSeverity(features.bestThreat)) {
        features.bestThreat = info.best;
    }
}

PositionFeatures collectPositionFeatures(const GameState& state) {
    PositionFeatures features;
    const int center = state.boardSize() / 2;

    for (int row = 0; row < state.boardSize(); ++row) {
        for (int col = 0; col < state.boardSize(); ++col) {
            const Move move {row, col};
            const Player cell = state.cellAt(row, col);
            if (cell == Player::Black) {
                features.black.centrality += 200 - 12 * (std::abs(row - center) + std::abs(col - center));
                continue;
            }
            if (cell == Player::White) {
                features.white.centrality += 200 - 12 * (std::abs(row - center) + std::abs(col - center));
                continue;
            }

            addThreatFeatures(features.black, state.threatInfoAt(move, Player::Black));
            addThreatFeatures(features.white, state.threatInfoAt(move, Player::White));
        }
    }
    return features;
}

int boundedPlayerPotential(const PlayerFeatures& features) {
    int score = 0;
    int divisor = 1;
    for (const int threat : features.topThreats) {
        score += threat / divisor;
        divisor *= 2;
    }

    // Low-grade mobility remains a tie-breaker, but cannot swamp one forcing
    // move through many overlapping hypothetical continuations.
    return score + features.quietPotential / 32;
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
            return 10;
        case ThreatType::BrokenThree:
            return 160;
        case ThreatType::OpenThree:
            return 8'000;
        case ThreatType::SimpleFour:
            return 160'000;
        case ThreatType::OpenFour:
            return 800'000;
        case ThreatType::Five:
            return 2'000'000;
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

int StaticEvaluator::evaluate(const GameState& state, Player perspective) {
    if (perspective != Player::Black && perspective != Player::White) {
        return 0;
    }
    if (state.result() == GameResult::Draw) {
        return 0;
    }
    if ((perspective == Player::Black && state.result() == GameResult::BlackWin)
        || (perspective == Player::White && state.result() == GameResult::WhiteWin)) {
        return kStaticMateScore;
    }
    if ((perspective == Player::Black && state.result() == GameResult::WhiteWin)
        || (perspective == Player::White && state.result() == GameResult::BlackWin)) {
        return -kStaticMateScore;
    }

    const PositionFeatures features = collectPositionFeatures(state);
    const PlayerFeatures& ownFeatures = perspective == Player::Black ? features.black : features.white;
    const PlayerFeatures& opponentFeatures = perspective == Player::Black ? features.white : features.black;
    const PlayerFeatures& sideFeatures = state.sideToMove() == Player::Black ? features.black : features.white;
    const Player side = state.sideToMove();
    if (side != Player::None
        && threatSeverity(sideFeatures.bestThreat) >= threatSeverity(ThreatType::Five)) {
        return side == perspective ? kImmediateWinScore : -kImmediateWinScore;
    }

    int score = boundedPlayerPotential(ownFeatures) - boundedPlayerPotential(opponentFeatures);
    score += (ownFeatures.centrality - opponentFeatures.centrality) / 4;

    if (state.sideToMove() == perspective) {
        score += 12;
    }

    return std::clamp(score, -kImmediateWinScore, kImmediateWinScore);
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

        if (candidate.threatInfo.best == ThreatType::Five) {
            candidate.score += 10'000'000;
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

}  // namespace gomoku
