#include "gomoku/GameState.hpp"

#include <algorithm>
#include <cstdint>

#include "gomoku/Threats.hpp"

namespace gomoku {

namespace {

GameResult winningResult(Player player) {
    return player == Player::Black ? GameResult::BlackWin : GameResult::WhiteWin;
}

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t rulesetHash(Ruleset ruleset) {
    return splitMix64(0x1000ULL + static_cast<std::uint64_t>(ruleset));
}

std::uint64_t sideToMoveHash(Player player) {
    return splitMix64(0x2000ULL + static_cast<std::uint64_t>(player));
}

std::uint64_t swapPendingHash() {
    return splitMix64(0x3000ULL);
}

std::uint64_t stoneHash(const RulesSpec& rules, Move move, Player player) {
    const std::uint64_t cellIndex = static_cast<std::uint64_t>(move.row * 32 + move.col);
    const std::uint64_t salt = 0x4000ULL + static_cast<std::uint64_t>(rules.ruleset) * 2048ULL + cellIndex * 4ULL
        + static_cast<std::uint64_t>(player == Player::Black ? 1U : 2U);
    return splitMix64(salt);
}

}  // namespace

GameState::GameState(const RulesSpec& rules)
    : rules_(&rules) {
    reset();
}

void GameState::reset() {
    board_.assign(static_cast<std::size_t>(rules_->boardSize * rules_->boardSize), Player::None);
    sideToMove_ = Player::Black;
    result_ = GameResult::Ongoing;
    swapPending_ = false;
    swapResolved_ = !rules_->swapOpening;
    occupiedCount_ = 0;
    actions_.clear();
    undoHistory_.clear();
    lastPlacedMove_.reset();
    rebuildDerivedState();
}

const RulesSpec& GameState::rules() const {
    return *rules_;
}

int GameState::boardSize() const {
    return rules_->boardSize;
}

Player GameState::sideToMove() const {
    return sideToMove_;
}

GameResult GameState::result() const {
    return result_;
}

bool GameState::isGameOver() const {
    return result_ != GameResult::Ongoing;
}

bool GameState::isSwapDecisionPending() const {
    return swapPending_;
}

bool GameState::isSwapOpeningResolved() const {
    return swapResolved_;
}

int GameState::moveCount() const {
    return occupiedCount_;
}

int GameState::actionCount() const {
    return static_cast<int>(actions_.size());
}

const std::vector<Player>& GameState::board() const {
    return board_;
}

const std::vector<Action>& GameState::actions() const {
    return actions_;
}

std::optional<Move> GameState::lastPlacedMove() const {
    return lastPlacedMove_;
}

const MoveThreatInfo& GameState::threatInfoAt(Move move, Player player) const {
    static const MoveThreatInfo kEmptyInfo {};
    if (!isInside(move.row, move.col)) {
        return kEmptyInfo;
    }

    const std::size_t index = indexOf(move);
    if (player == Player::Black) {
        return threatInfoBlack_[index];
    }
    if (player == Player::White) {
        return threatInfoWhite_[index];
    }
    return kEmptyInfo;
}

bool GameState::hasThreatAtLeast(Player player, ThreatType threshold) const {
    const std::vector<MoveThreatInfo>* threatInfo = nullptr;
    if (player == Player::Black) {
        threatInfo = &threatInfoBlack_;
    } else if (player == Player::White) {
        threatInfo = &threatInfoWhite_;
    } else {
        return false;
    }

    const int minimumSeverity = threatSeverity(threshold);
    for (std::size_t index = 0; index < board_.size(); ++index) {
        if (board_[index] != Player::None) {
            continue;
        }
        if (threatSeverity((*threatInfo)[index].best) >= minimumSeverity) {
            return true;
        }
    }

    return false;
}

int GameState::totalPotential(Player player) const {
    if (player == Player::Black) {
        return totalPotentialBlack_;
    }
    if (player == Player::White) {
        return totalPotentialWhite_;
    }
    return 0;
}

std::uint64_t GameState::positionHash() const {
    return positionHash_;
}

bool GameState::isInside(int row, int col) const {
    return row >= 0 && col >= 0 && row < boardSize() && col < boardSize();
}

Player GameState::cellAt(int row, int col) const {
    if (!isInside(row, col)) {
        return Player::None;
    }
    return board_[indexOf({row, col})];
}

std::uint16_t GameState::lineBits(Player player, int direction, int lineIndex) const {
    const auto& lines = [&]() -> const std::vector<std::uint16_t>& {
        if (player == Player::Black) {
            switch (direction) {
                case 0:
                    return rowBitsBlack_;
                case 1:
                    return colBitsBlack_;
                case 2:
                    return diagBitsBlack_;
                case 3:
                default:
                    return antiDiagBitsBlack_;
            }
        }

        switch (direction) {
            case 0:
                return rowBitsWhite_;
            case 1:
                return colBitsWhite_;
            case 2:
                return diagBitsWhite_;
            case 3:
            default:
                return antiDiagBitsWhite_;
        }
    }();

    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) {
        return 0;
    }
    return lines[static_cast<std::size_t>(lineIndex)];
}

GameState::LineLocation GameState::lineLocation(Move move, int direction) const {
    LineLocation location;
    const int size = boardSize();

    switch (direction) {
        case 0:
            location.lineIndex = move.row;
            location.offset = move.col;
            location.length = size;
            return location;
        case 1:
            location.lineIndex = move.col;
            location.offset = move.row;
            location.length = size;
            return location;
        case 2: {
            const int delta = move.row - move.col;
            location.lineIndex = delta + (size - 1);
            location.length = size - std::abs(delta);
            const int startRow = std::max(0, delta);
            location.offset = move.row - startRow;
            return location;
        }
        case 3:
        default: {
            const int sum = move.row + move.col;
            location.lineIndex = sum;
            location.length = std::min(sum + 1, 2 * size - 1 - sum);
            const int startRow = std::max(0, sum - (size - 1));
            location.offset = move.row - startRow;
            return location;
        }
    }
}

bool GameState::isLegalMove(Move move) const {
    if (isGameOver() || swapPending_ || !isInside(move.row, move.col)) {
        return false;
    }
    return board_[indexOf(move)] == Player::None;
}

std::vector<Move> GameState::legalMoves() const {
    std::vector<Move> moves;
    if (isGameOver() || swapPending_) {
        return moves;
    }

    moves.reserve(board_.size() - static_cast<std::size_t>(moveCount()));
    for (int row = 0; row < boardSize(); ++row) {
        for (int col = 0; col < boardSize(); ++col) {
            if (cellAt(row, col) == Player::None) {
                moves.push_back({row, col});
            }
        }
    }
    return moves;
}

bool GameState::applyMove(Move move) {
    if (!isLegalMove(move)) {
        return false;
    }

    undoHistory_.push_back(createMoveUndoRecord(move));

    board_[indexOf(move)] = sideToMove_;
    setBitboardOccupancy(move, sideToMove_, true);
    xorStoneHash(move, sideToMove_);
    ++occupiedCount_;
    actions_.push_back(Action::makeMove(move));
    lastPlacedMove_ = move;
    updateThreatInfoIndices(undoHistory_.back().affectedIndices);

    if (createsWinningLine(move, sideToMove_)) {
        result_ = winningResult(sideToMove_);
        setSideToMoveInternal(otherPlayer(sideToMove_));
        return true;
    }

    if (occupiedCount_ == boardSize() * boardSize()) {
        result_ = GameResult::Draw;
        setSideToMoveInternal(otherPlayer(sideToMove_));
        return true;
    }

    setSideToMoveInternal(otherPlayer(sideToMove_));

    if (rules_->swapOpening && actions_.size() == 3U) {
        setSwapPendingInternal(true);
        swapResolved_ = false;
    }

    return true;
}

bool GameState::applySwapChoice(SwapChoice choice) {
    if (isGameOver() || !swapPending_) {
        return false;
    }

    undoHistory_.push_back(createSwapUndoRecord());
    actions_.push_back(Action::makeSwapChoice(choice));
    setSwapPendingInternal(false);
    swapResolved_ = true;
    return true;
}

bool GameState::undo() {
    if (undoHistory_.empty()) {
        return false;
    }

    restore(undoHistory_.back());
    undoHistory_.pop_back();
    return true;
}

void GameState::setSideToMoveForAnalysis(Player player) {
    if (player == Player::None) {
        return;
    }

    if (swapPending_) {
        return;
    }

    setSideToMoveInternal(player);
}

std::size_t GameState::indexOf(Move move) const {
    return static_cast<std::size_t>(move.row * boardSize() + move.col);
}

Move GameState::moveFromIndex(std::size_t index) const {
    const int size = boardSize();
    return {
        static_cast<int>(index / static_cast<std::size_t>(size)),
        static_cast<int>(index % static_cast<std::size_t>(size)),
    };
}

int GameState::countDirection(Move move, int dRow, int dCol, Player player) const {
    int count = 0;
    int row = move.row + dRow;
    int col = move.col + dCol;

    while (isInside(row, col) && cellAt(row, col) == player) {
        ++count;
        row += dRow;
        col += dCol;
    }

    return count;
}

bool GameState::createsWinningLine(Move move, Player player) const {
    static const int kDirections[4][2] = {
        {1, 0},
        {0, 1},
        {1, 1},
        {1, -1},
    };

    for (const auto& direction : kDirections) {
        const int total = 1
            + countDirection(move, direction[0], direction[1], player)
            + countDirection(move, -direction[0], -direction[1], player);

        if (!rules_->exactFiveRequired && total >= 5) {
            return true;
        }

        if (rules_->exactFiveRequired && total == 5) {
            return true;
        }
    }

    return false;
}

GameState::UndoRecord GameState::createMoveUndoRecord(Move move) const {
    UndoRecord state;
    state.actionKind = Action::Kind::Move;
    state.sideToMove = sideToMove_;
    state.result = result_;
    state.swapPending = swapPending_;
    state.swapResolved = swapResolved_;
    state.occupiedCount = occupiedCount_;
    state.actionCount = actions_.size();
    state.lastPlacedMove = lastPlacedMove_;
    state.positionHash = positionHash_;
    state.changedMove = move;
    state.affectedIndices = collectThreatUpdateIndices(move);
    state.threatInfoBlack.reserve(state.affectedIndices.size());
    state.threatInfoWhite.reserve(state.affectedIndices.size());
    for (const std::size_t index : state.affectedIndices) {
        state.threatInfoBlack.push_back(threatInfoBlack_[index]);
        state.threatInfoWhite.push_back(threatInfoWhite_[index]);
    }
    state.totalPotentialBlack = totalPotentialBlack_;
    state.totalPotentialWhite = totalPotentialWhite_;
    return state;
}

GameState::UndoRecord GameState::createSwapUndoRecord() const {
    UndoRecord state;
    state.actionKind = Action::Kind::SwapChoice;
    state.sideToMove = sideToMove_;
    state.result = result_;
    state.swapPending = swapPending_;
    state.swapResolved = swapResolved_;
    state.occupiedCount = occupiedCount_;
    state.actionCount = actions_.size();
    state.lastPlacedMove = lastPlacedMove_;
    state.positionHash = positionHash_;
    state.totalPotentialBlack = totalPotentialBlack_;
    state.totalPotentialWhite = totalPotentialWhite_;
    return state;
}

void GameState::restore(const UndoRecord& state) {
    if (state.actionKind == Action::Kind::Move && state.changedMove.has_value()) {
        const std::size_t moveIndex = indexOf(*state.changedMove);
        const Player placedPlayer = board_[moveIndex];
        if (placedPlayer != Player::None) {
            board_[moveIndex] = Player::None;
            setBitboardOccupancy(*state.changedMove, placedPlayer, false);
        }

        for (std::size_t i = 0; i < state.affectedIndices.size(); ++i) {
            const std::size_t index = state.affectedIndices[i];
            threatInfoBlack_[index] = state.threatInfoBlack[i];
            threatInfoWhite_[index] = state.threatInfoWhite[i];
        }
        totalPotentialBlack_ = state.totalPotentialBlack;
        totalPotentialWhite_ = state.totalPotentialWhite;
    }

    sideToMove_ = state.sideToMove;
    result_ = state.result;
    swapPending_ = state.swapPending;
    swapResolved_ = state.swapResolved;
    occupiedCount_ = state.occupiedCount;
    actions_.resize(state.actionCount);
    lastPlacedMove_ = state.lastPlacedMove;
    positionHash_ = state.positionHash;
}

void GameState::rebuildDerivedState() {
    const int size = boardSize();
    const int diagonalCount = 2 * size - 1;
    rowBitsBlack_.assign(static_cast<std::size_t>(size), 0);
    rowBitsWhite_.assign(static_cast<std::size_t>(size), 0);
    colBitsBlack_.assign(static_cast<std::size_t>(size), 0);
    colBitsWhite_.assign(static_cast<std::size_t>(size), 0);
    diagBitsBlack_.assign(static_cast<std::size_t>(diagonalCount), 0);
    diagBitsWhite_.assign(static_cast<std::size_t>(diagonalCount), 0);
    antiDiagBitsBlack_.assign(static_cast<std::size_t>(diagonalCount), 0);
    antiDiagBitsWhite_.assign(static_cast<std::size_t>(diagonalCount), 0);
    threatInfoBlack_.assign(board_.size(), MoveThreatInfo {});
    threatInfoWhite_.assign(board_.size(), MoveThreatInfo {});
    totalPotentialBlack_ = 0;
    totalPotentialWhite_ = 0;
    positionHash_ = rulesetHash(rules_->ruleset) ^ sideToMoveHash(sideToMove_);
    if (swapPending_) {
        positionHash_ ^= swapPendingHash();
    }

    for (std::size_t index = 0; index < board_.size(); ++index) {
        const Player player = board_[index];
        if (player == Player::None) {
            continue;
        }

        const Move move = moveFromIndex(index);
        setBitboardOccupancy(move, player, true);
        xorStoneHash(move, player);
    }

    for (std::size_t index = 0; index < board_.size(); ++index) {
        if (board_[index] != Player::None) {
            continue;
        }

        const Move move = moveFromIndex(index);
        threatInfoBlack_[index] = computeMoveThreatInfo(*this, move, Player::Black);
        threatInfoWhite_[index] = computeMoveThreatInfo(*this, move, Player::White);
        totalPotentialBlack_ += threatInfoBlack_[index].totalScore;
        totalPotentialWhite_ += threatInfoWhite_[index].totalScore;
    }
}

void GameState::setBitboardOccupancy(Move move, Player player, bool occupied) {
    if (player == Player::None) {
        return;
    }

    const std::uint16_t bitRow = static_cast<std::uint16_t>(1U << move.col);
    const std::uint16_t bitCol = static_cast<std::uint16_t>(1U << move.row);
    const LineLocation diag = lineLocation(move, 2);
    const LineLocation antiDiag = lineLocation(move, 3);
    const std::uint16_t bitDiag = static_cast<std::uint16_t>(1U << diag.offset);
    const std::uint16_t bitAntiDiag = static_cast<std::uint16_t>(1U << antiDiag.offset);

    auto applyBit = [occupied](std::uint16_t& value, std::uint16_t bit) {
        if (occupied) {
            value = static_cast<std::uint16_t>(value | bit);
        } else {
            value = static_cast<std::uint16_t>(value & static_cast<std::uint16_t>(~bit));
        }
    };

    if (player == Player::Black) {
        applyBit(rowBitsBlack_[static_cast<std::size_t>(move.row)], bitRow);
        applyBit(colBitsBlack_[static_cast<std::size_t>(move.col)], bitCol);
        applyBit(diagBitsBlack_[static_cast<std::size_t>(diag.lineIndex)], bitDiag);
        applyBit(antiDiagBitsBlack_[static_cast<std::size_t>(antiDiag.lineIndex)], bitAntiDiag);
        return;
    }

    applyBit(rowBitsWhite_[static_cast<std::size_t>(move.row)], bitRow);
    applyBit(colBitsWhite_[static_cast<std::size_t>(move.col)], bitCol);
    applyBit(diagBitsWhite_[static_cast<std::size_t>(diag.lineIndex)], bitDiag);
    applyBit(antiDiagBitsWhite_[static_cast<std::size_t>(antiDiag.lineIndex)], bitAntiDiag);
}

std::vector<std::size_t> GameState::collectThreatUpdateIndices(Move move) const {
    std::vector<unsigned char> affected(board_.size(), 0);
    std::vector<std::size_t> indices;
    indices.reserve(64);

    auto mark = [&](Move candidate) {
        if (!isInside(candidate.row, candidate.col)) {
            return;
        }

        const std::size_t index = indexOf(candidate);
        if (affected[index] != 0U) {
            return;
        }
        affected[index] = 1U;
        indices.push_back(index);
    };

    static const int kDirections[4][2] = {
        {1, 0},
        {0, 1},
        {1, 1},
        {1, -1},
    };

    mark(move);
    for (const auto& direction : kDirections) {
        for (int delta = -6; delta <= 6; ++delta) {
            mark({move.row + delta * direction[0], move.col + delta * direction[1]});
        }
    }

    return indices;
}

void GameState::updateThreatInfoIndices(const std::vector<std::size_t>& indices) {
    for (const std::size_t index : indices) {
        totalPotentialBlack_ -= threatInfoBlack_[index].totalScore;
        totalPotentialWhite_ -= threatInfoWhite_[index].totalScore;

        if (board_[index] == Player::None) {
            const Move candidate = moveFromIndex(index);
            threatInfoBlack_[index] = computeMoveThreatInfo(*this, candidate, Player::Black);
            threatInfoWhite_[index] = computeMoveThreatInfo(*this, candidate, Player::White);
        } else {
            threatInfoBlack_[index] = MoveThreatInfo {};
            threatInfoWhite_[index] = MoveThreatInfo {};
        }

        totalPotentialBlack_ += threatInfoBlack_[index].totalScore;
        totalPotentialWhite_ += threatInfoWhite_[index].totalScore;
    }
}

void GameState::setSideToMoveInternal(Player player) {
    if (player == sideToMove_) {
        return;
    }

    positionHash_ ^= sideToMoveHash(sideToMove_);
    sideToMove_ = player;
    positionHash_ ^= sideToMoveHash(sideToMove_);
}

void GameState::setSwapPendingInternal(bool swapPending) {
    if (swapPending == swapPending_) {
        return;
    }

    positionHash_ ^= swapPendingHash();
    swapPending_ = swapPending;
}

void GameState::xorStoneHash(Move move, Player player) {
    if (player == Player::None) {
        return;
    }
    positionHash_ ^= stoneHash(*rules_, move, player);
}

}  // namespace gomoku
