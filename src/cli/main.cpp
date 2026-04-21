#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "gomoku/Match.hpp"
#include "gomoku/OpeningBook.hpp"
#include "gomoku/Replay.hpp"

namespace {

using gomoku::Action;
using gomoku::ControllerKind;
using gomoku::GameResult;
using gomoku::Match;
using gomoku::MatchConfig;
using gomoku::Move;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::SwapChoice;
using gomoku::ThreatType;

void printBoard(const Match& match) {
    const gomoku::GameState& state = match.state();

    std::cout << "\n   ";
    for (int col = 0; col < state.boardSize(); ++col) {
        std::cout << static_cast<char>('a' + col) << ' ';
    }
    std::cout << '\n';

    for (int row = state.boardSize() - 1; row >= 0; --row) {
        std::cout << (row + 1 < 10 ? " " : "") << row + 1 << ' ';
        for (int col = 0; col < state.boardSize(); ++col) {
            std::cout << gomoku::playerGlyph(state.cellAt(row, col)) << ' ';
        }
        std::cout << (row + 1 < 10 ? " " : "") << row + 1 << '\n';
    }

    std::cout << "   ";
    for (int col = 0; col < state.boardSize(); ++col) {
        std::cout << static_cast<char>('a' + col) << ' ';
    }
    std::cout << "\n\n";
}

void printStatus(const Match& match) {
    const gomoku::GameState& state = match.state();

    std::cout << "Rules: " << state.rules().name << '\n';
    std::cout << "Moves played: " << state.moveCount() << '\n';
    std::cout << "Result: " << gomoku::toString(state.result()) << '\n';
    std::cout << "AI move time: " << match.config().aiMoveTimeMs << " ms\n";

    if (state.isGameOver()) {
        return;
    }

    if (state.isSwapDecisionPending()) {
        std::cout << "Swap decision pending for " << gomoku::toString(match.seatToAct()) << '\n';
        return;
    }

    std::cout << "To move: " << gomoku::toString(state.sideToMove())
              << " (" << gomoku::toString(match.seatToAct()) << ", "
              << gomoku::toString(match.controllerToAct()) << ")\n";
    std::cout << "Controllers: opener=" << gomoku::toString(match.config().openerController)
              << ", chooser=" << gomoku::toString(match.config().chooserController) << '\n';

    if (const auto lastMove = state.lastPlacedMove()) {
        std::cout << "Last move: " << gomoku::moveToString(*lastMove) << '\n';
    }

    if (const auto bookHit = gomoku::lookupOpeningBookMove(state)) {
        std::cout << "Book hint: " << gomoku::moveToString(bookHit->move)
                  << " (" << bookHit->lineName << ")\n";
    }

    if (const auto& summary = match.lastSearchSummary()) {
        std::cout << "Search depth: " << summary->depthReached
                  << ", score: " << summary->score
                  << ", time: " << summary->elapsedMs << " ms"
                  << ", nodes: " << summary->nodes
                  << ", tt hits: " << summary->ttHits
                  << ", threat nodes: " << summary->threatNodes << '\n';

        if (summary->usedThreatSequence) {
            std::cout << "Threat sequence length: " << summary->threatSequenceLength << '\n';
        }
        if (summary->usedOpeningBook) {
            std::cout << "Opening book: " << summary->openingBookName << '\n';
        }
        if (!summary->principalVariation.empty()) {
            std::cout << "PV:";
            for (const Move& move : summary->principalVariation) {
                std::cout << ' ' << gomoku::moveToString(move);
            }
            std::cout << '\n';
        }
    }

}

void printHelp() {
    std::cout << "Commands:\n"
              << "  <coord>      play a move, e.g. h8\n"
              << "  keep         choose to keep colors in swap mode\n"
              << "  swap         choose to swap colors in swap mode\n"
              << "  undo         undo back to your turn\n"
              << "  restart      restart the match\n"
              << "  save         print replay log\n"
              << "  help         show this help\n"
              << "  quit         exit\n\n";
}

}  // namespace

int main(int argc, char** argv) {
    MatchConfig config;

    if (argc >= 2) {
        Ruleset ruleset;
        if (!gomoku::tryParseRuleset(argv[1], ruleset)) {
            std::cerr << "Unknown ruleset: " << argv[1] << '\n';
            return 1;
        }
        config.ruleset = ruleset;
    }

    if (argc >= 3) {
        ControllerKind controller;
        if (!gomoku::tryParseController(argv[2], controller)) {
            std::cerr << "Unknown opener controller: " << argv[2] << '\n';
            return 1;
        }
        config.openerController = controller;
    }

    if (argc >= 4) {
        ControllerKind controller;
        if (!gomoku::tryParseController(argv[3], controller)) {
            std::cerr << "Unknown chooser controller: " << argv[3] << '\n';
            return 1;
        }
        config.chooserController = controller;
    }

    if (argc >= 5) {
        std::istringstream timeStream(argv[4]);
        int moveTimeMs = 0;
        if (!(timeStream >> moveTimeMs) || moveTimeMs <= 0) {
            std::cerr << "Invalid move time: " << argv[4] << '\n';
            return 1;
        }
        config.aiMoveTimeMs = moveTimeMs;
    }

    Match match(config);

    std::cout << "gomoku_cli - text fallback\n";
    std::cout << "Usage: gomoku_cli [freestyle15|standard15] [human|rookie|club|tactical|expert|analyst|ai] [human|rookie|club|tactical|expert|analyst|ai] [move_time_ms]\n\n";
    printHelp();

    while (true) {
        while (match.isAiTurn() && !match.state().isGameOver()) {
            match.stepAi();
        }

        printBoard(match);
        printStatus(match);

        if (match.state().isGameOver()) {
            std::cout << "Game over. Type restart, save, or quit.\n> ";
        } else {
            std::cout << "> ";
        }

        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        std::string lowered = input;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (lowered == "quit" || lowered == "exit") {
            break;
        }
        if (lowered == "help") {
            printHelp();
            continue;
        }
        if (lowered == "restart") {
            match.reset();
            continue;
        }
        if (lowered == "undo") {
            if (!match.smartUndo()) {
                std::cout << "Nothing to undo.\n";
            }
            continue;
        }
        if (lowered == "save") {
            std::cout << serializeReplay(match.state()) << '\n';
            continue;
        }
        if (lowered == "keep" || lowered == "swap") {
            SwapChoice choice;
            if (!gomoku::tryParseSwapChoice(lowered, choice) || !match.applySwapChoice(choice)) {
                std::cout << "Swap decision not accepted right now.\n";
            }
            continue;
        }

        Move move;
        if (!gomoku::tryParseMove(lowered, move)) {
            std::cout << "Unrecognized command. Type help for commands.\n";
            continue;
        }

        if (!match.applyMove(move)) {
            std::cout << "Illegal move.\n";
        }
    }

    return 0;
}
