#include "gomoku/Match.hpp"

#include <algorithm>

namespace gomoku {

Match::Match(MatchConfig config)
    : config_(config)
    , state_(rulesFor(config.ruleset)) {
}

void Match::reset() {
    state_.reset();
    resolvedSwapChoice_.reset();
    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    lastProofAnalysis_.reset();
}

const MatchConfig& Match::config() const {
    return config_;
}

const GameState& Match::state() const {
    return state_;
}

GameState& Match::state() {
    return state_;
}

Seat Match::seatToAct() const {
    if (state_.rules().swapOpening) {
        if (state_.actionCount() < 3) {
            return Seat::Opener;
        }
        if (state_.isSwapDecisionPending()) {
            return Seat::Chooser;
        }
    }

    return seatForStone(state_.sideToMove());
}

ControllerKind Match::controllerForSeat(Seat seat) const {
    return seat == Seat::Opener ? config_.openerController : config_.chooserController;
}

ControllerKind Match::controllerToAct() const {
    return controllerForSeat(seatToAct());
}

bool Match::isHumanTurn() const {
    return controllerToAct() == ControllerKind::Human;
}

bool Match::isAiTurn() const {
    return controllerToAct() != ControllerKind::Human;
}

void Match::setAiMoveTimeMs(int aiMoveTimeMs) {
    config_.aiMoveTimeMs = std::max(1, aiMoveTimeMs);
}

bool Match::applyMove(Move move) {
    return state_.applyMove(move);
}

bool Match::applySwapChoice(SwapChoice choice) {
    const bool applied = state_.applySwapChoice(choice);
    if (applied) {
        resolvedSwapChoice_ = choice;
    }
    return applied;
}

bool Match::undo() {
    const bool undone = state_.undo();
    if (undone) {
        recomputeSwapChoice();
    }
    return undone;
}

std::optional<Move> Match::chooseAiMove() const {
    const SearchConfig searchConfig = makeSearchConfig();
    switch (controllerToAct()) {
        case ControllerKind::RookieAI:
            return RookieAI::chooseMove(state_, state_.sideToMove());
        case ControllerKind::ClubAI:
            return ClubAI::chooseMove(state_, state_.sideToMove(), searchConfig).bestMove;
        case ControllerKind::TacticalAI:
            return TacticalAI::chooseMove(state_, state_.sideToMove(), searchConfig).bestMove;
        case ControllerKind::ExpertAI:
            return ExpertAI::chooseMove(state_, state_.sideToMove(), searchConfig).bestMove;
        case ControllerKind::AnalystAI:
            return AnalystAI::chooseMove(state_, state_.sideToMove(), searchConfig).bestMove;
        case ControllerKind::Human:
        default:
            return std::nullopt;
    }
}

SwapChoice Match::chooseAiSwapChoice() const {
    switch (controllerToAct()) {
        case ControllerKind::RookieAI:
            return RookieAI::chooseSwapChoice(state_);
        case ControllerKind::ClubAI:
            return ClubAI::chooseSwapChoice(state_);
        case ControllerKind::TacticalAI:
            return TacticalAI::chooseSwapChoice(state_);
        case ControllerKind::ExpertAI:
            return ExpertAI::chooseSwapChoice(state_);
        case ControllerKind::AnalystAI:
            return AnalystAI::chooseSwapChoice(state_);
        case ControllerKind::Human:
        default:
            return SwapChoice::KeepColors;
    }
}

void Match::stepAi() {
    if (!isAiTurn() || state_.isGameOver()) {
        return;
    }

    if (state_.isSwapDecisionPending()) {
        applySwapChoice(chooseAiSwapChoice());
        lastSearchSummary_.reset();
        lastThreatSequence_.reset();
        lastProofAnalysis_.reset();
        return;
    }

    if (controllerToAct() == ControllerKind::ClubAI || controllerToAct() == ControllerKind::TacticalAI
        || controllerToAct() == ControllerKind::ExpertAI || controllerToAct() == ControllerKind::AnalystAI) {
        const SearchConfig searchConfig = makeSearchConfig();
        SearchResult result;
        if (controllerToAct() == ControllerKind::AnalystAI) {
            result = AnalystAI::chooseMove(state_, state_.sideToMove(), searchConfig);
        } else if (controllerToAct() == ControllerKind::ExpertAI) {
            result = ExpertAI::chooseMove(state_, state_.sideToMove(), searchConfig);
        } else if (controllerToAct() == ControllerKind::TacticalAI) {
            result = TacticalAI::chooseMove(state_, state_.sideToMove(), searchConfig);
        } else {
            result = ClubAI::chooseMove(state_, state_.sideToMove(), searchConfig);
        }
        lastSearchSummary_ = result.summary;
        lastThreatSequence_ = result.threatSequence;
        lastProofAnalysis_ = result.proofAnalysis;
        if (result.bestMove.has_value()) {
            applyMove(*result.bestMove);
        }
        return;
    }

    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    lastProofAnalysis_.reset();
    if (const std::optional<Move> move = RookieAI::chooseMove(state_, state_.sideToMove())) {
        applyMove(*move);
    }
}

std::optional<SwapChoice> Match::resolvedSwapChoice() const {
    return resolvedSwapChoice_;
}

const std::optional<SearchSummary>& Match::lastSearchSummary() const {
    return lastSearchSummary_;
}

const std::optional<ThreatSearchResult>& Match::lastThreatSequence() const {
    return lastThreatSequence_;
}

const std::optional<ProofAnalysisResult>& Match::lastProofAnalysis() const {
    return lastProofAnalysis_;
}

Seat Match::seatForStone(Player player) const {
    if (!state_.rules().swapOpening || !resolvedSwapChoice_.has_value()) {
        return player == Player::Black ? Seat::Opener : Seat::Chooser;
    }

    if (*resolvedSwapChoice_ == SwapChoice::KeepColors) {
        return player == Player::Black ? Seat::Opener : Seat::Chooser;
    }

    return player == Player::Black ? Seat::Chooser : Seat::Opener;
}

SearchConfig Match::makeSearchConfig() const {
    SearchConfig config;
    config.timeLimitMs = std::max(1, config_.aiMoveTimeMs);
    config.softTimeLimitMs = std::max(1, config.timeLimitMs * 3 / 4);
    config.maxNodes = std::max<std::uint64_t>(90000, static_cast<std::uint64_t>(config.timeLimitMs) * 1200ULL);
    return config;
}

void Match::recomputeSwapChoice() {
    resolvedSwapChoice_.reset();
    for (const Action& action : state_.actions()) {
        if (action.kind == Action::Kind::SwapChoice) {
            resolvedSwapChoice_ = action.swapChoice;
        }
    }
}

}  // namespace gomoku
