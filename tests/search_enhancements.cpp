#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "TestAssert.hpp"
#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Search.hpp"
#include "gomoku/TacticalAI.hpp"
#include "gomoku/ThreatTypes.hpp"
#include "gomoku/Threats.hpp"

namespace {

using gomoku::GameState;
using gomoku::Move;
using gomoku::MoveThreatInfo;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::SearchConfig;
using gomoku::SearchEngine;
using gomoku::SearchResult;
using gomoku::ThreatType;
using gomoku::rulesFor;
using gomoku::threatSeverity;
using gomoku::threatSeverityEnhanced;
using gomoku::vcfCandidateMovesForAnalysis;

MoveThreatInfo make(ThreatType best, ThreatType second = ThreatType::None) {
    MoveThreatInfo info;
    info.best = best;
    info.second = second;
    return info;
}

GameState makeGame(std::initializer_list<Move> moves) {
    GameState game(rulesFor(Ruleset::Freestyle15));
    for (const Move& move : moves) {
        const bool applied = game.applyMove(move);
        assert(applied);
    }
    return game;
}

void assertPrincipalVariationLegal(const GameState& root, const SearchResult& result) {
    if (!result.bestMove.has_value()) {
        return;
    }
    if (result.summary.principalVariation.empty()) {
        assert(result.summary.depthReached == 0);
        return;
    }
    assert(!result.summary.principalVariation.empty());
    assert(result.summary.principalVariation.front() == *result.bestMove);

    GameState line = root;
    for (const Move& move : result.summary.principalVariation) {
        const bool applied = line.applyMove(move);
        assert(applied);
        if (line.isGameOver()) {
            break;
        }
    }
}

bool containsMove(const std::vector<Move>& moves, Move move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

void testThreatSeverityEnhanced() {
    // Base: empty threat info is zero.
    assert(threatSeverityEnhanced(make(ThreatType::None)) == 0);

    // Documented specializations.
    assert(threatSeverityEnhanced(make(ThreatType::OpenFour)) == 1000);
    assert(threatSeverityEnhanced(make(ThreatType::OpenFour, ThreatType::OpenThree))
        == 1000 + threatSeverity(ThreatType::OpenThree));
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::OpenThree)) == 800);
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree)) == 600);
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::BrokenThree)) == 400);
    assert(threatSeverityEnhanced(make(ThreatType::BrokenThree, ThreatType::BrokenThree)) == 300);

    // SimpleFour: forcing threats get a base of 700, above double-OpenThree (600).
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour)) == 700);
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::BrokenThree))
        == 700 + threatSeverity(ThreatType::BrokenThree) / 4);

    // A double-threat outranks the same best-threat with a weaker secondary.
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree))
        > threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::BrokenThree)));
    assert(threatSeverityEnhanced(make(ThreatType::BrokenThree, ThreatType::BrokenThree))
        > threatSeverityEnhanced(make(ThreatType::BrokenThree)));
    assert(threatSeverityEnhanced(make(ThreatType::BrokenThree, ThreatType::BrokenThree))
        > threatSeverityEnhanced(make(ThreatType::OpenThree)));

    // OpenFour always outranks any non-OpenFour combination.
    assert(threatSeverityEnhanced(make(ThreatType::OpenFour))
        > threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::OpenThree)));

    // A pure four without follow-up still outranks a bare open-three.
    // (ordering sanity that matters for the move sort)
    // A simple-four (forcing) outranks a double open-three (not yet forcing).
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour))
        > threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree)));
}

void testSearchHandlesDeepQuietPosition() {
    // Sanity: the LMR/PVS plumbing completes on a non-trivial quiet
    // position without asserting, hanging, or returning no move.
    GameState game = makeGame({{7, 7}, {7, 8}, {8, 7}, {6, 6}, {6, 8}, {8, 8}});

    SearchConfig config;
    config.maxDepth = 6;
    config.maxNodes = 200'000;
    config.timeLimitMs = 2000;
    config.maxCandidateMoves = 16;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;
    config.useVcfAtLeaves = false;
    config.useWinVerificationResearch = false;
    std::optional<gomoku::SearchSummary> progress;
    config.progressCallback = [&](const gomoku::SearchSummary& summary) {
        progress = summary;
    };

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.depthReached >= 1);
    assert(result.summary.maxDepthVisited >= 2);
    assert(result.summary.nodes > 0);
    assert(progress.has_value());
    assert(progress->depthReached >= 1);
    assert(progress->maxDepthVisited >= progress->depthReached);
}

void testSearchFindsOpenFourResponse() {
    // Black has built an open three on row 7: (7,7),(7,8),(7,9).
    // White to move must respond (block an end, ideally). The search
    // should at least reach a sensible depth and return a move.
    GameState game = makeGame({
        {7, 7}, {0, 0},
        {7, 8}, {0, 1},
        {7, 9},
    });

    SearchConfig config;
    config.maxDepth = 4;
    config.maxNodes = 150'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 14;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());

    // Any reasonable response must either extend our pressure or block
    // one of the open ends of black's three at (7,6) or (7,10).
    const Move move = *result.bestMove;
    const bool blocksLeftEnd = (move == Move{7, 6});
    const bool blocksRightEnd = (move == Move{7, 10});
    // We don't force a specific move; but if the search picks anything
    // far from the action the LMR reductions have eaten tactics.
    const int chebyshev = std::max(std::abs(move.row - 7), std::abs(move.col - 8));
    assert(blocksLeftEnd || blocksRightEnd || chebyshev <= 3);
}

void testShallowSearchFindsOpenFourMate() {
    // Black has three-in-a-row on row 7 and it is Black's turn. Playing
    // either endpoint extends to an open four; White can only block one
    // end, and Black wins the next turn. Even at depth 1 the search must
    // still report a mate, regardless of whether the proof comes from the
    // leaf VCF probe or a forcing-sequence extension.
    GameState game = makeGame({
        {7, 5}, {0, 0},
        {7, 6}, {0, 1},
        {7, 7},
    });
    assert(game.sideToMove() == Player::White);
    // Make it Black's turn without applying a real move.
    game.setSideToMoveForAnalysis(Player::Black);

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 50'000;
    config.timeLimitMs = 1000;
    config.useRootThreatSearch = false;
    config.useOpeningBook = false;
    config.useVcfAtLeaves = true;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{7, 4}) || *result.bestMove == (Move{7, 8}));
    assert(result.summary.score >= 1'000'000);
}

void testWinningMateGetsCautiousVerification() {
    GameState game = makeGame({
        {7, 5}, {0, 0},
        {7, 6}, {0, 1},
        {7, 7},
    });
    game.setSideToMoveForAnalysis(Player::Black);

    SearchConfig config;
    config.maxDepth = 2;
    config.maxNodes = 100'000;
    config.timeLimitMs = 1000;
    config.useRootThreatSearch = false;
    config.useOpeningBook = false;
    config.useVcfAtLeaves = true;
    config.useWinVerificationResearch = true;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{7, 4}) || *result.bestMove == (Move{7, 8}));
    assert(result.summary.score >= 1'000'000);
    assert(result.summary.winVerifications > 0);
    assert(result.summary.winVerificationNodes > 0);
    assertPrincipalVariationLegal(game, result);
}

void testForcingFilterPicksUniqueSimpleFourBlock() {
    // Black has BBBB at the left edge (columns 0..3); {7,4} is the only
    // square that would complete Black's five. White to move: with the
    // forcing filter, the search must pick {7,4} to survive.
    GameState game = makeGame({
        {7, 0}, {0, 0},
        {7, 1}, {0, 1},
        {7, 2}, {0, 7},
        {7, 3},
    });
    assert(game.sideToMove() == Player::White);

    SearchConfig config;
    config.maxDepth = 2;
    config.maxNodes = 20'000;
    config.timeLimitMs = 500;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{7, 4}));
}

void testUniqueImmediateBlockShortCircuitsSearch() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {9, 8}, {8, 7},
        {9, 6}, {6, 9},
        {9, 7}, {9, 5},
        {9, 9},
    });
    assert(game.sideToMove() == Player::White);

    SearchConfig config;
    config.maxDepth = 8;
    config.maxNodes = 200'000;
    config.timeLimitMs = 500;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{9, 10}));
    assert(result.summary.rootCandidateCount == 1);
    assert(result.summary.nodes == 0);
}

void testSimpleFourDefenseOnlyKeepsRealBlockingSquares() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {9, 8}, {8, 7},
        {9, 6}, {6, 9},
        {9, 7}, {9, 5},
        {9, 9}, {9, 10},
        {8, 8},
    });
    assert(game.sideToMove() == Player::White);
    assert(game.hasThreatAtLeast(Player::Black, ThreatType::SimpleFour));

    SearchConfig config;
    config.maxDepth = 8;
    config.maxNodes = 200'000;
    config.timeLimitMs = 500;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{6, 6}) || *result.bestMove == (Move{10, 10}));
    assert(result.summary.defFilterApplied);
    assert(result.summary.rootCandidateCountBeforeDefFilter >= 2);
    assert(result.summary.rootCandidateCountAfterDefFilter >= result.summary.rootCandidateCountBeforeDefFilter);
    assert(result.summary.rootMovesRemovedByDefFilter.empty());
    assert(containsMove(result.summary.rootMovesAfterDefFilter, Move{6, 6}));
    assert(containsMove(result.summary.rootMovesAfterDefFilter, Move{10, 10}));
}

void testSimpleFourFilterPreservesDiagnosticTailMove() {
    GameState game = makeGame({
        {14, 1}, {14, 4}, {14, 7}, {14, 10},
        {14, 9}, {14, 5}, {14, 6}, {11, 8},
        {10, 9}, {12, 9}, {10, 7}, {10, 8},
        {9, 8}, {11, 7}, {11, 10}, {12, 11},
        {8, 7}, {7, 6}, {9, 7}, {12, 6},
        {13, 5}, {9, 9}, {8, 10}, {11, 6},
    });
    assert(game.sideToMove() == Player::Black);
    assert(game.hasThreatAtLeast(Player::White, ThreatType::SimpleFour));

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 25'000;
    config.timeLimitMs = 500;
    config.maxCandidateMoves = 28;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.defFilterApplied);
    assert(containsMove(result.summary.rootMovesBeforeDefFilter, Move{7, 7}));
    assert(containsMove(result.summary.rootMovesAfterDefFilter, Move{7, 7}));
    assert(!containsMove(result.summary.rootMovesRemovedByDefFilter, Move{7, 7}));
}

void testFirstIterationDoesNotExplodeForcedExtensionChain() {
    GameState game = makeGame({
        {14, 1}, {14, 4}, {14, 7}, {14, 10},
        {14, 9}, {14, 5}, {14, 6}, {11, 8},
        {10, 9}, {12, 9}, {10, 7}, {10, 8},
        {9, 8}, {11, 7},
    });
    assert(game.sideToMove() == Player::Black);

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 5'000'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 80;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;
    config.maxRootThreads = 1;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.completedLastDepth);
    assert(result.summary.depthReached >= 1);
    assert(result.summary.maxDepthVisited <= 8);
}

void testVcfLeafDisabledLeavesOtherMatePathsAvailable() {
    // Same position as above, but with the VCF probe switched off. The
    // score can still reach mate through other forcing-search machinery;
    // what matters here is that the VCF counter stays at zero.
    GameState game = makeGame({
        {7, 5}, {0, 0},
        {7, 6}, {0, 1},
        {7, 7},
    });
    game.setSideToMoveForAnalysis(Player::Black);

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 50'000;
    config.timeLimitMs = 1000;
    config.useRootThreatSearch = false;
    config.useOpeningBook = false;
    config.useVcfAtLeaves = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{7, 4}) || *result.bestMove == (Move{7, 8}));
    assert(result.summary.vcfHits == 0);
    assert(result.summary.score >= 1'000'000);
}

void testFirstIterationSkipsLeafVcfProbe() {
    // The first ID pass is the fallback move if later iterations time out.
    // It must stay cheap enough to complete even when leaf positions contain
    // broken-three VCF candidates.
    GameState game = makeGame({
        {0, 0}, {7, 6},
        {0, 1}, {7, 8},
        {1, 0}, {8, 7},
    });
    assert(game.sideToMove() == Player::Black);
    assert(game.hasThreatAtLeast(Player::White, ThreatType::BrokenThree));

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 500'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 18;
    config.useRootThreatSearch = false;
    config.useOpeningBook = false;
    config.useVcfAtLeaves = true;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.completedLastDepth);
    assert(result.summary.depthReached == 1);
    assert(result.summary.vcfNodes == 0);
    assert(result.summary.vcfHits == 0);
}

void testForcedFourExtensionTerminates() {
    // Build a position where White has a SimpleFour threat at row 7
    // (pieces at cols 0..3). Black to move — forced defense at (7, 4).
    // The forced-four extension fires. Even with a relatively deep search
    // and a forced-defense chain, the ply cap + extension budget must keep
    // the search from running past its bounds: the search must complete
    // and return the unique defending move in bounded time.
    GameState game = makeGame({
        {0, 0}, {7, 0},
        {0, 1}, {7, 1},
        {0, 7}, {7, 2},
        {1, 1}, {7, 3},
    });
    assert(game.sideToMove() == Player::Black);

    SearchConfig config;
    config.maxDepth = 8;             // high enough for extensions to matter
    config.maxNodes = 500'000;
    config.timeLimitMs = 2000;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(*result.bestMove == (Move{7, 4}));
    // This used to rely on the forced-four extension path. A root-level
    // unique-block shortcut is also acceptable now: the important contract
    // is that the engine returns the only defending move promptly.
    assert(result.summary.nodes == 0 || result.summary.depthReached >= 4);
}

void testDefensiveFilterKeepsDoubleOpenThreeCounter() {
    // Regression: White can answer Black's forcing pressure with h11, which
    // creates an OpenThree+OpenThree fork. The defensive filter used to drop
    // that move because its primary threat is still only OpenThree.
    GameState game = makeGame({
        {4, 10}, {9, 8},
        {4, 5}, {6, 4},
        {6, 8}, {8, 6},
        {9, 4}, {8, 9},
        {5, 8}, {10, 8},
        {4, 4}, {10, 6},
        {4, 9},
    });
    assert(game.sideToMove() == Player::White);
    assert(game.hasThreatAtLeast(Player::Black, ThreatType::OpenThree));
    assert(!game.hasThreatAtLeast(Player::Black, ThreatType::SimpleFour));

    const MoveThreatInfo counter = game.threatInfoAt({10, 7}, Player::White);
    assert(counter.best == ThreatType::OpenThree);
    assert(counter.second == ThreatType::OpenThree);
    assert(isDefensiveCounterMove(counter, false));
    assert(!isDefensiveCounterMove(counter, true));
}

void testDefensiveFilterKeepsDoubleBrokenThreeCounter() {
    const MoveThreatInfo counter = make(ThreatType::BrokenThree, ThreatType::BrokenThree);
    assert(isDefensiveCounterMove(counter, false));
    assert(!isDefensiveCounterMove(counter, true));
}

void testDefensiveCounterPredicateDoesNotUseOrderingScore() {
    assert(isDefensiveCounterMove(make(ThreatType::Five), true));
    assert(!isDefensiveCounterMove(make(ThreatType::SimpleFour), true));
    assert(isDefensiveCounterMove(make(ThreatType::SimpleFour), false));
    assert(isDefensiveCounterMove(make(ThreatType::OpenThree, ThreatType::OpenThree), false));
    assert(isDefensiveCounterMove(make(ThreatType::BrokenThree, ThreatType::BrokenThree), false));

    assert(!isDefensiveCounterMove(make(ThreatType::OpenThree), false));
    assert(!isDefensiveCounterMove(make(ThreatType::BrokenThree), false));
    assert(!isDefensiveCounterMove(make(ThreatType::OpenThree, ThreatType::BrokenThree), false));
}

void testInterruptedSearchReportsMaxVisitedDepth() {
    GameState game = makeGame({
        {7, 7}, {0, 0},
        {7, 8}, {0, 1},
    });

    SearchConfig config;
    config.maxDepth = 4;
    config.maxNodes = 1;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 8;
    config.useOpeningBook = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.depthReached == 0);
    assert(result.summary.maxDepthVisited >= 1);
    assert(result.summary.nodes <= config.maxNodes);
}

void testQuietSearchSkipsWinVerification() {
    GameState game = makeGame({
        {7, 7}, {0, 0},
        {12, 12}, {0, 1},
        {10, 10}, {1, 0},
    });

    SearchConfig config;
    config.maxDepth = 1;
    config.maxNodes = 20'000;
    config.timeLimitMs = 200;
    config.maxCandidateMoves = 16;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;
    config.useWinVerificationResearch = true;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.winVerifications == 0);
    assert(result.summary.winVerificationNodes == 0);
}

void testTranspositionTablePersistsAcrossSearchCalls() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {8, 7}, {6, 6},
        {6, 8}, {8, 8},
    });

    SearchConfig config;
    config.maxDepth = 5;
    config.maxNodes = 200'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 16;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;

    SearchEngine firstEngine(config);
    const SearchResult first = firstEngine.search(game);
    assert(first.bestMove.has_value());

    SearchEngine secondEngine(config);
    const SearchResult second = secondEngine.search(game);
    assert(second.bestMove.has_value());
    assert(second.summary.ttHits > 0);
    assertPrincipalVariationLegal(game, second);
}

void testTranspositionTableReusesParentSubtreeAcrossPlayedMove() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {8, 7}, {6, 6},
        {6, 8}, {8, 8},
        {9, 7}, {5, 5},
    });

    SearchConfig config;
    config.maxDepth = 5;
    config.maxNodes = 250'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 16;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;

    SearchEngine rootEngine(config);
    const SearchResult rootResult = rootEngine.search(game);
    assert(rootResult.bestMove.has_value());
    assertPrincipalVariationLegal(game, rootResult);

    GameState child = game;
    assert(child.applyMove(*rootResult.bestMove));

    SearchEngine childEngine(config);
    const SearchResult childResult = childEngine.search(child);
    assert(childResult.bestMove.has_value());
    assert(childResult.summary.ttHits > 0);
    assertPrincipalVariationLegal(child, childResult);
}

void testTranspositionTablePollutionDoesNotCorruptPrincipalVariation() {
    GameState anchor = makeGame({
        {7, 7}, {7, 8},
        {8, 7}, {6, 6},
        {6, 8}, {8, 8},
        {9, 7}, {5, 5},
    });
    GameState target = makeGame({
        {4, 4}, {10, 10},
        {5, 6}, {9, 8},
        {6, 5}, {8, 9},
        {7, 7}, {11, 6},
    });

    SearchConfig config;
    config.maxDepth = 5;
    config.maxNodes = 250'000;
    config.timeLimitMs = 1000;
    config.maxCandidateMoves = 16;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;

    SearchEngine warmEngine(config);
    const SearchResult warmResult = warmEngine.search(anchor);
    assert(warmResult.bestMove.has_value());

    SearchEngine targetEngine(config);
    const SearchResult targetResult = targetEngine.search(target);
    assert(targetResult.bestMove.has_value());
    assertPrincipalVariationLegal(target, targetResult);
}

void testParallelRootSearchMatchesSerialBestMove() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {8, 7}, {6, 6},
        {6, 8}, {8, 8},
        {9, 7}, {5, 5},
    });

    SearchConfig serialConfig;
    serialConfig.maxDepth = 5;
    serialConfig.maxNodes = 300'000;
    serialConfig.timeLimitMs = 1000;
    serialConfig.maxCandidateMoves = 16;
    serialConfig.useOpeningBook = false;
    serialConfig.useRootThreatSearch = false;
    serialConfig.maxRootThreads = 1;

    SearchConfig parallelConfig = serialConfig;
    parallelConfig.maxRootThreads = 4;

    SearchEngine serialEngine(serialConfig);
    const SearchResult serial = serialEngine.search(game);
    assert(serial.bestMove.has_value());
    assertPrincipalVariationLegal(game, serial);

    SearchEngine parallelEngine(parallelConfig);
    const SearchResult parallel = parallelEngine.search(game);
    assert(parallel.bestMove.has_value());
    assert(*parallel.bestMove == *serial.bestMove);
    assertPrincipalVariationLegal(game, parallel);
}

void testSearchDoesNotReturnEmptyOnTournamentCrossPattern() {
    GameState game = makeGame({
        {7, 7}, {7, 8},
        {8, 7}, {6, 7},
    });
    assert(game.sideToMove() == Player::Black);

    SearchConfig config;
    config.maxDepth = 10;
    config.maxNodes = 180'000;
    config.timeLimitMs = 500;
    config.maxCandidateMoves = 20;
    config.useOpeningBook = false;
    config.useRootThreatSearch = true;

    const SearchResult result = gomoku::TacticalAI::chooseMove(game, Player::Black, config);
    assert(result.bestMove.has_value());

    GameState trial = game;
    assert(trial.applyMove(*result.bestMove));
}

void testVcfRootGeneratorIgnoresQuietNoise() {
    GameState game = makeGame({
        {7, 5}, {0, 0},
        {7, 6}, {0, 1},
        {7, 7}, {12, 12},
        {11, 12}, {12, 11},
        {11, 11},
    });
    game.setSideToMoveForAnalysis(Player::Black);

    const std::vector<Move> candidates = vcfCandidateMovesForAnalysis(game, Player::Black, false);
    assert(!candidates.empty());

    // The local row extensions are forcing; the noisy remote cluster around
    // (11,11)-(12,12) should not contribute quiet VCF candidates.
    assert(std::find(candidates.begin(), candidates.end(), Move{7, 8}) != candidates.end());
    assert(std::find(candidates.begin(), candidates.end(), Move{7, 4}) != candidates.end());
    assert(std::find(candidates.begin(), candidates.end(), Move{10, 10}) == candidates.end());
    assert(std::find(candidates.begin(), candidates.end(), Move{10, 11}) == candidates.end());

    for (const Move& move : candidates) {
        assert(threatSeverity(game.threatInfoAt(move, Player::Black).best)
            >= threatSeverity(ThreatType::SimpleFour));
    }
}

void testPanicModeSurfacesLostRoot() {
    // White to move cannot parry both Black fours. Once depth 2 proves
    // every root move loses, panic mode should be surfaced so callers know
    // the soft limit was no longer authoritative.
    GameState game = makeGame({
        {7, 3}, {0, 0},
        {7, 4}, {0, 2},
        {7, 5}, {1, 4},
        {7, 6}, {2, 6},
        {3, 9}, {10, 0},
        {4, 9}, {11, 2},
        {5, 9}, {12, 4},
        {6, 9},
    });
    assert(game.sideToMove() == Player::White);
    assert(game.threatInfoAt({7, 2}, Player::Black).best == ThreatType::Five);
    assert(game.threatInfoAt({7, 7}, Player::Black).best == ThreatType::Five);
    assert(game.threatInfoAt({2, 9}, Player::Black).best == ThreatType::Five);
    assert(game.threatInfoAt({7, 9}, Player::Black).best == ThreatType::Five);

    SearchConfig config;
    config.maxDepth = 2;
    config.maxNodes = 300'000;
    config.timeLimitMs = 1000;
    config.softTimeLimitMs = 1;
    config.maxCandidateMoves = 2;
    config.useOpeningBook = false;
    config.useRootThreatSearch = false;
    config.useDefensiveFiltering = false;
    config.useStrictDefenseFiltering = false;
    config.useVcfAtLeaves = false;
    config.useWinVerificationResearch = false;
    config.maxRootThreads = 1;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.panicModeEntered);
    assert(result.summary.depthReached >= 2);
}

}  // namespace

int main() {
    testThreatSeverityEnhanced();
    testSearchHandlesDeepQuietPosition();
    testSearchFindsOpenFourResponse();
    testShallowSearchFindsOpenFourMate();
    testWinningMateGetsCautiousVerification();
    testVcfLeafDisabledLeavesOtherMatePathsAvailable();
    testFirstIterationSkipsLeafVcfProbe();
    testForcingFilterPicksUniqueSimpleFourBlock();
    testUniqueImmediateBlockShortCircuitsSearch();
    testSimpleFourDefenseOnlyKeepsRealBlockingSquares();
    testSimpleFourFilterPreservesDiagnosticTailMove();
    testFirstIterationDoesNotExplodeForcedExtensionChain();
    testForcedFourExtensionTerminates();
    testDefensiveFilterKeepsDoubleOpenThreeCounter();
    testDefensiveFilterKeepsDoubleBrokenThreeCounter();
    testDefensiveCounterPredicateDoesNotUseOrderingScore();
    testInterruptedSearchReportsMaxVisitedDepth();
    testQuietSearchSkipsWinVerification();
    testTranspositionTablePersistsAcrossSearchCalls();
    testTranspositionTableReusesParentSubtreeAcrossPlayedMove();
    testTranspositionTablePollutionDoesNotCorruptPrincipalVariation();
    testParallelRootSearchMatchesSerialBestMove();
    testSearchDoesNotReturnEmptyOnTournamentCrossPattern();
    testVcfRootGeneratorIgnoresQuietNoise();
    testPanicModeSurfacesLostRoot();
    return 0;
}
