#include "gomoku/OpeningBook.hpp"

#include <array>

namespace gomoku {

namespace {

struct OpeningBookEntry {
    Ruleset ruleset {Ruleset::Freestyle15};
    std::string_view lineName {};
    std::vector<Action> prefix;
    Move move {};
};

Action moveAction(Move move) {
    return Action::makeMove(move);
}

Action swapAction(SwapChoice choice) {
    return Action::makeSwapChoice(choice);
}

bool matchesAction(const Action& left, const Action& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == Action::Kind::Move) {
        return left.move == right.move;
    }
    return left.swapChoice == right.swapChoice;
}

enum class Symmetry {
    Identity,
    Rotate90,
    Rotate180,
    Rotate270,
    MirrorVertical,
    MirrorHorizontal,
    MirrorMainDiagonal,
    MirrorAntiDiagonal,
};

constexpr std::array<Symmetry, 8> kSymmetries = {
    Symmetry::Identity,
    Symmetry::Rotate90,
    Symmetry::Rotate180,
    Symmetry::Rotate270,
    Symmetry::MirrorVertical,
    Symmetry::MirrorHorizontal,
    Symmetry::MirrorMainDiagonal,
    Symmetry::MirrorAntiDiagonal,
};

Move transformMove(Move move, int boardSize, Symmetry symmetry) {
    const int last = boardSize - 1;
    switch (symmetry) {
        case Symmetry::Identity:
            return move;
        case Symmetry::Rotate90:
            return {move.col, last - move.row};
        case Symmetry::Rotate180:
            return {last - move.row, last - move.col};
        case Symmetry::Rotate270:
            return {last - move.col, move.row};
        case Symmetry::MirrorVertical:
            return {move.row, last - move.col};
        case Symmetry::MirrorHorizontal:
            return {last - move.row, move.col};
        case Symmetry::MirrorMainDiagonal:
            return {move.col, move.row};
        case Symmetry::MirrorAntiDiagonal:
            return {last - move.col, last - move.row};
    }
    return move;
}

Action transformAction(const Action& action, int boardSize, Symmetry symmetry) {
    if (action.kind != Action::Kind::Move) {
        return action;
    }
    return Action::makeMove(transformMove(action.move, boardSize, symmetry));
}

bool matchesPrefixWithSymmetry(
    const std::vector<Action>& actions,
    const OpeningBookEntry& entry,
    int boardSize,
    Symmetry symmetry) {
    for (std::size_t index = 0; index < actions.size(); ++index) {
        if (!matchesAction(actions[index], transformAction(entry.prefix[index], boardSize, symmetry))) {
            return false;
        }
    }
    return true;
}

const std::vector<OpeningBookEntry>& entries() {
    static const std::vector<OpeningBookEntry> kEntries = {
        {Ruleset::Freestyle15, "center_anchor", {}, {7, 7}},
        {Ruleset::Freestyle15, "east_reply", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Freestyle15, "cross_attach", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Freestyle15, "diagonal_clamp", {moveAction({7, 7}), moveAction({6, 7})}, {6, 8}},
        {Ruleset::Freestyle15, "diagonal_split", {moveAction({7, 7}), moveAction({6, 8})}, {8, 8}},
        {Ruleset::Freestyle15, "double_diagonal", {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7})}, {6, 8}},

        {Ruleset::Standard15, "center_anchor", {}, {7, 7}},
        {Ruleset::Standard15, "east_reply", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Standard15, "cross_attach", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Standard15, "diagonal_clamp", {moveAction({7, 7}), moveAction({6, 7})}, {6, 8}},
        {Ruleset::Standard15, "diagonal_split", {moveAction({7, 7}), moveAction({6, 8})}, {8, 8}},
        {Ruleset::Standard15, "double_diagonal", {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7})}, {6, 8}},

        {Ruleset::Swap16, "swap_triangle_1", {}, {7, 7}},
        {Ruleset::Swap16, "swap_triangle_2", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Swap16, "swap_triangle_3", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Swap16, "post_keep_balance",
            {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7}), swapAction(SwapChoice::KeepColors)}, {8, 8}},
        {Ruleset::Swap16, "post_swap_balance",
            {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7}), swapAction(SwapChoice::SwapColors)}, {8, 8}},

        // Imported Crazy-Sensei openings. Placed after the curated entries
        // so that hand-picked first moves (e.g. center_anchor) still win
        // when prefixes overlap. See scripts/convert_opening_book.py.
#include "OpeningBookData.inc"
    };
    return kEntries;
}

}  // namespace

std::optional<OpeningBookHit> lookupOpeningBookMove(const GameState& state) {
    if (state.isGameOver() || state.isSwapDecisionPending()) {
        return std::nullopt;
    }

    const auto& actions = state.actions();
    for (const OpeningBookEntry& entry : entries()) {
        if (entry.ruleset != state.rules().ruleset || entry.prefix.size() != actions.size()) {
            continue;
        }

        for (const Symmetry symmetry : kSymmetries) {
            if (!matchesPrefixWithSymmetry(actions, entry, state.boardSize(), symmetry)) {
                continue;
            }

            const Move bookMove = transformMove(entry.move, state.boardSize(), symmetry);
            if (!state.isLegalMove(bookMove)) {
                continue;
            }

            return OpeningBookHit {bookMove, entry.lineName};
        }
    }

    return std::nullopt;
}

std::vector<std::string> openingBookLines() {
    std::vector<std::string> lines;
    lines.reserve(entries().size());

    for (const OpeningBookEntry& entry : entries()) {
        std::string line = std::string(toString(entry.ruleset));
        line += " : ";
        line += entry.lineName;
        line += " -> ";
        line += moveToString(entry.move);
        if (!entry.prefix.empty()) {
            line += " after ";
            for (std::size_t index = 0; index < entry.prefix.size(); ++index) {
                if (index > 0) {
                    line += ", ";
                }

                if (entry.prefix[index].kind == Action::Kind::Move) {
                    line += moveToString(entry.prefix[index].move);
                } else {
                    line += std::string(toString(entry.prefix[index].swapChoice));
                }
            }
        }
        lines.push_back(line);
    }

    return lines;
}

}  // namespace gomoku
