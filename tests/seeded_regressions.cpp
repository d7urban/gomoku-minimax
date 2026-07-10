#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Search.hpp"
#include "gomoku/Threats.hpp"

namespace {

using gomoku::GameState;
using gomoku::Move;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::SearchConfig;
using gomoku::SearchEngine;
using gomoku::SearchResult;
using gomoku::ThreatType;
using gomoku::otherPlayer;
using gomoku::rulesFor;
using gomoku::threatSeverity;
using gomoku::toString;

struct RegressionCase {
    std::string_view name;
    std::uint64_t nodeBudget;
    std::vector<Move> prefix;
};

GameState makeGame(const std::vector<Move>& moves) {
    GameState game(rulesFor(Ruleset::Freestyle15));
    for (const Move& move : moves) {
        const bool applied = game.applyMove(move);
        assert(applied);
    }
    return game;
}

int replyThreatRisk(const GameState& state, Move move) {
    GameState next = state;
    if (!next.applyMove(move)) {
        return std::numeric_limits<int>::max();
    }
    if (next.isGameOver()) {
        return -1;
    }

    const Player opponent = next.sideToMove();
    int risk = 0;
    for (const Move& reply : next.legalMoves()) {
        risk = std::max(risk, threatSeverity(next.threatInfoAt(reply, opponent).best));
    }
    return risk;
}

std::vector<Move> minRiskMoves(const GameState& state, int* outRisk) {
    std::vector<Move> bestMoves;
    int bestRisk = std::numeric_limits<int>::max();
    for (const Move& move : state.legalMoves()) {
        const int risk = replyThreatRisk(state, move);
        if (risk < bestRisk) {
            bestRisk = risk;
            bestMoves = {move};
        } else if (risk == bestRisk) {
            bestMoves.push_back(move);
        }
    }
    *outRisk = bestRisk;
    return bestMoves;
}

std::vector<RegressionCase> regressionCases() {
    return {
        {"crazy-sensei-F5__F6__C3__G7__E6__G5", 30'000, {{10, 5}, {9, 5}, {12, 2}, {8, 6}, {9, 4}, {10, 6}}},
        {"crazy-sensei-F5__G5__E2__E6__G6__E4", 30'000, {{10, 5}, {10, 6}, {13, 4}, {9, 4}, {9, 6}, {11, 4}}},
        {"crazy-sensei-F5__H5__H2__F6__G4__E6", 30'000, {{10, 5}, {10, 7}, {13, 7}, {9, 5}, {11, 6}, {9, 4}, {9, 6}}},
        {"crazy-sensei-F5__H5__H2__F6__H6__G7", 30'000, {{10, 5}, {10, 7}, {13, 7}, {9, 5}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__E5__C5__G6__D4", 30'000, {{10, 5}, {10, 4}, {10, 2}, {9, 6}, {11, 3}}},
        {"crazy-sensei-F5__E5__C7__F7__E6", 30'000, {{10, 5}, {10, 4}, {8, 2}, {8, 5}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__E6__B5__F7__D5", 50'000, {{10, 5}, {9, 4}, {10, 1}, {8, 5}, {10, 3}}},
        {"crazy-sensei-F5__E6__B5__G6__D6", 50'000, {{10, 5}, {9, 4}, {10, 1}, {9, 6}, {9, 3}, {8, 5}}},
        {"crazy-sensei-F5__E6__E2__G5__E7", 50'000, {{10, 5}, {9, 4}, {13, 4}, {10, 6}, {8, 4}}},
        {"crazy-sensei-F5__E7__B7__G7__E6", 50'000, {{10, 5}, {8, 4}, {8, 1}, {8, 6}, {9, 4}, {7, 5}}},
        {"crazy-sensei-F5__F6__C3__G5__E7", 50'000, {{10, 5}, {9, 5}, {12, 2}, {10, 6}, {8, 4}, {9, 7}}},
        {"crazy-sensei-F5__F6__C3__G7__E5", 50'000, {{10, 5}, {9, 5}, {12, 2}, {8, 6}, {10, 4}}},
        {"crazy-sensei-F5__F6__C4__G6__H6", 50'000, {{10, 5}, {9, 5}, {11, 2}, {9, 6}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__F6__D3__E4__E6", 80'000, {{10, 5}, {9, 5}, {12, 3}, {11, 4}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__F6__E2__E4__E7", 80'000, {{10, 5}, {9, 5}, {13, 4}, {11, 4}, {8, 4}}},
        {"crazy-sensei-F5__F7__C3__G6__E8", 80'000, {{10, 5}, {8, 5}, {12, 2}, {9, 6}, {7, 4}, {9, 7}}},
        {"crazy-sensei-F5__F7__C7__E5__E6", 80'000, {{10, 5}, {8, 5}, {8, 2}, {10, 4}, {9, 4}}},
        {"crazy-sensei-F5__F7__J9__G6__H5", 80'000, {{10, 5}, {8, 5}, {6, 8}, {9, 6}, {10, 7}, {9, 4}}},
        {"crazy-sensei-F5__G5__E2__E6__E7", 80'000, {{10, 5}, {10, 6}, {13, 4}, {9, 4}, {8, 4}}},
        {"crazy-sensei-F5__G5__G2__E4__F4", 80'000, {{10, 5}, {10, 6}, {13, 6}, {11, 4}, {11, 5}, {9, 5}}},
        {"crazy-sensei-F5__G5__G2__E6__H4", 120'000, {{10, 5}, {10, 6}, {13, 6}, {9, 4}, {11, 7}}},
        {"crazy-sensei-F5__G5__J2__J6__G4", 120'000, {{10, 5}, {10, 6}, {13, 8}, {9, 8}, {11, 6}, {12, 7}}},
        {"crazy-sensei-F5__G5__J3__H6__F4", 120'000, {{10, 5}, {10, 6}, {12, 8}, {9, 7}, {11, 5}}},
        {"crazy-sensei-F5__G6__C4__F6__H6", 120'000, {{10, 5}, {9, 6}, {11, 2}, {9, 5}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__G6__C5__E5__D4", 120'000, {{10, 5}, {9, 6}, {10, 2}, {10, 4}, {11, 3}}},
        {"crazy-sensei-F5__G6__H2__E5__E6", 120'000, {{10, 5}, {9, 6}, {13, 7}, {10, 4}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__G6__H2__H5__J4", 120'000, {{10, 5}, {9, 6}, {13, 7}, {10, 7}, {11, 8}}},
        {"crazy-sensei-F5__G6__J9__F7__H5", 120'000, {{10, 5}, {9, 6}, {6, 8}, {8, 5}, {10, 7}, {9, 4}}},
    };
}

SearchResult chooseRegressionMove(const GameState& game, std::uint64_t nodeBudget) {
    SearchConfig config;
    config.maxDepth = 24;
    config.maxNodes = nodeBudget;
    config.timeLimitMs = 0;
    config.softTimeLimitMs = 0;
    config.maxCandidateMoves = 28;
    config.useOpeningBook = false;
    SearchEngine engine(config);
    return engine.search(game);
}

void testConcreteWinningThreatIsBlockedAtEveryBudget() {
    const GameState game = makeGame({
        {7, 0}, {0, 0},
        {7, 1}, {2, 5},
        {7, 2}, {4, 10},
        {7, 3},
    });

    for (const std::uint64_t budget : {100ULL, 2'000ULL, 20'000ULL}) {
        const SearchResult result = chooseRegressionMove(game, budget);
        assert(result.bestMove == (Move{7, 4}));
    }
}

void testSeededSearchDoesNotIntroduceForcingReplyWithMoreNodes() {
    bool ok = true;
    for (const RegressionCase& regression : regressionCases()) {
        GameState game = makeGame(regression.prefix);
        int minRisk = std::numeric_limits<int>::max();
        const std::vector<Move> bestMoves = minRiskMoves(game, &minRisk);

        constexpr std::uint64_t kShortBudget = 2'000;
        const std::uint64_t longBudget = std::min<std::uint64_t>(regression.nodeBudget, 30'000);
        const SearchResult shortResult = chooseRegressionMove(game, kShortBudget);
        const SearchResult longResult = chooseRegressionMove(game, longBudget);
        if (!shortResult.bestMove.has_value() || !longResult.bestMove.has_value()) {
            std::cerr << "no move for regression " << regression.name << '\n';
            ok = false;
            continue;
        }

        const int shortRisk = replyThreatRisk(game, *shortResult.bestMove);
        const int longRisk = replyThreatRisk(game, *longResult.bestMove);
        const bool introducesForcingReply = shortRisk < threatSeverity(ThreatType::SimpleFour)
            && longRisk >= threatSeverity(ThreatType::SimpleFour);
        if (introducesForcingReply) {
            std::cerr << "regression failure " << regression.name
                      << ": short {" << shortResult.bestMove->row << ',' << shortResult.bestMove->col << "} risk=" << shortRisk
                      << " long {" << longResult.bestMove->row << ',' << longResult.bestMove->col << "} risk=" << longRisk
                      << " best_risk=" << minRisk << " best_moves=";
            for (const Move& move : bestMoves) {
                std::cerr << " {" << move.row << ',' << move.col << '}';
            }
            std::cerr << " short_depth=" << shortResult.summary.depthReached
                      << " long_depth=" << longResult.summary.depthReached << '\n';
            ok = false;
        }
    }
    assert(ok);
}

void testNodeBudgetSearchIsDeterministic() {
    const RegressionCase regression = regressionCases().front();
    const GameState game = makeGame(regression.prefix);
    const SearchResult first = chooseRegressionMove(game, 15'000);
    const SearchResult second = chooseRegressionMove(game, 15'000);

    assert(first.bestMove == second.bestMove);
    assert(first.summary.depthReached == second.summary.depthReached);
    assert(first.summary.score == second.summary.score);
    assert(first.summary.nodes == second.summary.nodes);
    assert(first.summary.principalVariation == second.summary.principalVariation);
}

}  // namespace

int main() {
    testConcreteWinningThreatIsBlockedAtEveryBudget();
    testSeededSearchDoesNotIntroduceForcingReplyWithMoreNodes();
    testNodeBudgetSearchIsDeterministic();
    return 0;
}
