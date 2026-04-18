#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "gomoku/ProofSearch.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    using namespace gomoku;

    int moveTimeMs = 500;
    if (argc >= 2) {
        moveTimeMs = std::max(1, std::atoi(argv[1]));
    }

    ProofAnalysisConfig config;
    config.maxDepth = 8;
    config.maxNodes = std::max<std::uint64_t>(200000, static_cast<std::uint64_t>(moveTimeMs) * 900ULL);
    config.timeLimitMs = moveTimeMs;
    config.maxCandidateMoves = 8;
    config.maxThreatMoves = 10;

    const std::vector<BenchPosition> positions = {
        {"black_finish_line", Ruleset::Freestyle15, {{7, 7}, {0, 0}, {7, 8}, {0, 1}, {7, 9}, {0, 2}, {7, 10}, {0, 3}}},
        {"white_must_block", Ruleset::Freestyle15, {{7, 7}, {7, 6}, {7, 8}, {0, 0}, {7, 9}, {1, 0}, {7, 10}}},
        {"double_diagonal_attack", Ruleset::Freestyle15, {{7, 7}, {0, 0}, {8, 8}, {0, 1}, {6, 8}, {1, 1}, {8, 7}, {1, 2}}},
    };

    std::cout << "Proof benchmark move_time=" << moveTimeMs << " ms\n\n";
    for (const BenchPosition& position : positions) {
        const GameState state = buildPosition(position);
        ProofAnalyzer analyzer(config);
        const ProofAnalysisResult result = analyzer.analyze(state, state.sideToMove());

        std::cout << position.name << '\n';
        std::cout << "  side: " << toString(state.sideToMove()) << '\n';
        std::cout << "  outcome: " << toString(result.outcome) << '\n';
        std::cout << "  best: " << (result.bestMove.has_value() ? moveToString(*result.bestMove) : std::string("none")) << '\n';
        std::cout << "  nodes: " << result.nodes << '\n';
        std::cout << "  time: " << result.elapsedMs << " ms\n";
        if (!result.principalVariation.empty()) {
            std::cout << "  pv:";
            for (const Move& move : result.principalVariation) {
                std::cout << ' ' << moveToString(move);
            }
            std::cout << '\n';
        }
        if (!result.rootMoves.empty()) {
            std::cout << "  roots:";
            for (std::size_t index = 0; index < std::min<std::size_t>(5, result.rootMoves.size()); ++index) {
                std::cout << ' ' << moveToString(result.rootMoves[index].move) << ':' << toString(result.rootMoves[index].outcome);
            }
            std::cout << '\n';
        }
        std::cout << '\n';
    }

    return 0;
}
