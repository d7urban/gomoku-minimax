#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "gomoku/Rules.hpp"
#include "gomoku/ThreatTypes.hpp"

namespace gomoku {

class GameState {
public:
    struct LineLocation {
        int lineIndex {-1};
        int offset {-1};
        int length {0};
    };

    explicit GameState(const RulesSpec& rules = rulesFor(Ruleset::Freestyle15));

    void reset();

    const RulesSpec& rules() const;
    int boardSize() const;

    Player sideToMove() const;
    GameResult result() const;

    bool isGameOver() const;
    bool isSwapDecisionPending() const;
    bool isSwapOpeningResolved() const;

    int moveCount() const;
    int actionCount() const;

    const std::vector<Player>& board() const;
    const std::vector<Action>& actions() const;
    std::optional<Move> lastPlacedMove() const;
    const MoveThreatInfo& threatInfoAt(Move move, Player player) const;
    bool canCreateThreatAtLeast(Player player, ThreatType threshold) const;
    std::vector<Move> movesCreatingThreatAtLeast(Player player, ThreatType threshold) const;
    int totalPotential(Player player) const;
    std::uint64_t positionHash() const;

    bool isInside(int row, int col) const;
    Player cellAt(int row, int col) const;
    std::uint16_t lineBits(Player player, int direction, int lineIndex) const;
    LineLocation lineLocation(Move move, int direction) const;

    bool isLegalMove(Move move) const;
    std::vector<Move> legalMoves() const;

    bool applyMove(Move move);
    bool applySwapChoice(SwapChoice choice);
    bool undo();
    void setSideToMoveForAnalysis(Player player);

private:
    struct UndoRecord {
        Action::Kind actionKind {Action::Kind::Move};
        Player sideToMove {Player::Black};
        GameResult result {GameResult::Ongoing};
        bool swapPending {false};
        bool swapResolved {false};
        int occupiedCount {0};
        std::size_t actionCount {0};
        std::optional<Move> lastPlacedMove;
        std::uint64_t positionHash {0};
        std::optional<Move> changedMove;
        std::vector<std::size_t> affectedIndices;
        std::vector<MoveThreatInfo> threatInfoBlack;
        std::vector<MoveThreatInfo> threatInfoWhite;
        int totalPotentialBlack {0};
        int totalPotentialWhite {0};
    };

    const RulesSpec* rules_ {nullptr};
    std::vector<Player> board_;
    Player sideToMove_ {Player::Black};
    GameResult result_ {GameResult::Ongoing};
    bool swapPending_ {false};
    bool swapResolved_ {false};
    int occupiedCount_ {0};
    std::vector<Action> actions_;
    std::vector<UndoRecord> undoHistory_;
    std::optional<Move> lastPlacedMove_;
    std::vector<std::uint16_t> rowBitsBlack_;
    std::vector<std::uint16_t> rowBitsWhite_;
    std::vector<std::uint16_t> colBitsBlack_;
    std::vector<std::uint16_t> colBitsWhite_;
    std::vector<std::uint16_t> diagBitsBlack_;
    std::vector<std::uint16_t> diagBitsWhite_;
    std::vector<std::uint16_t> antiDiagBitsBlack_;
    std::vector<std::uint16_t> antiDiagBitsWhite_;
    std::vector<MoveThreatInfo> threatInfoBlack_;
    std::vector<MoveThreatInfo> threatInfoWhite_;
    int totalPotentialBlack_ {0};
    int totalPotentialWhite_ {0};
    std::uint64_t positionHash_ {0};

    std::size_t indexOf(Move move) const;
    Move moveFromIndex(std::size_t index) const;
    int countDirection(Move move, int dRow, int dCol, Player player) const;
    bool createsWinningLine(Move move, Player player) const;
    UndoRecord createMoveUndoRecord(Move move) const;
    UndoRecord createSwapUndoRecord() const;
    void restore(const UndoRecord& snapshot);
    void rebuildDerivedState();
    void setBitboardOccupancy(Move move, Player player, bool occupied);
    std::vector<std::size_t> collectThreatUpdateIndices(Move move) const;
    void updateThreatInfoIndices(const std::vector<std::size_t>& indices);
    void setSideToMoveInternal(Player player);
    void setSwapPendingInternal(bool swapPending);
    void xorStoneHash(Move move, Player player);
};

}  // namespace gomoku
