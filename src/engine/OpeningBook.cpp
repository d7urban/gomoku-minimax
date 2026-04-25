#include "gomoku/OpeningBook.hpp"

#include <algorithm>
#include <array>
#include <tuple>
#include <unordered_map>
#include <vector>

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

bool matchesAction(const Action& left, const Action& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == Action::Kind::Move) {
        return left.move == right.move;
    }
    return left.swapChoice == right.swapChoice;
}

struct CanonicalActionKey {
    int kind {0};
    int row {0};
    int col {0};
    int swapChoice {0};

    auto tie() const {
        return std::tie(kind, row, col, swapChoice);
    }

    bool operator<(const CanonicalActionKey& other) const {
        return tie() < other.tie();
    }

    bool operator==(const CanonicalActionKey& other) const {
        return tie() == other.tie();
    }
};

CanonicalActionKey canonicalizeAction(const Action& action) {
    CanonicalActionKey key;
    key.kind = static_cast<int>(action.kind);
    if (action.kind == Action::Kind::Move) {
        key.row = action.move.row;
        key.col = action.move.col;
    } else {
        key.swapChoice = static_cast<int>(action.swapChoice);
    }
    return key;
}

struct CanonicalEntryKey {
    std::vector<CanonicalActionKey> prefix;
    CanonicalActionKey move {};

    bool operator<(const CanonicalEntryKey& other) const {
        if (prefix != other.prefix) {
            return prefix < other.prefix;
        }
        return move < other.move;
    }
};

CanonicalEntryKey canonicalEntryKey(const OpeningBookEntry& entry, int boardSize) {
    std::optional<CanonicalEntryKey> best;
    for (const Symmetry symmetry : kSymmetries) {
        CanonicalEntryKey candidate;
        candidate.prefix.reserve(entry.prefix.size());
        for (const Action& action : entry.prefix) {
            candidate.prefix.push_back(canonicalizeAction(transformAction(action, boardSize, symmetry)));
        }
        candidate.move = canonicalizeAction(Action::makeMove(transformMove(entry.move, boardSize, symmetry)));
        if (!best.has_value() || candidate < *best) {
            best = std::move(candidate);
        }
    }
    return *best;
}

bool isImportedEntry(const OpeningBookEntry& entry) {
    return entry.lineName.starts_with("cs") || entry.lineName.starts_with("lb");
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

        // Imported Crazy-Sensei openings. Placed after the curated entries
        // so that hand-picked first moves (e.g. center_anchor) still win
        // when prefixes overlap. See scripts/convert_opening_book.py.
#include "OpeningBookData.inc"
    };
    return kEntries;
}

struct PrefixGroup {
    std::vector<const OpeningBookEntry*> entries;
};

struct BookIndex {
    std::unordered_map<int, PrefixGroup> groups;
    bool built {false};
};

BookIndex& bookIndex() {
    static BookIndex index;
    if (!index.built) {
        index.built = true;
        const auto& allEntries = entries();
        for (const OpeningBookEntry& entry : allEntries) {
            const int key = (static_cast<int>(entry.ruleset) << 16) | static_cast<int>(entry.prefix.size());
            index.groups[key].entries.push_back(&entry);
        }
        for (auto& [key, group] : index.groups) {
            (void)key;
            std::stable_sort(group.entries.begin(), group.entries.end(),
                [](const OpeningBookEntry* left, const OpeningBookEntry* right) {
                    const bool leftImported = isImportedEntry(*left);
                    const bool rightImported = isImportedEntry(*right);
                    if (leftImported != rightImported) {
                        return !leftImported;
                    }

                    const CanonicalEntryKey leftKey = canonicalEntryKey(*left, 15);
                    const CanonicalEntryKey rightKey = canonicalEntryKey(*right, 15);
                    if (leftKey < rightKey) {
                        return true;
                    }
                    if (rightKey < leftKey) {
                        return false;
                    }
                    return left->lineName < right->lineName;
                });
        }
    }
    return index;
}

}  // namespace

std::optional<OpeningBookHit> lookupOpeningBookMove(const GameState& state) {
    if (state.isGameOver() || state.isSwapDecisionPending()) {
        return std::nullopt;
    }

    const auto& actions = state.actions();
    const int key = (static_cast<int>(state.rules().ruleset) << 16) | static_cast<int>(actions.size());

    const BookIndex& index = bookIndex();
    const auto it = index.groups.find(key);
    if (it == index.groups.end()) {
        return std::nullopt;
    }

    for (const OpeningBookEntry* entry : it->second.entries) {
        for (const Symmetry symmetry : kSymmetries) {
            bool match = true;
            for (std::size_t i = 0; i < actions.size(); ++i) {
                if (!matchesAction(actions[i], transformAction(entry->prefix[i], state.boardSize(), symmetry))) {
                    match = false;
                    break;
                }
            }
            if (!match) {
                continue;
            }

            const Move bookMove = transformMove(entry->move, state.boardSize(), symmetry);
            if (!state.isLegalMove(bookMove)) {
                continue;
            }

            return OpeningBookHit {bookMove, entry->lineName};
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
