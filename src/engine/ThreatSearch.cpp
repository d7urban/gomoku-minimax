#include "gomoku/ThreatSearch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#include "gomoku/PatternAnalysis.hpp"

namespace gomoku {

namespace {

using Clock = std::chrono::steady_clock;

bool isWinningResultFor(GameResult result, Player player) {
    return (player == Player::Black && result == GameResult::BlackWin)
        || (player == Player::White && result == GameResult::WhiteWin);
}

bool moveLess(Move left, Move right) {
    if (left.row != right.row) {
        return left.row < right.row;
    }
    return left.col < right.col;
}

template <typename T>
void addUniqueValue(std::vector<T>& values, const T& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void normalizeMoves(std::vector<Move>& moves) {
    std::sort(moves.begin(), moves.end(), moveLess);
    moves.erase(std::unique(moves.begin(), moves.end()), moves.end());
}

Move moveForLineOffset(const GameState& state, int direction, int lineIndex, int offset) {
    const int size = state.boardSize();

    switch (direction) {
        case 0:
            return {lineIndex, offset};
        case 1:
            return {offset, lineIndex};
        case 2: {
            const int delta = lineIndex - (size - 1);
            const int startRow = std::max(0, delta);
            const int startCol = std::max(0, -delta);
            return {startRow + offset, startCol + offset};
        }
        case 3:
        default: {
            const int sum = lineIndex;
            const int startRow = std::max(0, sum - (size - 1));
            const int startCol = sum - startRow;
            return {startRow + offset, startCol - offset};
        }
    }
}

std::optional<ThreatStep> deriveThreat(const GameState& state, Move move, Player attacker, int direction, ThreatType type) {
    ThreatStep step;
    step.move = move;
    step.type = type;
    step.direction = direction;

    const bool exactFiveRequired = state.rules().exactFiveRequired;

    const GameState::LineLocation line = state.lineLocation(move, direction);
    const std::uint16_t ownBits = state.lineBits(attacker, direction, line.lineIndex);
    const std::uint16_t opponentBits = state.lineBits(otherPlayer(attacker), direction, line.lineIndex);

    bool matchedWindow = false;

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
                    continue;
                }

                if ((ownBits & bit) != 0U) {
                    cells[static_cast<std::size_t>(index)] = PatternCell::Own;
                    addUniqueValue(step.supportMoves, moveForLineOffset(state, direction, line.lineIndex, bitIndex));
                } else if ((opponentBits & bit) != 0U) {
                    cells[static_cast<std::size_t>(index)] = PatternCell::Opponent;
                }
            }

            const bool ownBefore = startOffset > 0
                && (ownBits & static_cast<std::uint16_t>(1U << (startOffset - 1))) != 0U;
            const bool ownAfter = startOffset + length < line.length
                && (ownBits & static_cast<std::uint16_t>(1U << (startOffset + length))) != 0U;

            if (classifyPatternWindow(cells, targetOffset, exactFiveRequired, ownBefore, ownAfter) != type) {
                continue;
            }

            matchedWindow = true;

            if (type == ThreatType::Five) {
                continue;
            }

            if (type == ThreatType::OpenFour || type == ThreatType::SimpleFour) {
                for (int index = 0; index < length; ++index) {
                    if (cells[static_cast<std::size_t>(index)] != PatternCell::Empty) {
                        continue;
                    }

                    auto extended = cells;
                    extended[static_cast<std::size_t>(index)] = PatternCell::Own;
                    if (!containsFiveIncludingTarget(extended, targetOffset, exactFiveRequired, ownBefore, ownAfter)) {
                        continue;
                    }

                    const Move continuation = moveForLineOffset(state, direction, line.lineIndex, startOffset + index);
                    addUniqueValue(step.defenseMoves, continuation);
                    addUniqueValue(step.continuationMoves, continuation);
                    addUniqueValue(step.requiredEmpty, continuation);
                }
                continue;
            }

            if (type == ThreatType::OpenThree || type == ThreatType::BrokenThree) {
                for (int index = 0; index < length; ++index) {
                    if (cells[static_cast<std::size_t>(index)] != PatternCell::Empty) {
                        continue;
                    }

                    auto extended = cells;
                    extended[static_cast<std::size_t>(index)] = PatternCell::Own;
                    if (countWinningContinuations(extended, targetOffset, exactFiveRequired, ownBefore, ownAfter) < 2) {
                        continue;
                    }

                    const Move continuation = moveForLineOffset(state, direction, line.lineIndex, startOffset + index);
                    addUniqueValue(step.defenseMoves, continuation);
                    addUniqueValue(step.continuationMoves, continuation);
                    addUniqueValue(step.requiredEmpty, continuation);

                    for (int follow = 0; follow < length; ++follow) {
                        if (extended[static_cast<std::size_t>(follow)] != PatternCell::Empty) {
                            continue;
                        }

                        auto winning = extended;
                        winning[static_cast<std::size_t>(follow)] = PatternCell::Own;
                        if (!containsFiveIncludingTarget(winning, targetOffset, exactFiveRequired, ownBefore, ownAfter)) {
                            continue;
                        }

                        const Move required = moveForLineOffset(state, direction, line.lineIndex, startOffset + follow);
                        addUniqueValue(step.requiredEmpty, required);
                        if (type == ThreatType::BrokenThree) {
                            addUniqueValue(step.defenseMoves, required);
                        }
                    }
                }
            }
        }
    }

    if (!matchedWindow) {
        return std::nullopt;
    }

    normalizeMoves(step.supportMoves);
    normalizeMoves(step.defenseMoves);
    normalizeMoves(step.continuationMoves);
    normalizeMoves(step.requiredEmpty);
    return step;
}

ThreatEnumerationResult enumerateThreatsInternal(
    const GameState& state,
    Player attacker,
    std::size_t maxThreatMoves,
    int timeLimitMs = 0,
    ThreatType minimumThreat = ThreatType::OpenThree) {
    ThreatEnumerationResult result;
    std::vector<ThreatStep>& threats = result.threats;
    if (state.isGameOver() || state.isSwapDecisionPending() || state.sideToMove() != attacker) {
        return result;
    }

    const Clock::time_point startTime = Clock::now();

    for (const Move& move : state.legalMoves()) {
        ++result.nodes;
        if (timeLimitMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime).count();
            if (elapsed >= timeLimitMs) {
                break;
            }
        }

        const MoveThreatInfo info = StaticEvaluator::analyzeMove(state, move, attacker);
        if (threatSeverity(info.best) < threatSeverity(minimumThreat)) {
            continue;
        }

        for (int direction = 0; direction < 4; ++direction) {
            const ThreatType type = info.lineThreats[static_cast<std::size_t>(direction)];
            if (threatSeverity(type) < threatSeverity(minimumThreat)) {
                continue;
            }

            const auto threat = deriveThreat(state, move, attacker, direction, type);
            if (!threat.has_value()) {
                continue;
            }
            threats.push_back(*threat);
        }
    }

    std::sort(threats.begin(), threats.end(), [](const ThreatStep& left, const ThreatStep& right) {
        if (threatSeverity(left.type) != threatSeverity(right.type)) {
            return threatSeverity(left.type) > threatSeverity(right.type);
        }
        if (left.move == right.move) {
            return left.direction < right.direction;
        }
        return moveLess(left.move, right.move);
    });

    if (threats.size() > maxThreatMoves) {
        threats.resize(maxThreatMoves);
    }

    return result;
}

bool allSquaresEmpty(const GameState& state, const std::vector<Move>& moves) {
    for (const Move& move : moves) {
        if (!state.isInside(move.row, move.col) || state.cellAt(move.row, move.col) != Player::None) {
            return false;
        }
    }
    return true;
}

bool intersects(const std::vector<Move>& left, const std::vector<Move>& right) {
    for (const Move& move : left) {
        if (std::find(right.begin(), right.end(), move) != right.end()) {
            return true;
        }
    }
    return false;
}

class ThreatSearchRunner {
public:
    explicit ThreatSearchRunner(ThreatSequenceConfig config)
        : config_(config) {
    }

    ThreatSearchResult run(const GameState& state, Player attacker) {
        ThreatSearchResult result;
        startTime_ = Clock::now();
        if (state.sideToMove() != attacker) {
            return result;
        }

        std::vector<ThreatStep> sequence;
        std::vector<Move> refutations;
        std::vector<ThreatStep> path;
        std::vector<Move> reservedEmpty;

        result.foundWin = searchActual(state, attacker, config_.maxDepth, reservedEmpty, path, sequence, refutations);
        result.sequence = sequence;
        result.refutations = refutations;
        result.graph = graph_;
        result.nodes = nodes_;
        return result;
    }

private:
    ThreatSequenceConfig config_ {};
    Clock::time_point startTime_ {};
    std::uint64_t nodes_ {0};
    std::vector<ThreatGraphNode> graph_;

    bool shouldStop() const {
        if (config_.maxNodes > 0 && nodes_ > config_.maxNodes) {
            return true;
        }
        if (config_.timeLimitMs <= 0) {
            return false;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime_).count();
        return elapsed >= config_.timeLimitMs;
    }

    bool searchActual(const GameState& state, Player attacker, int depth, const std::vector<Move>& reservedEmpty, const std::vector<ThreatStep>& path,
        std::vector<ThreatStep>& outSequence, std::vector<Move>& outRefutations) {
        ++nodes_;
        if (shouldStop()) {
            return false;
        }

        if (!allSquaresEmpty(state, reservedEmpty)) {
            return false;
        }

        if (isWinningResultFor(state.result(), attacker)) {
            return true;
        }
        if (state.isGameOver() || depth <= 0 || state.sideToMove() != attacker) {
            return false;
        }

        const auto threats = enumerateThreatsInternal(
            state, attacker, config_.maxThreatMoves, 0, config_.minimumThreat).threats;
        for (ThreatStep threat : threats) {
            if (std::find(reservedEmpty.begin(), reservedEmpty.end(), threat.move) != reservedEmpty.end() || !allSquaresEmpty(state, threat.requiredEmpty)) {
                continue;
            }

            threat.nodeId = appendGraphNode(threat, path);

            GameState afterAttack = state;
            if (!afterAttack.applyMove(threat.move)) {
                continue;
            }

            std::vector<Move> nextReserved = reservedEmpty;
            for (const Move& move : threat.requiredEmpty) {
                addUniqueValue(nextReserved, move);
            }
            normalizeMoves(nextReserved);

            if (isWinningResultFor(afterAttack.result(), attacker)) {
                outSequence = {threat};
                return true;
            }

            if (depth <= 1) {
                continue;
            }

            GameState abstractAfter = applyAllDefenses(afterAttack, attacker, threat);
            if (abstractAfter.isGameOver() && !isWinningResultFor(abstractAfter.result(), attacker)) {
                continue;
            }
            abstractAfter.setSideToMoveForAnalysis(attacker);
            if (!abstractCanWin(abstractAfter, attacker, depth - 1, nextReserved)) {
                continue;
            }

            auto nextPath = path;
            nextPath.push_back(threat);
            const auto replies = collectReplies(afterAttack, attacker, threat);
            if (replies.empty()) {
                std::vector<ThreatStep> candidate {threat};
                std::vector<Move> refutations;
                if (sequenceRefutedByCounterThreats(state, attacker, candidate, refutations)) {
                    for (const Move& refutation : refutations) {
                        addUniqueValue(outRefutations, refutation);
                    }
                    continue;
                }
                outSequence = std::move(candidate);
                return true;
            }

            if (config_.useAllDefensesTrick && threat.type != ThreatType::OpenThree) {
                GameState defended = applyAllDefenses(afterAttack, attacker, threat);
                if (defended.isGameOver() && !isWinningResultFor(defended.result(), attacker)) {
                    continue;
                }
                defended.setSideToMoveForAnalysis(attacker);

                std::vector<ThreatStep> continuation;
                std::vector<Move> recursiveRefutations;
                if (!searchActual(defended, attacker, depth - 1, nextReserved, nextPath, continuation, recursiveRefutations)) {
                    for (const Move& refutation : recursiveRefutations) {
                        addUniqueValue(outRefutations, refutation);
                    }
                    continue;
                }

                std::vector<ThreatStep> candidate {threat};
                candidate.insert(candidate.end(), continuation.begin(), continuation.end());
                std::vector<Move> refutations;
                if (sequenceRefutedByCounterThreats(state, attacker, candidate, refutations)) {
                    for (const Move& refutation : refutations) {
                        addUniqueValue(outRefutations, refutation);
                    }
                    continue;
                }

                outSequence = std::move(candidate);
                return true;
            }

            bool allBranchesWin = true;
            std::vector<ThreatStep> representativeSequence;
            std::vector<Move> branchRefutations;

            for (const Move& reply : replies) {
                GameState defended = afterAttack;
                defended.setSideToMoveForAnalysis(otherPlayer(attacker));
                if (!defended.applyMove(reply)) {
                    continue;
                }
                if (isWinningResultFor(defended.result(), otherPlayer(attacker))) {
                    allBranchesWin = false;
                    addUniqueValue(outRefutations, reply);
                    break;
                }

                defended.setSideToMoveForAnalysis(attacker);
                std::vector<ThreatStep> continuation;
                std::vector<Move> recursiveRefutations;
                if (!searchActual(defended, attacker, depth - 1, nextReserved, nextPath, continuation, recursiveRefutations)) {
                    allBranchesWin = false;
                    addUniqueValue(outRefutations, reply);
                    for (const Move& refutation : recursiveRefutations) {
                        addUniqueValue(outRefutations, refutation);
                    }
                    break;
                }

                if (representativeSequence.empty()) {
                    representativeSequence = continuation;
                }
            }

            if (!allBranchesWin) {
                continue;
            }

            std::vector<ThreatStep> candidate {threat};
            candidate.insert(candidate.end(), representativeSequence.begin(), representativeSequence.end());
            std::vector<Move> refutations;
            if (sequenceRefutedByCounterThreats(state, attacker, candidate, refutations)) {
                for (const Move& refutation : refutations) {
                    addUniqueValue(outRefutations, refutation);
                }
                continue;
            }

            outSequence = std::move(candidate);
            return true;
        }

        return false;
    }

    bool abstractCanWin(const GameState& state, Player attacker, int depth, const std::vector<Move>& reservedEmpty) {
        ++nodes_;
        if (shouldStop()) {
            return false;
        }

        if (!allSquaresEmpty(state, reservedEmpty)) {
            return false;
        }

        if (isWinningResultFor(state.result(), attacker)) {
            return true;
        }
        if (state.isGameOver() || depth <= 0 || state.sideToMove() != attacker) {
            return false;
        }

        const auto threats = enumerateThreatsInternal(
            state, attacker, config_.maxThreatMoves, 0, config_.minimumThreat).threats;
        for (const ThreatStep& threat : threats) {
            if (std::find(reservedEmpty.begin(), reservedEmpty.end(), threat.move) != reservedEmpty.end() || !allSquaresEmpty(state, threat.requiredEmpty)) {
                continue;
            }

            GameState afterAttack = state;
            if (!afterAttack.applyMove(threat.move)) {
                continue;
            }
            if (isWinningResultFor(afterAttack.result(), attacker)) {
                return true;
            }

            std::vector<Move> nextReserved = reservedEmpty;
            for (const Move& move : threat.requiredEmpty) {
                addUniqueValue(nextReserved, move);
            }
            normalizeMoves(nextReserved);

            GameState abstractAfter = applyAllDefenses(afterAttack, attacker, threat);
            if (abstractAfter.isGameOver() && !isWinningResultFor(abstractAfter.result(), attacker)) {
                continue;
            }
            abstractAfter.setSideToMoveForAnalysis(attacker);
            if (abstractCanWin(abstractAfter, attacker, depth - 1, nextReserved)) {
                return true;
            }
        }

        return false;
    }

    std::vector<Move> futureRequiredSquares(const std::vector<ThreatStep>& sequence, std::size_t startIndex) const {
        std::vector<Move> required;
        for (std::size_t index = startIndex; index < sequence.size(); ++index) {
            addUniqueValue(required, sequence[index].move);
            for (const Move& move : sequence[index].requiredEmpty) {
                addUniqueValue(required, move);
            }
        }
        normalizeMoves(required);
        return required;
    }

    bool counterThreatInterferes(const ThreatStep& counter, const std::vector<Move>& futureRequired) const {
        if (threatSeverity(counter.type) >= threatSeverity(ThreatType::OpenFour)) {
            return true;
        }
        if (threatSeverity(counter.type) < threatSeverity(ThreatType::SimpleFour)) {
            return false;
        }
        if (std::find(futureRequired.begin(), futureRequired.end(), counter.move) != futureRequired.end()) {
            return true;
        }
        return intersects(counter.continuationMoves, futureRequired)
            || intersects(counter.defenseMoves, futureRequired)
            || intersects(counter.requiredEmpty, futureRequired);
    }

    bool sequenceRefutedByCounterThreats(const GameState& root,
                                         Player attacker,
                                         const std::vector<ThreatStep>& sequence,
                                         std::vector<Move>& outRefutations) const {
        GameState state = root;
        const Player defender = otherPlayer(attacker);
        std::uint64_t refutationNodes = 0;

        for (std::size_t index = 0; index < sequence.size(); ++index) {
            if (!state.applyMove(sequence[index].move)) {
                return true;
            }
            if (isWinningResultFor(state.result(), attacker)) {
                return false;
            }

            GameState defenderTurn = state;
            defenderTurn.setSideToMoveForAnalysis(defender);
            ThreatEnumerationResult counters = enumerateThreatsInternal(
                defenderTurn, defender, config_.maxThreatMoves, 0, ThreatType::SimpleFour);
            refutationNodes += counters.nodes;
            const std::vector<Move> futureRequired = futureRequiredSquares(sequence, index + 1);
            for (const ThreatStep& counter : counters.threats) {
                if (counterThreatInterferes(counter, futureRequired)) {
                    addUniqueValue(outRefutations, counter.move);
                    return true;
                }
            }
            if (config_.refutationNodeBudget > 0 && refutationNodes >= config_.refutationNodeBudget) {
                return true;
            }

            state = applyAllDefenses(state, attacker, sequence[index]);
            if (state.isGameOver() && !isWinningResultFor(state.result(), attacker)) {
                return true;
            }
            state.setSideToMoveForAnalysis(attacker);
        }

        return false;
    }

    GameState applyAllDefenses(const GameState& afterAttack, Player attacker, const ThreatStep& threat) const {
        GameState defended = afterAttack;
        const Player defender = otherPlayer(attacker);
        for (const Move& defense : threat.defenseMoves) {
            if (!defended.isInside(defense.row, defense.col) || defended.cellAt(defense.row, defense.col) != Player::None || defended.isGameOver()) {
                continue;
            }
            defended.setSideToMoveForAnalysis(defender);
            defended.applyMove(defense);
        }
        return defended;
    }

    std::vector<Move> collectReplies(const GameState& afterAttack, Player attacker, const ThreatStep& threat) const {
        std::vector<Move> replies;
        for (const Move& defense : threat.defenseMoves) {
            if (afterAttack.isLegalMove(defense)) {
                addUniqueValue(replies, defense);
            }
        }

        const Player defender = otherPlayer(attacker);
        auto counterThreats = enumerateThreatsInternal(
            afterAttack, defender, config_.maxThreatMoves, 0, config_.minimumThreat).threats;
        for (const ThreatStep& counter : counterThreats) {
            if (threatSeverity(counter.type) >= threatSeverity(threat.type)) {
                addUniqueValue(replies, counter.move);
            }
        }

        normalizeMoves(replies);
        return replies;
    }

    int appendGraphNode(const ThreatStep& threat, const std::vector<ThreatStep>& path) {
        std::vector<int> dependencies;
        for (const ThreatStep& prior : path) {
            if (std::find(threat.supportMoves.begin(), threat.supportMoves.end(), prior.move) != threat.supportMoves.end()) {
                addUniqueValue(dependencies, prior.nodeId);
            }
        }

        ThreatGraphNode node;
        node.kind = ThreatGraphNodeKind::Threat;
        node.move = threat.move;
        node.type = threat.type;
        node.defenseMoves = threat.defenseMoves;
        node.continuationMoves = threat.continuationMoves;
        node.requiredEmpty = threat.requiredEmpty;

        if (dependencies.size() > 1) {
            ThreatGraphNode combination;
            combination.id = static_cast<int>(graph_.size());
            combination.kind = ThreatGraphNodeKind::Combination;
            combination.dependencies = dependencies;
            graph_.push_back(combination);
            node.dependencies = {combination.id};
        } else {
            node.dependencies = dependencies;
        }

        node.id = static_cast<int>(graph_.size());
        graph_.push_back(node);
        return node.id;
    }
};

}  // namespace

std::string_view toString(ThreatGraphNodeKind kind) {
    switch (kind) {
        case ThreatGraphNodeKind::Threat:
            return "threat";
        case ThreatGraphNodeKind::Combination:
            return "combination";
        default:
            return "unknown";
    }
}

ThreatSequenceSearcher::ThreatSequenceSearcher(ThreatSequenceConfig config)
    : config_(config) {
}

std::vector<ThreatStep> ThreatSequenceSearcher::enumerateThreats(const GameState& state, Player attacker) const {
    return enumerateThreatsInternal(
        state, attacker, config_.maxThreatMoves, config_.timeLimitMs, config_.minimumThreat).threats;
}

ThreatEnumerationResult ThreatSequenceSearcher::enumerateThreatsWithStats(const GameState& state, Player attacker) const {
    return enumerateThreatsInternal(
        state, attacker, config_.maxThreatMoves, config_.timeLimitMs, config_.minimumThreat);
}

ThreatSearchResult ThreatSequenceSearcher::searchWinningSequence(const GameState& state, Player attacker) const {
    ThreatSearchRunner runner(config_);
    return runner.run(state, attacker);
}

}  // namespace gomoku
