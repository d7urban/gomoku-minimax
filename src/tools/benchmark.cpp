#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "gomoku/ClubAI.hpp"
#include "gomoku/ExpertAI.hpp"
#include "gomoku/TacticalAI.hpp"
#include "gomoku/Threats.hpp"

namespace {

struct BenchPosition {
    const char* name {nullptr};
    gomoku::Ruleset ruleset {gomoku::Ruleset::Freestyle15};
    std::vector<gomoku::Move> moves;
};

gomoku::GameState buildPosition(const BenchPosition& position) {
    gomoku::GameState state(gomoku::rulesFor(position.ruleset));
    for (const gomoku::Move& move : position.moves) {
        state.applyMove(move);
    }
    return state;
}

gomoku::SearchResult runController(gomoku::ControllerKind controller, const gomoku::GameState& state, gomoku::SearchConfig config) {
    switch (controller) {
        case gomoku::ControllerKind::ClubAI:
            return gomoku::ClubAI::chooseMove(state, state.sideToMove(), config);
        case gomoku::ControllerKind::TacticalAI:
            return gomoku::TacticalAI::chooseMove(state, state.sideToMove(), config);
        case gomoku::ControllerKind::ExpertAI:
        default:
            return gomoku::ExpertAI::chooseMove(state, state.sideToMove(), config);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace gomoku;

    ControllerKind controller = ControllerKind::ExpertAI;
    int moveTimeMs = 500;

    if (argc >= 2) {
        if (!tryParseController(argv[1], controller) || controller == ControllerKind::Human || controller == ControllerKind::RookieAI) {
            std::cerr << "Benchmark controller must be club, tactical, or expert.\n";
            return 1;
        }
    }
    if (argc >= 3) {
        moveTimeMs = std::max(1, std::atoi(argv[2]));
    }

    SearchConfig config;
    config.timeLimitMs = moveTimeMs;
    config.softTimeLimitMs = moveTimeMs * 3 / 4;
    config.maxNodes = std::max<std::uint64_t>(90000, static_cast<std::uint64_t>(moveTimeMs) * 1200ULL);

    const std::vector<BenchPosition> positions = {
        {"opening_book_root", Ruleset::Freestyle15, {}},
        {"white_must_block", Ruleset::Freestyle15, {{7, 7}, {7, 6}, {7, 8}, {0, 0}, {7, 9}, {1, 0}, {7, 10}}},
        {"black_finish_line", Ruleset::Freestyle15, {{7, 7}, {0, 0}, {7, 8}, {0, 1}, {7, 9}, {0, 2}, {7, 10}, {0, 3}}},
        {"standard_overline_trap", Ruleset::Standard15, {{7, 4}, {0, 0}, {7, 5}, {0, 1}, {7, 7}, {0, 2}, {7, 8}, {0, 3}, {7, 9}, {1, 3}}},
        {"forcing_midgame_depth0_regression", Ruleset::Freestyle15, {{14, 1}, {14, 4}, {14, 7}, {14, 10}, {14, 9}, {14, 5}, {14, 6}, {11, 8}, {10, 9}, {12, 9}, {10, 7}, {10, 8}, {9, 8}, {11, 7}}},
    };

    std::cout << "Benchmark controller=" << toString(controller) << ", move_time=" << moveTimeMs << " ms\n\n";

    for (const BenchPosition& position : positions) {
        const GameState state = buildPosition(position);
        const bool profilingEnabled = profilingCountersEnabled();
        if (profilingEnabled) {
            resetProfilingCounters();
        }
        const SearchResult result = runController(controller, state, config);
        const ProfilingCounters counters = profilingEnabled ? readProfilingCounters() : ProfilingCounters {};

        std::cout << position.name << '\n';
        std::cout << "  side: " << toString(state.sideToMove()) << '\n';
        std::cout << "  move: " << (result.bestMove.has_value() ? moveToString(*result.bestMove) : std::string("none")) << '\n';
        std::cout << "  score: " << result.summary.score << '\n';
        std::cout << "  depth: " << result.summary.depthReached << '\n';
        std::cout << "  time: " << result.summary.elapsedMs << " ms\n";
        std::cout << "  nodes: " << result.summary.nodes << '\n';
        if (profilingEnabled) {
            std::cout << "  gen_calls: " << counters.generateCandidateCalls << '\n';
            std::cout << "  legal_calls: " << counters.legalMovesCalls << '\n';
            std::cout << "  near_checks: " << counters.nearStoneChecks << '\n';
            std::cout << "  analyze_calls: " << counters.analyzeMoveCalls << '\n';
            std::cout << "  compute_threat: " << counters.computeThreatInfoCalls << '\n';
            std::cout << "  pattern_windows: " << counters.patternWindowsScanned << '\n';
        }
        if (profilingEnabled && result.summary.nodes > 0) {
            const double perNodeGen = static_cast<double>(counters.generateCandidateCalls) / static_cast<double>(result.summary.nodes);
            const double perNodeNear = static_cast<double>(counters.nearStoneChecks) / static_cast<double>(result.summary.nodes);
            const double perNodeAnalyze = static_cast<double>(counters.analyzeMoveCalls) / static_cast<double>(result.summary.nodes);
            std::cout << "  per_node_gen: " << perNodeGen << '\n';
            std::cout << "  per_node_near: " << perNodeNear << '\n';
            std::cout << "  per_node_analyze: " << perNodeAnalyze << '\n';
        }
        if (result.summary.usedOpeningBook) {
            std::cout << "  book: " << result.summary.openingBookName << '\n';
        }
        if (!result.summary.principalVariation.empty()) {
            std::cout << "  pv:";
            for (const Move& move : result.summary.principalVariation) {
                std::cout << ' ' << moveToString(move);
            }
            std::cout << '\n';
        }
        std::cout << '\n';
    }

    return 0;
}
