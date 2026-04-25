#pragma once

#include <array>
#include <optional>
#include <vector>

#include "gomoku/ClockState.hpp"
#include "gomoku/ClubAI.hpp"
#include "gomoku/ExpertAI.hpp"
#include "gomoku/RookieAI.hpp"
#include "gomoku/TacticalAI.hpp"

namespace gomoku {

struct MatchConfig {
    Ruleset ruleset {Ruleset::Freestyle15};
    ControllerKind openerController {ControllerKind::Human};
    ControllerKind chooserController {ControllerKind::ExpertAI};
    int aiMoveTimeMs {500};
    int searchThreads {0};
    bool strictDefenseFiltering {true};
    AiTimeControlPreset aiTimeControlPreset {AiTimeControlPreset::FixedPerMove};
};

struct MatchAiClockView {
    bool active {false};
    std::int64_t timeLeftMs {-1};
    std::int64_t periodTimeMs {0};
    int movesPlayedInPeriod {0};
    int movesToReset {0};
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
    void setSearchThreads(int searchThreads);
    void setAiTimeControlPreset(AiTimeControlPreset preset);
    void setAiClockState(Seat seat, std::int64_t timeLeftMs, int movesPlayedInPeriod);
    void clearUndoHistory();

    // Clock state observed from the match protocol. The gomocup adapter
    // (and any future analogue) is expected to update this whenever a
    // clock-bearing protocol message arrives; Match is the authoritative
    // owner. makeSearchConfig() stamps a snapshot into each search call.
    const ClockState& clockState() const;
    void setClockState(const ClockState& clockState);
    std::optional<MatchAiClockView> aiClockForSeat(Seat seat) const;

    bool applyMove(Move move);
    bool applySwapChoice(SwapChoice choice);
    bool undo();
    bool smartUndo();

    SearchResult searchAiTurn(const std::function<void(const SearchSummary&)>& progressCallback = {}) const;
    std::optional<Move> chooseAiMove() const;
    SwapChoice chooseAiSwapChoice() const;
    bool commitAiSearchResult(const SearchResult& result, std::int64_t elapsedMs);
    bool commitAiSwapChoice(SwapChoice choice, std::int64_t elapsedMs);
    void stepAi();

    std::optional<SwapChoice> resolvedSwapChoice() const;
    const std::optional<SearchSummary>& lastSearchSummary() const;
    const std::optional<ThreatSearchResult>& lastThreatSequence() const;

private:
    MatchConfig config_ {};
    GameState state_;
    ClockState clockState_ {};
    std::optional<SwapChoice> resolvedSwapChoice_;
    std::optional<SearchSummary> lastSearchSummary_;
    std::optional<ThreatSearchResult> lastThreatSequence_;
    struct SeatAiClockState {
        std::int64_t timeLeftMs {-1};
        int movesPlayedInPeriod {0};
    };
    struct MatchSnapshot {
        std::array<SeatAiClockState, 2> aiClocks;
        std::optional<SwapChoice> resolvedSwapChoice;
    };
    std::array<SeatAiClockState, 2> aiClocks_ {};
    std::vector<MatchSnapshot> history_;

    Seat seatForStone(Player player) const;
    static std::size_t seatIndex(Seat seat);
    bool timeControlActive() const;
    bool timeControlActiveForSeat(Seat seat) const;
    void resetAiClocks();
    MatchSnapshot snapshot() const;
    void restoreSnapshot(const MatchSnapshot& snapshot);
    void pushSnapshot();
    bool restoreLastSnapshot();
    SeatAiClockState& aiClockState(Seat seat);
    const SeatAiClockState& aiClockState(Seat seat) const;
    std::optional<ClockState> effectiveClockStateForTurn() const;
    void chargeAiClock(Seat seat, std::int64_t elapsedMs, bool countsAsPeriodMove);
    SearchConfig makeSearchConfig() const;
    void recomputeSwapChoice();
};

}  // namespace gomoku
