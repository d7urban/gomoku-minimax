#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "TestAssert.hpp"
#include "gomoku/PatternAnalysis.hpp"
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

bool hasCombinationNodeForMove(const ThreatSearchResult& result, Move move) {
    for (const auto& node : result.graph) {
        if (node.kind != ThreatGraphNodeKind::Combination || node.dependencies.size() != 2U) {
            continue;
        }

        bool dependenciesMatch = true;
        for (const int dependency : node.dependencies) {
            if (dependency < 0 || dependency >= static_cast<int>(result.graph.size())) {
                dependenciesMatch = false;
                break;
            }
            const auto& parent = result.graph[static_cast<std::size_t>(dependency)];
            if (parent.kind != ThreatGraphNodeKind::Threat || parent.move != move) {
                dependenciesMatch = false;
                break;
            }
        }
        if (dependenciesMatch) {
            return true;
        }
    }
    return false;
}

void assertContinuationSubsetOfRequired(const ThreatStep& threat) {
    for (const Move& move : threat.continuationMoves) {
        assert(containsMove(threat.requiredEmpty, move));
    }
}

int pow3(int exponent) {
    int result = 1;
    for (int i = 0; i < exponent; ++i) {
        result *= 3;
    }
    return result;
}

template<int Length>
void assertBitPatternTableMatchesWindowTable() {
    const ThreatType* table = gomoku::patternTableValues(Length, false);
    std::vector<gomoku::PatternCell> cells(static_cast<std::size_t>(Length), gomoku::PatternCell::Empty);
    for (int code = 0; code < pow3(Length); ++code) {
        int remaining = code;
        std::uint16_t ownBits = 0;
        std::uint16_t opponentBits = 0;
        for (int index = Length - 1; index >= 0; --index) {
            const int digit = remaining % 3;
            remaining /= 3;
            cells[static_cast<std::size_t>(index)] = static_cast<gomoku::PatternCell>(digit);
            if (digit == 1) {
                ownBits = static_cast<std::uint16_t>(ownBits | (1U << index));
            } else if (digit == 2) {
                opponentBits = static_cast<std::uint16_t>(opponentBits | (1U << index));
            }
        }

        for (int target = 0; target < Length; ++target) {
            if (cells[static_cast<std::size_t>(target)] != gomoku::PatternCell::Own) {
                continue;
            }
            for (int boundary = 0; boundary < 4; ++boundary) {
                const bool ownBefore = (boundary & 2) != 0;
                const bool ownAfter = (boundary & 1) != 0;
                const ThreatType slow = gomoku::classifyPatternWindow(cells, target, false, ownBefore, ownAfter);
                const ThreatType fast = gomoku::classifyPatternBits<Length>(
                    ownBits, opponentBits, 0, target, ownBefore, ownAfter, table);
                assert(slow == fast);
            }
        }
    }
}

ThreatType maxThreat(ThreatType left, ThreatType right) {
    return threatSeverity(left) >= threatSeverity(right) ? left : right;
}

template<int WindowLength>
ThreatType slowWindowThreat(const std::string& line, int start, int target) {
    std::vector<gomoku::PatternCell> cells(static_cast<std::size_t>(WindowLength), gomoku::PatternCell::Empty);
    for (int index = 0; index < WindowLength; ++index) {
        const int lineIndex = start + index;
        if (lineIndex == target || line[static_cast<std::size_t>(lineIndex)] == 'X') {
            cells[static_cast<std::size_t>(index)] = gomoku::PatternCell::Own;
        } else if (line[static_cast<std::size_t>(lineIndex)] == 'O') {
            cells[static_cast<std::size_t>(index)] = gomoku::PatternCell::Opponent;
        }
    }
    const bool ownBefore = start > 0 && line[static_cast<std::size_t>(start - 1)] == 'X';
    const bool ownAfter = start + WindowLength < static_cast<int>(line.size())
        && line[static_cast<std::size_t>(start + WindowLength)] == 'X';
    return gomoku::classifyPatternWindow(cells, target - start, false, ownBefore, ownAfter);
}

template<int WindowLength>
ThreatType fastWindowThreat(const std::string& line, int start, int target) {
    std::uint16_t ownBits = 0;
    std::uint16_t opponentBits = 0;
    for (int index = 0; index < static_cast<int>(line.size()); ++index) {
        if (line[static_cast<std::size_t>(index)] == 'X') {
            ownBits = static_cast<std::uint16_t>(ownBits | (1U << index));
        } else if (line[static_cast<std::size_t>(index)] == 'O') {
            opponentBits = static_cast<std::uint16_t>(opponentBits | (1U << index));
        }
    }

    const std::uint16_t ownBitsWithTarget = static_cast<std::uint16_t>(ownBits | (1U << target));
    const bool ownBefore = start > 0 && (ownBits & static_cast<std::uint16_t>(1U << (start - 1))) != 0U;
    const bool ownAfter = start + WindowLength < static_cast<int>(line.size())
        && (ownBits & static_cast<std::uint16_t>(1U << (start + WindowLength))) != 0U;
    return gomoku::classifyPatternBits<WindowLength>(
        ownBitsWithTarget,
        opponentBits,
        start,
        target - start,
        ownBefore,
        ownAfter,
        gomoku::patternTableValues(WindowLength, false));
}

void assertLongLineSubdivisionMatchesSlowClassifier() {
    const std::string base = ".XX..X.O..XX.O..";
    for (int length = 8; length <= 16; ++length) {
        const std::string line = base.substr(0, static_cast<std::size_t>(length));
        for (int target = 0; target < length; ++target) {
            if (line[static_cast<std::size_t>(target)] == 'O') {
                continue;
            }

            ThreatType slow = ThreatType::None;
            ThreatType fast = ThreatType::None;
            for (int windowLength = 5; windowLength <= 7; ++windowLength) {
                for (int targetOffset = 0; targetOffset < windowLength; ++targetOffset) {
                    const int start = target - targetOffset;
                    if (start < 0 || start + windowLength > length) {
                        continue;
                    }

                    if (windowLength == 5) {
                        slow = maxThreat(slow, slowWindowThreat<5>(line, start, target));
                        fast = maxThreat(fast, fastWindowThreat<5>(line, start, target));
                    } else if (windowLength == 6) {
                        slow = maxThreat(slow, slowWindowThreat<6>(line, start, target));
                        fast = maxThreat(fast, fastWindowThreat<6>(line, start, target));
                    } else {
                        slow = maxThreat(slow, slowWindowThreat<7>(line, start, target));
                        fast = maxThreat(fast, fastWindowThreat<7>(line, start, target));
                    }
                }
            }
            assert(slow == fast);
        }
    }
}

}  // namespace

int main() {
    using namespace gomoku;

    {
        assertBitPatternTableMatchesWindowTable<5>();
        assertBitPatternTableMatchesWindowTable<6>();
        assertBitPatternTableMatchesWindowTable<7>();
        assertLongLineSubdivisionMatchesSlowClassifier();
    }

    {
        // Behavioral broken three: after Black plays {7,3}, the horizontal
        // line is O_X*X__. It has exactly one open-four builder, but the
        // opponent must answer one of the involved empty squares.
        GameState game = makeGame({{7, 2}, {7, 0}, {7, 4}});
        const Move target {7, 3};
        const MoveThreatInfo info = StaticEvaluator::analyzeMove(game, target, Player::Black);
        assert(info.lineThreats[0] == ThreatType::BrokenThree);
        assert(info.best == ThreatType::BrokenThree);

        game.setSideToMoveForAnalysis(Player::Black);
        ThreatSequenceConfig config;
        config.minimumThreat = ThreatType::BrokenThree;
        ThreatSequenceSearcher searcher(config);
        const auto threats = searcher.enumerateThreats(game, Player::Black);
        const ThreatStep* broken = findThreat(threats, target, ThreatType::BrokenThree);
        assert(broken != nullptr);
        assert(containsMove(broken->continuationMoves, {7, 5}));
        assert(containsMove(broken->defenseMoves, {7, 1}));
        assert(containsMove(broken->defenseMoves, {7, 5}));
        assert(containsMove(broken->defenseMoves, {7, 6}));
        assert(containsMove(broken->requiredEmpty, {7, 1}));
        assert(containsMove(broken->requiredEmpty, {7, 5}));
        assert(containsMove(broken->requiredEmpty, {7, 6}));
        assertContinuationSubsetOfRequired(*broken);
    }

    {
        // Same shape in two directions. This must not degrade to a double
        // two: it is a double broken-three fork and should be visible through
        // the two best directional threats.
        GameState game = makeGame({
            {7, 2}, {7, 0},
            {7, 4}, {4, 3},
            {6, 3}, {0, 0},
            {8, 3},
        });
        const MoveThreatInfo info = StaticEvaluator::analyzeMove(game, {7, 3}, Player::Black);
        assert(info.lineThreats[0] == ThreatType::BrokenThree);
        assert(info.lineThreats[1] == ThreatType::BrokenThree);
        assert(info.best == ThreatType::BrokenThree);
        assert(info.second == ThreatType::BrokenThree);

        game.setSideToMoveForAnalysis(Player::Black);
        ThreatSequenceConfig config;
        config.minimumThreat = ThreatType::BrokenThree;
        config.maxDepth = 1;
        ThreatSequenceSearcher searcher(config);
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);
        assert(hasCombinationNodeForMove(result, {7, 3}));
    }

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
        bool sawOpenThree = false;
        for (const auto& node : result.graph) {
            if (node.kind == ThreatGraphNodeKind::Combination) {
                assert(node.dependencies.size() >= 2U);
                continue;
            }
            assert(threatSeverity(node.type) >= threatSeverity(ThreatType::BrokenThree));
            sawOpenThree = sawOpenThree || node.type == ThreatType::OpenThree;
            assert(!node.defenseMoves.empty());
            assert(!node.requiredEmpty.empty());
        }
        assert(sawOpenThree);
    }

    {
        // Black has a plausible forcing attack, but White already has an
        // immediate winning counter. Threat-space search must reject the
        // black line and surface the counter as a refutation.
        GameState game = makeGame({
            {7, 7}, {0, 0},
            {7, 8}, {0, 1},
            {10, 10}, {0, 2},
            {10, 11}, {0, 3},
        });
        assert(game.sideToMove() == Player::Black);

        ThreatSequenceConfig config;
        config.maxDepth = 3;
        config.refutationNodeBudget = 2000;
        ThreatSequenceSearcher searcher(config);
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);

        assert(!result.foundWin);
        assert(containsMove(result.refutations, {0, 4}));
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
