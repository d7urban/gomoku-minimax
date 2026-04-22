#include <algorithm>
#include <initializer_list>
#include <optional>
#include <vector>

#include "TestAssert.hpp"
#include "gomoku/ThreatSearch.hpp"

namespace {

using gomoku::GameState;
using gomoku::Move;
using gomoku::MoveThreatInfo;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::StaticEvaluator;
using gomoku::ThreatGraphNodeKind;
using gomoku::ThreatSearchResult;
using gomoku::ThreatSequenceSearcher;
using gomoku::ThreatStep;
using gomoku::ThreatType;
using gomoku::rulesFor;

GameState makeGame(std::initializer_list<Move> moves) {
    GameState game(rulesFor(Ruleset::Freestyle15));
    for (const Move& move : moves) {
        const bool applied = game.applyMove(move);
        assert(applied);
    }
    return game;
}

const ThreatStep* findThreat(const std::vector<ThreatStep>& threats, Move move, ThreatType type) {
    for (const ThreatStep& threat : threats) {
        if (threat.move == move && threat.type == type) {
            return &threat;
        }
    }
    return nullptr;
}

bool containsMove(const std::vector<Move>& moves, Move move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

void assertContinuationSubsetOfRequired(const ThreatStep& threat) {
    for (const Move& move : threat.continuationMoves) {
        assert(containsMove(threat.requiredEmpty, move));
    }
}

}  // namespace

int main() {
    using namespace gomoku;

    {
        GameState game = makeGame({{7, 7}, {0, 0}, {7, 8}, {0, 1}});

        ThreatSequenceSearcher searcher;
        const auto threats = searcher.enumerateThreats(game, Player::Black);

        const ThreatStep* leftOpenThree = findThreat(threats, {7, 6}, ThreatType::OpenThree);
        const ThreatStep* rightOpenThree = findThreat(threats, {7, 9}, ThreatType::OpenThree);
        const ThreatStep* nonForcing = findThreat(threats, {7, 10}, ThreatType::OpenThree);

        assert(leftOpenThree != nullptr);
        assert(rightOpenThree != nullptr);
        assert(nonForcing == nullptr);
        assert(leftOpenThree->defenseMoves.size() == 2U);
        assert(rightOpenThree->defenseMoves.size() == 2U);
        assert(leftOpenThree->requiredEmpty.size() >= 4U);
        assert(rightOpenThree->requiredEmpty.size() >= 4U);
        assertContinuationSubsetOfRequired(*leftOpenThree);
        assertContinuationSubsetOfRequired(*rightOpenThree);
    }

    {
        GameState game = makeGame({{7, 7}, {0, 0}, {7, 8}, {0, 1}});

        ThreatSequenceSearcher searcher;
        const auto threats = searcher.enumerateThreats(game, Player::Black);
        const ThreatStep* rightOpenThree = findThreat(threats, {7, 9}, ThreatType::OpenThree);
        assert(rightOpenThree != nullptr);
        assert(containsMove(rightOpenThree->requiredEmpty, {7, 11}));

        game.setSideToMoveForAnalysis(Player::White);
        const bool applied = game.applyMove({7, 11});
        assert(applied);
        game.setSideToMoveForAnalysis(Player::Black);

        const auto blockedThreats = searcher.enumerateThreats(game, Player::Black);
        assert(findThreat(blockedThreats, {7, 9}, ThreatType::OpenThree) == nullptr);
        assert(findThreat(blockedThreats, {7, 6}, ThreatType::OpenThree) != nullptr);
    }

    {
        GameState game = makeGame({{7, 7}, {0, 0}, {7, 8}, {0, 1}});

        ThreatSequenceSearcher searcher;
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);

        assert(!result.foundWin);
        assert(result.sequence.empty());
        assert(result.graph.size() >= 2U);
        for (const auto& node : result.graph) {
            assert(node.kind == ThreatGraphNodeKind::Threat);
            assert(node.type == ThreatType::OpenThree);
            assert(!node.defenseMoves.empty());
            assert(!node.requiredEmpty.empty());
        }
    }

    {
        GameState game = makeGame({{7, 7}, {0, 0}, {7, 8}, {0, 1}, {7, 9}, {0, 2}, {7, 10}, {0, 3}});

        ThreatSequenceSearcher searcher;
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);

        assert(result.foundWin);
        assert(result.sequence.size() == 1U);
        assert(!result.graph.empty());
        const ThreatStep& step = result.sequence.front();
        assert(step.nodeId >= 0);
        assert(step.nodeId < static_cast<int>(result.graph.size()));

        const ThreatGraphNode& node = result.graph[static_cast<std::size_t>(step.nodeId)];
        assert(node.kind == ThreatGraphNodeKind::Threat);
        assert(node.move == step.move);
        assert(node.type == step.type);
        assert(node.defenseMoves == step.defenseMoves);
        assert(node.requiredEmpty == step.requiredEmpty);
    }

    return 0;
}
