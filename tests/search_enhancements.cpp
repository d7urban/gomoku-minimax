#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <initializer_list>

#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Search.hpp"
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

    // SimpleFour: forcing threats get a base of 700, above double-OpenThree (600).
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour)) == 700);
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::BrokenThree))
        == 700 + threatSeverity(ThreatType::BrokenThree) / 4);

    // A double-threat outranks the same best-threat with a weaker secondary.
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree))
        > threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::BrokenThree)));

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

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.depthReached >= 2);
    assert(result.summary.nodes > 0);
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
    // Depth must have advanced past the first iteration (otherwise the
    // extension/cap interaction would be hiding a hang or abort).
    assert(result.summary.depthReached >= 4);
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

}  // namespace

int main() {
    testThreatSeverityEnhanced();
    testSearchHandlesDeepQuietPosition();
    testSearchFindsOpenFourResponse();
    testShallowSearchFindsOpenFourMate();
    testVcfLeafDisabledLeavesOtherMatePathsAvailable();
    testForcingFilterPicksUniqueSimpleFourBlock();
    testForcedFourExtensionTerminates();
    testDefensiveFilterKeepsDoubleOpenThreeCounter();
    return 0;
}
