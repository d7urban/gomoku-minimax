#include "gomoku/ProofSearch.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>

namespace gomoku {

namespace {

constexpr std::uint64_t kProofInfinity = std::numeric_limits<std::uint64_t>::max() / 4;

using Clock = std::chrono::steady_clock;

bool isWinningResultFor(GameResult result, Player player) {
    return (player == Player::Black && result == GameResult::BlackWin)
        || (player == Player::White && result == GameResult::WhiteWin);
}

void addUniqueMove(std::vector<Move>& moves, Move move) {
    if (std::find(moves.begin(), moves.end(), move) == moves.end()) {
        moves.push_back(move);
    }
}

std::uint64_t cappedAdd(std::uint64_t left, std::uint64_t right) {
    if (left >= kProofInfinity || right >= kProofInfinity) {
        return kProofInfinity;
    }
    if (left > kProofInfinity - right) {
        return kProofInfinity;
    }
    return left + right;
}

struct NodeResult {
    ProofOutcome outcome {ProofOutcome::Unknown};
    std::uint64_t proofNumber {1};
    std::uint64_t disproofNumber {1};
    std::vector<Move> principalVariation;
    int maxDepthReached {0};
};

struct CandidateSet {
    std::vector<Move> moves;
    bool complete {false};
};

class ProofSearchRunner {
public:
    explicit ProofSearchRunner(ProofAnalysisConfig config)
        : config_(config) {
    }

    ProofAnalysisResult analyze(const GameState& state, Player attacker) {
        ProofAnalysisResult result;
        result.attacker = attacker;
        startTime_ = Clock::now();

        if (state.sideToMove() != attacker || state.isSwapDecisionPending()) {
            result.elapsedMs = elapsedMs();
            return result;
        }

        ThreatSequenceConfig threatConfig;
        threatConfig.maxDepth = std::max(2, config_.maxDepth);
        threatConfig.maxNodes = std::max<std::uint64_t>(1000, config_.maxNodes / 3);
        threatConfig.timeLimitMs = std::max(50, config_.timeLimitMs / 3);
        threatConfig.maxThreatMoves = config_.maxThreatMoves;

        ThreatSequenceSearcher threatSearcher(threatConfig);
        ThreatSearchResult threatResult = threatSearcher.searchWinningSequence(state, attacker);
        if (threatResult.foundWin && !threatResult.sequence.empty()) {
            result.threatSequence = threatResult;
        }

        CandidateSet rootCandidates = generateCandidates(state, attacker, true);
        if (result.threatSequence.has_value()) {
            const Move threatMove = result.threatSequence->sequence.front().move;
            const auto found = std::find(rootCandidates.moves.begin(), rootCandidates.moves.end(), threatMove);
            if (found == rootCandidates.moves.end()) {
                rootCandidates.moves.insert(rootCandidates.moves.begin(), threatMove);
            } else {
                std::rotate(rootCandidates.moves.begin(), found, std::next(found));
            }
        }
        const std::vector<Move>& rootMoves = rootCandidates.moves;
        if (rootMoves.empty()) {
            result.outcome = state.isGameOver() && !isWinningResultFor(state.result(), attacker) ? ProofOutcome::ProvenLoss : ProofOutcome::Unknown;
            result.nodes = nodes_ + threatResult.nodes;
            result.elapsedMs = elapsedMs();
            return result;
        }

        result.rootProofNumber = kProofInfinity;
        result.rootDisproofNumber = 0;

        bool anyUnknown = false;
        bool allLosses = true;
        bool searchedAnyChild = false;
        bool searchedAllChildren = true;
        NodeResult bestUnknown;
        std::optional<Move> bestUnknownMove;

        for (const Move& move : rootMoves) {
            if (shouldStop()) {
                searchedAllChildren = false;
                break;
            }

            GameState child = state;
            if (!child.applyMove(move)) {
                continue;
            }
            searchedAnyChild = true;

            const NodeResult childResult = solve(child, attacker, config_.maxDepth - 1, 1);

            ProofMoveSummary summary;
            summary.move = move;
            summary.threatType = StaticEvaluator::analyzeMove(state, move, attacker).best;
            summary.outcome = childResult.outcome;
            summary.proofNumber = childResult.proofNumber;
            summary.disproofNumber = childResult.disproofNumber;
            result.rootMoves.push_back(summary);

            result.maxDepthReached = std::max(result.maxDepthReached, childResult.maxDepthReached);
            result.rootProofNumber = std::min(result.rootProofNumber, childResult.proofNumber);
            result.rootDisproofNumber = cappedAdd(result.rootDisproofNumber, childResult.disproofNumber);

            if (childResult.outcome == ProofOutcome::ProvenWin) {
                result.outcome = ProofOutcome::ProvenWin;
                result.bestMove = move;
                result.principalVariation = {move};
                result.principalVariation.insert(result.principalVariation.end(), childResult.principalVariation.begin(), childResult.principalVariation.end());
                allLosses = false;
                break;
            }

            if (childResult.outcome != ProofOutcome::ProvenLoss) {
                allLosses = false;
                anyUnknown = true;
                if (!bestUnknownMove.has_value() || childResult.proofNumber < bestUnknown.proofNumber
                    || (childResult.proofNumber == bestUnknown.proofNumber && childResult.disproofNumber > bestUnknown.disproofNumber)) {
                    bestUnknown = childResult;
                    bestUnknownMove = move;
                }
            }
        }

        if (result.outcome != ProofOutcome::ProvenWin) {
            if (allLosses && searchedAnyChild && searchedAllChildren && rootCandidates.complete) {
                result.outcome = ProofOutcome::ProvenLoss;
                result.rootProofNumber = kProofInfinity;
                result.rootDisproofNumber = 0;
            } else {
                result.outcome = ProofOutcome::Unknown;
                if (bestUnknownMove.has_value()) {
                    result.bestMove = bestUnknownMove;
                    result.principalVariation = {*bestUnknownMove};
                    result.principalVariation.insert(result.principalVariation.end(), bestUnknown.principalVariation.begin(), bestUnknown.principalVariation.end());
                }
                if (result.rootProofNumber == kProofInfinity) {
                    result.rootProofNumber = 1;
                }
                if (result.rootDisproofNumber == 0) {
                    result.rootDisproofNumber = anyUnknown ? 1 : result.rootDisproofNumber;
                }
            }
        }

        std::sort(result.rootMoves.begin(), result.rootMoves.end(), [](const ProofMoveSummary& left, const ProofMoveSummary& right) {
            if (left.outcome != right.outcome) {
                return static_cast<int>(left.outcome) > static_cast<int>(right.outcome);
            }
            if (threatSeverity(left.threatType) != threatSeverity(right.threatType)) {
                return threatSeverity(left.threatType) > threatSeverity(right.threatType);
            }
            if (left.proofNumber != right.proofNumber) {
                return left.proofNumber < right.proofNumber;
            }
            if (left.move.row != right.move.row) {
                return left.move.row < right.move.row;
            }
            return left.move.col < right.move.col;
        });

        result.nodes = nodes_ + threatResult.nodes;
        result.elapsedMs = elapsedMs();
        return result;
    }

private:
    ProofAnalysisConfig config_ {};
    Clock::time_point startTime_ {};
    std::uint64_t nodes_ {0};

    int elapsedMs() const {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startTime_).count());
    }

    int remainingTimeMs() const {
        if (config_.timeLimitMs <= 0) {
            return 0;
        }
        return std::max(0, config_.timeLimitMs - elapsedMs());
    }

    bool shouldStop() const {
        if (config_.maxNodes > 0 && nodes_ >= config_.maxNodes) {
            return true;
        }
        return config_.timeLimitMs > 0 && elapsedMs() >= config_.timeLimitMs;
    }

    CandidateSet generateCandidates(const GameState& state, Player player, bool preferForcing) {
        CandidateSet candidates;
        std::vector<Move>& moves = candidates.moves;
        const std::vector<Move> legalMoves = state.legalMoves();

        if (!preferForcing) {
            for (const CandidateMove& candidate :
                StaticEvaluator::generateCandidateMoves(state, player, legalMoves.size())) {
                addUniqueMove(moves, candidate.move);
            }
            for (const Move& move : legalMoves) {
                addUniqueMove(moves, move);
            }
            candidates.complete = true;
            return candidates;
        }

        ThreatSequenceConfig threatConfig;
        threatConfig.maxDepth = 2;
        threatConfig.maxNodes = 5000;
        threatConfig.timeLimitMs = remainingTimeMs() > 0 ? std::max(5, remainingTimeMs() / 4) : 0;
        threatConfig.maxThreatMoves = config_.maxThreatMoves;

        ThreatSequenceSearcher threatSearcher(threatConfig);
        const ThreatEnumerationResult threatEnumeration = threatSearcher.enumerateThreatsWithStats(state, player);
        nodes_ += threatEnumeration.nodes;
        for (const ThreatStep& threat : threatEnumeration.threats) {
            addUniqueMove(moves, threat.move);
        }

        const std::size_t staticTarget = std::max(config_.maxCandidateMoves, config_.maxThreatMoves);
        for (const CandidateMove& candidate : StaticEvaluator::generateCandidateMoves(state, player, staticTarget * 2)) {
            if (!preferForcing || threatSeverity(candidate.threatInfo.best) >= threatSeverity(ThreatType::Two) || moves.empty()) {
                addUniqueMove(moves, candidate.move);
            }
            if (moves.size() >= staticTarget) {
                break;
            }
        }

        if (moves.empty()) {
            for (const Move& move : legalMoves) {
                addUniqueMove(moves, move);
                if (moves.size() >= config_.maxCandidateMoves) {
                    break;
                }
            }
        }

        if (moves.size() > config_.maxCandidateMoves) {
            moves.resize(config_.maxCandidateMoves);
        }
        candidates.complete = moves.size() == legalMoves.size();
        return candidates;
    }

    NodeResult heuristicLeaf(const GameState& state, Player attacker, int ply) const {
        NodeResult leaf;
        leaf.maxDepthReached = ply;

        if (isWinningResultFor(state.result(), attacker)) {
            leaf.outcome = ProofOutcome::ProvenWin;
            leaf.proofNumber = 0;
            leaf.disproofNumber = kProofInfinity;
            return leaf;
        }

        if (state.isGameOver()) {
            leaf.outcome = ProofOutcome::ProvenLoss;
            leaf.proofNumber = kProofInfinity;
            leaf.disproofNumber = 0;
            return leaf;
        }

        const int eval = StaticEvaluator::evaluate(state, attacker);
        const bool attackerThreat = state.canCreateThreatAtLeast(attacker, ThreatType::OpenThree);
        const bool defenderThreat = state.canCreateThreatAtLeast(otherPlayer(attacker), ThreatType::OpenThree);
        if (attackerThreat && !defenderThreat) {
            leaf.proofNumber = 1;
            leaf.disproofNumber = 3;
        } else if (defenderThreat && !attackerThreat) {
            leaf.proofNumber = 3;
            leaf.disproofNumber = 1;
        } else if (eval > 400) {
            leaf.proofNumber = 1;
            leaf.disproofNumber = 2;
        } else if (eval < -400) {
            leaf.proofNumber = 2;
            leaf.disproofNumber = 1;
        }
        return leaf;
    }

    NodeResult solve(const GameState& state, Player attacker, int depth, int ply) {
        ++nodes_;
        if (shouldStop() || depth < 0 || state.sideToMove() == Player::None || state.isSwapDecisionPending()) {
            return heuristicLeaf(state, attacker, ply);
        }

        if (isWinningResultFor(state.result(), attacker) || state.isGameOver() || depth == 0) {
            return heuristicLeaf(state, attacker, ply);
        }

        const bool attackerTurn = state.sideToMove() == attacker;
        const CandidateSet generated = generateCandidates(state, state.sideToMove(), attackerTurn);
        if (generated.moves.empty()) {
            return heuristicLeaf(state, attacker, ply);
        }
        const std::vector<Move>& candidates = generated.moves;

        NodeResult node;
        node.maxDepthReached = ply;

        if (attackerTurn) {
            node.proofNumber = kProofInfinity;
            node.disproofNumber = 0;

            NodeResult bestUnknown;
            std::optional<Move> bestUnknownMove;
            bool allLosses = true;
            bool searchedAnyChild = false;
            bool searchedAllChildren = true;
            for (const Move& move : candidates) {
                if (shouldStop()) {
                    searchedAllChildren = false;
                    break;
                }

                GameState child = state;
                if (!child.applyMove(move)) {
                    continue;
                }
                searchedAnyChild = true;

                NodeResult childResult = solve(child, attacker, depth - 1, ply + 1);
                node.maxDepthReached = std::max(node.maxDepthReached, childResult.maxDepthReached);
                node.proofNumber = std::min(node.proofNumber, childResult.proofNumber);
                node.disproofNumber = cappedAdd(node.disproofNumber, childResult.disproofNumber);

                if (childResult.outcome == ProofOutcome::ProvenWin) {
                    node.outcome = ProofOutcome::ProvenWin;
                    node.proofNumber = 0;
                    node.disproofNumber = kProofInfinity;
                    node.principalVariation = {move};
                    node.principalVariation.insert(node.principalVariation.end(), childResult.principalVariation.begin(), childResult.principalVariation.end());
                    return node;
                }

                if (childResult.outcome != ProofOutcome::ProvenLoss) {
                    allLosses = false;
                    if (!bestUnknownMove.has_value() || childResult.proofNumber < bestUnknown.proofNumber
                        || (childResult.proofNumber == bestUnknown.proofNumber && childResult.disproofNumber > bestUnknown.disproofNumber)) {
                        bestUnknown = childResult;
                        bestUnknownMove = move;
                    }
                }
            }

            if (allLosses && searchedAnyChild && searchedAllChildren && generated.complete) {
                node.outcome = ProofOutcome::ProvenLoss;
                node.proofNumber = kProofInfinity;
                node.disproofNumber = 0;
                return node;
            }

            node.outcome = ProofOutcome::Unknown;
            if (bestUnknownMove.has_value()) {
                node.principalVariation = {*bestUnknownMove};
                node.principalVariation.insert(node.principalVariation.end(), bestUnknown.principalVariation.begin(), bestUnknown.principalVariation.end());
            }
            if (node.proofNumber == kProofInfinity) {
                node.proofNumber = 1;
            }
            if (node.disproofNumber == 0) {
                node.disproofNumber = 1;
            }
            return node;
        }

        node.proofNumber = 0;
        node.disproofNumber = kProofInfinity;
        bool allWins = true;
        bool searchedAnyChild = false;
        bool searchedAllChildren = true;

        for (const Move& move : candidates) {
            if (shouldStop()) {
                searchedAllChildren = false;
                break;
            }

            GameState child = state;
            if (!child.applyMove(move)) {
                continue;
            }
            searchedAnyChild = true;

            NodeResult childResult = solve(child, attacker, depth - 1, ply + 1);
            node.maxDepthReached = std::max(node.maxDepthReached, childResult.maxDepthReached);
            node.proofNumber = cappedAdd(node.proofNumber, childResult.proofNumber);
            node.disproofNumber = std::min(node.disproofNumber, childResult.disproofNumber);

            if (childResult.outcome == ProofOutcome::ProvenLoss) {
                node.outcome = ProofOutcome::ProvenLoss;
                node.proofNumber = kProofInfinity;
                node.disproofNumber = 0;
                node.principalVariation = {move};
                node.principalVariation.insert(node.principalVariation.end(), childResult.principalVariation.begin(), childResult.principalVariation.end());
                return node;
            }

            if (childResult.outcome != ProofOutcome::ProvenWin) {
                allWins = false;
                if (node.principalVariation.empty() || childResult.disproofNumber < node.disproofNumber) {
                    node.principalVariation = {move};
                    node.principalVariation.insert(node.principalVariation.end(), childResult.principalVariation.begin(), childResult.principalVariation.end());
                }
            }
        }

        if (allWins && searchedAnyChild && searchedAllChildren && generated.complete) {
            node.outcome = ProofOutcome::ProvenWin;
            node.proofNumber = 0;
            node.disproofNumber = kProofInfinity;
            return node;
        }

        node.outcome = ProofOutcome::Unknown;
        if (node.proofNumber == 0) {
            node.proofNumber = 1;
        }
        if (node.disproofNumber == kProofInfinity) {
            node.disproofNumber = 1;
        }
        return node;
    }
};

}  // namespace

std::string_view toString(ProofOutcome outcome) {
    switch (outcome) {
        case ProofOutcome::ProvenWin:
            return "proven_win";
        case ProofOutcome::ProvenLoss:
            return "proven_loss";
        case ProofOutcome::Unknown:
        default:
            return "unknown";
    }
}

ProofAnalyzer::ProofAnalyzer(ProofAnalysisConfig config)
    : config_(config) {
}

ProofAnalysisResult ProofAnalyzer::analyze(const GameState& state, Player attacker) const {
    ProofSearchRunner runner(config_);
    return runner.analyze(state, attacker);
}

}  // namespace gomoku
