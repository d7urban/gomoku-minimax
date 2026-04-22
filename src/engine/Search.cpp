#include "gomoku/Search.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
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
        generation_ = 1;
        for (auto& cluster : clusters_) {
            for (auto& entry : cluster) {
                entry = TTEntry {};
            }
        }
    }

    void newGeneration() {
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
        }
    }

    TTEntry* find(std::uint64_t hash) {
        Cluster& cluster = clusterFor(hash);
        for (auto& entry : cluster) {
            if (entry.occupied && entry.hash == hash) {
                entry.generation = generation_;
                return &entry;
            }
        }
        return nullptr;
    }

    const TTEntry* find(std::uint64_t hash) const {
        const Cluster& cluster = clusterFor(hash);
        for (const auto& entry : cluster) {
            if (entry.occupied && entry.hash == hash) {
                return &entry;
            }
        }
        return nullptr;
    }

    void store(std::uint64_t hash, int depth, int score, BoundType bound, std::optional<Move> bestMove) {
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

enum class CandidateStage {
    Default,
    OpeningLarge,
    DefendSimpleFour,
    DefendOpenThree,
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

        const Player side = state.sideToMove();
        if (const std::vector<Move> winningMoves = collectImmediateWinningMoves(state, side); !winningMoves.empty()) {
            result.bestMove = winningMoves.front();
            result.summary.score = kMateScore;
            result.summary.depthReached = 1;
            result.summary.maxDepthVisited = 1;
            result.summary.rootCandidateCount = static_cast<int>(winningMoves.size());
            result.summary.completedLastDepth = true;
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
            result.summary.principalVariation = {opponentWinningMoves.front()};
            return finalizeResult(std::move(result));
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

            ThreatSequenceSearcher defSearcher(defConfig);
            ThreatSearchResult defResult = defSearcher.searchWinningSequence(state, opponent);
            result.summary.threatNodes += defResult.nodes;
            if (defResult.foundWin && !defResult.sequence.empty()) {
                defensiveBlockingMoves_.clear();
                for (const ThreatStep& step : defResult.sequence) {
                    defensiveBlockingMoves_.push_back(step.move);
                    for (const Move& defense : step.defenseMoves) {
                        defensiveBlockingMoves_.push_back(defense);
                    }
                }
            }
        }

        GameState rootState = state;
        auto rootMoves = generateOrderedCandidates(rootState, 0);
        result.summary.rootCandidateCount = static_cast<int>(rootMoves.size());
        if (rootMoves.empty()) {
            result.summary.score = StaticEvaluator::evaluate(rootState, rootState.sideToMove());
            return finalizeResult(std::move(result));
        }

        result.bestMove = rootMoves.front().move;
        result.summary.score = rootMoves.front().score;

        int previousScore = result.summary.score;
        int lastIterationCostMs = 0;
        for (int depth = 1; depth <= config_.maxDepth; ++depth) {
            if (shouldStop() || (depth > 1 && softLimitReached())) {
                break;
            }

            tt_.newGeneration();

            // ID affordability: skip starting the next iteration if the
            // predicted cost would push us past the hard cap with slack.
            // Only engaged when the governor is active (budget_.has_value)
            // and has given us a branching estimate.
            if (depth > 1 && lastIterationCostMs > 0 && budget_
                && budget_->nextIterBranchingEstimate > 0.0
                && budget_->hardCapMs > 0)
            {
                const std::int64_t predictedNext = static_cast<std::int64_t>(
                    static_cast<double>(lastIterationCostMs) * budget_->nextIterBranchingEstimate);
                const std::int64_t boundary = budget_->hardCapMs - budget_->finalizationSlackMs;
                if (static_cast<std::int64_t>(elapsedMs()) + predictedNext > boundary) {
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
            publishProgress(result.summary);

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
            lastIterationBestMove_ = iteration.bestMove;

            if (softLimitReached()) {
                break;
            }
        }

        return finalizeResult(std::move(result));
    }

private:
    SearchConfig config_ {};
    std::optional<MoveBudget> budget_ {};
    Clock::time_point startTime_ {};
    std::uint64_t nodes_ {0};
    std::uint64_t ttHits_ {0};
    std::uint64_t vcfProbeNodes_ {0};
    int vcfHits_ {0};
    int rootIterationDepth_ {0};
    int maxPlyVisited_ {0};
    bool completedDepth_ {true};
    // Best-move instability tracking for dynamic soft-limit modulation.
    // See effectiveSoftLimitMs — kept here rather than in the ID loop so
    // softLimitReached() can consult them.
    std::optional<Move> lastIterationBestMove_ {};
    int  stableIterationCount_ {0};
    bool bestMoveChangedLastIter_ {false};
    TranspositionTable& tt_;
    std::vector<std::array<std::optional<Move>, 2>> killerMoves_;
    std::vector<int> historyScores_;
    std::vector<std::optional<Move>> counterMoves_;
    std::vector<Move> defensiveBlockingMoves_;

    SearchResult finalizeResult(SearchResult result) const {
        result.summary.maxDepthVisited = std::max(result.summary.maxDepthVisited,
            std::max(result.summary.depthReached, maxPlyVisited_));
        result.summary.nodes = nodes_;
        result.summary.ttHits = ttHits_;
        result.summary.vcfNodes = vcfProbeNodes_;
        result.summary.vcfHits = vcfHits_;
        result.summary.elapsedMs = elapsedMs();
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
        progress.ttHits = ttHits_;
        progress.vcfNodes = vcfProbeNodes_;
        progress.vcfHits = vcfHits_;
        progress.elapsedMs = elapsedMs();
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
        if (bestMoveChangedLastIter_) {
            scale = budget_->bestMoveUnstableScale;
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

    bool shouldRunRootThreatSearch(const GameState& state) const {
        const Player side = state.sideToMove();
        const Player opponent = otherPlayer(side);
        return state.hasThreatAtLeast(side, ThreatType::OpenThree)
            || state.hasThreatAtLeast(opponent, ThreatType::OpenThree);
    }

    std::size_t candidateBudget(const GameState& state) const {
        const Player opponent = otherPlayer(state.sideToMove());
        if (state.hasThreatAtLeast(opponent, ThreatType::SimpleFour)) {
            return std::max<std::size_t>(config_.maxCandidateMoves, 32);
        }
        return std::max<std::size_t>(1, config_.maxCandidateMoves);
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

    CandidateMove buildCandidateMove(const GameState& state, Move move, Player player) const {
        CandidateMove candidate;
        candidate.move = move;
        candidate.threatInfo = state.threatInfoAt(move, player);
        candidate.score = candidate.threatInfo.totalScore
            + candidateCentralityScore(state, move)
            + candidateNeighborhoodPressure(state, move, player);

        const MoveThreatInfo defensiveInfo = state.threatInfoAt(move, otherPlayer(player));
        candidate.score += defensiveInfo.totalScore;
        if (threatSeverity(defensiveInfo.best) >= threatSeverity(ThreatType::SimpleFour)) {
            candidate.score += 500'000;
        } else if (threatSeverityEnhanced(defensiveInfo) >= 600) {
            candidate.score += 500'000;
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
                                                              std::size_t maxMoves) const {
        return scoreAndSortMoves(state, player, collectNeighborhoodMoves(state, 2), maxMoves);
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
            const bool defends = threatSeverity(opponentThreatHere.best) >= threatSeverity(ThreatType::OpenFour)
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

    CandidateStage chooseCandidateStage(const GameState& state, int ply) const {
        const Player player = state.sideToMove();
        const Player opponent = otherPlayer(player);
        if (config_.useDefensiveFiltering && state.hasThreatAtLeast(opponent, ThreatType::SimpleFour)) {
            return CandidateStage::DefendSimpleFour;
        }
        if (config_.useDefensiveFiltering && state.hasThreatAtLeast(opponent, ThreatType::OpenThree)) {
            return CandidateStage::DefendOpenThree;
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

    std::vector<CandidateMove> generateOrderedCandidates(const GameState& state, int ply, std::optional<Move> preferredMove = std::nullopt) {
        const Player player = state.sideToMove();
        const CandidateStage stage = chooseCandidateStage(state, ply);
        const std::size_t budget = candidateBudget(state);

        std::vector<CandidateMove> candidates;
        switch (stage) {
            case CandidateStage::DefendSimpleFour:
                candidates = generateDefendStageCandidates(state, player, true, std::max<std::size_t>(budget, 16));
                break;
            case CandidateStage::DefendOpenThree:
                candidates = generateDefendStageCandidates(state, player, false, std::max<std::size_t>(budget, 20));
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
                candidates = generateDefaultStageCandidates(state, player, budget);
                break;
        }
        if (candidates.empty()) {
            return candidates;
        }

        std::optional<Move> ttBestMove;
        if (const TTEntry* found = tt_.find(state.positionHash())) {
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
        constexpr int kRootExtensionBudget = 12;
        const bool rootExtensionAllowed = depth < (rootIterationDepth_ + kRootExtensionBudget);
        const int forcedDefenseExtension = (rootExtensionAllowed
            && state.hasThreatAtLeast(rootOpponent, ThreatType::SimpleFour)) ? 1 : 0;

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const ThreatType attackThreat = candidates[index].threatInfo.best;
            if (!state.applyMove(candidates[index].move)) {
                continue;
            }

            int checkExtension = 0;
            if (rootExtensionAllowed
                && threatSeverity(attackThreat) >= threatSeverity(ThreatType::SimpleFour)) {
                checkExtension = 1;
            }
            const int childDepth = depth - 1 + forcedDefenseExtension + checkExtension;

            searchedAnyChild = true;
            int score = 0;
            if (index == 0) {
                score = -negamax(state, childDepth, -beta, -alpha, 1, true);
            } else {
                score = -negamax(state, childDepth, -alpha - 1, -alpha, 1, true);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(state, childDepth, -beta, -alpha, 1, true);
                }
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
                    bool checkExtensionAllowed) {
        if (checkExtensionAllowed
            && threatSeverity(attackThreat) >= threatSeverity(ThreatType::SimpleFour)) {
            extension += 1;
        }
        const int childDepth = depth - 1 + extension;
        if (index == 0) {
            return -negamax(child, childDepth, -beta, -alpha, ply + 1, true);
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
        const bool tryLmr = !isTactical
            && depth >= kLmrDepthThreshold
            && index >= kLmrMoveThreshold
            && reduction > 0;

        if (tryLmr) {
            const int lmrDepth = childDepth - reduction;
            int score = -negamax(child, lmrDepth, -alpha - 1, -alpha, ply + 1, true);
            // On a fail-high, verify at full depth before accepting.
            if (completedDepth_ && score > alpha) {
                score = -negamax(child, childDepth, -alpha - 1, -alpha, ply + 1, true);
                if (completedDepth_ && score > alpha && score < beta) {
                    score = -negamax(child, childDepth, -beta, -alpha, ply + 1, true);
                }
            }
            return score;
        }

        int score = -negamax(child, childDepth, -alpha - 1, -alpha, ply + 1, true);
        if (completedDepth_ && score > alpha && score < beta) {
            score = -negamax(child, childDepth, -beta, -alpha, ply + 1, true);
        }
        return score;
    }

    int negamax(GameState& state, int depth, int alpha, int beta, int ply, bool allowNullMove) {
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
        if (const TTEntry* found = tt_.find(key); found != nullptr && found->depth >= depth) {
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

        if (depth == 0) {
            if (config_.useVcfAtLeaves && !state.isGameOver()) {
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
        if (!isPvNode && !nearMate && !underForcingThreat && ply > 0) {
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
                const int razored = negamax(state, 0, alpha, beta, ply, allowNullMove);
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
            const int reduction = depth >= 6 ? 3 : 2;
            const int nullScore = -negamax(nullState, depth - 1 - reduction, -beta, -beta + 1, ply + 1, false);
            if (!completedDepth_) {
                return 0;
            }
            if (nullScore >= beta) {
                if (nullScore >= kMateThreshold) {
                    // Do not trust mate scores from null-move: the "free move"
                    // given to the opponent means mate-distance is unreliable.
                    // Verify with a shallow re-search instead.
                    const int verifyDepth = std::max(1, depth - 1 - reduction);
                    const int verifyScore = -negamax(state, verifyDepth, -beta, -beta + 1, ply, true);
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
            if (const TTEntry* found = tt_.find(key); found != nullptr && found->bestMove.has_value()) {
                haveTtMove = true;
            }
            if (!haveTtMove) {
                negamax(state, depth / 2, alpha, beta, ply, allowNullMove);
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
        constexpr int kExtensionBudget = 12;
        const bool extensionAllowed = (ply + depth) < (rootIterationDepth_ + kExtensionBudget);
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

            const int score = searchChild(state, depth, alpha, beta, ply, index, attackThreat, blockThreat, forcedDefenseExtension, extensionAllowed);
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
            const TTEntry* found = tt_.find(line.positionHash());
            if (found == nullptr || !found->bestMove.has_value()) {
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

    return threatSeverity(info.best) >= threatSeverity(ThreatType::SimpleFour)
        || threatSeverityEnhanced(info) >= 600;
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
        const ThreatAssessment assessment = assessRootThreats(state);
        budget = governor.computeBaselineBudget(effective.clock, govCfg, assessment);
        if (budget) {
            effective.timeLimitMs = static_cast<int>(budget->hardCapMs);
            effective.softTimeLimitMs = static_cast<int>(budget->targetMs);
        }
    }

    SearchRunner runner(effective, sharedTranspositionTable(), std::move(budget));
    return runner.run(state);
}

}  // namespace gomoku
