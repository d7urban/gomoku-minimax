#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include "gomoku/ExpertAI.hpp"
#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Search.hpp"
#include "gomoku/Threats.hpp"

namespace {

using gomoku::ExpertAI;
using gomoku::GameState;
using gomoku::Move;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::SearchConfig;
using gomoku::SearchResult;
using gomoku::ThreatType;
using gomoku::otherPlayer;
using gomoku::rulesFor;
using gomoku::threatSeverity;
using gomoku::toString;

struct RegressionCase {
    std::string_view name;
    int timeMs;
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

bool containsMove(const std::vector<Move>& moves, Move move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

std::vector<RegressionCase> regressionCases() {
    return {
        {"crazy-sensei-F5__F6__C3__G7__E6__G5", 2000, {{10, 5}, {9, 5}, {12, 2}, {8, 6}, {9, 4}, {10, 6}}},
        {"crazy-sensei-F5__G5__E2__E6__G6__E4", 2000, {{10, 5}, {10, 6}, {13, 4}, {9, 4}, {9, 6}, {11, 4}}},
        {"crazy-sensei-F5__H5__H2__F6__G4__E6", 2000, {{10, 5}, {10, 7}, {13, 7}, {9, 5}, {11, 6}, {9, 4}, {9, 6}}},
        {"crazy-sensei-F5__H5__H2__F6__H6__G7", 2000, {{10, 5}, {10, 7}, {13, 7}, {9, 5}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__E5__C5__G6__D4", 2000, {{10, 5}, {10, 4}, {10, 2}, {9, 6}, {11, 3}}},
        {"crazy-sensei-F5__E5__C7__F7__E6", 2000, {{10, 5}, {10, 4}, {8, 2}, {8, 5}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__E6__B5__F7__D5", 5000, {{10, 5}, {9, 4}, {10, 1}, {8, 5}, {10, 3}}},
        {"crazy-sensei-F5__E6__B5__G6__D6", 5000, {{10, 5}, {9, 4}, {10, 1}, {9, 6}, {9, 3}, {8, 5}}},
        {"crazy-sensei-F5__E6__E2__G5__E7", 5000, {{10, 5}, {9, 4}, {13, 4}, {10, 6}, {8, 4}}},
        {"crazy-sensei-F5__E7__B7__G7__E6", 5000, {{10, 5}, {8, 4}, {8, 1}, {8, 6}, {9, 4}, {7, 5}}},
        {"crazy-sensei-F5__F6__C3__G5__E7", 5000, {{10, 5}, {9, 5}, {12, 2}, {10, 6}, {8, 4}, {9, 7}}},
        {"crazy-sensei-F5__F6__C3__G7__E5", 5000, {{10, 5}, {9, 5}, {12, 2}, {8, 6}, {10, 4}}},
        {"crazy-sensei-F5__F6__C4__G6__H6", 5000, {{10, 5}, {9, 5}, {11, 2}, {9, 6}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__F6__D3__E4__E6", 10000, {{10, 5}, {9, 5}, {12, 3}, {11, 4}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__F6__E2__E4__E7", 10000, {{10, 5}, {9, 5}, {13, 4}, {11, 4}, {8, 4}}},
        {"crazy-sensei-F5__F7__C3__G6__E8", 10000, {{10, 5}, {8, 5}, {12, 2}, {9, 6}, {7, 4}, {9, 7}}},
        {"crazy-sensei-F5__F7__C7__E5__E6", 10000, {{10, 5}, {8, 5}, {8, 2}, {10, 4}, {9, 4}}},
        {"crazy-sensei-F5__F7__J9__G6__H5", 10000, {{10, 5}, {8, 5}, {6, 8}, {9, 6}, {10, 7}, {9, 4}}},
        {"crazy-sensei-F5__G5__E2__E6__E7", 10000, {{10, 5}, {10, 6}, {13, 4}, {9, 4}, {8, 4}}},
        {"crazy-sensei-F5__G5__G2__E4__F4", 10000, {{10, 5}, {10, 6}, {13, 6}, {11, 4}, {11, 5}, {9, 5}}},
        {"crazy-sensei-F5__G5__G2__E6__H4", 20000, {{10, 5}, {10, 6}, {13, 6}, {9, 4}, {11, 7}}},
        {"crazy-sensei-F5__G5__J2__J6__G4", 20000, {{10, 5}, {10, 6}, {13, 8}, {9, 8}, {11, 6}, {12, 7}}},
        {"crazy-sensei-F5__G5__J3__H6__F4", 20000, {{10, 5}, {10, 6}, {12, 8}, {9, 7}, {11, 5}}},
        {"crazy-sensei-F5__G6__C4__F6__H6", 20000, {{10, 5}, {9, 6}, {11, 2}, {9, 5}, {9, 7}, {8, 6}}},
        {"crazy-sensei-F5__G6__C5__E5__D4", 20000, {{10, 5}, {9, 6}, {10, 2}, {10, 4}, {11, 3}}},
        {"crazy-sensei-F5__G6__H2__E5__E6", 20000, {{10, 5}, {9, 6}, {13, 7}, {10, 4}, {9, 4}, {11, 6}}},
        {"crazy-sensei-F5__G6__H2__H5__J4", 20000, {{10, 5}, {9, 6}, {13, 7}, {10, 7}, {11, 8}}},
        {"crazy-sensei-F5__G6__J9__F7__H5", 20000, {{10, 5}, {9, 6}, {6, 8}, {8, 5}, {10, 7}, {9, 4}}},
    };
}

SearchResult chooseRegressionMove(const GameState& game, int timeMs) {
    SearchConfig config;
    config.maxNodes = 120'000;
    config.timeLimitMs = std::min(timeMs, 150);
    config.softTimeLimitMs = config.timeLimitMs;
    config.maxCandidateMoves = 8;
    config.useOpeningBook = false;
    return ExpertAI::chooseMove(game, game.sideToMove(), config);
}

void testSeededLossesPreferBestImmediateDefense() {
    bool ok = true;
    for (const RegressionCase& regression : regressionCases()) {
        GameState game = makeGame(regression.prefix);
        const SearchResult result = chooseRegressionMove(game, regression.timeMs);
        if (!result.bestMove.has_value()) {
            std::cerr << "no move for regression " << regression.name << "\n";
            ok = false;
            continue;
        }

        int minRisk = std::numeric_limits<int>::max();
        const std::vector<Move> bestMoves = minRiskMoves(game, &minRisk);
        const int chosenRisk = replyThreatRisk(game, *result.bestMove);
        if (chosenRisk != minRisk || !containsMove(bestMoves, *result.bestMove)) {
            std::cerr << "regression failure " << regression.name
                      << ": chose {" << result.bestMove->row << ',' << result.bestMove->col << "} risk=" << chosenRisk
                      << " best_risk=" << minRisk << " best_moves=";
            for (const Move& move : bestMoves) {
                std::cerr << " {" << move.row << ',' << move.col << '}';
            }
            std::cerr << " depth=" << result.summary.depthReached
                      << " score=" << result.summary.score
                      << " nodes=" << result.summary.nodes << '\n';
            ok = false;
        }
    }
    assert(ok);
}

}  // namespace

int main() {
    testSeededLossesPreferBestImmediateDefense();
    return 0;
}
