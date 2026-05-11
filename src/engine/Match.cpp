#include "gomoku/Match.hpp"

#include <algorithm>
#include <chrono>

#include "gomoku/Threats.hpp"
#include "gomoku/TimeGovernor.hpp"

namespace gomoku {

namespace {

using SteadyClock = std::chrono::steady_clock;

void applySearchBudgetHeuristics(SearchConfig& config, int budgetMs, int softBudgetMs = 0) {
    config.timeLimitMs = std::max(1, budgetMs);
    config.softTimeLimitMs = softBudgetMs > 0
        ? std::clamp(softBudgetMs, 1, config.timeLimitMs)
        : std::max(1, config.timeLimitMs * 3 / 4);

    if (config.timeLimitMs >= 10000) {
        config.maxDepth = 64;
    } else if (config.timeLimitMs >= 5000) {
        config.maxDepth = 50;
    } else if (config.timeLimitMs >= 2000) {
        config.maxDepth = 40;
    } else if (config.timeLimitMs >= 1000) {
        config.maxDepth = 30;
    } else if (config.timeLimitMs >= 500) {
        config.maxDepth = 24;
    } else {
        config.maxDepth = 18;
    }

    const std::uint64_t timeFactor = static_cast<std::uint64_t>(config.timeLimitMs);
    config.maxNodes = std::max<std::uint64_t>(500'000ULL, timeFactor * 1500ULL);
}

}  // namespace

Match::Match(MatchConfig config)
    : config_(config)
    , state_(rulesFor(config.ruleset)) {
    resetAiClocks();
}

void Match::reset() {
    state_.reset();
    clockState_ = {};
    resolvedSwapChoice_.reset();
    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    history_.clear();
    resetAiClocks();
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

void Match::setSearchThreads(int searchThreads) {
    config_.searchThreads = std::max(0, searchThreads);
}

void Match::setAiTimeControlPreset(AiTimeControlPreset preset) {
    config_.aiTimeControlPreset = preset;
    resetAiClocks();
}

void Match::setAiClockState(Seat seat, std::int64_t timeLeftMs, int movesPlayedInPeriod) {
    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    SeatAiClockState& seatClock = aiClockState(seat);
    if (spec.periodTimeMs <= 0 || spec.periodMoves <= 0) {
        seatClock.timeLeftMs = timeLeftMs;
        seatClock.movesPlayedInPeriod = std::max(0, movesPlayedInPeriod);
        return;
    }

    seatClock.timeLeftMs = std::clamp<std::int64_t>(timeLeftMs, 0, spec.periodTimeMs);
    seatClock.movesPlayedInPeriod = std::clamp(movesPlayedInPeriod, 0, spec.periodMoves - 1);
}

void Match::clearUndoHistory() {
    history_.clear();
}

const ClockState& Match::clockState() const {
    return clockState_;
}

void Match::setClockState(const ClockState& clockState) {
    clockState_ = clockState;
}

std::optional<MatchAiClockView> Match::aiClockForSeat(Seat seat) const {
    if (!timeControlActiveForSeat(seat)) {
        return std::nullopt;
    }

    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    const SeatAiClockState& seatClock = aiClockState(seat);
    MatchAiClockView view;
    view.active = true;
    view.timeLeftMs = seatClock.timeLeftMs;
    view.periodTimeMs = spec.periodTimeMs;
    view.movesPlayedInPeriod = seatClock.movesPlayedInPeriod;
    view.movesToReset = std::max(0, spec.periodMoves - seatClock.movesPlayedInPeriod);
    return view;
}

bool Match::applyMove(Move move) {
    if (!state_.isLegalMove(move)) {
        return false;
    }
    pushSnapshot();
    if (!state_.applyMove(move)) {
        restoreLastSnapshot();
        return false;
    }
    return true;
}

bool Match::applySwapChoice(SwapChoice choice) {
    pushSnapshot();
    const bool applied = state_.applySwapChoice(choice);
    if (applied) {
        resolvedSwapChoice_ = choice;
    } else {
        restoreLastSnapshot();
    }
    return applied;
}

bool Match::undo() {
    if (history_.empty()) {
        return false;
    }
    const bool undone = state_.undo();
    if (undone) {
        restoreLastSnapshot();
    }
    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    return undone;
}

bool Match::smartUndo() {
    if (state_.actionCount() == 0) {
        return false;
    }

    const bool onlyOneSeatIsHuman =
        (config_.openerController == ControllerKind::Human) != (config_.chooserController == ControllerKind::Human);

    if (!onlyOneSeatIsHuman) {
        return undo();
    }

    const ControllerKind humanSeat = (config_.openerController == ControllerKind::Human)
        ? ControllerKind::Human : config_.chooserController;

    int undos = 0;
    for (int i = 0; i < 2 && state_.actionCount() > 0; ++i) {
        if (!undo()) {
            break;
        }
        ++undos;
        if (controllerToAct() == humanSeat) {
            break;
        }
    }
    return undos > 0;
}

std::optional<Move> Match::chooseAiMove() const {
    return searchAiTurn().bestMove;
}

SearchResult Match::searchAiTurn(const std::function<void(const SearchSummary&)>& progressCallback) const {
    const SearchConfig searchConfig = makeSearchConfig();
    SearchConfig configWithProgress = searchConfig;
    configWithProgress.progressCallback = progressCallback;
    switch (controllerToAct()) {
        case ControllerKind::RookieAI:
            return {};
        case ControllerKind::ClubAI:
            return ClubAI::chooseMove(state_, state_.sideToMove(), configWithProgress);
        case ControllerKind::TacticalAI:
            return TacticalAI::chooseMove(state_, state_.sideToMove(), configWithProgress);
        case ControllerKind::ExpertAI:
            return ExpertAI::chooseMove(state_, state_.sideToMove(), configWithProgress);
        case ControllerKind::Human:
        default:
            return {};
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
        case ControllerKind::Human:
        default:
            return SwapChoice::KeepColors;
    }
}

bool Match::commitAiSearchResult(const SearchResult& result, std::int64_t elapsedMs) {
    if (!isAiTurn() || state_.isGameOver() || state_.isSwapDecisionPending() || controllerToAct() == ControllerKind::Human) {
        return false;
    }

    const Seat actingSeat = seatToAct();
    lastSearchSummary_ = result.summary;
    lastThreatSequence_ = result.threatSequence;
    if (!result.bestMove.has_value()) {
        return false;
    }

    pushSnapshot();
    chargeAiClock(actingSeat, elapsedMs, true);
    if (!state_.applyMove(*result.bestMove)) {
        restoreLastSnapshot();
        return false;
    }
    return true;
}

bool Match::commitAiSwapChoice(SwapChoice choice, std::int64_t elapsedMs) {
    if (!isAiTurn() || state_.isGameOver() || !state_.isSwapDecisionPending()) {
        return false;
    }

    const Seat actingSeat = seatToAct();
    pushSnapshot();
    chargeAiClock(actingSeat, elapsedMs, false);
    if (!state_.applySwapChoice(choice)) {
        restoreLastSnapshot();
        return false;
    }
    resolvedSwapChoice_ = choice;
    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    return true;
}

void Match::stepAi() {
    if (!isAiTurn() || state_.isGameOver()) {
        return;
    }

    if (state_.isSwapDecisionPending()) {
        const auto start = SteadyClock::now();
        const SwapChoice choice = chooseAiSwapChoice();
        const auto end = SteadyClock::now();
        commitAiSwapChoice(choice, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        return;
    }

    if (controllerToAct() == ControllerKind::ClubAI || controllerToAct() == ControllerKind::TacticalAI
        || controllerToAct() == ControllerKind::ExpertAI) {
        const auto start = SteadyClock::now();
        const SearchResult result = searchAiTurn();
        const auto end = SteadyClock::now();
        commitAiSearchResult(result, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        return;
    }

    const Seat actingSeat = seatToAct();
    lastSearchSummary_.reset();
    lastThreatSequence_.reset();
    const auto start = SteadyClock::now();
    if (const std::optional<Move> move = RookieAI::chooseMove(state_, state_.sideToMove())) {
        const auto end = SteadyClock::now();
        pushSnapshot();
        chargeAiClock(actingSeat,
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(),
            true);
        if (!state_.applyMove(*move)) {
            restoreLastSnapshot();
        }
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

Seat Match::seatForStone(Player player) const {
    if (!state_.rules().swapOpening || !resolvedSwapChoice_.has_value()) {
        return player == Player::Black ? Seat::Opener : Seat::Chooser;
    }

    if (*resolvedSwapChoice_ == SwapChoice::KeepColors) {
        return player == Player::Black ? Seat::Opener : Seat::Chooser;
    }

    return player == Player::Black ? Seat::Chooser : Seat::Opener;
}

std::size_t Match::seatIndex(Seat seat) {
    return seat == Seat::Opener ? 0U : 1U;
}

bool Match::timeControlActive() const {
    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    return spec.periodTimeMs > 0 && spec.periodMoves > 0;
}

bool Match::timeControlActiveForSeat(Seat seat) const {
    return timeControlActive() && controllerForSeat(seat) != ControllerKind::Human;
}

void Match::resetAiClocks() {
    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    for (SeatAiClockState& seatClock : aiClocks_) {
        seatClock.timeLeftMs = spec.periodTimeMs > 0 ? spec.periodTimeMs : -1;
        seatClock.movesPlayedInPeriod = 0;
    }
}

Match::MatchSnapshot Match::snapshot() const {
    MatchSnapshot current;
    current.aiClocks = aiClocks_;
    current.resolvedSwapChoice = resolvedSwapChoice_;
    return current;
}

void Match::restoreSnapshot(const MatchSnapshot& snapshot) {
    aiClocks_ = snapshot.aiClocks;
    resolvedSwapChoice_ = snapshot.resolvedSwapChoice;
}

void Match::pushSnapshot() {
    history_.push_back(snapshot());
}

bool Match::restoreLastSnapshot() {
    if (history_.empty()) {
        return false;
    }
    restoreSnapshot(history_.back());
    history_.pop_back();
    return true;
}

Match::SeatAiClockState& Match::aiClockState(Seat seat) {
    return aiClocks_[seatIndex(seat)];
}

const Match::SeatAiClockState& Match::aiClockState(Seat seat) const {
    return aiClocks_[seatIndex(seat)];
}

std::optional<ClockState> Match::effectiveClockStateForTurn() const {
    if (clockState_.hasGameClock()) {
        return clockState_;
    }

    const Seat actingSeat = seatToAct();
    if (!timeControlActiveForSeat(actingSeat)) {
        return std::nullopt;
    }

    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    const SeatAiClockState& seatClock = aiClockState(actingSeat);
    ClockState effective;
    effective.timeLeftMs = seatClock.timeLeftMs;
    effective.timeoutMatchMs = spec.periodTimeMs;
    effective.movesToReset = std::max(1, spec.periodMoves - seatClock.movesPlayedInPeriod);
    return effective;
}

void Match::chargeAiClock(Seat seat, std::int64_t elapsedMs, bool countsAsPeriodMove) {
    if (!timeControlActiveForSeat(seat)) {
        return;
    }

    const AiTimeControlSpec spec = aiTimeControlSpec(config_.aiTimeControlPreset);
    SeatAiClockState& seatClock = aiClockState(seat);
    if (seatClock.timeLeftMs < 0) {
        seatClock.timeLeftMs = spec.periodTimeMs;
    }
    seatClock.timeLeftMs = std::max<std::int64_t>(0, seatClock.timeLeftMs - std::max<std::int64_t>(0, elapsedMs));
    if (!countsAsPeriodMove) {
        return;
    }

    ++seatClock.movesPlayedInPeriod;
    if (seatClock.movesPlayedInPeriod >= spec.periodMoves) {
        seatClock.timeLeftMs = spec.periodTimeMs;
        seatClock.movesPlayedInPeriod = 0;
    }
}

SearchConfig Match::makeSearchConfig() const {
    SearchConfig config;
    config.maxRootThreads = config_.searchThreads;
    config.useOpeningBook = config_.openingBookEnabled;
    config.useNullMovePruning = config_.nullMovePruningEnabled;
    config.useDefensiveFiltering = config_.defensiveFilteringEnabled;
    config.useStrictDefenseFiltering = config_.strictDefenseFiltering && config.useDefensiveFiltering;
    config.disableOpeningBook = !config_.openingBookEnabled;
    config.disableNullMovePruning = !config_.nullMovePruningEnabled;
    config.disableDefensiveFiltering = !config_.defensiveFilteringEnabled;
    config.compareNoDefFilterSearch = config_.compareNoDefFilterSearch;
    config.nextIterBranchingEstimate = config_.nextIterBranchingEstimate;
    if (const auto effectiveClock = effectiveClockStateForTurn()) {
        config.clock = *effectiveClock;
        config.clock.moveNumber = static_cast<std::uint32_t>(state_.moveCount());

        if (config.clock.hasGameClock()) {
            TimeGovernorConfig governorConfig;
            if (config.nextIterBranchingEstimate > 0.0) {
                governorConfig.nextIterBranchingEstimate = config.nextIterBranchingEstimate;
            }

            TimeGovernor governor;
            const std::optional<MoveBudget> budget =
                governor.computeBaselineBudget(config.clock, governorConfig, assessRootThreats(state_));
            if (budget.has_value()) {
                applySearchBudgetHeuristics(config,
                    static_cast<int>(budget->hardCapMs),
                    static_cast<int>(budget->targetMs));
                return config;
            }
        }
    }

    applySearchBudgetHeuristics(config, std::max(1, config_.aiMoveTimeMs));
    config.clock.moveNumber = static_cast<std::uint32_t>(state_.moveCount());
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
