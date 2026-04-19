#pragma once

#include <optional>

#include "gomoku/AnalystAI.hpp"
#include "gomoku/ClockState.hpp"
#include "gomoku/ClubAI.hpp"
#include "gomoku/ExpertAI.hpp"
#include "gomoku/ProofSearch.hpp"
#include "gomoku/RookieAI.hpp"
#include "gomoku/TacticalAI.hpp"

namespace gomoku {

struct MatchConfig {
    Ruleset ruleset {Ruleset::Freestyle15};
    ControllerKind openerController {ControllerKind::Human};
    ControllerKind chooserController {ControllerKind::AnalystAI};
    int aiMoveTimeMs {500};
};

class Match {
public:
    explicit Match(MatchConfig config = {});

    void reset();

    const MatchConfig& config() const;
    const GameState& state() const;
    GameState& state();

    Seat seatToAct() const;
    ControllerKind controllerForSeat(Seat seat) const;
    ControllerKind controllerToAct() const;

    bool isHumanTurn() const;
    bool isAiTurn() const;

    void setAiMoveTimeMs(int aiMoveTimeMs);

    // Clock state observed from the match protocol. The gomocup adapter
    // (and any future analogue) is expected to update this whenever a
    // clock-bearing protocol message arrives; Match is the authoritative
    // owner. makeSearchConfig() stamps a snapshot into each search call.
    const ClockState& clockState() const;
    void setClockState(const ClockState& clockState);

    bool applyMove(Move move);
    bool applySwapChoice(SwapChoice choice);
    bool undo();

    std::optional<Move> chooseAiMove() const;
    SwapChoice chooseAiSwapChoice() const;
    void stepAi();

    std::optional<SwapChoice> resolvedSwapChoice() const;
    const std::optional<SearchSummary>& lastSearchSummary() const;
    const std::optional<ThreatSearchResult>& lastThreatSequence() const;
    const std::optional<ProofAnalysisResult>& lastProofAnalysis() const;

private:
    MatchConfig config_ {};
    GameState state_;
    ClockState clockState_ {};
    std::optional<SwapChoice> resolvedSwapChoice_;
    std::optional<SearchSummary> lastSearchSummary_;
    std::optional<ThreatSearchResult> lastThreatSequence_;
    std::optional<ProofAnalysisResult> lastProofAnalysis_;

    Seat seatForStone(Player player) const;
    SearchConfig makeSearchConfig() const;
    void recomputeSwapChoice();
};

}  // namespace gomoku
