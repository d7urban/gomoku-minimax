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

    // Fallback: baseScore + second/4.
    const int baseSimpleFour = threatSeverity(ThreatType::SimpleFour);
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour)) == baseSimpleFour);
    assert(threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::BrokenThree))
        == baseSimpleFour + threatSeverity(ThreatType::BrokenThree) / 4);

    // A double-threat outranks the same best-threat with a weaker secondary.
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree))
        > threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::BrokenThree)));

    // OpenFour always outranks any non-OpenFour combination.
    assert(threatSeverityEnhanced(make(ThreatType::OpenFour))
        > threatSeverityEnhanced(make(ThreatType::SimpleFour, ThreatType::OpenThree)));

    // A pure four without follow-up still outranks a bare open-three.
    // (ordering sanity that matters for the move sort)
    // NOTE: this is the *documented* weakness: SimpleFour+None returns a
    // small number; OpenThree+OpenThree returns 600. The function trades
    // absolute severity for combination awareness. This test pins that
    // behavior intentionally.
    assert(threatSeverityEnhanced(make(ThreatType::OpenThree, ThreatType::OpenThree))
        > threatSeverityEnhanced(make(ThreatType::SimpleFour)));
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

void testVcfLeafFindsOpenFourMate() {
    // Black has three-in-a-row on row 7 and it is Black's turn. Playing
    // {7, 4} makes an open four; White can only block one end, and Black
    // wins the next turn. The VCF-at-leaf probe must find this even with
    // a tiny main-search depth so the score reports a mate.
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
    config.useOpeningBook = false;
    config.useVcfAtLeaves = true;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.score >= 1'000'000);
    assert(result.summary.vcfHits >= 1);
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

void testVcfLeafDisabledFallsBackToEval() {
    // Same position as above, but with the VCF probe switched off: at
    // depth 1 without deeper search there is no mate score yet.
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
    config.useOpeningBook = false;
    config.useVcfAtLeaves = false;

    SearchEngine engine(config);
    const SearchResult result = engine.search(game);
    assert(result.bestMove.has_value());
    assert(result.summary.vcfHits == 0);
    assert(result.summary.score < 1'000'000);
}

}  // namespace

int main() {
    testThreatSeverityEnhanced();
    testSearchHandlesDeepQuietPosition();
    testSearchFindsOpenFourResponse();
    testVcfLeafFindsOpenFourMate();
    testVcfLeafDisabledFallsBackToEval();
    testForcingFilterPicksUniqueSimpleFourBlock();
    return 0;
}
