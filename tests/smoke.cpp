#include <cassert>
#include <string>

#include "gomoku/AnalystAI.hpp"
#include "gomoku/ClubAI.hpp"
#include "gomoku/ExpertAI.hpp"
#include "gomoku/Match.hpp"
#include "gomoku/ProofSearch.hpp"
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
        GameState game(rulesFor(Ruleset::Swap16));
        assert(game.applyMove({7, 7}));
        assert(game.applyMove({7, 8}));
        assert(game.applyMove({8, 7}));
        assert(game.isSwapDecisionPending());

        const Player sideBefore = game.sideToMove();
        const std::uint64_t hashBefore = game.positionHash();
        game.setSideToMoveForAnalysis(Player::Black);
        assert(game.sideToMove() == sideBefore);
        assert(game.positionHash() == hashBefore);

        assert(game.applySwapChoice(SwapChoice::SwapColors));
        assert(!game.isSwapDecisionPending());
        assert(game.sideToMove() == Player::White);
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
        assert(result.summary.depthReached >= 1);
        assert(result.summary.nodes > 0);
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
        assert(game.applyMove({0, 0}));

        SearchConfig config;
        config.maxDepth = 3;
        config.maxNodes = 5;
        config.timeLimitMs = 500;
        config.maxCandidateMoves = 3;

        SearchEngine engine(config);
        const SearchResult result = engine.search(game);
        assert(result.bestMove.has_value());
        assert(result.summary.depthReached == 1);
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

        ProofAnalysisConfig proofConfig;
        proofConfig.maxDepth = 6;
        proofConfig.maxNodes = 80000;
        proofConfig.timeLimitMs = 500;
        ProofAnalyzer analyzer(proofConfig);
        const ProofAnalysisResult proof = analyzer.analyze(game, Player::Black);
        assert(proof.outcome == ProofOutcome::ProvenWin);
        assert(proof.bestMove.has_value());
        const bool proofWinningMove = (*proof.bestMove == Move {7, 6}) || (*proof.bestMove == Move {7, 11});
        assert(proofWinningMove);
        assert(proof.nodes > 0);

        PositionAnnotation annotation;
        annotation.analysisPlayer = Player::Black;
        annotation.proofOutcome = proof.outcome;
        annotation.proofNodes = proof.nodes;
        annotation.principalVariation = proof.principalVariation;
        for (const ProofMoveSummary& rootMove : proof.rootMoves) {
            if (rootMove.outcome == ProofOutcome::ProvenWin) {
                annotation.provenWinningMoves.push_back(rootMove.move);
            } else if (rootMove.outcome == ProofOutcome::ProvenLoss) {
                annotation.provenLosingMoves.push_back(rootMove.move);
            }
        }

        const std::string annotated = serializeAnnotatedPosition(game, annotation);
        GameState loadedAnnotated;
        PositionAnnotation loadedAnnotation;
        std::string annotationError;
        assert(deserializeAnnotatedPosition(annotated, loadedAnnotated, loadedAnnotation, annotationError));
        assert(loadedAnnotated.positionHash() == game.positionHash());
        assert(loadedAnnotation.analysisPlayer == annotation.analysisPlayer);
        assert(loadedAnnotation.proofOutcome == annotation.proofOutcome);
        assert(loadedAnnotation.principalVariation == annotation.principalVariation);
    }

    {
        ControllerKind controller;
        assert(tryParseController("tactical", controller));
        assert(controller == ControllerKind::TacticalAI);
        assert(tryParseController("expert", controller));
        assert(controller == ControllerKind::ExpertAI);
        assert(tryParseController("analyst", controller));
        assert(controller == ControllerKind::AnalystAI);
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
        assert(result.summary.usedThreatSequence);
        assert(result.threatSequence.has_value());
        assert(result.threatSequence->foundWin);
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
        Match match({Ruleset::Swap16, ControllerKind::Human, ControllerKind::ClubAI});
        assert(match.state().rules().swapOpening);
        assert(match.seatToAct() == Seat::Opener);
        assert(match.applyMove({7, 7}));
        assert(match.applyMove({7, 8}));
        assert(match.applyMove({8, 7}));
        assert(match.seatToAct() == Seat::Chooser);
        assert(match.applySwapChoice(SwapChoice::SwapColors));
        assert(match.seatToAct() == Seat::Opener);
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
        match.stepAi();
        assert(match.state().moveCount() == 1);
        assert(match.state().lastPlacedMove() == (Move{7, 7}));
        assert(match.lastSearchSummary().has_value());
        assert(match.lastSearchSummary()->usedOpeningBook);
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
        assert(match.lastSearchSummary()->usedThreatSequence);
        assert(match.lastThreatSequence().has_value());
        assert(match.lastThreatSequence()->foundWin);
    }

    {
        Match match({Ruleset::Freestyle15, ControllerKind::AnalystAI, ControllerKind::Human});
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
        assert(match.lastProofAnalysis().has_value());
        assert(match.lastProofAnalysis()->outcome == ProofOutcome::ProvenWin);
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

    return 0;
}
