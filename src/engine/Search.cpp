#include "gomoku/Search.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "gomoku/OpeningBook.hpp"
#include "gomoku/Threats.hpp"

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

struct VcfProbe {
    std::uint64_t nodeBudget {600};
    std::uint64_t nodes {0};

    static int chebyshev(Move a, Move b) {
        return std::max(std::abs(a.row - b.row), std::abs(a.col - b.col));
    }

    // Collect empty squares in the 9x9 neighborhood of `anchor` where `player`
    // playing would form an immediate Five.
    static std::vector<Move> fiveCompletions(const GameState& state, Player player, Move anchor) {
        std::vector<Move> completions;
        for (const Move& m : state.legalMoves()) {
            if (chebyshev(m, anchor) > 4) {
                continue;
            }
            if (state.threatInfoAt(m, player).best == ThreatType::Five) {
                completions.push_back(m);
            }
        }
        return completions;
    }

    // True if `player` has any move on the board that would form Five on its turn.
    static bool hasFiveCreation(const GameState& state, Player player) {
        for (const Move& m : state.legalMoves()) {
            if (state.threatInfoAt(m, player).best == ThreatType::Five) {
                return true;
            }
        }
        return false;
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

        std::vector<CandidateMove> candidates =
            StaticEvaluator::generateCandidateMoves(state, attacker, 32);
        if (candidates.empty()) {
            return false;
        }

        // Put Five-creating moves first, then OpenFour, then SimpleFour.
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const CandidateMove& left, const CandidateMove& right) {
                return threatSeverity(left.threatInfo.best) > threatSeverity(right.threatInfo.best);
            });

        for (const CandidateMove& candidate : candidates) {
            const ThreatType attackType = candidate.threatInfo.best;
            if (attackType != ThreatType::Five
                && attackType != ThreatType::OpenFour
                && attackType != ThreatType::SimpleFour) {
                break; // candidates are sorted; nothing forcing remains
            }

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
            // they just play it and win before our follow-up.
            if (hasFiveCreation(state, defender)) {
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

            // If the forced block also creates a Five-threat for defender, we
            // must defend instead of continuing the VCF attack — abort branch.
            if (hasFiveCreation(state, defender)) {
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
    explicit SearchRunner(SearchConfig config)
        : config_(config) {
    }

    SearchResult run(const GameState& state) {
        SearchResult result;
        startTime_ = Clock::now();
        historyScores_.assign(16U * 16U, 0);
        killerMoves_.assign(static_cast<std::size_t>(std::max(config_.maxDepth + 8, kMaxSearchPly)), {});
        counterMoves_.assign(16U * 16U, std::nullopt);

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

        if (shouldRunRootThreatSearch(state)) {
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
        for (int depth = 1; depth <= config_.maxDepth; ++depth) {
            if (shouldStop() || (depth > 1 && softLimitReached())) {
                break;
            }

            completedDepth_ = true;
            rootIterationDepth_ = depth;
            RootSearchResult iteration;

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
    std::uint64_t vcfProbeNodes_ {0};
    int vcfHits_ {0};
    int rootIterationDepth_ {0};
    bool completedDepth_ {true};
    std::unordered_map<std::uint64_t, TTEntry> tt_;
    std::vector<std::array<std::optional<Move>, 2>> killerMoves_;
    std::vector<int> historyScores_;
    std::vector<std::optional<Move>> counterMoves_;

    SearchResult finalizeResult(SearchResult result) const {
        result.summary.nodes = nodes_;
        result.summary.ttHits = ttHits_;
        result.summary.vcfNodes = vcfProbeNodes_;
        result.summary.vcfHits = vcfHits_;
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

    // When the opponent already has a forcing threat on the board, prune the
    // candidate list to moves that either neutralise the threat or create an
    // equally-fast counter-attack. Mirrors PentaZen's generate<DEFEND_B4>
    // and generate<DEFEND_F3> stages — large node reduction and also a
    // correctness win (we never waste tempo on quiet moves when forced).
    std::vector<CandidateMove> applyDefensiveFilter(const GameState& state, std::vector<CandidateMove> candidates) const {
        if (!config_.useDefensiveFiltering || candidates.size() <= 1) {
            return candidates;
        }

        const Player opponent = otherPlayer(state.sideToMove());
        if (!state.hasThreatAtLeast(opponent, ThreatType::OpenThree)) {
            return candidates;
        }

        // Classify the opponent's best on-board threat. Anything at or above
        // SimpleFour means the opponent is one ply from forming Five, so only
        // our own Five counts as a counter — a four of our own doesn't win the
        // tempo race. At OpenThree level, a counter-four forces the opponent
        // to abandon their extension and defend instead.
        const bool opponentFourOnBoard = state.hasThreatAtLeast(opponent, ThreatType::SimpleFour);
        const ThreatType counterThreshold = opponentFourOnBoard
            ? ThreatType::Five
            : ThreatType::SimpleFour;

        // Defender squares are the opponent's own extension points: cells
        // where *they* playing would jump to OpenFour/Five. Occupying those
        // cells denies the extension.
        std::vector<Move> defenderSquares;
        defenderSquares.reserve(candidates.size());
        for (const CandidateMove& candidate : candidates) {
            const ThreatType opponentThreatHere = state.threatInfoAt(candidate.move, opponent).best;
            if (threatSeverity(opponentThreatHere) >= threatSeverity(ThreatType::OpenFour)) {
                defenderSquares.push_back(candidate.move);
            }
        }

        std::vector<CandidateMove> filtered;
        filtered.reserve(candidates.size());
        for (const CandidateMove& candidate : candidates) {
            const bool defends = std::find(defenderSquares.begin(), defenderSquares.end(), candidate.move) != defenderSquares.end();
            const bool counters = threatSeverity(candidate.threatInfo.best) >= threatSeverity(counterThreshold);
            if (defends || counters) {
                filtered.push_back(candidate);
            }
        }

        // If nothing survives (defender squares pruned out by earlier filters
        // and no in-range counter), fall back to the full candidate list so
        // the search still has moves to consider.
        return filtered.empty() ? candidates : filtered;
    }

    std::vector<CandidateMove> generateOrderedCandidates(const GameState& state, int ply, std::optional<Move> preferredMove = std::nullopt) {
        auto candidates = StaticEvaluator::generateCandidateMoves(state, state.sideToMove(), candidateBudget(state));
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
        const Player rootOpponent = otherPlayer(state.sideToMove());
        constexpr int kRootExtensionBudget = 8;
        const bool rootExtensionAllowed = depth < (rootIterationDepth_ + kRootExtensionBudget);
        const int forcedDefenseExtension = (rootExtensionAllowed
            && state.hasThreatAtLeast(rootOpponent, ThreatType::SimpleFour)) ? 1 : 0;
        const int childDepth = depth - 1 + forcedDefenseExtension;

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (!state.applyMove(candidates[index].move)) {
                continue;
            }

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

    int searchChild(GameState& child,
                    int depth,
                    int alpha,
                    int beta,
                    int ply,
                    std::size_t index,
                    ThreatType attackThreat,
                    ThreatType blockThreat,
                    int extension) {
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
            if (config_.useVcfAtLeaves && !state.isGameOver()) {
                const Player attacker = state.sideToMove();
                // Cheap gate: only probe when there's already four-making potential.
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
                return nullScore;
            }
        }

        // Internal iterative deepening: at deep PV nodes with no usable TT
        // move, run a shallow search first so generateOrderedCandidates has
        // a real ordering seed from the TT when the full-depth search starts.
        if (depth >= 7 && beta - alpha > 1) {
            bool haveTtMove = false;
            if (const auto found = tt_.find(key); found != tt_.end() && found->second.bestMove.has_value()) {
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
        // horizon doesn't fall inside the forced sequence. Cap total
        // extensions along a path so mutual forced-four chains can't push
        // past the root iteration depth without bound.
        constexpr int kExtensionBudget = 8;
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

            const int score = searchChild(state, depth, alpha, beta, ply, index, attackThreat, blockThreat, forcedDefenseExtension);
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
