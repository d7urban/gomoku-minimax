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

const std::vector<OpeningBookEntry>& entries() {
    static const std::vector<OpeningBookEntry> kEntries = {
        {Ruleset::Freestyle15, "center_anchor", {}, {7, 7}},
        {Ruleset::Freestyle15, "east_reply", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Freestyle15, "cross_attach", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Freestyle15, "double_diagonal", {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7})}, {6, 8}},

        {Ruleset::Standard15, "center_anchor", {}, {7, 7}},
        {Ruleset::Standard15, "east_reply", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Standard15, "cross_attach", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Standard15, "double_diagonal", {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7})}, {6, 8}},

        {Ruleset::Swap16, "swap_triangle_1", {}, {7, 7}},
        {Ruleset::Swap16, "swap_triangle_2", {moveAction({7, 7})}, {7, 8}},
        {Ruleset::Swap16, "swap_triangle_3", {moveAction({7, 7}), moveAction({7, 8})}, {8, 7}},
        {Ruleset::Swap16, "post_keep_balance",
            {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7}), swapAction(SwapChoice::KeepColors)}, {8, 8}},
        {Ruleset::Swap16, "post_swap_balance",
            {moveAction({7, 7}), moveAction({7, 8}), moveAction({8, 7}), swapAction(SwapChoice::SwapColors)}, {8, 8}},
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

        bool matches = true;
        for (std::size_t index = 0; index < actions.size(); ++index) {
            if (!matchesAction(actions[index], entry.prefix[index])) {
                matches = false;
                break;
            }
        }

        if (!matches || !state.isLegalMove(entry.move)) {
            continue;
        }

        return OpeningBookHit {entry.move, entry.lineName};
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
