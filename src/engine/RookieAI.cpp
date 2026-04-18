#include "gomoku/RookieAI.hpp"

#include <cassert>
#include <cmath>

namespace gomoku {

namespace {

bool isWinningResultFor(GameResult result, Player player) {
    return (player == Player::Black && result == GameResult::BlackWin)
        || (player == Player::White && result == GameResult::WhiteWin);
}

bool isImmediateWinningMove(const GameState& state, Move move, Player player) {
    if (state.sideToMove() != player) {
        return false;
    }

    GameState trial = state;
    if (!trial.applyMove(move)) {
        return false;
    }
    return isWinningResultFor(trial.result(), player);
}

int countImmediateWinningMoves(const GameState& state, Player player) {
    if (state.sideToMove() != player) {
        return 0;
    }

    int count = 0;
    for (const Move& move : state.legalMoves()) {
        if (isImmediateWinningMove(state, move, player)) {
            ++count;
        }
    }
    return count;
}

int localStonePressure(const GameState& state, Move move, Player player) {
    int score = 0;
    for (int dRow = -2; dRow <= 2; ++dRow) {
        for (int dCol = -2; dCol <= 2; ++dCol) {
            if (dRow == 0 && dCol == 0) {
                continue;
            }

            const int row = move.row + dRow;
            const int col = move.col + dCol;
            if (!state.isInside(row, col)) {
                continue;
            }

            const Player cell = state.cellAt(row, col);
            if (cell == player) {
                score += (std::abs(dRow) <= 1 && std::abs(dCol) <= 1) ? 30 : 10;
            } else if (cell != Player::None) {
                score += 6;
            }
        }
    }
    return score;
}

int moveScore(const GameState& state, Move move, Player player) {
    const int center = state.boardSize() / 2;
    const int distance = std::abs(move.row - center) + std::abs(move.col - center);
    int score = 1000 - 25 * distance;
    score += localStonePressure(state, move, player);
    return score;
}

int boardScoreFor(const GameState& state, Player player) {
    const int center = state.boardSize() / 2;
    int score = 0;

    for (int row = 0; row < state.boardSize(); ++row) {
        for (int col = 0; col < state.boardSize(); ++col) {
            if (state.cellAt(row, col) != player) {
                continue;
            }

            const int distance = std::abs(row - center) + std::abs(col - center);
            score += 100 - 5 * distance;
            score += localStonePressure(state, {row, col}, player);
        }
    }

    return score;
}

}  // namespace

std::optional<Move> RookieAI::chooseMove(const GameState& state, Player player) {
    const std::vector<Move> legalMoves = state.legalMoves();
    if (legalMoves.empty()) {
        return std::nullopt;
    }

    for (const Move& move : legalMoves) {
        if (isImmediateWinningMove(state, move, player)) {
            return move;
        }
    }

    const Player opponent = otherPlayer(player);

    std::optional<Move> bestMove;
    int bestOpponentReplyCount = 0;
    int bestScore = -1;

    for (const Move& move : legalMoves) {
        GameState trial = state;
        if (!trial.applyMove(move)) {
            continue;
        }

        const int opponentReplyCount = countImmediateWinningMoves(trial, opponent);
        const int score = moveScore(state, move, player);

        if (!bestMove.has_value() || opponentReplyCount < bestOpponentReplyCount
            || (opponentReplyCount == bestOpponentReplyCount && score > bestScore)) {
            bestMove = move;
            bestOpponentReplyCount = opponentReplyCount;
            bestScore = score;
        }
    }

    assert(bestMove.has_value());
    return bestMove;
}

SwapChoice RookieAI::chooseSwapChoice(const GameState& state) {
    const int blackScore = boardScoreFor(state, Player::Black);
    const int whiteScore = boardScoreFor(state, Player::White);
    return blackScore >= whiteScore ? SwapChoice::SwapColors : SwapChoice::KeepColors;
}

}  // namespace gomoku
