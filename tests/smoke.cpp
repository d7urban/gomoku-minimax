#include <array>
#include <string>

#include "TestAssert.hpp"
#include "gomoku/ClubAI.hpp"
#include "gomoku/ExpertAI.hpp"
#include "gomoku/Match.hpp"
#include "gomoku/OpeningBook.hpp"
#include "gomoku/Replay.hpp"
#include "gomoku/TacticalAI.hpp"
#include "gomoku/ThreatSearch.hpp"

int main() {
    using namespace gomoku;

    const auto hasImmediateWin = [](const GameState& state, Player player) {
        for (const Move& move : state.legalMoves()) {
            GameState trial = state;
            if (!trial.applyMove(move)) {
                continue;
            }
            const bool won = (player == Player::Black && trial.result() == GameResult::BlackWin)
                || (player == Player::White && trial.result() == GameResult::WhiteWin);
            if (won) {
                return true;
            }
        }
        return false;
    };

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({1, 0}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({1, 1}));
        assert(game.applyMove({7, 10}));
        assert(game.applyMove({1, 2}));
        assert(game.applyMove({7, 11}));
        assert(game.result() == GameResult::BlackWin);
        assert(game.sideToMove() == Player::White);
    }

    {
        GameState game(rulesFor(Ruleset::Standard15));
        assert(game.applyMove({7, 4}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 5}));
        assert(game.applyMove({0, 1}));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 2}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 3}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({1, 3}));

        const Move overlineMove {7, 6};
        const MoveThreatInfo overlineThreat = StaticEvaluator::analyzeMove(game, overlineMove, Player::Black);
        assert(overlineThreat.best != ThreatType::Five);

        ThreatSequenceSearcher searcher;
        bool foundFalseFive = false;
        for (const auto& threat : searcher.enumerateThreats(game, Player::Black)) {
            if (threat.move == overlineMove && threat.type == ThreatType::Five) {
                foundFalseFive = true;
            }
        }
        assert(!foundFalseFive);

        assert(game.applyMove(overlineMove));
        assert(game.result() == GameResult::Ongoing);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({6, 8}));
        assert(game.applyMove({8, 8}));
        assert(game.applyMove({5, 9}));
        const std::uint64_t hashBefore = game.positionHash();

        assert(game.lineBits(Player::Black, 0, 7) == static_cast<std::uint16_t>(1U << 7));
        assert(game.lineBits(Player::Black, 0, 8) == static_cast<std::uint16_t>(1U << 8));
        assert(game.lineBits(Player::White, 0, 6) == static_cast<std::uint16_t>(1U << 8));
        assert(game.lineBits(Player::White, 0, 5) == static_cast<std::uint16_t>(1U << 9));
        assert(game.lineBits(Player::Black, 2, 14) == static_cast<std::uint16_t>((1U << 7) | (1U << 8)));

        const Move focus {7, 9};
        assert(game.cellAt(focus.row, focus.col) == Player::None);
        assert(StaticEvaluator::analyzeMove(game, focus, Player::Black) == computeMoveThreatInfo(game, focus, Player::Black));
        assert(StaticEvaluator::analyzeMove(game, focus, Player::White) == computeMoveThreatInfo(game, focus, Player::White));

        const int evalBefore = StaticEvaluator::evaluate(game, Player::Black);
        assert(game.applyMove({7, 9}));
        assert(game.undo());
        assert(StaticEvaluator::evaluate(game, Player::Black) == evalBefore);
        assert(game.positionHash() == hashBefore);
        assert(StaticEvaluator::analyzeMove(game, focus, Player::Black) == computeMoveThreatInfo(game, focus, Player::Black));
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 1}));

        ThreatSequenceSearcher searcher;
        const auto threats = searcher.enumerateThreats(game, Player::Black);
        assert(game.hasThreatAtLeast(Player::Black, ThreatType::OpenThree));
        bool foundOpenThree = false;
        for (const auto& threat : threats) {
            if (threat.move == Move {7, 9} && threat.type == ThreatType::OpenThree) {
                foundOpenThree = true;
                assert(!threat.continuationMoves.empty());
                assert(!threat.requiredEmpty.empty());
            }
        }
        assert(foundOpenThree);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({7, 6}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({0, 1}));
        assert(game.applyMove({7, 10}));
        const auto move = RookieAI::chooseMove(game, Player::White);
        assert(move.has_value());
        GameState trial = game;
        assert(trial.applyMove(*move));
        const bool blocksImmediateWin = !hasImmediateWin(trial, Player::Black);
        assert(blocksImmediateWin);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({7, 6}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({1, 0}));
        assert(game.applyMove({7, 10}));
        SearchConfig config;
        config.maxDepth = 3;
        config.maxNodes = 30000;
        config.timeLimitMs = 500;
        config.maxCandidateMoves = 12;
        const SearchResult result = ClubAI::chooseMove(game, Player::White, config);
        assert(result.bestMove.has_value());
        GameState trial = game;
        assert(trial.applyMove(*result.bestMove));
        assert(!hasImmediateWin(trial, Player::Black));
        assert(result.summary.nodes == 0 || result.summary.depthReached >= 1);
    }

    {
        MatchConfig enabledConfig;
        enabledConfig.ruleset = Ruleset::Freestyle15;
        enabledConfig.openerController = ControllerKind::Human;
        enabledConfig.chooserController = ControllerKind::ClubAI;
        enabledConfig.aiMoveTimeMs = 200;
        enabledConfig.openingBookEnabled = false;
        Match enabled(enabledConfig);
        assert(enabled.applyMove({7, 7}));
        assert(enabled.applyMove({0, 0}));
        assert(enabled.applyMove({7, 8}));
        assert(enabled.applyMove({1, 0}));
        assert(enabled.applyMove({7, 9}));
        assert(enabled.applyMove({2, 0}));
        assert(enabled.applyMove({7, 10}));
        SearchResult enabledResult = enabled.searchAiTurn();
        assert(enabledResult.bestMove.has_value());
        assert(enabledResult.summary.defFilterApplied);

        MatchConfig disabledConfig = enabledConfig;
        disabledConfig.defensiveFilteringEnabled = false;
        Match disabled(disabledConfig);
        assert(disabled.applyMove({7, 7}));
        assert(disabled.applyMove({0, 0}));
        assert(disabled.applyMove({7, 8}));
        assert(disabled.applyMove({1, 0}));
        assert(disabled.applyMove({7, 9}));
        assert(disabled.applyMove({2, 0}));
        assert(disabled.applyMove({7, 10}));
        SearchResult disabledResult = disabled.searchAiTurn();
        assert(disabledResult.bestMove.has_value());
        assert(!disabledResult.summary.defFilterApplied);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        const SearchResult result = ExpertAI::chooseMove(game, Player::Black);
        assert(result.bestMove == (Move{7, 7}));
        assert(result.summary.score == StaticEvaluator::evaluate(game, Player::Black));
        assert(result.summary.usedOpeningBook);
        assert(result.summary.openingBookName == "center_anchor");
        assert(result.summary.principalVariation.size() == 1U);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({6, 7}));
        const auto hit = lookupOpeningBookMove(game);
        assert(hit.has_value());
        assert(hit->lineName == "diagonal_clamp");
        assert(hit->move == (Move{6, 8}));
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({7, 6}));
        const auto hit = lookupOpeningBookMove(game);
        assert(hit.has_value());
        assert(hit->lineName == "diagonal_clamp");
        assert(hit->move == (Move{6, 6}));
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({6, 8}));
        const auto hit = lookupOpeningBookMove(game);
        assert(hit.has_value());
        assert(hit->lineName == "diagonal_split");
        assert(hit->move == (Move{8, 8}));
    }

    {
        // Imported Crazy-Sensei entry cs0002 has prefix (4,5) with reply (3,5).
        // Exercise each of the 8 D4 symmetries: every transformed first move
        // must produce the correspondingly transformed reply via the book.
        struct SymmetryCase {
            Move firstMove;
            Move expectedReply;
        };
        const std::array<SymmetryCase, 8> cases = {{
            {{4, 5}, {3, 5}},   // Identity
            {{5, 10}, {5, 11}}, // Rot90
            {{10, 9}, {11, 9}}, // Rot180
            {{9, 4}, {9, 3}},   // Rot270
            {{4, 9}, {3, 9}},   // MirrorVertical
            {{10, 5}, {11, 5}}, // MirrorHorizontal
            {{5, 4}, {5, 3}},   // MirrorMainDiagonal
            {{9, 10}, {9, 11}}, // MirrorAntiDiagonal
        }};
        for (const SymmetryCase& test : cases) {
            GameState game(rulesFor(Ruleset::Freestyle15));
            assert(game.applyMove(test.firstMove));
            const auto hit = lookupOpeningBookMove(game);
            assert(hit.has_value());
            assert(hit->lineName.substr(0, 2) == "cs");
            assert(hit->move == test.expectedReply);
        }
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));

        SearchConfig config;
        config.maxDepth = 3;
        config.maxNodes = 5;
        config.timeLimitMs = 500;
        config.maxCandidateMoves = 3;

        SearchEngine engine(config);
        const SearchResult result = engine.search(game);
        assert(result.bestMove.has_value());
        assert(result.summary.depthReached <= 1);
        assert(result.summary.maxDepthVisited >= 1);
        assert(result.summary.nodes <= config.maxNodes);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 1}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({0, 2}));
        assert(game.applyMove({7, 10}));
        assert(game.applyMove({0, 3}));

        ThreatSequenceSearcher searcher;
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);
        assert(result.foundWin);
        assert(!result.sequence.empty());
        const bool foundWinningMove = (result.sequence.front().move == Move {7, 6}) || (result.sequence.front().move == Move {7, 11});
        assert(foundWinningMove);
        assert(result.nodes > 0);

        PositionAnnotation annotation;
        annotation.analysisPlayer = Player::Black;
        annotation.label = "threat_win";
        annotation.principalVariation = {result.sequence.front().move};

        const std::string annotated = serializeAnnotatedPosition(game, annotation);
        GameState loadedAnnotated;
        PositionAnnotation loadedAnnotation;
        std::string annotationError;
        assert(deserializeAnnotatedPosition(annotated, loadedAnnotated, loadedAnnotation, annotationError));
        assert(loadedAnnotated.positionHash() == game.positionHash());
        assert(loadedAnnotation.analysisPlayer == annotation.analysisPlayer);
        assert(loadedAnnotation.label == annotation.label);
        assert(loadedAnnotation.principalVariation == annotation.principalVariation);
    }

    {
        ControllerKind controller;
        assert(tryParseController("tactical", controller));
        assert(controller == ControllerKind::TacticalAI);
        assert(tryParseController("expert", controller));
        assert(controller == ControllerKind::ExpertAI);
        assert(tryParseController("analyst", controller));
        assert(controller == ControllerKind::ExpertAI);
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 1}));
        assert(game.applyMove({7, 9}));
        assert(game.applyMove({0, 2}));
        assert(game.applyMove({7, 10}));
        assert(game.applyMove({0, 3}));

        const SearchResult result = TacticalAI::chooseMove(game, Player::Black);
        assert(result.bestMove.has_value());
        const bool foundWinningMove = (*result.bestMove == Move {7, 6}) || (*result.bestMove == Move {7, 11});
        assert(foundWinningMove);
        assert(result.summary.usedThreatSequence || result.summary.score >= 1'000'000);
        if (result.summary.usedThreatSequence) {
            assert(result.threatSequence.has_value());
            assert(result.threatSequence->foundWin);
        }
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({0, 1}));
        assert(game.applyMove({8, 7}));
        assert(game.applyMove({0, 2}));
        assert(game.applyMove({8, 8}));
        assert(game.applyMove({0, 3}));

        ThreatSequenceSearcher searcher;
        const ThreatSearchResult result = searcher.searchWinningSequence(game, Player::Black);
        assert(!result.foundWin);
    }

    {
        Match match({Ruleset::Freestyle15, ControllerKind::Human, ControllerKind::ClubAI});
        assert(match.applyMove({7, 7}));
        match.stepAi();
        assert(match.state().moveCount() == 2);
        assert(match.lastSearchSummary().has_value());
    }

    {
        Match match({Ruleset::Freestyle15, ControllerKind::ExpertAI, ControllerKind::Human, 250});
        assert(match.config().aiMoveTimeMs == 250);
        match.setSearchThreads(4);
        assert(match.config().searchThreads == 4);
        match.setSearchThreads(-1);
        assert(match.config().searchThreads == 0);
        match.stepAi();
        assert(match.state().moveCount() == 1);
        assert(match.state().lastPlacedMove() == (Move{7, 7}));
        assert(match.lastSearchSummary().has_value());
        assert(match.lastSearchSummary()->usedOpeningBook);
    }

    {
        MatchConfig config;
        config.ruleset = Ruleset::Freestyle15;
        config.openerController = ControllerKind::ExpertAI;
        config.chooserController = ControllerKind::Human;
        config.aiTimeControlPreset = AiTimeControlPreset::Blitz;

        Match match(config);
        const auto initialClock = match.aiClockForSeat(Seat::Opener);
        assert(initialClock.has_value());
        assert(initialClock->timeLeftMs == 5LL * 60LL * 1000LL);
        assert(initialClock->movesToReset == 40);

        match.stepAi();
        const auto afterMoveClock = match.aiClockForSeat(Seat::Opener);
        assert(afterMoveClock.has_value());
        assert(afterMoveClock->movesToReset == 39);
        assert(afterMoveClock->timeLeftMs >= 0);
        assert(afterMoveClock->timeLeftMs <= initialClock->timeLeftMs);

        assert(match.undo());
        const auto restoredClock = match.aiClockForSeat(Seat::Opener);
        assert(restoredClock.has_value());
        assert(restoredClock->timeLeftMs == initialClock->timeLeftMs);
        assert(restoredClock->movesToReset == initialClock->movesToReset);
    }

    {
        Match match({Ruleset::Freestyle15, ControllerKind::TacticalAI, ControllerKind::Human});
        assert(match.applyMove({7, 7}));
        assert(match.applyMove({0, 0}));
        assert(match.applyMove({7, 8}));
        assert(match.applyMove({0, 1}));
        assert(match.applyMove({7, 9}));
        assert(match.applyMove({0, 2}));
        assert(match.applyMove({7, 10}));
        assert(match.applyMove({0, 3}));
        match.stepAi();
        assert(match.state().isGameOver());
        assert(match.lastSearchSummary().has_value());
        assert(match.lastSearchSummary()->usedThreatSequence || match.lastSearchSummary()->score >= 1'000'000);
        if (match.lastSearchSummary()->usedThreatSequence) {
            assert(match.lastThreatSequence().has_value());
            assert(match.lastThreatSequence()->foundWin);
        }
    }

    {
        Match match({Ruleset::Freestyle15, ControllerKind::ExpertAI, ControllerKind::Human});
        assert(match.applyMove({7, 7}));
        assert(match.applyMove({0, 0}));
        assert(match.applyMove({7, 8}));
        assert(match.applyMove({0, 1}));
        assert(match.applyMove({7, 9}));
        assert(match.applyMove({0, 2}));
        assert(match.applyMove({7, 10}));
        assert(match.applyMove({0, 3}));
        match.stepAi();
        assert(match.state().isGameOver());
        assert(match.lastSearchSummary().has_value());
    }

    {
        GameState game(rulesFor(Ruleset::Freestyle15));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({0, 0}));
        const std::string replay = serializeReplay(game);
        GameState loaded(rulesFor(Ruleset::Freestyle15));
        std::string error;
        assert(deserializeReplay(rulesFor(Ruleset::Freestyle15), replay, loaded, error));
        assert(loaded.moveCount() == game.moveCount());
        assert(loaded.cellAt(7, 7) == Player::Black);
        assert(loaded.cellAt(0, 0) == Player::White);
        assert(StaticEvaluator::evaluate(loaded, Player::Black) == StaticEvaluator::evaluate(game, Player::Black));
        assert(loaded.positionHash() == game.positionHash());
    }

    {
        MatchConfig config;
        config.ruleset = Ruleset::Freestyle15;
        config.openerController = ControllerKind::Human;
        config.chooserController = ControllerKind::ExpertAI;
        config.searchThreads = 4;
        config.aiTimeControlPreset = AiTimeControlPreset::Blitz;

        Match match(config);
        assert(match.applyMove({7, 7}));
        match.stepAi();
        const auto chooserClock = match.aiClockForSeat(Seat::Chooser);
        assert(chooserClock.has_value());

        const std::string session = serializeMatchSession(match);
        Match loaded;
        std::string error;
        assert(deserializeMatchSession(session, loaded, error));
        assert(loaded.config().ruleset == match.config().ruleset);
        assert(loaded.config().openerController == match.config().openerController);
        assert(loaded.config().chooserController == match.config().chooserController);
        assert(loaded.config().searchThreads == match.config().searchThreads);
        assert(loaded.config().aiTimeControlPreset == match.config().aiTimeControlPreset);
        assert(loaded.state().positionHash() == match.state().positionHash());
        const auto loadedChooserClock = loaded.aiClockForSeat(Seat::Chooser);
        assert(loadedChooserClock.has_value());
        assert(loadedChooserClock->timeLeftMs == chooserClock->timeLeftMs);
        assert(loadedChooserClock->movesToReset == chooserClock->movesToReset);
    }

    return 0;
}
