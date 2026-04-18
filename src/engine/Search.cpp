#include "gomoku/Search.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "gomoku/OpeningBook.hpp"

namespace gomoku {

namespace {

constexpr int kInfinity = 1'000'000'000;
constexpr int kMateScore = 10'000'000;
constexpr int kMateThreshold = kMateScore - 1000;
constexpr int kDefaultAspirationWindow = 80;
constexpr int kMaxSearchPly = 64;

enum class BoundType : std::uint8_t {
    Exact,
    Lower,
    Upper,
};

struct TTEntry {
    int depth {0};
    int score {0};
    BoundType bound {BoundType::Exact};
    std::optional<Move> bestMove;
};

struct RootSearchResult {
    std::optional<Move> bestMove;
    int score {-kInfinity};
    int rootCandidateCount {0};
};

constexpr int scoreToTT(int score, int ply) {
    if (score >= kMateThreshold) {
        return score + ply;
    }
    if (score <= -kMateThreshold) {
        return score - ply;
    }
    return score;
}

constexpr int scoreFromTT(int score, int ply) {
    if (score >= kMateThreshold) {
        return score - ply;
    }
    if (score <= -kMateThreshold) {
        return score + ply;
    }
    return score;
}

static_assert(scoreToTT(kMateScore - 4, 4) == kMateScore);
static_assert(scoreFromTT(kMateScore, 7) == kMateScore - 7);
static_assert(scoreToTT(-kMateScore + 4, 4) == -kMateScore);
static_assert(scoreFromTT(-kMateScore, 7) == -kMateScore + 7);
static_assert(scoreFromTT(scoreToTT(12345, 6), 9) == 12345);

using Clock = std::chrono::steady_clock;

class SearchRunner {
public:
    explicit SearchRunner(SearchConfig config)
        : config_(config) {
    }

    SearchResult run(const GameState& state) {
        SearchResult result;
        startTime_ = Clock::now();
        historyScores_.assign(16U * 16U, 0);
        killerMoves_.assign(static_cast<std::size_t>(std::max(config_.maxDepth + 8, kMaxSearchPly)), {});

        if (config_.useOpeningBook) {
            if (const auto bookHit = lookupOpeningBookMove(state)) {
                result.bestMove = bookHit->move;
                result.summary.score = StaticEvaluator::evaluate(state, state.sideToMove());
                result.summary.rootCandidateCount = 1;
                result.summary.completedLastDepth = true;
                result.summary.usedOpeningBook = true;
                result.summary.openingBookName = std::string(bookHit->lineName);
                result.summary.principalVariation = {bookHit->move};
                return finalizeResult(std::move(result));
            }
        }

        ThreatSequenceConfig threatConfig;
        threatConfig.maxDepth = std::max(2, config_.maxDepth + 2);
        threatConfig.maxNodes = std::max<std::uint64_t>(1000, config_.maxNodes / 3);
        threatConfig.timeLimitMs = hardTimeLimitMs() > 0 ? std::max(10, hardTimeLimitMs() / 4) : 0;
        threatConfig.maxThreatMoves = std::max<std::size_t>(6, config_.maxCandidateMoves);

        ThreatSequenceSearcher threatSearcher(threatConfig);
        ThreatSearchResult threatResult = threatSearcher.searchWinningSequence(state, state.sideToMove());
        result.summary.threatNodes = threatResult.nodes;
        if (threatResult.foundWin && !threatResult.sequence.empty()) {
            result.bestMove = threatResult.sequence.front().move;
            result.summary.score = kMateScore;
            result.summary.depthReached = static_cast<int>(threatResult.sequence.size());
            result.summary.threatSequenceLength = static_cast<int>(threatResult.sequence.size());
            result.summary.usedThreatSequence = true;
            result.summary.completedLastDepth = true;
            result.threatSequence = threatResult;
            result.summary.principalVariation.reserve(threatResult.sequence.size());
            for (const ThreatStep& step : threatResult.sequence) {
                result.summary.principalVariation.push_back(step.move);
            }
            return finalizeResult(std::move(result));
        }

        auto rootMoves = generateOrderedCandidates(state, 0);
        result.summary.rootCandidateCount = static_cast<int>(rootMoves.size());
        if (rootMoves.empty()) {
            result.summary.score = StaticEvaluator::evaluate(state, state.sideToMove());
            return finalizeResult(std::move(result));
        }

        result.bestMove = rootMoves.front().move;
        result.summary.score = rootMoves.front().score;

        int previousScore = result.summary.score;
        for (int depth = 1; depth <= config_.maxDepth; ++depth) {
            if (shouldStop() || (depth > 1 && softLimitReached())) {
                break;
            }

            completedDepth_ = true;
            RootSearchResult iteration;

            if (config_.useAspirationWindows && depth > 1 && std::abs(previousScore) < kMateThreshold) {
                int aspiration = kDefaultAspirationWindow;
                int alpha = std::max(-kInfinity, previousScore - aspiration);
                int beta = std::min(kInfinity, previousScore + aspiration);

                while (true) {
                    iteration = searchRoot(state, depth, alpha, beta, result.bestMove);
                    if (!completedDepth_) {
                        break;
                    }

                    if (iteration.score <= alpha) {
                        aspiration *= 2;
                        alpha = std::max(-kInfinity, previousScore - aspiration);
                        continue;
                    }
                    if (iteration.score >= beta) {
                        aspiration *= 2;
                        beta = std::min(kInfinity, previousScore + aspiration);
                        continue;
                    }
                    break;
                }
            } else {
                iteration = searchRoot(state, depth, -kInfinity, kInfinity, result.bestMove);
            }

            if (!completedDepth_ || !iteration.bestMove.has_value()) {
                break;
            }

            result.bestMove = iteration.bestMove;
            result.summary.depthReached = depth;
            result.summary.score = iteration.score;
            result.summary.completedLastDepth = true;
            result.summary.rootCandidateCount = iteration.rootCandidateCount;
            result.summary.principalVariation = extractPrincipalVariation(state, *iteration.bestMove, depth);
            previousScore = iteration.score;

            if (softLimitReached()) {
                break;
            }
        }

        return finalizeResult(std::move(result));
    }

private:
    SearchConfig config_ {};
    Clock::time_point startTime_ {};
    std::uint64_t nodes_ {0};
    std::uint64_t ttHits_ {0};
    bool completedDepth_ {true};
    std::unordered_map<std::uint64_t, TTEntry> tt_;
    std::vector<std::array<std::optional<Move>, 2>> killerMoves_;
    std::vector<int> historyScores_;

    SearchResult finalizeResult(SearchResult result) const {
        result.summary.nodes = nodes_;
        result.summary.ttHits = ttHits_;
        result.summary.elapsedMs = elapsedMs();
        return result;
    }

    int elapsedMs() const {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime_).count());
    }

    int hardTimeLimitMs() const {
        return config_.timeLimitMs;
    }

    int softTimeLimitMs() const {
        if (config_.softTimeLimitMs > 0) {
            return std::min(config_.softTimeLimitMs, hardTimeLimitMs());
        }
        if (hardTimeLimitMs() <= 0) {
            return 0;
        }
        return std::max(1, hardTimeLimitMs() * 3 / 4);
    }

    bool shouldStop() {
        if (config_.maxNodes > 0 && nodes_ >= config_.maxNodes) {
            completedDepth_ = false;
            return true;
        }

        if (hardTimeLimitMs() > 0 && elapsedMs() >= hardTimeLimitMs()) {
            completedDepth_ = false;
            return true;
        }

        return false;
    }

    bool softLimitReached() const {
        return softTimeLimitMs() > 0 && elapsedMs() >= softTimeLimitMs();
    }

    int terminalScore(const GameState& state, int ply) const {
        if (state.result() == GameResult::Draw) {
            return 0;
        }

        const Player side = state.sideToMove();
        if ((side == Player::Black && state.result() == GameResult::BlackWin)
            || (side == Player::White && state.result() == GameResult::WhiteWin)) {
            return kMateScore - ply;
        }

        return -kMateScore + ply;
    }

    int historyIndex(Move move) const {
        return move.row * 16 + move.col;
    }

    void recordCutoffMove(Move move, int depth, int ply) {
        if (ply >= 0 && ply < static_cast<int>(killerMoves_.size())) {
            auto& killers = killerMoves_[static_cast<std::size_t>(ply)];
            if (killers[0] != move) {
                killers[1] = killers[0];
                killers[0] = move;
            }
        }

        const int index = historyIndex(move);
        if (index >= 0 && index < static_cast<int>(historyScores_.size())) {
            historyScores_[static_cast<std::size_t>(index)] += depth * depth;
        }
    }

    bool shouldTryNullMove(const GameState& state, int depth, int beta, int ply, bool allowNullMove, int staticEval) const {
        if (!allowNullMove || !config_.useNullMovePruning || depth < 3 || ply <= 0) {
            return false;
        }
        if (state.moveCount() < 10 || beta >= kMateThreshold || staticEval < beta) {
            return false;
        }

        const Player side = state.sideToMove();
        const Player opponent = otherPlayer(side);
        if (state.hasThreatAtLeast(side, ThreatType::OpenThree) || state.hasThreatAtLeast(opponent, ThreatType::OpenThree)) {
            return false;
        }

        return true;
    }

    std::vector<CandidateMove> applyDefensiveFilter(const GameState& state, std::vector<CandidateMove> candidates) const {
        if (!config_.useDefensiveFiltering || candidates.size() <= 1) {
            return candidates;
        }

        const Player side = state.sideToMove();
        const Player opponent = otherPlayer(side);

        ThreatType strongestOpponentThreat = ThreatType::None;
        std::vector<Move> urgentBlocks;
        for (const CandidateMove& candidate : candidates) {
            const ThreatType threat = state.threatInfoAt(candidate.move, opponent).best;
            if (threatSeverity(threat) < threatSeverity(ThreatType::OpenThree)) {
                continue;
            }

            if (threatSeverity(threat) > threatSeverity(strongestOpponentThreat)) {
                strongestOpponentThreat = threat;
                urgentBlocks.clear();
            }
            if (threatSeverity(threat) == threatSeverity(strongestOpponentThreat)) {
                urgentBlocks.push_back(candidate.move);
            }
        }

        if (threatSeverity(strongestOpponentThreat) < threatSeverity(ThreatType::OpenThree)) {
            return candidates;
        }

        std::vector<CandidateMove> filtered;
        filtered.reserve(candidates.size());
        for (const CandidateMove& candidate : candidates) {
            const bool blocksThreat = std::find(urgentBlocks.begin(), urgentBlocks.end(), candidate.move) != urgentBlocks.end();
            const bool createsCounterThreat = threatSeverity(candidate.threatInfo.best) >= threatSeverity(strongestOpponentThreat);
            if (blocksThreat || createsCounterThreat) {
                filtered.push_back(candidate);
            }
        }

        if (filtered.empty()) {
            return candidates;
        }

        if (threatSeverity(strongestOpponentThreat) >= threatSeverity(ThreatType::SimpleFour)) {
            return filtered;
        }

        const std::size_t relaxedTarget = std::min(candidates.size(), std::max<std::size_t>(4, config_.maxCandidateMoves / 2));
        for (const CandidateMove& candidate : candidates) {
            if (filtered.size() >= relaxedTarget) {
                break;
            }
            if (std::find_if(filtered.begin(), filtered.end(), [&](const CandidateMove& current) {
                    return current.move == candidate.move;
                }) != filtered.end()) {
                continue;
            }
            filtered.push_back(candidate);
        }

        return filtered;
    }

    std::vector<CandidateMove> generateOrderedCandidates(const GameState& state, int ply, std::optional<Move> preferredMove = std::nullopt) {
        auto candidates = StaticEvaluator::generateCandidateMoves(state, state.sideToMove(), config_.maxCandidateMoves);
        if (candidates.empty()) {
            return candidates;
        }

        candidates = applyDefensiveFilter(state, std::move(candidates));

        std::optional<Move> ttBestMove;
        if (const auto found = tt_.find(state.positionHash()); found != tt_.end()) {
            ttBestMove = found->second.bestMove;
        }

        const std::optional<Move> firstKiller = ply < static_cast<int>(killerMoves_.size()) ? killerMoves_[static_cast<std::size_t>(ply)][0] : std::nullopt;
        const std::optional<Move> secondKiller = ply < static_cast<int>(killerMoves_.size()) ? killerMoves_[static_cast<std::size_t>(ply)][1] : std::nullopt;

        std::stable_sort(candidates.begin(), candidates.end(), [&](const CandidateMove& left, const CandidateMove& right) {
            const bool leftPreferred = preferredMove.has_value() && left.move == *preferredMove;
            const bool rightPreferred = preferredMove.has_value() && right.move == *preferredMove;
            if (leftPreferred != rightPreferred) {
                return leftPreferred;
            }

            const bool leftTt = ttBestMove.has_value() && left.move == *ttBestMove;
            const bool rightTt = ttBestMove.has_value() && right.move == *ttBestMove;
            if (leftTt != rightTt) {
                return leftTt;
            }

            const bool leftKillerOne = firstKiller.has_value() && left.move == *firstKiller;
            const bool rightKillerOne = firstKiller.has_value() && right.move == *firstKiller;
            if (leftKillerOne != rightKillerOne) {
                return leftKillerOne;
            }

            const bool leftKillerTwo = secondKiller.has_value() && left.move == *secondKiller;
            const bool rightKillerTwo = secondKiller.has_value() && right.move == *secondKiller;
            if (leftKillerTwo != rightKillerTwo) {
                return leftKillerTwo;
            }

            const int leftThreat = threatSeverity(left.threatInfo.best);
            const int rightThreat = threatSeverity(right.threatInfo.best);
            if (leftThreat != rightThreat) {
                return leftThreat > rightThreat;
            }

            const int leftHistory = historyScores_[static_cast<std::size_t>(historyIndex(left.move))];
            const int rightHistory = historyScores_[static_cast<std::size_t>(historyIndex(right.move))];
            if (leftHistory != rightHistory) {
                return leftHistory > rightHistory;
            }

            return left.score > right.score;
        });

        return candidates;
    }

    RootSearchResult searchRoot(const GameState& state, int depth, int alpha, int beta, const std::optional<Move>& preferredMove) {
        RootSearchResult result;
        auto candidates = generateOrderedCandidates(state, 0, preferredMove);
        result.rootCandidateCount = static_cast<int>(candidates.size());
        if (candidates.empty()) {
            result.score = StaticEvaluator::evaluate(state, state.sideToMove());
            return result;
        }

        bool searchedAnyChild = false;
        int bestScore = -kInfinity;
        std::optional<Move> bestMove;
        const int originalAlpha = alpha;

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            GameState child = state;
            if (!child.applyMove(candidates[index].move)) {
                continue;
            }

            searchedAnyChild = true;
            int score = 0;
            if (index == 0) {
                score = -negamax(child, depth - 1, -beta, -alpha, 1, true);
            } else {
                score = -negamax(child, depth - 1, -alpha - 1, -alpha, 1, true);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(child, depth - 1, -beta, -alpha, 1, true);
                }
            }

            if (!completedDepth_) {
                return result;
            }

            if (!bestMove.has_value() || score > bestScore) {
                bestScore = score;
                bestMove = candidates[index].move;
            }

            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                recordCutoffMove(candidates[index].move, depth, 0);
                break;
            }
        }

        if (!searchedAnyChild || !bestMove.has_value()) {
            result.score = StaticEvaluator::evaluate(state, state.sideToMove());
            return result;
        }

        result.bestMove = bestMove;
        result.score = bestScore;

        TTEntry entry;
        entry.depth = depth;
        entry.score = scoreToTT(bestScore, 0);
        entry.bestMove = bestMove;
        if (bestScore <= originalAlpha) {
            entry.bound = BoundType::Upper;
        } else if (bestScore >= beta) {
            entry.bound = BoundType::Lower;
        } else {
            entry.bound = BoundType::Exact;
        }
        tt_[state.positionHash()] = entry;
        return result;
    }

    int negamax(const GameState& state, int depth, int alpha, int beta, int ply, bool allowNullMove) {
        ++nodes_;
        if (shouldStop()) {
            return 0;
        }

        if (state.isGameOver()) {
            return terminalScore(state, ply);
        }

        const std::uint64_t key = state.positionHash();
        if (const auto found = tt_.find(key); found != tt_.end() && found->second.depth >= depth) {
            ++ttHits_;
            const TTEntry& entry = found->second;
            const int ttScore = scoreFromTT(entry.score, ply);
            if (entry.bound == BoundType::Exact) {
                return ttScore;
            }
            if (entry.bound == BoundType::Lower) {
                alpha = std::max(alpha, ttScore);
            } else {
                beta = std::min(beta, ttScore);
            }
            if (alpha >= beta) {
                return ttScore;
            }
        }

        if (depth == 0) {
            return StaticEvaluator::evaluate(state, state.sideToMove());
        }

        const int staticEval = StaticEvaluator::evaluate(state, state.sideToMove());
        if (shouldTryNullMove(state, depth, beta, ply, allowNullMove, staticEval)) {
            GameState nullState = state;
            nullState.setSideToMoveForAnalysis(otherPlayer(state.sideToMove()));
            const int reduction = depth >= 6 ? 3 : 2;
            const int nullScore = -negamax(nullState, depth - 1 - reduction, -beta, -beta + 1, ply + 1, false);
            if (!completedDepth_) {
                return 0;
            }
            if (nullScore >= beta) {
                return nullScore;
            }
        }

        auto candidates = generateOrderedCandidates(state, ply);
        if (candidates.empty()) {
            return staticEval;
        }

        const int originalAlpha = alpha;
        int bestScore = -kInfinity;
        std::optional<Move> bestMove;

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            GameState child = state;
            if (!child.applyMove(candidates[index].move)) {
                continue;
            }

            int score = 0;
            if (index == 0) {
                score = -negamax(child, depth - 1, -beta, -alpha, ply + 1, true);
            } else {
                score = -negamax(child, depth - 1, -alpha - 1, -alpha, ply + 1, true);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(child, depth - 1, -beta, -alpha, ply + 1, true);
                }
            }

            if (!completedDepth_) {
                return 0;
            }

            if (!bestMove.has_value() || score > bestScore) {
                bestScore = score;
                bestMove = candidates[index].move;
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                recordCutoffMove(candidates[index].move, depth, ply);
                break;
            }
        }

        if (!bestMove.has_value()) {
            return staticEval;
        }

        TTEntry entry;
        entry.depth = depth;
        entry.score = scoreToTT(bestScore, ply);
        entry.bestMove = bestMove;
        if (bestScore <= originalAlpha) {
            entry.bound = BoundType::Upper;
        } else if (bestScore >= beta) {
            entry.bound = BoundType::Lower;
        } else {
            entry.bound = BoundType::Exact;
        }
        tt_[key] = entry;
        return bestScore;
    }

    std::vector<Move> extractPrincipalVariation(const GameState& root, Move firstMove, int depth) const {
        std::vector<Move> pv;
        pv.reserve(static_cast<std::size_t>(depth));

        GameState line = root;
        if (!line.applyMove(firstMove)) {
            return pv;
        }
        pv.push_back(firstMove);

        for (int remaining = depth - 1; remaining > 0; --remaining) {
            const auto found = tt_.find(line.positionHash());
            if (found == tt_.end() || !found->second.bestMove.has_value()) {
                break;
            }

            const Move move = *found->second.bestMove;
            if (!line.applyMove(move)) {
                break;
            }
            pv.push_back(move);
            if (line.isGameOver()) {
                break;
            }
        }

        return pv;
    }
};

}  // namespace

SearchEngine::SearchEngine(SearchConfig config)
    : config_(config) {
}

SearchResult SearchEngine::search(const GameState& state) {
    SearchRunner runner(config_);
    return runner.run(state);
}

}  // namespace gomoku
