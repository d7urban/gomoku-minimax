#include "gomoku/Search.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gomoku/OpeningBook.hpp"
#include "gomoku/Threats.hpp"
#include "gomoku/TimeGovernor.hpp"

namespace gomoku {

namespace {

constexpr int kInfinity = 1'000'000'000;
constexpr int kMateScore = 10'000'000;
constexpr int kMateThreshold = kMateScore - 1000;
constexpr int kDefaultAspirationWindow = 80;
constexpr int kMaxSearchPly = 64;
constexpr int kLmrTableSize = 64;
constexpr int kPanicLosingThreshold = kMateThreshold;
constexpr int kQuiescenceMaxPly = 6;
constexpr std::size_t kQuiescenceMaxMoves = 8;

// Precomputed log/log LMR reduction table. PV nodes get one less reduction
// than non-PV so the principal line is explored at a higher depth.
struct LmrTable {
    std::array<std::array<std::array<int, kLmrTableSize>, kLmrTableSize>, 2> data {};
    LmrTable() {
        for (int pv = 0; pv < 2; ++pv) {
            for (int d = 1; d < kLmrTableSize; ++d) {
                for (int m = 1; m < kLmrTableSize; ++m) {
                    const double raw = std::log(static_cast<double>(d)) * std::log(static_cast<double>(m)) / 2.0;
                    int reduction = static_cast<int>(std::round(raw)) - pv;
                    data[static_cast<std::size_t>(pv)][static_cast<std::size_t>(d)][static_cast<std::size_t>(m)] = std::max(0, reduction);
                }
            }
        }
    }
};

const LmrTable kLmrTable {};

enum class BoundType : std::uint8_t {
    Exact,
    Lower,
    Upper,
};

struct TTEntry {
    std::uint64_t hash {0};
    int depth {0};
    int score {0};
    BoundType bound {BoundType::Exact};
    std::optional<Move> bestMove;
    std::uint8_t generation {0};
    bool occupied {false};
};

class TranspositionTable {
public:
    static constexpr std::size_t kClusterSize = 4;

    explicit TranspositionTable(std::size_t log2EntryCount = 20)
        : mask_((std::size_t{1} << clusterLog2(log2EntryCount)) - 1)
        , clusters_(mask_ + 1) {
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_ = 1;
        for (auto& cluster : clusters_) {
            for (auto& entry : cluster) {
                entry = TTEntry {};
            }
        }
    }

    void newGeneration() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
        }
    }

    std::optional<TTEntry> probe(std::uint64_t hash) {
        std::lock_guard<std::mutex> lock(mutex_);
        Cluster& cluster = clusterFor(hash);
        for (auto& entry : cluster) {
            if (entry.occupied && entry.hash == hash) {
                entry.generation = generation_;
                return entry;
            }
        }
        return std::nullopt;
    }

    std::optional<TTEntry> probe(std::uint64_t hash) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const Cluster& cluster = clusterFor(hash);
        for (const auto& entry : cluster) {
            if (entry.occupied && entry.hash == hash) {
                return entry;
            }
        }
        return std::nullopt;
    }

    void store(std::uint64_t hash, int depth, int score, BoundType bound, std::optional<Move> bestMove) {
        std::lock_guard<std::mutex> lock(mutex_);
        Cluster& cluster = clusterFor(hash);
        TTEntry* target = nullptr;
        for (auto& entry : cluster) {
            if (entry.occupied && entry.hash == hash) {
                target = &entry;
                break;
            }
        }
        if (target == nullptr) {
            for (auto& entry : cluster) {
                if (!entry.occupied) {
                    target = &entry;
                    break;
                }
            }
        }
        if (target == nullptr) {
            target = &cluster.front();
            int weakestValue = replacementValue(*target);
            for (auto& entry : cluster) {
                const int value = replacementValue(entry);
                if (value < weakestValue) {
                    weakestValue = value;
                    target = &entry;
                }
            }
        }

        if (target->occupied && target->hash == hash) {
            target->generation = generation_;
            if (!bestMove.has_value()) {
                bestMove = target->bestMove;
            }

            const bool exactUpgrade = bound == BoundType::Exact && target->bound != BoundType::Exact;
            const bool deeperOrEqual = depth >= target->depth;
            const bool nearDepthRefresh = target->generation != generation_ && depth >= target->depth - 2;
            const bool shouldOverwrite = exactUpgrade || deeperOrEqual || nearDepthRefresh;
            if (!shouldOverwrite) {
                if (bestMove.has_value()) {
                    target->bestMove = std::move(bestMove);
                }
                return;
            }
        }

        target->hash = hash;
        target->depth = depth;
        target->score = score;
        target->bound = bound;
        target->bestMove = std::move(bestMove);
        target->generation = generation_;
        target->occupied = true;
    }

private:
    using Cluster = std::array<TTEntry, kClusterSize>;

    static constexpr std::size_t clusterLog2(std::size_t log2EntryCount) {
        std::size_t value = (log2EntryCount > 0) ? log2EntryCount : 1;
        while ((std::size_t{1} << value) < kClusterSize) {
            ++value;
        }
        if (value >= 2) {
            return value - 2;
        }
        return 0;
    }

    int replacementValue(const TTEntry& entry) const {
        if (!entry.occupied) {
            return std::numeric_limits<int>::min();
        }
        const int age = static_cast<int>(generation_ - entry.generation);
        const int exactBonus = entry.bound == BoundType::Exact ? 6 : 0;
        return entry.depth + exactBonus - age * 8;
    }

    Cluster& clusterFor(std::uint64_t hash) {
        return clusters_[hash & mask_];
    }

    const Cluster& clusterFor(std::uint64_t hash) const {
        return clusters_[hash & mask_];
    }

    std::size_t mask_;
    std::vector<Cluster> clusters_;
    std::uint8_t generation_ {1};
    mutable std::mutex mutex_;
};

TranspositionTable& sharedTranspositionTable() {
    static TranspositionTable table;
    return table;
}

std::mutex& sharedSearchMutex() {
    static std::mutex mutex;
    return mutex;
}

struct RootSearchResult {
    std::optional<Move> bestMove;
    int score {-kInfinity};
    int rootCandidateCount {0};
};

struct RootWorkerResult {
    bool searched {false};
    bool completed {true};
    int score {-kInfinity};
    std::uint64_t nodes {0};
    std::uint64_t ttHits {0};
    std::uint64_t vcfNodes {0};
    std::uint64_t winVerificationNodes {0};
    std::uint64_t quiescenceNodes {0};
    int vcfHits {0};
    int winVerifications {0};
    int maxPlyVisited {0};
};

struct NodeTacticalState {
    bool opponentThreatSearched {false};
    bool opponentThreatFound {false};
    bool ownThreatSearched {false};
    bool ownThreatFound {false};
    bool nullGuardSearched {false};
    bool nullGuardFoundThreatSequence {false};
    int nullGuardMaxDepthSearched {0};
    bool panicMode {false};
    std::vector<Move> defenseSet;
};

enum class CandidateStage {
    Default,
    OpeningLarge,
    DefendSimpleFour,
    ForcingRoot,
    ForcingChild,
};

std::vector<Move> collectImmediateWinningMoves(const GameState& state, Player player) {
    std::vector<Move> winningMoves;
    winningMoves.reserve(4);
    for (const Move& move : state.legalMoves()) {
        if (state.threatInfoAt(move, player).best == ThreatType::Five) {
            winningMoves.push_back(move);
        }
    }
    return winningMoves;
}

void addUniqueMove(std::vector<Move>& moves, Move move) {
    if (std::find(moves.begin(), moves.end(), move) == moves.end()) {
        moves.push_back(move);
    }
}

std::vector<Move> candidateMoveList(const std::vector<CandidateMove>& candidates) {
    std::vector<Move> moves;
    moves.reserve(candidates.size());
    for (const CandidateMove& candidate : candidates) {
        moves.push_back(candidate.move);
    }
    return moves;
}

bool containsMove(const std::vector<Move>& moves, Move move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

std::vector<Move> removedCandidateMoves(const std::vector<CandidateMove>& before,
                                         const std::vector<CandidateMove>& after) {
    const std::vector<Move> afterMoves = candidateMoveList(after);
    std::vector<Move> removed;
    removed.reserve(before.size());
    for (const CandidateMove& candidate : before) {
        if (!containsMove(afterMoves, candidate.move)) {
            removed.push_back(candidate.move);
        }
    }
    return removed;
}

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

struct VcfProbe {
    std::uint64_t nodeBudget {600};
    std::uint64_t nodes {0};

    static std::vector<Move> collectNeighborhoodMoves(const GameState& state, std::optional<Move> anchor, int radius) {
        std::vector<Move> moves;
        const int boardSize = state.boardSize();
        const std::vector<Player>& board = state.board();

        if (anchor.has_value()) {
            const int rowMin = std::max(0, anchor->row - radius);
            const int rowMax = std::min(boardSize - 1, anchor->row + radius);
            const int colMin = std::max(0, anchor->col - radius);
            const int colMax = std::min(boardSize - 1, anchor->col + radius);
            for (int row = rowMin; row <= rowMax; ++row) {
                for (int col = colMin; col <= colMax; ++col) {
                    const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
                    if (board[index] == Player::None) {
                        moves.push_back({row, col});
                    }
                }
            }
            return moves;
        }

        const std::vector<std::uint8_t>& nearCounts = state.nearStoneCounts();
        for (int row = 0; row < boardSize; ++row) {
            for (int col = 0; col < boardSize; ++col) {
                const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
                if (board[index] == Player::None && nearCounts[index] > 0) {
                    moves.push_back({row, col});
                }
            }
        }
        return moves;
    }

    static int forcingScore(Move move, const MoveThreatInfo& info, std::optional<Move> anchor) {
        int score = threatSeverityEnhanced(info);
        if (anchor.has_value()) {
            const int distance = std::max(std::abs(move.row - anchor->row), std::abs(move.col - anchor->col));
            score -= distance;
        }
        return score;
    }

    static std::vector<CandidateMove> generateForcingRootMoves(const GameState& state, Player attacker) {
        std::vector<CandidateMove> candidates;
        const Player defender = otherPlayer(attacker);
        const bool defenderHasFour = state.hasThreatAtLeast(defender, ThreatType::SimpleFour);
        const std::vector<Move> moves = collectNeighborhoodMoves(state, std::nullopt, 2);
        candidates.reserve(moves.size());

        for (const Move& move : moves) {
            const MoveThreatInfo attackInfo = state.threatInfoAt(move, attacker);
            const MoveThreatInfo defendInfo = state.threatInfoAt(move, defender);
            const bool createsForcingThreat = threatSeverity(attackInfo.best) >= threatSeverity(ThreatType::SimpleFour);
            if (defenderHasFour) {
                const bool blocksImmediateThreat = threatSeverity(defendInfo.best) >= threatSeverity(ThreatType::OpenFour);
                if (blocksImmediateThreat && createsForcingThreat) {
                    candidates.push_back({move, attackInfo, forcingScore(move, attackInfo, state.lastPlacedMove())});
                }
                continue;
            }
            if (!createsForcingThreat) {
                continue;
            }
            candidates.push_back({move, attackInfo, forcingScore(move, attackInfo, state.lastPlacedMove())});
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const CandidateMove& left, const CandidateMove& right) {
            return left.score > right.score;
        });
        return candidates;
    }

    static std::vector<CandidateMove> generateForcingChildMoves(const GameState& state, Player attacker) {
        const std::optional<Move> anchor = state.lastPlacedMove();
        std::vector<CandidateMove> candidates;
        const std::vector<Move> moves = collectNeighborhoodMoves(state, anchor, 4);
        candidates.reserve(moves.size());
        for (const Move& move : moves) {
            const MoveThreatInfo attackInfo = state.threatInfoAt(move, attacker);
            if (threatSeverity(attackInfo.best) < threatSeverity(ThreatType::SimpleFour)) {
                continue;
            }
            candidates.push_back({move, attackInfo, forcingScore(move, attackInfo, anchor)});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const CandidateMove& left, const CandidateMove& right) {
            return left.score > right.score;
        });
        return candidates;
    }

    // Collect empty squares in the 9x9 (Chebyshev-4) neighborhood of
    // `anchor` where `player` playing would form an immediate Five.
    // Scans the bounded window directly instead of calling legalMoves()
    // to avoid the per-call vector allocation.
    static std::vector<Move> fiveCompletions(const GameState& state, Player player, Move anchor) {
        std::vector<Move> completions;
        const int size = state.boardSize();
        const int rowMin = std::max(0, anchor.row - 4);
        const int rowMax = std::min(size - 1, anchor.row + 4);
        const int colMin = std::max(0, anchor.col - 4);
        const int colMax = std::min(size - 1, anchor.col + 4);
        for (int row = rowMin; row <= rowMax; ++row) {
            for (int col = colMin; col <= colMax; ++col) {
                if (state.cellAt(row, col) != Player::None) {
                    continue;
                }
                if (state.threatInfoAt({row, col}, player).best == ThreatType::Five) {
                    completions.push_back({row, col});
                }
            }
        }
        return completions;
    }

    // True if `player` has any move on the board that would form Five on
    // its turn. Delegates to the O(N) non-allocating hasThreatAtLeast
    // instead of calling legalMoves() which allocates a ~200-entry vector.
    static bool hasFiveCreation(const GameState& state, Player player) {
        return state.hasThreatAtLeast(player, ThreatType::Five);
    }

    // Returns true if `attacker` (to move) can force a Five in <= depthRemaining
    // attacker-plies using only moves that create a SimpleFour/OpenFour/Five.
    bool attack(GameState& state, Player attacker, int depthRemaining) {
        if (nodes >= nodeBudget || depthRemaining <= 0) {
            return false;
        }
        if (state.sideToMove() != attacker || state.isGameOver()) {
            return false;
        }
        ++nodes;

        const Player defender = otherPlayer(attacker);

        std::vector<CandidateMove> candidates = state.lastPlacedMove().has_value()
            ? generateForcingChildMoves(state, attacker)
            : generateForcingRootMoves(state, attacker);
        if (candidates.empty()) {
            return false;
        }

        for (const CandidateMove& candidate : candidates) {
            if (!state.applyMove(candidate.move)) {
                continue;
            }

            if (state.isGameOver()
                && ((attacker == Player::Black && state.result() == GameResult::BlackWin)
                    || (attacker == Player::White && state.result() == GameResult::WhiteWin))) {
                state.undo();
                return true;
            }

            // Defender may have a Five-creating move of their own — in that case
            // they just play it and win before our follow-up. An OpenFour is
            // equally decisive: the defender completes it on their next move
            // regardless of any block, so the VCF cannot succeed.
            if (hasFiveCreation(state, defender) || state.hasThreatAtLeast(defender, ThreatType::OpenFour)) {
                state.undo();
                continue;
            }

            const std::vector<Move> completions = fiveCompletions(state, attacker, candidate.move);
            if (completions.empty()) {
                state.undo();
                continue;
            }

            if (completions.size() >= 2) {
                // Unblockable: defender can only cover one completion square.
                state.undo();
                return true;
            }

            // SimpleFour: defender must block the single completion square.
            const Move defense = completions.front();
            if (!state.applyMove(defense)) {
                state.undo();
                continue;
            }

            // If the forced block also creates a Five-threat or OpenFour for the
            // defender, the VCF cannot succeed — abort this branch.
            if (hasFiveCreation(state, defender) || state.hasThreatAtLeast(defender, ThreatType::OpenFour)) {
                state.undo();
                state.undo();
                continue;
            }

            const bool forced = attack(state, attacker, depthRemaining - 1);
            state.undo();
            state.undo();
            if (forced) {
                return true;
            }
        }

        return false;
    }
};

class SearchRunner {
public:
    explicit SearchRunner(SearchConfig config, TranspositionTable& table, std::optional<MoveBudget> budget = std::nullopt)
        : config_(config)
        , budget_(std::move(budget))
        , tt_(table) {
    }

    SearchResult run(const GameState& state) {
        SearchResult result;
        startTime_ = Clock::now();
        tt_.newGeneration();
        historyScores_.assign(16U * 16U, 0);
        killerMoves_.assign(static_cast<std::size_t>(std::max(config_.maxDepth + 8, kMaxSearchPly)), {});
        counterMoves_.assign(16U * 16U, std::nullopt);
        defensiveBlockingMoves_.clear();
        nodeTacticalStateCache_.clear();
        panicModeEntered_ = false;
        rootCandidateCountBeforeDefFilter_ = 0;
        rootCandidateCountAfterDefFilter_ = 0;
        rootDefFilterApplied_ = false;
        rootDefFilterReason_ = "none";
        rootMovesBeforeDefFilter_.clear();
        rootMovesAfterDefFilter_.clear();
        rootMovesRemovedByDefFilter_.clear();

        if (config_.useOpeningBook) {
            if (const auto bookHit = lookupOpeningBookMove(state)) {
                result.bestMove = bookHit->move;
                result.summary.score = StaticEvaluator::evaluate(state, state.sideToMove());
                result.summary.rootCandidateCount = 1;
                result.summary.completedLastDepth = true;
                result.summary.usedOpeningBook = true;
                result.summary.decisionSource = "opening_book";
                result.summary.stopReason = "opening_book";
                result.summary.openingBookName = std::string(bookHit->lineName);
                result.summary.principalVariation = {bookHit->move};
                return finalizeResult(std::move(result));
            }
        }

        const Player side = state.sideToMove();
        if (const std::vector<Move> winningMoves = collectImmediateWinningMoves(state, side); !winningMoves.empty()) {
            result.bestMove = winningMoves.front();
            result.summary.score = kMateScore;
            result.summary.depthReached = 1;
            result.summary.maxDepthVisited = 1;
            result.summary.rootCandidateCount = static_cast<int>(winningMoves.size());
            result.summary.completedLastDepth = true;
            result.summary.decisionSource = "immediate_win";
            result.summary.stopReason = "immediate_win";
            result.summary.principalVariation = {winningMoves.front()};
            return finalizeResult(std::move(result));
        }

        const Player opponent = otherPlayer(side);
        const std::vector<Move> opponentWinningMoves = collectImmediateWinningMoves(state, opponent);
        if (opponentWinningMoves.size() == 1U) {
            result.bestMove = opponentWinningMoves.front();
            result.summary.score = StaticEvaluator::evaluate(state, side);
            result.summary.maxDepthVisited = 1;
            result.summary.rootCandidateCount = 1;
            result.summary.completedLastDepth = true;
            result.summary.decisionSource = "forced_block";
            result.summary.stopReason = "forced_block";
            result.summary.principalVariation = {opponentWinningMoves.front()};
            return finalizeResult(std::move(result));
        }
        if (opponentWinningMoves.size() > 1U) {
            panicModeEntered_ = true;
            result.summary.panicModeEntered = true;
            nodeTacticalStateCache_[state.positionHash()].panicMode = true;
        }

        if (config_.useRootThreatSearch && shouldRunRootThreatSearch(state)) {
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
                result.summary.maxDepthVisited = static_cast<int>(threatResult.sequence.size());
                result.summary.threatSequenceLength = static_cast<int>(threatResult.sequence.size());
                result.summary.usedThreatSequence = true;
                result.summary.completedLastDepth = true;
                result.summary.decisionSource = "threat_sequence";
                result.summary.stopReason = "threat_sequence";
                result.threatSequence = threatResult;
                result.summary.principalVariation.reserve(threatResult.sequence.size());
                for (const ThreatStep& step : threatResult.sequence) {
                    result.summary.principalVariation.push_back(step.move);
                }
                return finalizeResult(std::move(result));
            }

            // Defensive threat search: check if the opponent has a forced
            // winning sequence. If so, mark the critical blocking moves
            // so the search prioritises them.
            ThreatSequenceConfig defConfig;
            defConfig.maxDepth = std::max(2, config_.maxDepth);
            defConfig.maxNodes = std::max<std::uint64_t>(500, config_.maxNodes / 6);
            defConfig.timeLimitMs = hardTimeLimitMs() > 0 ? std::max(5, hardTimeLimitMs() / 8) : 0;
            defConfig.maxThreatMoves = std::max<std::size_t>(4, config_.maxCandidateMoves / 2);

            GameState opponentThreatState = state;
            opponentThreatState.setSideToMoveForAnalysis(opponent);
            ThreatSequenceSearcher defSearcher(defConfig);
            ThreatSearchResult defResult = defSearcher.searchWinningSequence(opponentThreatState, opponent);
            result.summary.threatNodes += defResult.nodes;
            if (defResult.foundWin && !defResult.sequence.empty()) {
                defensiveBlockingMoves_.clear();
                for (const ThreatStep& step : defResult.sequence) {
                    addUniqueMove(defensiveBlockingMoves_, step.move);
                    for (const Move& defense : step.defenseMoves) {
                        addUniqueMove(defensiveBlockingMoves_, defense);
                    }
                    for (const Move& required : step.requiredEmpty) {
                        addUniqueMove(defensiveBlockingMoves_, required);
                    }
                }
            }
        }

        GameState rootState = state;
        auto rootMoves = generateOrderedCandidates(rootState, 0);
        result.summary.rootCandidateCount = static_cast<int>(rootMoves.size());
        if (rootMoves.empty()) {
            result.summary.score = StaticEvaluator::evaluate(rootState, rootState.sideToMove());
            result.summary.stopReason = "no_candidates";
            return finalizeResult(std::move(result));
        }

        result.bestMove = rootMoves.front().move;
        result.summary.score = rootMoves.front().score;

        int previousScore = result.summary.score;
        int lastIterationCostMs = 0;
        int nextIterationEstimateMs = 0;
        std::string stopReason = "max_depth";
        for (int depth = 1; depth <= config_.maxDepth; ++depth) {
            if (shouldStop()) {
                stopReason = stopReasonFromLimits();
                break;
            }
            if (depth > 1 && !panicModeEntered_ && softLimitReached()) {
                stopReason = "soft_limit";
                break;
            }

            tt_.newGeneration();

            // ID affordability: skip starting the next iteration if the
            // predicted cost would push us past the hard cap with slack.
            // Only engaged when the governor is active (budget_.has_value)
            // and has given us a branching estimate.
            if (depth > 1 && lastIterationCostMs > 0 && budget_
                && effectiveNextIterationEstimate() > 0.0
                && budget_->hardCapMs > 0)
            {
                nextIterationEstimateMs = predictedNextIterationMs(lastIterationCostMs);
                const std::int64_t predictedNext = nextIterationEstimateMs;
                const std::int64_t boundary = budget_->hardCapMs - budget_->finalizationSlackMs;
                if (static_cast<std::int64_t>(elapsedMs()) + predictedNext > boundary) {
                    stopReason = "affordability";
                    break;
                }
            }

            completedDepth_ = true;
            rootIterationDepth_ = depth;
            RootSearchResult iteration;
            const int iterationStartMs = elapsedMs();

            if (config_.useAspirationWindows && depth > 1 && std::abs(previousScore) < kMateThreshold) {
                int aspiration = kDefaultAspirationWindow;
                int alpha = std::max(-kInfinity, previousScore - aspiration);
                int beta = std::min(kInfinity, previousScore + aspiration);

                while (true) {
                    iteration = searchRoot(rootState, depth, alpha, beta, result.bestMove);
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
                iteration = searchRoot(rootState, depth, -kInfinity, kInfinity, result.bestMove);
            }

            if (!completedDepth_ || !iteration.bestMove.has_value()) {
                stopReason = stopReasonFromLimits();
                break;
            }

            result.bestMove = iteration.bestMove;
            result.summary.depthReached = depth;
            result.summary.score = iteration.score;
            result.summary.completedLastDepth = true;
            result.summary.rootCandidateCount = iteration.rootCandidateCount;
            result.summary.principalVariation = extractPrincipalVariation(state, *iteration.bestMove, depth);
            previousScore = iteration.score;
            lastIterationCostMs = std::max(0, elapsedMs() - iterationStartMs);
            result.summary.lastIterationMs = lastIterationCostMs;
            if (iteration.score <= -kPanicLosingThreshold) {
                panicModeEntered_ = true;
                result.summary.panicModeEntered = true;
                nodeTacticalStateCache_[rootState.positionHash()].panicMode = true;
            }

            // Update best-move stability state *before* checking softLimit
            // so the effective soft cap reflects this iteration's result.
            if (lastIterationBestMove_.has_value() && *lastIterationBestMove_ == *iteration.bestMove) {
                stableIterationCount_++;
                bestMoveChangedLastIter_ = false;
            } else {
                // First iteration has no baseline — treat as neither stable nor changed.
                bestMoveChangedLastIter_ = lastIterationBestMove_.has_value();
                stableIterationCount_ = 0;
            }
            scoreSwingLastIter_ = lastIterationScore_.has_value()
                && budget_
                && budget_->scoreSwingThreshold > 0
                && std::abs(iteration.score - *lastIterationScore_) >= budget_->scoreSwingThreshold;
            lastIterationBestMove_ = iteration.bestMove;
            lastIterationScore_ = iteration.score;
            nextIterationEstimateMs = predictedNextIterationMs(lastIterationCostMs);
            result.summary.nextIterationEstimateMs = nextIterationEstimateMs;
            publishProgress(result.summary);

            if (!panicModeEntered_ && softLimitReached()) {
                stopReason = "soft_limit";
                break;
            }
        }

        result.summary.stopReason = stopReason;
        return finalizeResult(std::move(result));
    }

private:
    SearchConfig config_ {};
    std::optional<MoveBudget> budget_ {};
    Clock::time_point startTime_ {};
    std::uint64_t nodes_ {0};
    std::uint64_t ttHits_ {0};
    std::uint64_t vcfProbeNodes_ {0};
    std::uint64_t winVerificationNodes_ {0};
    std::uint64_t quiescenceNodes_ {0};
    int vcfHits_ {0};
    int winVerificationCount_ {0};
    int rootIterationDepth_ {0};
    int maxPlyVisited_ {0};
    bool completedDepth_ {true};
    // Best-move instability tracking for dynamic soft-limit modulation.
    // See effectiveSoftLimitMs — kept here rather than in the ID loop so
    // softLimitReached() can consult them.
    std::optional<Move> lastIterationBestMove_ {};
    std::optional<int> lastIterationScore_ {};
    int  stableIterationCount_ {0};
    bool bestMoveChangedLastIter_ {false};
    bool scoreSwingLastIter_ {false};
    bool panicModeEntered_ {false};
    TranspositionTable& tt_;
    std::vector<std::array<std::optional<Move>, 2>> killerMoves_;
    std::vector<int> historyScores_;
    std::vector<std::optional<Move>> counterMoves_;
    std::vector<Move> defensiveBlockingMoves_;
    std::unordered_map<std::uint64_t, NodeTacticalState> nodeTacticalStateCache_;
    int rootCandidateCountBeforeDefFilter_ {0};
    int rootCandidateCountAfterDefFilter_ {0};
    bool rootDefFilterApplied_ {false};
    std::string rootDefFilterReason_ {"none"};
    std::vector<Move> rootMovesBeforeDefFilter_;
    std::vector<Move> rootMovesAfterDefFilter_;
    std::vector<Move> rootMovesRemovedByDefFilter_;

    SearchResult finalizeResult(SearchResult result) const {
        result.summary.maxDepthVisited = std::max(result.summary.maxDepthVisited,
            std::max(result.summary.depthReached, maxPlyVisited_));
        result.summary.nodes = nodes_;
        result.summary.maxNodes = config_.maxNodes;
        result.summary.ttHits = ttHits_;
        result.summary.vcfNodes = vcfProbeNodes_;
        result.summary.winVerificationNodes = winVerificationNodes_;
        result.summary.vcfHits = vcfHits_;
        result.summary.winVerifications = winVerificationCount_;
        result.summary.elapsedMs = elapsedMs();
        result.summary.softLimitMs = softTimeLimitMs();
        result.summary.hardLimitMs = hardTimeLimitMs();
        result.summary.requestedRootThreads = config_.maxRootThreads;
        result.summary.panicModeEntered = result.summary.panicModeEntered || panicModeEntered_;
        result.summary.rootCandidateCountBeforeDefFilter = rootCandidateCountBeforeDefFilter_;
        result.summary.rootCandidateCountAfterDefFilter = rootCandidateCountAfterDefFilter_;
        result.summary.defFilterApplied = result.summary.defFilterApplied || rootDefFilterApplied_;
        result.summary.defFilterReason = rootDefFilterReason_;
        result.summary.rootMovesBeforeDefFilter = rootMovesBeforeDefFilter_;
        result.summary.rootMovesAfterDefFilter = rootMovesAfterDefFilter_;
        result.summary.rootMovesRemovedByDefFilter = rootMovesRemovedByDefFilter_;
        return result;
    }

    void publishProgress(const SearchSummary& summary) const {
        if (!config_.progressCallback) {
            return;
        }
        SearchSummary progress = summary;
        progress.maxDepthVisited = std::max(progress.maxDepthVisited,
            std::max(progress.depthReached, maxPlyVisited_));
        progress.nodes = nodes_;
        progress.maxNodes = config_.maxNodes;
        progress.ttHits = ttHits_;
        progress.vcfNodes = vcfProbeNodes_;
        progress.winVerificationNodes = winVerificationNodes_;
        progress.vcfHits = vcfHits_;
        progress.winVerifications = winVerificationCount_;
        progress.elapsedMs = elapsedMs();
        progress.softLimitMs = softTimeLimitMs();
        progress.hardLimitMs = hardTimeLimitMs();
        progress.requestedRootThreads = config_.maxRootThreads;
        progress.panicModeEntered = progress.panicModeEntered || panicModeEntered_;
        progress.rootCandidateCountBeforeDefFilter = rootCandidateCountBeforeDefFilter_;
        progress.rootCandidateCountAfterDefFilter = rootCandidateCountAfterDefFilter_;
        progress.defFilterApplied = progress.defFilterApplied || rootDefFilterApplied_;
        progress.defFilterReason = rootDefFilterReason_;
        progress.rootMovesBeforeDefFilter = rootMovesBeforeDefFilter_;
        progress.rootMovesAfterDefFilter = rootMovesAfterDefFilter_;
        progress.rootMovesRemovedByDefFilter = rootMovesRemovedByDefFilter_;
        config_.progressCallback(progress);
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

    std::string stopReasonFromLimits() const {
        if (config_.maxNodes > 0 && nodes_ >= config_.maxNodes) {
            return "node_limit";
        }
        if (hardTimeLimitMs() > 0 && elapsedMs() >= hardTimeLimitMs()) {
            return "hard_limit";
        }
        return "incomplete";
    }

    // Effective soft limit used for early-stop decisions. Always derived
    // from the baseline softTimeLimitMs — never from a previously
    // modulated value — so scale factors do not compound across
    // iterations. Hard cap always dominates; falls back to the baseline
    // when the governor is not engaged or has disabled the feature
    // (stableIterationsNeeded < 0).
    int effectiveSoftLimitMs() const {
        const int base = softTimeLimitMs();
        if (base <= 0 || !budget_ || budget_->stableIterationsNeeded < 0) {
            return base;
        }
        double scale = 1.0;
        if (bestMoveChangedLastIter_ || scoreSwingLastIter_) {
            scale = budget_->bestMoveUnstableScale;
            if (scoreSwingLastIter_) {
                scale = std::max(scale, budget_->scoreSwingUnstableScale);
            }
        } else if (stableIterationCount_ >= budget_->stableIterationsNeeded) {
            scale = budget_->bestMoveStableScale;
        }
        int adjusted = static_cast<int>(std::llround(static_cast<double>(base) * scale));
        const int hard = hardTimeLimitMs();
        if (hard > 0 && adjusted > hard) adjusted = hard;
        if (adjusted < 1) adjusted = 1;
        return adjusted;
    }

    bool softLimitReached() const {
        const int limit = effectiveSoftLimitMs();
        return limit > 0 && elapsedMs() >= limit;
    }

    double effectiveNextIterationEstimate() const {
        if (!budget_ || budget_->nextIterBranchingEstimate <= 0.0) {
            return 0.0;
        }

        double estimate = budget_->nextIterBranchingEstimate;
        const bool unstable = bestMoveChangedLastIter_ || scoreSwingLastIter_;
        if (unstable) {
            estimate *= budget_->nextIterUnstableScale;
        } else if (stableIterationCount_ >= budget_->stableIterationsNeeded) {
            estimate *= budget_->nextIterStableScale;
        }
        return std::max(0.0, estimate);
    }

    int predictedNextIterationMs(int lastIterationCostMs) const {
        if (lastIterationCostMs <= 0) {
            return 0;
        }
        const double estimate = effectiveNextIterationEstimate();
        if (estimate <= 0.0) {
            return 0;
        }
        const std::int64_t predicted = static_cast<std::int64_t>(
            static_cast<double>(lastIterationCostMs) * estimate);
        return static_cast<int>(std::min<std::int64_t>(
            predicted, static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    }

    bool shouldRunRootThreatSearch(const GameState& state) const {
        const Player side = state.sideToMove();
        const Player opponent = otherPlayer(side);
        return state.hasThreatAtLeast(side, ThreatType::OpenThree)
            || state.hasThreatAtLeast(opponent, ThreatType::OpenThree);
    }

    static std::size_t rootCandidateBudgetForTimeMs(int timeLimitMs) {
        if (timeLimitMs >= 20'000) {
            return 96;
        }
        if (timeLimitMs >= 10'000) {
            return 80;
        }
        if (timeLimitMs >= 5'000) {
            return 64;
        }
        if (timeLimitMs >= 2'000) {
            return 48;
        }
        return 32;
    }

    static std::size_t earlyCandidateBudgetForTimeMs(int timeLimitMs) {
        if (timeLimitMs >= 20'000) {
            return 56;
        }
        if (timeLimitMs >= 10'000) {
            return 48;
        }
        if (timeLimitMs >= 5'000) {
            return 40;
        }
        if (timeLimitMs >= 2'000) {
            return 32;
        }
        return 28;
    }

    std::size_t candidateBudget(const GameState& state, int ply) const {
        std::size_t budget = std::max<std::size_t>(1, config_.maxCandidateMoves);
        if (config_.maxCandidateMoves >= 28) {
            if (ply == 0) {
                budget = std::max(budget, rootCandidateBudgetForTimeMs(hardTimeLimitMs()));
            } else if (ply <= 2) {
                budget = std::max(budget, earlyCandidateBudgetForTimeMs(hardTimeLimitMs()));
            }
        }

        const Player opponent = otherPlayer(state.sideToMove());
        if (state.hasThreatAtLeast(opponent, ThreatType::SimpleFour)) {
            return std::max<std::size_t>(budget, 32);
        }
        return budget;
    }

    static int candidateCentralityScore(const GameState& state, Move move) {
        const int center = state.boardSize() / 2;
        return 200 - 12 * (std::abs(move.row - center) + std::abs(move.col - center));
    }

    static int candidateNeighborhoodPressure(const GameState& state, Move move, Player player) {
        const std::vector<Player>& board = state.board();
        const int boardSize = state.boardSize();
        const Player opponent = otherPlayer(player);
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

    static int attackThreatBonus(const MoveThreatInfo& info) {
        if (threatSeverity(info.best) >= threatSeverity(ThreatType::SimpleFour)) {
            return 250'000;
        }
        const int enhanced = threatSeverityEnhanced(info);
        if (enhanced >= 600) {
            return 150'000;
        }
        if (enhanced >= 400) {
            return 100'000;
        }
        if (enhanced >= 300) {
            return 80'000;
        }
        if (threatSeverity(info.best) >= threatSeverity(ThreatType::OpenThree)) {
            return 50'000;
        }
        if (threatSeverity(info.best) >= threatSeverity(ThreatType::BrokenThree)) {
            return 5'000;
        }
        return 0;
    }

    CandidateMove buildCandidateMove(const GameState& state, Move move, Player player) const {
        CandidateMove candidate;
        candidate.move = move;
        candidate.threatInfo = state.threatInfoAt(move, player);
        candidate.score = candidate.threatInfo.totalScore
            + candidateCentralityScore(state, move)
            + candidateNeighborhoodPressure(state, move, player);
        candidate.score += attackThreatBonus(candidate.threatInfo);

        const MoveThreatInfo defensiveInfo = state.threatInfoAt(move, otherPlayer(player));
        candidate.score += defensiveInfo.totalScore;
        if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
            candidate.score += 500'000;
        } else if (threatSeverityEnhanced(defensiveInfo) >= 600) {
            candidate.score += 500'000;
        } else if (threatSeverityEnhanced(defensiveInfo) >= 300) {
            candidate.score += 20'000;
        } else if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::OpenThree)) {
            candidate.score += 50'000;
        } else if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::BrokenThree)) {
            candidate.score += 5'000;
        }
        if (candidate.threatInfo.best == ThreatType::Five) {
            candidate.score += 10'000'000;
        }
        return candidate;
    }

    std::vector<Move> collectNeighborhoodMoves(const GameState& state, int radius) const {
        std::vector<Move> moves;
        if (state.isGameOver() || state.isSwapDecisionPending()) {
            return moves;
        }
        const int boardSize = state.boardSize();
        const std::vector<Player>& board = state.board();

        if (state.moveCount() == 0) {
            moves.push_back({boardSize / 2, boardSize / 2});
            return moves;
        }

        if (radius <= 2) {
            const std::vector<std::uint8_t>& nearCounts = state.nearStoneCounts();
            for (int row = 0; row < boardSize; ++row) {
                for (int col = 0; col < boardSize; ++col) {
                    const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
                    if (board[index] == Player::None && nearCounts[index] > 0) {
                        moves.push_back({row, col});
                    }
                }
            }
            return moves;
        }

        std::vector<unsigned char> marked(static_cast<std::size_t>(boardSize * boardSize), 0U);
        for (int row = 0; row < boardSize; ++row) {
            for (int col = 0; col < boardSize; ++col) {
                const std::size_t stoneIndex = static_cast<std::size_t>(row * boardSize + col);
                if (board[stoneIndex] == Player::None) {
                    continue;
                }
                const int rowMin = std::max(0, row - radius);
                const int rowMax = std::min(boardSize - 1, row + radius);
                const int colMin = std::max(0, col - radius);
                const int colMax = std::min(boardSize - 1, col + radius);
                for (int targetRow = rowMin; targetRow <= rowMax; ++targetRow) {
                    for (int targetCol = colMin; targetCol <= colMax; ++targetCol) {
                        const std::size_t index = static_cast<std::size_t>(targetRow * boardSize + targetCol);
                        if (board[index] != Player::None || marked[index] != 0U) {
                            continue;
                        }
                        marked[index] = 1U;
                        moves.push_back({targetRow, targetCol});
                    }
                }
            }
        }
        return moves;
    }

    std::vector<CandidateMove> scoreAndSortMoves(const GameState& state,
                                                 Player player,
                                                 const std::vector<Move>& moves,
                                                 std::size_t maxMoves) const {
        std::vector<CandidateMove> candidates;
        candidates.reserve(moves.size());
        for (const Move& move : moves) {
            candidates.push_back(buildCandidateMove(state, move, player));
        }
        std::sort(candidates.begin(), candidates.end(), [](const CandidateMove& left, const CandidateMove& right) {
            return left.score > right.score;
        });
        if (candidates.size() > maxMoves) {
            candidates.resize(maxMoves);
        }
        return candidates;
    }

    std::vector<CandidateMove> generateDefaultStageCandidates(const GameState& state,
                                                              Player player,
                                                              std::size_t maxMoves,
                                                              int radius = 2) const {
        return scoreAndSortMoves(state, player, collectNeighborhoodMoves(state, radius), maxMoves);
    }

    std::vector<CandidateMove> generateOpeningLargeCandidates(const GameState& state,
                                                              Player player,
                                                              std::size_t maxMoves) const {
        const std::size_t widenedBudget = std::max<std::size_t>(maxMoves, 24);
        return scoreAndSortMoves(state, player, collectNeighborhoodMoves(state, 3), widenedBudget);
    }

    std::vector<CandidateMove> generateDefendStageCandidates(const GameState& state,
                                                             Player player,
                                                             bool opponentFourOnBoard,
                                                             std::size_t maxMoves) const {
        const Player opponent = otherPlayer(player);
        std::vector<CandidateMove> candidates;
        const std::vector<Move> moves = collectNeighborhoodMoves(state, 2);
        candidates.reserve(moves.size());
        for (const Move& move : moves) {
            const MoveThreatInfo attackInfo = state.threatInfoAt(move, player);
            const MoveThreatInfo opponentThreatHere = state.threatInfoAt(move, opponent);
            const ThreatType defenseThreshold = opponentFourOnBoard ? ThreatType::OpenFour : ThreatType::SimpleFour;
            const bool defends = threatSeverity(opponentThreatHere.best) >= threatSeverity(defenseThreshold)
                || (!opponentFourOnBoard && threatSeverityEnhanced(opponentThreatHere) >= 600);
            const bool counters = isDefensiveCounterMove(attackInfo, opponentFourOnBoard);
            if (!defends && !counters) {
                continue;
            }
            candidates.push_back(buildCandidateMove(state, move, player));
        }
        std::sort(candidates.begin(), candidates.end(), [](const CandidateMove& left, const CandidateMove& right) {
            return left.score > right.score;
        });
        if (candidates.size() > maxMoves) {
            candidates.resize(maxMoves);
        }
        return candidates;
    }

    std::vector<CandidateMove> generateForcingStageCandidates(const GameState& state,
                                                              Player player,
                                                              bool rootStage,
                                                              std::size_t maxMoves) const {
        std::vector<Move> scopedMoves;
        if (rootStage || !state.lastPlacedMove().has_value()) {
            scopedMoves = collectNeighborhoodMoves(state, 2);
        } else {
            const int radius = 4;
            const Move anchor = *state.lastPlacedMove();
            const int boardSize = state.boardSize();
            const std::vector<Player>& board = state.board();
            std::vector<unsigned char> marked(static_cast<std::size_t>(boardSize * boardSize), 0U);
            const int rowMin = std::max(0, anchor.row - radius);
            const int rowMax = std::min(boardSize - 1, anchor.row + radius);
            const int colMin = std::max(0, anchor.col - radius);
            const int colMax = std::min(boardSize - 1, anchor.col + radius);
            for (int row = rowMin; row <= rowMax; ++row) {
                for (int col = colMin; col <= colMax; ++col) {
                    const std::size_t index = static_cast<std::size_t>(row * boardSize + col);
                    if (board[index] != Player::None || marked[index] != 0U) {
                        continue;
                    }
                    marked[index] = 1U;
                    scopedMoves.push_back({row, col});
                }
            }
        }

        std::vector<CandidateMove> candidates;
        candidates.reserve(scopedMoves.size());
        for (const Move& move : scopedMoves) {
            const MoveThreatInfo info = state.threatInfoAt(move, player);
            if (threatSeverity(info.best) < threatSeverity(ThreatType::SimpleFour)
                && threatSeverityEnhanced(info) < 400) {
                continue;
            }
            candidates.push_back(buildCandidateMove(state, move, player));
        }
        std::sort(candidates.begin(), candidates.end(), [](const CandidateMove& left, const CandidateMove& right) {
            return left.score > right.score;
        });
        if (candidates.empty()) {
            return generateDefaultStageCandidates(state, player, maxMoves);
        }
        if (candidates.size() > maxMoves) {
            candidates.resize(maxMoves);
        }
        return candidates;
    }

    int opponentReplyThreatRisk(const GameState& state, Move move, Player player) const {
        GameState next = state;
        if (!next.applyMove(move)) {
            return std::numeric_limits<int>::max();
        }
        if (next.isGameOver()) {
            return -1;
        }

        const Player opponent = otherPlayer(player);
        int risk = 0;
        for (const Move& reply : next.legalMoves()) {
            risk = std::max(risk, threatSeverity(next.threatInfoAt(reply, opponent).best));
        }
        return risk;
    }

    std::vector<Move> applyRootReplySafetyFilter(const GameState& state, Player player, std::vector<CandidateMove>& candidates) const {
        if (candidates.size() <= 1) {
            return {};
        }

        int bestRisk = std::numeric_limits<int>::max();
        std::vector<int> risks;
        risks.reserve(candidates.size());
        for (const CandidateMove& candidate : candidates) {
            const int risk = opponentReplyThreatRisk(state, candidate.move, player);
            risks.push_back(risk);
            bestRisk = std::min(bestRisk, risk);
        }

        std::vector<Move> missingBestRiskMoves;
        for (const Move& move : state.legalMoves()) {
            const bool alreadyCandidate = std::any_of(candidates.begin(), candidates.end(),
                [&](const CandidateMove& candidate) { return candidate.move == move; });
            if (alreadyCandidate) {
                continue;
            }
            const int risk = opponentReplyThreatRisk(state, move, player);
            if (risk < bestRisk) {
                bestRisk = risk;
                missingBestRiskMoves = {move};
            } else if (risk == bestRisk) {
                addUniqueMove(missingBestRiskMoves, move);
            }
        }
        for (const Move& move : missingBestRiskMoves) {
            candidates.push_back(buildCandidateMove(state, move, player));
        }
        if (!missingBestRiskMoves.empty()) {
            risks.clear();
            risks.reserve(candidates.size());
            bestRisk = std::numeric_limits<int>::max();
            for (const CandidateMove& candidate : candidates) {
                const int risk = opponentReplyThreatRisk(state, candidate.move, player);
                risks.push_back(risk);
                bestRisk = std::min(bestRisk, risk);
            }
        }

        if (bestRisk > threatSeverity(ThreatType::Two)) {
            return {};
        }

        const Player opponent = otherPlayer(player);
        const bool preserveTail = state.hasThreatAtLeast(opponent, ThreatType::SimpleFour);
        std::vector<Move> safeReplyMoves;
        safeReplyMoves.reserve(candidates.size());
        if (!preserveTail) {
            std::vector<CandidateMove> filtered;
            filtered.reserve(candidates.size());
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                if (risks[index] == bestRisk
                    || threatSeverity(candidates[index].threatInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
                    addUniqueMove(safeReplyMoves, candidates[index].move);
                    filtered.push_back(candidates[index]);
                }
            }
            if (!filtered.empty()) {
                candidates = std::move(filtered);
            }
            return safeReplyMoves;
        }

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const bool immediateCounter =
                threatSeverity(candidates[index].threatInfo.best) >= threatSeverity(ThreatType::Five);
            if (risks[index] == bestRisk
                || immediateCounter) {
                addUniqueMove(safeReplyMoves, candidates[index].move);
            }
        }
        std::vector<CandidateMove> filtered;
        filtered.reserve(safeReplyMoves.size());
        for (const CandidateMove& candidate : candidates) {
            if (containsMove(safeReplyMoves, candidate.move)) {
                filtered.push_back(candidate);
            }
        }
        if (!filtered.empty()) {
            candidates = std::move(filtered);
        }
        return safeReplyMoves;
    }

    bool shouldRunStrictDefenseSearch(const GameState& state, Player defender) const {
        if (!config_.useDefensiveFiltering || !config_.useStrictDefenseFiltering || state.isGameOver()) {
            return false;
        }
        const Player attacker = otherPlayer(defender);
        return state.hasThreatAtLeast(attacker, ThreatType::SimpleFour);
    }

    ThreatSequenceConfig strictDefenseThreatConfig() const {
        ThreatSequenceConfig threatConfig;
        threatConfig.maxDepth = std::max(2, std::min(4, config_.maxDepth));
        threatConfig.maxNodes = config_.maxNodes > 0
            ? std::clamp<std::uint64_t>(config_.maxNodes / 64, 500, 4000)
            : 2000;
        threatConfig.timeLimitMs = hardTimeLimitMs() > 0 ? std::max(1, hardTimeLimitMs() / 64) : 0;
        threatConfig.maxThreatMoves = std::clamp<std::size_t>(config_.maxCandidateMoves / 2, 4, 12);
        threatConfig.minimumThreat = ThreatType::OpenThree;
        return threatConfig;
    }

    NodeTacticalState& cachedStrictDefenseSet(const GameState& state, Player defender) {
        NodeTacticalState& entry = nodeTacticalStateCache_[state.positionHash()];
        if (entry.opponentThreatSearched) {
            return entry;
        }

        entry.opponentThreatSearched = true;
        if (!shouldRunStrictDefenseSearch(state, defender)) {
            return entry;
        }

        const Player attacker = otherPlayer(defender);
        GameState attackerTurn = state;
        attackerTurn.setSideToMoveForAnalysis(attacker);

        ThreatSequenceSearcher searcher(strictDefenseThreatConfig());
        const ThreatSearchResult threat = searcher.searchWinningSequence(attackerTurn, attacker);
        if (!threat.foundWin || threat.sequence.empty()) {
            return entry;
        }

        for (const ThreatStep& step : threat.sequence) {
            addUniqueMove(entry.defenseSet, step.move);
            for (const Move& move : step.defenseMoves) {
                addUniqueMove(entry.defenseSet, move);
            }
            for (const Move& move : step.requiredEmpty) {
                addUniqueMove(entry.defenseSet, move);
            }
        }
        for (const Move& move : threat.refutations) {
            addUniqueMove(entry.defenseSet, move);
        }

        for (const Move& move : state.legalMoves()) {
            const MoveThreatInfo info = state.threatInfoAt(move, defender);
            if (threatSeverity(info.best) >= threatSeverity(ThreatType::SimpleFour)
                && isDefensiveCounterMove(info, false)) {
                addUniqueMove(entry.defenseSet, move);
            }
        }

        entry.opponentThreatFound = !entry.defenseSet.empty();
        return entry;
    }

    std::vector<CandidateMove> strictDefenseCandidates(const GameState& state, Player player) {
        std::vector<CandidateMove> candidates;
        NodeTacticalState& defenseSet = cachedStrictDefenseSet(state, player);
        if (!defenseSet.opponentThreatFound) {
            return candidates;
        }

        candidates.reserve(defenseSet.defenseSet.size());
        for (const Move& move : defenseSet.defenseSet) {
            if (state.isLegalMove(move)) {
                candidates.push_back(buildCandidateMove(state, move, player));
            }
        }
        return candidates;
    }

    static bool containsCandidateMove(const std::vector<CandidateMove>& candidates, Move move) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const CandidateMove& candidate) {
            return candidate.move == move;
        });
    }

    static std::vector<CandidateMove> mergePreferredCandidates(std::vector<CandidateMove> preferred,
                                                               const std::vector<CandidateMove>& tail) {
        for (const CandidateMove& candidate : tail) {
            if (!containsCandidateMove(preferred, candidate.move)) {
                preferred.push_back(candidate);
            }
        }
        return preferred;
    }

    CandidateStage chooseCandidateStage(const GameState& state, int ply) const {
        const Player player = state.sideToMove();
        const Player opponent = otherPlayer(player);
        if (config_.useDefensiveFiltering && state.hasThreatAtLeast(opponent, ThreatType::SimpleFour)) {
            return CandidateStage::DefendSimpleFour;
        }
        if (ply < 2 && state.moveCount() < 5) {
            return CandidateStage::OpeningLarge;
        }
        if (state.hasThreatAtLeast(player, ThreatType::OpenThree)) {
            return ply == 0 ? CandidateStage::ForcingRoot : CandidateStage::ForcingChild;
        }
        return CandidateStage::Default;
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

    bool hasNoisyTacticalThreat(const GameState& state) const {
        if (state.isGameOver() || state.isSwapDecisionPending()) {
            return false;
        }
        const Player side = state.sideToMove();
        const Player opponent = otherPlayer(side);
        return state.hasThreatAtLeast(side, ThreatType::OpenThree)
            || state.hasThreatAtLeast(opponent, ThreatType::OpenThree);
    }

    std::uint64_t quiescenceNodeLimit() const {
        std::uint64_t limit = 4'000;
        if (hardTimeLimitMs() > 0) {
            limit += static_cast<std::uint64_t>(hardTimeLimitMs()) * 8U;
        }
        if (config_.maxNodes > 0) {
            limit = std::min<std::uint64_t>(limit,
                std::max<std::uint64_t>(1'000, config_.maxNodes / 4));
        }
        return std::clamp<std::uint64_t>(limit, 1'000, 80'000);
    }

    static int quiescenceOrderingScore(const CandidateMove& candidate, const MoveThreatInfo& blockInfo) {
        int score = candidate.score;
        if (candidate.threatInfo.best == ThreatType::Five) {
            score += 20'000'000;
        }
        if (blockInfo.best == ThreatType::Five) {
            score += 12'000'000;
        }
        if (threatSeverity(candidate.threatInfo.best) >= threatSeverity(ThreatType::OpenFour)) {
            score += 3'000'000;
        }
        if (threatSeverity(blockInfo.best) >= threatSeverity(ThreatType::OpenFour)) {
            score += 2'500'000;
        }
        if (threatSeverity(candidate.threatInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
            score += 1'000'000;
        }
        if (threatSeverity(blockInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
            score += 900'000;
        }
        if (threatSeverity(candidate.threatInfo.best) >= threatSeverity(ThreatType::OpenThree)) {
            score += 250'000;
        }
        if (threatSeverity(blockInfo.best) >= threatSeverity(ThreatType::OpenThree)) {
            score += 225'000;
        }
        return score;
    }

    std::vector<CandidateMove> generateQuiescenceCandidates(const GameState& state, Player player) const {
        const Player opponent = otherPlayer(player);
        const bool mustRespond = state.hasThreatAtLeast(opponent, ThreatType::OpenThree);
        const bool opponentFourOnBoard = state.hasThreatAtLeast(opponent, ThreatType::SimpleFour);
        std::vector<CandidateMove> candidates;
        const std::vector<Move> moves = collectNeighborhoodMoves(state, 2);
        candidates.reserve(moves.size());

        for (const Move& move : moves) {
            const MoveThreatInfo attackInfo = state.threatInfoAt(move, player);
            const MoveThreatInfo blockInfo = state.threatInfoAt(move, opponent);
            const bool createsTactical =
                threatSeverity(attackInfo.best) >= threatSeverity(ThreatType::OpenThree);
            const bool blocksTactical =
                threatSeverity(blockInfo.best) >= threatSeverity(ThreatType::OpenThree);
            const bool counterThreat = isDefensiveCounterMove(attackInfo, opponentFourOnBoard);

            if (mustRespond) {
                if (!blocksTactical && !counterThreat) {
                    continue;
                }
            } else if (!createsTactical && !blocksTactical) {
                continue;
            }

            candidates.push_back(buildCandidateMove(state, move, player));
        }

        std::sort(candidates.begin(), candidates.end(), [&](const CandidateMove& left, const CandidateMove& right) {
            const MoveThreatInfo leftBlock = state.threatInfoAt(left.move, opponent);
            const MoveThreatInfo rightBlock = state.threatInfoAt(right.move, opponent);
            return quiescenceOrderingScore(left, leftBlock) > quiescenceOrderingScore(right, rightBlock);
        });

        const std::size_t maxMoves = mustRespond ? std::max<std::size_t>(kQuiescenceMaxMoves, 12) : kQuiescenceMaxMoves;
        if (candidates.size() > maxMoves) {
            candidates.resize(maxMoves);
        }
        return candidates;
    }

    int quiescence(GameState& state, int alpha, int beta, int ply, int quiescencePly) {
        maxPlyVisited_ = std::max(maxPlyVisited_, ply);
        if (quiescencePly > 0) {
            ++nodes_;
        }
        ++quiescenceNodes_;
        if (shouldStop()) {
            return 0;
        }
        if (state.isGameOver()) {
            return terminalScore(state, ply);
        }

        const int standPat = StaticEvaluator::evaluate(state, state.sideToMove());
        if (!hasNoisyTacticalThreat(state)
            || ply >= kMaxSearchPly
            || quiescencePly >= kQuiescenceMaxPly
            || quiescenceNodes_ >= quiescenceNodeLimit()) {
            return standPat;
        }

        const Player player = state.sideToMove();
        const bool mustRespond = state.hasThreatAtLeast(otherPlayer(player), ThreatType::OpenThree);
        int bestScore = standPat;
        if (!mustRespond) {
            if (standPat >= beta) {
                return standPat;
            }
            alpha = std::max(alpha, standPat);
        } else {
            bestScore = -kInfinity;
        }

        const std::vector<CandidateMove> candidates = generateQuiescenceCandidates(state, player);
        if (candidates.empty()) {
            return standPat;
        }

        for (const CandidateMove& candidate : candidates) {
            if (!state.applyMove(candidate.move)) {
                continue;
            }
            const int score = -quiescence(state, -beta, -alpha, ply + 1, quiescencePly + 1);
            state.undo();
            if (!completedDepth_) {
                return 0;
            }
            if (score > bestScore) {
                bestScore = score;
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                break;
            }
        }

        return bestScore == -kInfinity ? standPat : bestScore;
    }

    int historyIndex(Move move) const {
        return move.row * 16 + move.col;
    }

    void recordCutoffMove(Move move, int depth, int ply, std::optional<Move> previousMove) {
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

        if (previousMove.has_value()) {
            const int previousIndex = historyIndex(*previousMove);
            if (previousIndex >= 0 && previousIndex < static_cast<int>(counterMoves_.size())) {
                counterMoves_[static_cast<std::size_t>(previousIndex)] = move;
            }
        }
    }

    bool firstRootIteration() const {
        return rootIterationDepth_ <= 1;
    }

    int extensionBudgetForRootIteration() const {
        return rootIterationDepth_ > 0 ? 3 : 0;
    }

    bool opponentHasBoundedThreatSequenceForNullGuard(const GameState& state, Player defender, int depth) {
        NodeTacticalState& entry = nodeTacticalStateCache_[state.positionHash()];
        if (entry.opponentThreatFound) {
            return true;
        }
        const int threatDepth = std::clamp(depth, 2, 4);
        if (entry.nullGuardSearched
            && (entry.nullGuardFoundThreatSequence || entry.nullGuardMaxDepthSearched >= threatDepth)) {
            return entry.nullGuardFoundThreatSequence;
        }

        entry.nullGuardSearched = true;
        entry.nullGuardMaxDepthSearched = std::max(entry.nullGuardMaxDepthSearched, threatDepth);
        const Player attacker = otherPlayer(defender);
        GameState attackerTurn = state;
        attackerTurn.setSideToMoveForAnalysis(attacker);

        ThreatSequenceConfig threatConfig;
        threatConfig.maxDepth = threatDepth;
        threatConfig.maxNodes = config_.maxNodes > 0
            ? std::clamp<std::uint64_t>(config_.maxNodes / 128, 300, 2000)
            : 1000;
        threatConfig.timeLimitMs = hardTimeLimitMs() > 0 ? std::max(1, hardTimeLimitMs() / 96) : 0;
        threatConfig.maxThreatMoves = std::clamp<std::size_t>(config_.maxCandidateMoves / 3, 4, 10);
        threatConfig.minimumThreat = ThreatType::BrokenThree;

        ThreatSequenceSearcher searcher(threatConfig);
        const ThreatSearchResult threat = searcher.searchWinningSequence(attackerTurn, attacker);
        entry.nullGuardFoundThreatSequence = entry.nullGuardFoundThreatSequence || threat.foundWin;
        return entry.nullGuardFoundThreatSequence;
    }

    bool shouldTryNullMove(GameState& state, int depth, int beta, int ply, bool allowNullMove, int staticEval) {
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

        if (opponentHasBoundedThreatSequenceForNullGuard(state, side, depth)) {
            return false;
        }

        return true;
    }

    bool shouldVerifyWinningScore(int score,
                                  int childDepth,
                                  ThreatType attackThreat,
                                  ThreatType blockThreat,
                                  bool allowWinVerification,
                                  bool cautiousVerification) const {
        if (!allowWinVerification || cautiousVerification || !config_.useWinVerificationResearch
            || !completedDepth_ || firstRootIteration()) {
            return false;
        }
        if (score < kMateThreshold || childDepth <= 0) {
            return false;
        }
        return threatSeverity(attackThreat) >= threatSeverity(ThreatType::OpenThree)
            || threatSeverity(blockThreat) >= threatSeverity(ThreatType::OpenThree);
    }

    int verifyWinningScore(GameState& child,
                           int childDepth,
                           int alpha,
                           int beta,
                           int ply,
                           int originalScore) {
        ++winVerificationCount_;
        const std::uint64_t nodesBefore = nodes_;
        const int verifyScore = -negamax(child, childDepth, -beta, -alpha, ply,
            false, false, true);
        winVerificationNodes_ += nodes_ - nodesBefore;
        if (!completedDepth_) {
            return originalScore;
        }
        return verifyScore;
    }

    unsigned effectiveRootThreadCount(std::size_t candidateCount, int depth) const {
        if (depth < 3 || candidateCount < 3) {
            return 1;
        }
        if (config_.maxNodes > 0 && config_.maxNodes < 250'000) {
            return 1;
        }
        unsigned desired = config_.maxRootThreads > 0
            ? static_cast<unsigned>(config_.maxRootThreads)
            : std::thread::hardware_concurrency();
        if (desired == 0) {
            desired = 1;
        }
        desired = std::min<unsigned>(desired, 8U);
        desired = std::min<unsigned>(desired, static_cast<unsigned>(candidateCount - 1));
        return std::max(1U, desired);
    }

    RootWorkerResult evaluateRootMoveParallel(const GameState& rootState,
                                              Move move,
                                              int childDepth,
                                              int alpha,
                                              int beta,
                                              ThreatType attackThreat,
                                              ThreatType blockThreat) const {
        RootWorkerResult result;
        GameState child = rootState;
        if (!child.applyMove(move)) {
            return result;
        }

        SearchRunner worker(config_, tt_, budget_);
        worker.startTime_ = startTime_;
        worker.rootIterationDepth_ = rootIterationDepth_;
        worker.killerMoves_ = killerMoves_;
        worker.historyScores_ = historyScores_;
        worker.counterMoves_ = counterMoves_;
        worker.defensiveBlockingMoves_ = defensiveBlockingMoves_;

        result.searched = true;
        result.score = -worker.negamax(child, childDepth, -beta, -alpha, 1, true, true, false);
        if (worker.shouldVerifyWinningScore(result.score, childDepth, attackThreat, blockThreat, true, false)) {
            result.score = worker.verifyWinningScore(child, childDepth, alpha, beta, 1, result.score);
        }
        result.completed = worker.completedDepth_;
        result.nodes = worker.nodes_;
        result.ttHits = worker.ttHits_;
        result.vcfNodes = worker.vcfProbeNodes_;
        result.winVerificationNodes = worker.winVerificationNodes_;
        result.quiescenceNodes = worker.quiescenceNodes_;
        result.vcfHits = worker.vcfHits_;
        result.winVerifications = worker.winVerificationCount_;
        result.maxPlyVisited = worker.maxPlyVisited_;
        return result;
    }

    void absorbRootWorkerStats(const RootWorkerResult& worker) {
        nodes_ += worker.nodes;
        ttHits_ += worker.ttHits;
        vcfProbeNodes_ += worker.vcfNodes;
        winVerificationNodes_ += worker.winVerificationNodes;
        vcfHits_ += worker.vcfHits;
        winVerificationCount_ += worker.winVerifications;
        quiescenceNodes_ += worker.quiescenceNodes;
        maxPlyVisited_ = std::max(maxPlyVisited_, worker.maxPlyVisited);
    }

    void recordRootDefFilterTelemetry(const std::vector<CandidateMove>& before,
                                      const std::vector<CandidateMove>& after,
                                      bool applied,
                                      const std::string& reason) {
        rootCandidateCountBeforeDefFilter_ = static_cast<int>(before.size());
        rootCandidateCountAfterDefFilter_ = static_cast<int>(after.size());
        rootDefFilterApplied_ = applied;
        rootDefFilterReason_ = applied ? reason : "none";
        rootMovesBeforeDefFilter_ = candidateMoveList(before);
        rootMovesAfterDefFilter_ = candidateMoveList(after);
        rootMovesRemovedByDefFilter_ = applied ? removedCandidateMoves(before, after) : std::vector<Move> {};
    }

    std::vector<CandidateMove> generateOrderedCandidates(const GameState& state, int ply, std::optional<Move> preferredMove = std::nullopt) {
        const Player player = state.sideToMove();
        const CandidateStage stage = chooseCandidateStage(state, ply);
        const std::size_t budget = candidateBudget(state, ply);
        const int rootDefaultRadius = (ply == 0 && budget >= 64) ? 3 : 2;
        const bool rootStage = ply == 0;
        const bool rootMayUseDefFilter = rootStage
            && config_.useDefensiveFiltering
            && state.hasThreatAtLeast(otherPlayer(player), ThreatType::SimpleFour);
        std::vector<CandidateMove> rootCandidatesBeforeDefFilter;
        if (rootMayUseDefFilter) {
            rootCandidatesBeforeDefFilter = generateDefaultStageCandidates(state, player, budget, rootDefaultRadius);
        }

        std::vector<CandidateMove> candidates = strictDefenseCandidates(state, player);
        bool defFilterApplied = rootStage && !candidates.empty();
        std::string defFilterReason = defFilterApplied ? "strict_simple_four" : "none";
        std::vector<Move> preferredDefensiveMoves;
        if (candidates.empty()) {
            switch (stage) {
                case CandidateStage::DefendSimpleFour:
                    candidates = generateDefendStageCandidates(state, player, true, std::max<std::size_t>(budget, 16));
                    if (rootStage && !candidates.empty()) {
                        defFilterApplied = true;
                        defFilterReason = "simple_four";
                    }
                    break;
                case CandidateStage::ForcingRoot:
                    candidates = generateForcingStageCandidates(state, player, true, std::max<std::size_t>(budget, 16));
                    break;
                case CandidateStage::ForcingChild:
                    candidates = generateForcingStageCandidates(state, player, false, std::max<std::size_t>(budget, 12));
                    break;
                case CandidateStage::OpeningLarge:
                    candidates = generateOpeningLargeCandidates(state, player, std::max<std::size_t>(budget, 24));
                    break;
                case CandidateStage::Default:
                default:
                    if (rootMayUseDefFilter && !rootCandidatesBeforeDefFilter.empty()) {
                        candidates = rootCandidatesBeforeDefFilter;
                    } else {
                        candidates = generateDefaultStageCandidates(state, player, budget, rootDefaultRadius);
                    }
                    break;
            }
        }
        if (stage == CandidateStage::DefendSimpleFour && !candidates.empty()) {
            preferredDefensiveMoves = candidateMoveList(candidates);
            std::vector<CandidateMove> tail;
            if (rootMayUseDefFilter && !rootCandidatesBeforeDefFilter.empty()) {
                tail = rootCandidatesBeforeDefFilter;
            } else {
                tail = generateDefaultStageCandidates(state, player, budget, rootDefaultRadius);
            }
            candidates = mergePreferredCandidates(std::move(candidates), tail);
        }
        if (candidates.empty() && stage != CandidateStage::Default) {
            // Staged tactical generators are intentionally selective, but
            // they must never strand the search with no legal move on a
            // non-terminal position. Fall back to the generic candidate
            // set if a specialized stage rejects everything.
            if (rootMayUseDefFilter && !rootCandidatesBeforeDefFilter.empty()) {
                candidates = rootCandidatesBeforeDefFilter;
            } else {
                candidates = generateDefaultStageCandidates(state, player, budget, rootDefaultRadius);
            }
            defFilterApplied = false;
            defFilterReason = "none";
        }
        if (rootStage) {
            const std::vector<CandidateMove>& before = rootMayUseDefFilter
                ? rootCandidatesBeforeDefFilter
                : candidates;
            recordRootDefFilterTelemetry(before, candidates, defFilterApplied, defFilterReason);
        }
        if (candidates.empty()) {
            return candidates;
        }
        std::vector<Move> rootReplySafeMoves;
        if (ply == 0) {
            rootReplySafeMoves = applyRootReplySafetyFilter(state, player, candidates);
        }

        std::optional<Move> ttBestMove;
        if (const auto found = tt_.probe(state.positionHash()); found.has_value()) {
            ttBestMove = found->bestMove;
        }

        const std::optional<Move> firstKiller = ply < static_cast<int>(killerMoves_.size()) ? killerMoves_[static_cast<std::size_t>(ply)][0] : std::nullopt;
        const std::optional<Move> secondKiller = ply < static_cast<int>(killerMoves_.size()) ? killerMoves_[static_cast<std::size_t>(ply)][1] : std::nullopt;

        // Counter-move: the best refuting reply seen earlier against this
        // opponent's last move. Useful ordering nudge for quiet positions.
        std::optional<Move> counterMove;
        if (const auto previousMove = state.lastPlacedMove(); previousMove.has_value()) {
            const int previousIndex = historyIndex(*previousMove);
            if (previousIndex >= 0 && previousIndex < static_cast<int>(counterMoves_.size())) {
                counterMove = counterMoves_[static_cast<std::size_t>(previousIndex)];
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](const CandidateMove& left, const CandidateMove& right) {
            const bool leftSafeReply = containsMove(rootReplySafeMoves, left.move);
            const bool rightSafeReply = containsMove(rootReplySafeMoves, right.move);
            if (leftSafeReply != rightSafeReply) {
                return leftSafeReply;
            }

            const bool leftPreferred = preferredMove.has_value() && left.move == *preferredMove;
            const bool rightPreferred = preferredMove.has_value() && right.move == *preferredMove;
            if (leftPreferred != rightPreferred) {
                return leftPreferred;
            }

            const bool leftPreferredDefense = containsMove(preferredDefensiveMoves, left.move);
            const bool rightPreferredDefense = containsMove(preferredDefensiveMoves, right.move);
            if (leftPreferredDefense != rightPreferredDefense) {
                return leftPreferredDefense;
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

            const int leftThreat = threatSeverityEnhanced(left.threatInfo);
            const int rightThreat = threatSeverityEnhanced(right.threatInfo);
            if (leftThreat != rightThreat) {
                return leftThreat > rightThreat;
            }

            const bool leftCounter = counterMove.has_value() && left.move == *counterMove;
            const bool rightCounter = counterMove.has_value() && right.move == *counterMove;
            if (leftCounter != rightCounter) {
                return leftCounter;
            }

            const int leftHistory = historyScores_[static_cast<std::size_t>(historyIndex(left.move))];
            const int rightHistory = historyScores_[static_cast<std::size_t>(historyIndex(right.move))];
            if (leftHistory != rightHistory) {
                return leftHistory > rightHistory;
            }

            const bool leftDefBlock = std::find(defensiveBlockingMoves_.begin(), defensiveBlockingMoves_.end(), left.move) != defensiveBlockingMoves_.end();
            const bool rightDefBlock = std::find(defensiveBlockingMoves_.begin(), defensiveBlockingMoves_.end(), right.move) != defensiveBlockingMoves_.end();
            if (leftDefBlock != rightDefBlock) {
                return leftDefBlock;
            }

            return left.score > right.score;
        });

        return candidates;
    }

    RootSearchResult searchRoot(GameState& state, int depth, int alpha, int beta, const std::optional<Move>& preferredMove) {
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

        // Forced-four defense extension at the root (see negamax for detail).
        // Check-style attack extension also applies at root.
        const Player rootOpponent = otherPlayer(state.sideToMove());
        const bool rootExtensionAllowed = depth < (rootIterationDepth_ + extensionBudgetForRootIteration());
        const int forcedDefenseExtension = (rootExtensionAllowed
            && state.hasThreatAtLeast(rootOpponent, ThreatType::SimpleFour)) ? 1 : 0;

        auto childDepthFor = [&](ThreatType attackThreat) {
            int checkExtension = 0;
            if (rootExtensionAllowed
                && threatSeverity(attackThreat) >= threatSeverity(ThreatType::SimpleFour)) {
                checkExtension = 1;
            }
            return depth - 1 + forcedDefenseExtension + checkExtension;
        };

        const unsigned rootThreads = effectiveRootThreadCount(candidates.size(), depth);
        const bool parallelRoot = rootThreads > 1;

        std::size_t index = 0;
        if (!parallelRoot || candidates.empty()) {
            index = 0;
        }

        if (!candidates.empty()) {
            const ThreatType attackThreat = candidates[index].threatInfo.best;
            const ThreatType blockThreat = state.threatInfoAt(candidates[index].move, rootOpponent).best;
            if (!state.applyMove(candidates[index].move)) {
                ++index;
            } else {
                const int childDepth = childDepthFor(attackThreat);

                searchedAnyChild = true;
                int score = -negamax(state, childDepth, -beta, -alpha, 1, true, true, false);
                if (shouldVerifyWinningScore(score, childDepth, attackThreat, blockThreat, true, false)) {
                    score = verifyWinningScore(state, childDepth, alpha, beta, 1, score);
                }
                state.undo();

                if (!completedDepth_) {
                    return result;
                }

                bestScore = score;
                bestMove = candidates[index].move;
                alpha = std::max(alpha, score);
                ++index;
            }
        }

        if (completedDepth_ && alpha < beta && parallelRoot && index < candidates.size()) {
            const int parallelAlpha = alpha;
            std::vector<RootWorkerResult> workerResults(candidates.size());
            std::atomic<std::size_t> nextIndex {index};
            std::vector<std::future<void>> workers;
            workers.reserve(rootThreads);

            for (unsigned workerIndex = 0; workerIndex < rootThreads; ++workerIndex) {
                workers.push_back(std::async(std::launch::async, [&]() {
                    while (true) {
                        const std::size_t current = nextIndex.fetch_add(1);
                        if (current >= candidates.size()) {
                            break;
                        }
                        const ThreatType attackThreat = candidates[current].threatInfo.best;
                        const ThreatType blockThreat = state.threatInfoAt(candidates[current].move, rootOpponent).best;
                        const int childDepth = childDepthFor(attackThreat);
                        workerResults[current] = evaluateRootMoveParallel(
                            state, candidates[current].move, childDepth, parallelAlpha, beta, attackThreat, blockThreat);
                    }
                }));
            }

            for (auto& worker : workers) {
                worker.get();
            }

            for (; index < candidates.size(); ++index) {
                const RootWorkerResult& worker = workerResults[index];
                if (!worker.searched) {
                    continue;
                }
                searchedAnyChild = true;
                absorbRootWorkerStats(worker);
                if (!worker.completed) {
                    completedDepth_ = false;
                    return result;
                }
                if (!bestMove.has_value() || worker.score > bestScore) {
                    bestScore = worker.score;
                    bestMove = candidates[index].move;
                }
                alpha = std::max(alpha, worker.score);
            }
        } else {
            for (; index < candidates.size(); ++index) {
                const ThreatType attackThreat = candidates[index].threatInfo.best;
                const ThreatType blockThreat = state.threatInfoAt(candidates[index].move, rootOpponent).best;
                if (!state.applyMove(candidates[index].move)) {
                    continue;
                }

                const int childDepth = childDepthFor(attackThreat);

                searchedAnyChild = true;
                int score = -negamax(state, childDepth, -alpha - 1, -alpha, 1, true, true, false);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(state, childDepth, -beta, -alpha, 1, true, true, false);
                }
                if (shouldVerifyWinningScore(score, childDepth, attackThreat, blockThreat, true, false)) {
                    score = verifyWinningScore(state, childDepth, alpha, beta, 1, score);
                }
                state.undo();

                if (!completedDepth_) {
                    return result;
                }

                if (!bestMove.has_value() || score > bestScore) {
                    bestScore = score;
                    bestMove = candidates[index].move;
                }

                alpha = std::max(alpha, score);
                if (alpha >= beta) {
                    recordCutoffMove(candidates[index].move, depth, 0, state.lastPlacedMove());
                    break;
                }
            }
        }

        if (alpha >= beta && bestMove.has_value()) {
            recordCutoffMove(*bestMove, depth, 0, state.lastPlacedMove());
        }

        if (!searchedAnyChild || !bestMove.has_value()) {
            result.score = StaticEvaluator::evaluate(state, state.sideToMove());
            return result;
        }

        result.bestMove = bestMove;
        result.score = bestScore;

        BoundType rootBound = BoundType::Exact;
        if (bestScore <= originalAlpha) {
            rootBound = BoundType::Upper;
        } else if (bestScore >= beta) {
            rootBound = BoundType::Lower;
        }
        tt_.store(state.positionHash(), depth, scoreToTT(bestScore, 0), rootBound, bestMove);
        return result;
    }

    int searchChild(GameState& child,
                    int depth,
                    int alpha,
                    int beta,
                    int ply,
                    std::size_t index,
                    ThreatType attackThreat,
                    ThreatType blockThreat,
                    int extension,
                    bool checkExtensionAllowed,
                    bool allowWinVerification,
                    bool cautiousVerification) {
        if (checkExtensionAllowed
            && threatSeverity(attackThreat) >= threatSeverity(ThreatType::SimpleFour)) {
            extension += 1;
        }
        const int childDepth = depth - 1 + extension;
        const bool childAllowNullMove = !cautiousVerification;
        if (index == 0) {
            int score = -negamax(child, childDepth, -beta, -alpha, ply + 1,
                childAllowNullMove, allowWinVerification, cautiousVerification);
            if (shouldVerifyWinningScore(score, childDepth, attackThreat, blockThreat,
                    allowWinVerification, cautiousVerification)) {
                score = verifyWinningScore(child, childDepth, alpha, beta, ply + 1, score);
            }
            return score;
        }

        // Tactical moves (creating or blocking a forcing threat) are not reduced.
        const bool isTactical =
            threatSeverity(attackThreat) >= threatSeverity(ThreatType::OpenThree)
            || threatSeverity(blockThreat) >= threatSeverity(ThreatType::OpenThree);

        constexpr int kLmrDepthThreshold = 3;
        constexpr int kLmrMoveThreshold = 2;
        const bool isPvNode = (beta - alpha) > 1;
        const std::size_t depthIdx = static_cast<std::size_t>(std::min(depth, kLmrTableSize - 1));
        const std::size_t moveIdx = static_cast<std::size_t>(std::min<std::size_t>(index, kLmrTableSize - 1));
        int reduction = kLmrTable.data[isPvNode ? 1U : 0U][depthIdx][moveIdx];
        // Keep the reduced search at a useful depth (>= 1).
        reduction = std::min(reduction, depth - 2);
        const bool tryLmr = !cautiousVerification
            && !isTactical
            && depth >= kLmrDepthThreshold
            && index >= kLmrMoveThreshold
            && reduction > 0;

        if (tryLmr) {
            const int lmrDepth = childDepth - reduction;
            int score = -negamax(child, lmrDepth, -alpha - 1, -alpha, ply + 1,
                childAllowNullMove, allowWinVerification, cautiousVerification);
            // On a fail-high, verify at full depth before accepting.
            if (completedDepth_ && score > alpha) {
                score = -negamax(child, childDepth, -alpha - 1, -alpha, ply + 1,
                    childAllowNullMove, allowWinVerification, cautiousVerification);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(child, childDepth, -beta, -alpha, ply + 1,
                        childAllowNullMove, allowWinVerification, cautiousVerification);
                }
            }
            if (shouldVerifyWinningScore(score, childDepth, attackThreat, blockThreat,
                    allowWinVerification, cautiousVerification)) {
                score = verifyWinningScore(child, childDepth, alpha, beta, ply + 1, score);
            }
            return score;
        }

        int score = -negamax(child, childDepth, -alpha - 1, -alpha, ply + 1,
            childAllowNullMove, allowWinVerification, cautiousVerification);
        if (completedDepth_ && score > alpha && score < beta) {
            score = -negamax(child, childDepth, -beta, -alpha, ply + 1,
                childAllowNullMove, allowWinVerification, cautiousVerification);
        }
        if (shouldVerifyWinningScore(score, childDepth, attackThreat, blockThreat,
                allowWinVerification, cautiousVerification)) {
            score = verifyWinningScore(child, childDepth, alpha, beta, ply + 1, score);
        }
        return score;
    }

    int negamax(GameState& state,
                int depth,
                int alpha,
                int beta,
                int ply,
                bool allowNullMove,
                bool allowWinVerification,
                bool cautiousVerification) {
        maxPlyVisited_ = std::max(maxPlyVisited_, ply);
        ++nodes_;
        if (shouldStop()) {
            return 0;
        }

        // Hard ply cap — protects the stack against extension chains or
        // pathological forcing sequences that would recurse without bound.
        if (ply >= kMaxSearchPly) {
            return StaticEvaluator::evaluate(state, state.sideToMove());
        }

        if (state.isGameOver()) {
            return terminalScore(state, ply);
        }

        const std::uint64_t key = state.positionHash();
        if (!cautiousVerification) {
            if (const auto found = tt_.probe(key); found.has_value() && found->depth >= depth) {
                ++ttHits_;
                const int ttScore = scoreFromTT(found->score, ply);
                if (found->bound == BoundType::Exact) {
                    return ttScore;
                }
                if (found->bound == BoundType::Lower) {
                    alpha = std::max(alpha, ttScore);
                } else {
                    beta = std::min(beta, ttScore);
                }
                if (alpha >= beta) {
                    return ttScore;
                }
            }
        }

        if (depth == 0) {
            if (config_.useVcfAtLeaves && !firstRootIteration() && !state.isGameOver()) {
                const Player attacker = state.sideToMove();
                if (state.hasThreatAtLeast(attacker, ThreatType::BrokenThree)) {
                    VcfProbe probe;
                    probe.nodeBudget = config_.vcfNodeBudget;
                    GameState working = state;
                    if (probe.attack(working, attacker, config_.vcfMaxDepth)) {
                        vcfProbeNodes_ += probe.nodes;
                        ++vcfHits_;
                        return kMateScore - ply;
                    }
                    vcfProbeNodes_ += probe.nodes;
                }
            }
            if (config_.useQuiescenceSearch && hasNoisyTacticalThreat(state)) {
                return quiescence(state, alpha, beta, ply, 0);
            }
            return StaticEvaluator::evaluate(state, state.sideToMove());
        }

        const int staticEval = StaticEvaluator::evaluate(state, state.sideToMove());

        // Razoring and reverse (extended) futility pruning — only at shallow
        // non-PV nodes, never close to a mate score, and never when we're
        // forced to defend a threat (static eval would lie).
        const bool isPvNode = (beta - alpha) > 1;
        const bool nearMate = std::abs(alpha) >= kMateThreshold || std::abs(beta) >= kMateThreshold;
        const Player opponent = otherPlayer(state.sideToMove());
        const bool underForcingThreat = state.hasThreatAtLeast(opponent, ThreatType::OpenThree);
        if (!cautiousVerification && !isPvNode && !nearMate && !underForcingThreat && ply > 0) {
            constexpr int kFutilityMarginPerDepth = 45;
            // Reverse futility: if static eval already beats beta by a fat
            // margin, trust it and return without searching.
            if (depth < 7 && staticEval < kMateThreshold
                && staticEval - kFutilityMarginPerDepth * depth >= beta) {
                return staticEval;
            }
            // Razoring: if static eval is far below alpha at very shallow
            // depth, drop to a depth-0 probe. If that stays below alpha,
            // the position is hopeless and we return the probe score.
            if (depth < 5 && alpha > -kMateThreshold
                && staticEval + kFutilityMarginPerDepth * depth < alpha) {
                const int razored = negamax(state, 0, alpha, beta, ply,
                    allowNullMove, allowWinVerification, cautiousVerification);
                if (!completedDepth_) {
                    return 0;
                }
                if (razored < alpha) {
                    return razored;
                }
            }
        }

        if (shouldTryNullMove(state, depth, beta, ply, allowNullMove, staticEval)) {
            GameState nullState = state;
            nullState.setSideToMoveForAnalysis(otherPlayer(state.sideToMove()));
            const int reduction = std::min(depth - 1, std::max(2, depth / 2 + 1));
            const int nullScore = -negamax(nullState, depth - 1 - reduction, -beta, -beta + 1, ply + 1,
                false, false, cautiousVerification);
            if (!completedDepth_) {
                return 0;
            }
            if (nullScore >= beta) {
                if (nullScore >= kMateThreshold) {
                    // Do not trust mate scores from null-move: the "free move"
                    // given to the opponent means mate-distance is unreliable.
                    // Verify with a shallow re-search instead.
                    const int verifyDepth = std::max(1, depth - 1 - reduction);
                    const int verifyScore = -negamax(state, verifyDepth, -beta, -beta + 1, ply,
                        false, false, true);
                    if (completedDepth_ && verifyScore >= beta) {
                        return verifyScore;
                    }
                } else {
                    return nullScore;
                }
            }
        }

        // Internal iterative deepening: at deep PV nodes with no usable TT
        // move, run a shallow search first so generateOrderedCandidates has
        // a real ordering seed from the TT when the full-depth search starts.
        if (depth >= 7 && beta - alpha > 1) {
            bool haveTtMove = false;
            if (const auto found = tt_.probe(key); found.has_value() && found->bestMove.has_value()) {
                haveTtMove = true;
            }
            if (!haveTtMove) {
                negamax(state, depth / 2, alpha, beta, ply,
                    allowNullMove, allowWinVerification, cautiousVerification);
                if (!completedDepth_) {
                    return 0;
                }
            }
        }

        auto candidates = generateOrderedCandidates(state, ply);
        if (candidates.empty()) {
            return staticEval;
        }

        // Forced-four defense extension: if the opponent is threatening a
        // SimpleFour/OpenFour at this position, every child is either a
        // forced defense or an immediate loss — extend search by +1 so the
        // horizon doesn't fall inside the forced sequence.
        // Check-style attack extension: if the side to move created an
        // OpenThree+ threat with their last move (captured in the child's
        // attackThreat), the opponent is forced to respond — similarly
        // extend so the forced continuation isn't lost at the horizon.
        // Cap total extensions along a path so mutual forcing chains
        // can't push past the root iteration depth without bound.
        const bool extensionAllowed = (ply + depth) < (rootIterationDepth_ + extensionBudgetForRootIteration());
        const int forcedDefenseExtension = (extensionAllowed
            && state.hasThreatAtLeast(opponent, ThreatType::SimpleFour)) ? 1 : 0;

        const int originalAlpha = alpha;
        int bestScore = -kInfinity;
        std::optional<Move> bestMove;

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const ThreatType attackThreat = candidates[index].threatInfo.best;
            const ThreatType blockThreat = state.threatInfoAt(candidates[index].move, otherPlayer(state.sideToMove())).best;
            if (!state.applyMove(candidates[index].move)) {
                continue;
            }

            const int score = searchChild(state, depth, alpha, beta, ply, index,
                attackThreat, blockThreat, forcedDefenseExtension, extensionAllowed,
                allowWinVerification, cautiousVerification);
            state.undo();
            if (!completedDepth_) {
                return 0;
            }

            if (!bestMove.has_value() || score > bestScore) {
                bestScore = score;
                bestMove = candidates[index].move;
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                recordCutoffMove(candidates[index].move, depth, ply, state.lastPlacedMove());
                break;
            }
        }

        if (!bestMove.has_value()) {
            return staticEval;
        }

        BoundType nodeBound = BoundType::Exact;
        if (bestScore <= originalAlpha) {
            nodeBound = BoundType::Upper;
        } else if (bestScore >= beta) {
            nodeBound = BoundType::Lower;
        }
        tt_.store(key, depth, scoreToTT(bestScore, ply), nodeBound, bestMove);
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
            const auto found = tt_.probe(line.positionHash());
            if (!found.has_value() || !found->bestMove.has_value()) {
                break;
            }

            const Move move = *found->bestMove;
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

bool isDefensiveCounterMove(const MoveThreatInfo& info, bool opponentFourOnBoard) {
    if (opponentFourOnBoard) {
        return threatSeverity(info.best) >= threatSeverity(ThreatType::Five);
    }

    if (threatSeverity(info.best) >= threatSeverity(ThreatType::SimpleFour)) {
        return true;
    }

    // `threatSeverityEnhanced` is an ordering key, not a semantic counter
    // predicate. Keep this whitelist explicit so mixed threats do not enter
    // defensive move generation just because their ordering score is high.
    return (info.best == ThreatType::OpenThree && info.second == ThreatType::OpenThree)
        || (info.best == ThreatType::BrokenThree && info.second == ThreatType::BrokenThree);
}

std::vector<Move> vcfCandidateMovesForAnalysis(const GameState& state, Player attacker, bool childStage) {
    const std::vector<CandidateMove> candidates = childStage
        ? VcfProbe::generateForcingChildMoves(state, attacker)
        : VcfProbe::generateForcingRootMoves(state, attacker);
    std::vector<Move> moves;
    moves.reserve(candidates.size());
    for (const CandidateMove& candidate : candidates) {
        moves.push_back(candidate.move);
    }
    return moves;
}

SearchEngine::SearchEngine(SearchConfig config)
    : config_(config) {
}

SearchResult SearchEngine::search(const GameState& state) {
    std::lock_guard<std::mutex> lock(sharedSearchMutex());
    SearchConfig effective = config_;
    std::optional<MoveBudget> budget;

    // Global time governor. Engaged only when the caller supplied an
    // authoritative game clock; otherwise we preserve the caller's
    // per-turn scheduling untouched.
    if (effective.clock.hasGameClock()) {
        TimeGovernor governor;
        TimeGovernorConfig govCfg;  // defaults for now; tunable later
        if (effective.nextIterBranchingEstimate > 0.0) {
            govCfg.nextIterBranchingEstimate = effective.nextIterBranchingEstimate;
        }
        const ThreatAssessment assessment = assessRootThreats(state);
        budget = governor.computeBaselineBudget(effective.clock, govCfg, assessment);
        if (budget) {
            effective.timeLimitMs = static_cast<int>(budget->hardCapMs);
            effective.softTimeLimitMs = static_cast<int>(budget->targetMs);
        }
    }

    SearchRunner runner(effective, sharedTranspositionTable(), budget);
    SearchResult result = runner.run(state);

    if (effective.compareNoDefFilterSearch
        && effective.useDefensiveFiltering
        && result.summary.defFilterApplied
        && !state.isGameOver()
        && !state.isSwapDecisionPending())
    {
        SearchConfig diagnostic = effective;
        diagnostic.useDefensiveFiltering = false;
        diagnostic.useStrictDefenseFiltering = false;
        diagnostic.disableDefensiveFiltering = true;
        diagnostic.compareNoDefFilterSearch = false;
        diagnostic.progressCallback = {};

        TranspositionTable diagnosticTable(18);
        SearchRunner diagnosticRunner(diagnostic, diagnosticTable, budget);
        SearchResult diagnosticResult = diagnosticRunner.run(state);
        result.summary.noDefFilterBestMove = diagnosticResult.bestMove;
        if (diagnosticResult.bestMove.has_value()) {
            result.summary.noDefFilterBestMoveDiffers =
                !result.bestMove.has_value() || *diagnosticResult.bestMove != *result.bestMove;
            result.summary.noDefFilterBestMoveWasInBeforeDefFilter =
                containsMove(result.summary.rootMovesBeforeDefFilter, *diagnosticResult.bestMove);
            result.summary.filteredOutBestMoveFromWiderSearch =
                result.summary.noDefFilterBestMoveDiffers
                && result.summary.noDefFilterBestMoveWasInBeforeDefFilter
                && !containsMove(result.summary.rootMovesAfterDefFilter, *diagnosticResult.bestMove);
        }
    }

    return result;
}

}  // namespace gomoku
