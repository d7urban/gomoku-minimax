#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "gomoku/Match.hpp"

namespace {

bool isAiController(gomoku::ControllerKind controller) {
    return controller != gomoku::ControllerKind::Human;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace gomoku;

    int games = 4;
    MatchConfig config;
    config.openerController = ControllerKind::ExpertAI;
    config.chooserController = ControllerKind::ExpertAI;

    if (argc >= 2) {
        games = std::max(1, std::atoi(argv[1]));
    }
    if (argc >= 3) {
        Ruleset ruleset;
        if (!tryParseRuleset(argv[2], ruleset)) {
            std::cerr << "Unknown ruleset: " << argv[2] << '\n';
            return 1;
        }
        config.ruleset = ruleset;
    }
    if (argc >= 4) {
        ControllerKind controller;
        if (!tryParseController(argv[3], controller)) {
            std::cerr << "Unknown opener controller: " << argv[3] << '\n';
            return 1;
        }
        config.openerController = controller;
    }
    if (argc >= 5) {
        ControllerKind controller;
        if (!tryParseController(argv[4], controller)) {
            std::cerr << "Unknown chooser controller: " << argv[4] << '\n';
            return 1;
        }
        config.chooserController = controller;
    }
    if (argc >= 6) {
        config.aiMoveTimeMs = std::max(1, std::atoi(argv[5]));
    }

    if (!isAiController(config.openerController) || !isAiController(config.chooserController)) {
        std::cerr << "Self-play requires AI controllers on both seats.\n";
        return 1;
    }

    int blackWins = 0;
    int whiteWins = 0;
    int draws = 0;
    int totalMoves = 0;
    std::uint64_t totalNodes = 0;
    std::uint64_t searchedPlies = 0;
    long long totalSearchTimeMs = 0;

    std::cout << "Self-play: " << games << " games, rules=" << toString(config.ruleset)
              << ", opener=" << toString(config.openerController)
              << ", chooser=" << toString(config.chooserController)
              << ", move_time=" << config.aiMoveTimeMs << " ms\n\n";

    for (int gameIndex = 0; gameIndex < games; ++gameIndex) {
        Match match(config);

        while (!match.state().isGameOver()) {
            if (!match.isAiTurn()) {
                std::cerr << "Self-play reached a non-AI turn unexpectedly.\n";
                return 1;
            }

            const int actionsBefore = match.state().actionCount();
            match.stepAi();
            if (match.state().actionCount() == actionsBefore) {
                std::cerr << "AI stalled without making progress.\n";
                return 1;
            }

            if (const auto& summary = match.lastSearchSummary()) {
                totalNodes += summary->nodes;
                totalSearchTimeMs += summary->elapsedMs;
                ++searchedPlies;
            }
        }

        totalMoves += match.state().moveCount();
        switch (match.state().result()) {
            case GameResult::BlackWin:
                ++blackWins;
                break;
            case GameResult::WhiteWin:
                ++whiteWins;
                break;
            case GameResult::Draw:
                ++draws;
                break;
            case GameResult::Ongoing:
            default:
                break;
        }

        std::cout << "Game " << (gameIndex + 1)
                  << ": result=" << toString(match.state().result())
                  << ", moves=" << match.state().moveCount() << '\n';
    }

    std::cout << "\nSummary\n";
    std::cout << "Black wins: " << blackWins << '\n';
    std::cout << "White wins: " << whiteWins << '\n';
    std::cout << "Draws: " << draws << '\n';
    std::cout << "Average moves: " << (games > 0 ? totalMoves / games : 0) << '\n';
    if (searchedPlies > 0) {
        std::cout << "Average search time: " << (totalSearchTimeMs / static_cast<long long>(searchedPlies)) << " ms\n";
        std::cout << "Average searched nodes: " << (totalNodes / searchedPlies) << '\n';
    }

    return 0;
}
