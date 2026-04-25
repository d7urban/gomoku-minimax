#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gomoku/GameState.hpp"
#include "gomoku/Rules.hpp"
#include "gomoku/Search.hpp"
#include "gomoku/Threats.hpp"
#include "gomoku/Types.hpp"

namespace {

using gomoku::CandidateMove;
using gomoku::GameState;
using gomoku::Move;
using gomoku::Player;
using gomoku::Ruleset;
using gomoku::SearchConfig;
using gomoku::SearchEngine;
using gomoku::SearchResult;
using gomoku::StaticEvaluator;

struct Options {
    std::filesystem::path output {"OpeningBookData.generated.inc"};
    std::vector<Move> rootMoves;
    Ruleset ruleset {Ruleset::Freestyle15};
    int maxPositions {128};
    int maxPly {24};
    int timeMs {1000};
    int maxDepth {64};
    std::uint64_t maxNodes {0};
    std::size_t maxCandidates {8};
    std::string linePrefix {"lb"};
};

struct BookEntry {
    std::vector<Move> prefix;
    Move move {};
    int score {0};
    int depth {0};
    int elapsedMs {0};
    std::uint64_t nodes {0};
};

struct QueueItem {
    std::vector<Move> prefix;
    double reach {1.0};
    double priority {1.0};
    int parentScore {0};
    int siblingRank {0};
    std::uint64_t serial {0};
};

struct QueueCompare {
    bool operator()(const QueueItem& left, const QueueItem& right) const {
        if (left.priority != right.priority) {
            return left.priority < right.priority;
        }
        return left.serial > right.serial;
    }
};

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

constexpr Symmetry kSymmetries[] = {
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

std::string moveKey(Move move) {
    return std::to_string(move.row) + "," + std::to_string(move.col);
}

std::string prefixKeyForSymmetry(const std::vector<Move>& prefix, int boardSize, Symmetry symmetry) {
    std::string key;
    for (Move move : prefix) {
        const Move transformed = transformMove(move, boardSize, symmetry);
        key += moveKey(transformed);
        key += ";";
    }
    return key;
}

std::string canonicalPrefixKey(const std::vector<Move>& prefix, int boardSize) {
    std::optional<std::string> best;
    for (const Symmetry symmetry : kSymmetries) {
        const std::string candidate = prefixKeyForSymmetry(prefix, boardSize, symmetry);
        if (!best.has_value() || candidate < *best) {
            best = candidate;
        }
    }
    return best.value_or(std::string {});
}

std::string canonicalEntryKey(const std::vector<Move>& prefix, Move move, int boardSize) {
    std::optional<std::string> best;
    for (const Symmetry symmetry : kSymmetries) {
        std::string candidate = prefixKeyForSymmetry(prefix, boardSize, symmetry);
        candidate += "->";
        candidate += moveKey(transformMove(move, boardSize, symmetry));
        if (!best.has_value() || candidate < *best) {
            best = candidate;
        }
    }
    return best.value_or(std::string {});
}

bool sameMove(Move left, Move right) {
    return left.row == right.row && left.col == right.col;
}

std::string formatMoveLiteral(Move move) {
    return "{" + std::to_string(move.row) + ", " + std::to_string(move.col) + "}";
}

std::string formatMoveList(const std::vector<Move>& moves) {
    if (moves.empty()) {
        return "<empty>";
    }

    std::string text;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (i > 0) {
            text += ",";
        }
        text += gomoku::moveToString(moves[i]);
    }
    return text;
}

std::string formatPrefixLiteral(const std::vector<Move>& prefix) {
    std::string text = "{";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += "moveAction(";
        text += formatMoveLiteral(prefix[i]);
        text += ")";
    }
    text += "}";
    return text;
}

bool parseNonNegativeInt(std::string_view text, int& value) {
    std::string owned(text);
    char* end = nullptr;
    const long parsed = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != '\0' || parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parseUint64(std::string_view text, std::uint64_t& value) {
    std::string owned(text);
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parseMoveList(std::string_view text, std::vector<Move>& moves) {
    moves.clear();
    std::string token;
    const auto flushToken = [&]() {
        if (token.empty()) {
            return true;
        }
        Move move;
        const bool ok = gomoku::tryParseMove(token, move);
        if (ok) {
            moves.push_back(move);
        }
        token.clear();
        return ok;
    };

    for (const char ch : text) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!flushToken()) {
                return false;
            }
            continue;
        }
        token.push_back(ch);
    }
    return flushToken();
}

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Build a generated OpeningBookData.inc using deep search and a Lincke-style\n"
        << "priority queue over plausible sibling positions.\n"
        << "\n"
        << "Options:\n"
        << "  --output PATH        Output include file (default: OpeningBookData.generated.inc)\n"
        << "  --root MOVES         Comma/space separated coordinate prefix, e.g. h8,i8,h9\n"
        << "  --rules RULESET      freestyle15 or standard15 (default: freestyle15)\n"
        << "  --positions N        Maximum book entries to emit (default: 128)\n"
        << "  --max-ply N          Do not expand prefixes at or beyond this ply (default: 24)\n"
        << "  --time-ms N          Search budget per expanded position (default: 1000)\n"
        << "  --max-depth N        Search depth cap per position (default: 64)\n"
        << "  --max-nodes N        Optional node cap per position (default: time-ms * 1200)\n"
        << "  --candidates N       Sibling candidates to enqueue from each position (default: 8)\n"
        << "  --line-prefix TEXT   Opening line-name prefix (default: lb)\n"
        << "  --help               Show this help\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto requireValue = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value.\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--output") {
            if (const char* value = requireValue(arg)) {
                options.output = value;
                continue;
            }
            return false;
        }
        if (arg == "--root") {
            if (const char* value = requireValue(arg)) {
                if (!parseMoveList(value, options.rootMoves)) {
                    std::cerr << "Invalid --root move list: " << value << "\n";
                    return false;
                }
                continue;
            }
            return false;
        }
        if (arg == "--rules") {
            if (const char* value = requireValue(arg)) {
                if (!gomoku::tryParseRuleset(value, options.ruleset)) {
                    std::cerr << "Invalid --rules value: " << value << "\n";
                    return false;
                }
                continue;
            }
            return false;
        }
        if (arg == "--positions") {
            int parsed = 0;
            if (const char* value = requireValue(arg)) {
                if (!parseNonNegativeInt(value, parsed) || parsed <= 0) {
                    std::cerr << "Invalid --positions value: " << value << "\n";
                    return false;
                }
                options.maxPositions = parsed;
                continue;
            }
            return false;
        }
        if (arg == "--max-ply") {
            int parsed = 0;
            if (const char* value = requireValue(arg)) {
                if (!parseNonNegativeInt(value, parsed)) {
                    std::cerr << "Invalid --max-ply value: " << value << "\n";
                    return false;
                }
                options.maxPly = parsed;
                continue;
            }
            return false;
        }
        if (arg == "--time-ms") {
            int parsed = 0;
            if (const char* value = requireValue(arg)) {
                if (!parseNonNegativeInt(value, parsed) || parsed <= 0) {
                    std::cerr << "Invalid --time-ms value: " << value << "\n";
                    return false;
                }
                options.timeMs = parsed;
                continue;
            }
            return false;
        }
        if (arg == "--max-depth") {
            int parsed = 0;
            if (const char* value = requireValue(arg)) {
                if (!parseNonNegativeInt(value, parsed) || parsed <= 0) {
                    std::cerr << "Invalid --max-depth value: " << value << "\n";
                    return false;
                }
                options.maxDepth = parsed;
                continue;
            }
            return false;
        }
        if (arg == "--max-nodes") {
            if (const char* value = requireValue(arg)) {
                if (!parseUint64(value, options.maxNodes)) {
                    std::cerr << "Invalid --max-nodes value: " << value << "\n";
                    return false;
                }
                continue;
            }
            return false;
        }
        if (arg == "--candidates") {
            int parsed = 0;
            if (const char* value = requireValue(arg)) {
                if (!parseNonNegativeInt(value, parsed) || parsed <= 0) {
                    std::cerr << "Invalid --candidates value: " << value << "\n";
                    return false;
                }
                options.maxCandidates = static_cast<std::size_t>(parsed);
                continue;
            }
            return false;
        }
        if (arg == "--line-prefix") {
            if (const char* value = requireValue(arg)) {
                options.linePrefix = value;
                if (options.linePrefix.empty()) {
                    std::cerr << "--line-prefix cannot be empty.\n";
                    return false;
                }
                continue;
            }
            return false;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        return false;
    }

    options.maxCandidates = std::max<std::size_t>(1, options.maxCandidates);
    options.maxPositions = std::max(1, options.maxPositions);
    options.maxPly = std::max(0, options.maxPly);
    return true;
}

bool applyPrefix(GameState& state, const std::vector<Move>& prefix, std::string& error) {
    for (Move move : prefix) {
        if (!state.isLegalMove(move)) {
            error = "illegal move " + gomoku::moveToString(move) + " in prefix " + formatMoveList(prefix);
            return false;
        }
        if (!state.applyMove(move)) {
            error = "failed to apply move " + gomoku::moveToString(move);
            return false;
        }
    }
    return true;
}

SearchConfig makeSearchConfig(const Options& options) {
    SearchConfig config;
    config.maxDepth = options.maxDepth;
    config.maxNodes = options.maxNodes == 0
        ? std::max<std::uint64_t>(10000, static_cast<std::uint64_t>(options.timeMs) * 1200ULL)
        : options.maxNodes;
    config.timeLimitMs = options.timeMs;
    config.softTimeLimitMs = options.timeMs;
    config.maxCandidateMoves = options.maxCandidates;
    config.useOpeningBook = false;
    return config;
}

CandidateMove candidateForMove(const GameState& state, Move move, Player player, int score) {
    CandidateMove candidate;
    candidate.move = move;
    candidate.threatInfo = StaticEvaluator::analyzeMove(state, move, player);
    candidate.score = score;
    return candidate;
}

std::vector<CandidateMove> expansionCandidates(
    const GameState& state,
    Player player,
    std::optional<Move> searchBest,
    std::size_t maxCandidates) {
    std::vector<CandidateMove> generated = StaticEvaluator::generateCandidateMoves(state, player, maxCandidates);
    int bestStaticScore = generated.empty() ? 0 : generated.front().score;

    std::vector<CandidateMove> ordered;
    ordered.reserve(std::max<std::size_t>(1, generated.size()));

    if (searchBest.has_value() && state.isLegalMove(*searchBest)) {
        auto it = std::find_if(generated.begin(), generated.end(), [&](const CandidateMove& candidate) {
            return sameMove(candidate.move, *searchBest);
        });
        if (it != generated.end()) {
            CandidateMove promoted = *it;
            promoted.score = std::max(promoted.score, bestStaticScore + 1);
            ordered.push_back(promoted);
        } else {
            ordered.push_back(candidateForMove(state, *searchBest, player, bestStaticScore + 1));
        }
        bestStaticScore = std::max(bestStaticScore, ordered.front().score);
    }

    for (const CandidateMove& candidate : generated) {
        const bool duplicate = std::any_of(ordered.begin(), ordered.end(), [&](const CandidateMove& existing) {
            return sameMove(existing.move, candidate.move);
        });
        if (!duplicate) {
            ordered.push_back(candidate);
        }
        if (ordered.size() >= maxCandidates) {
            break;
        }
    }

    return ordered;
}

double linckeStylePriority(const QueueItem& parent, int searchScore, int bestStaticScore, int candidateScore, int siblingRank, int childPly) {
    const int staticDelta = std::max(0, bestStaticScore - candidateScore);
    const double closeness = 1.0 / (1.0 + static_cast<double>(staticDelta) / 50000.0);
    const double rankFactor = 1.0 / (1.0 + static_cast<double>(siblingRank) * 0.35);
    const double balanceFactor = 1.0 / (1.0 + static_cast<double>(std::abs(searchScore)) / 200000.0);
    const double depthFactor = 1.0 / (1.0 + static_cast<double>(childPly) * 0.10);
    const double childReach = parent.reach * closeness * rankFactor;

    return childReach * balanceFactor * depthFactor;
}

std::vector<BookEntry> buildBook(const Options& options) {
    const auto& rules = gomoku::rulesFor(options.ruleset);
    const SearchConfig searchConfig = makeSearchConfig(options);

    std::priority_queue<QueueItem, std::vector<QueueItem>, QueueCompare> queue;
    std::set<std::string> queued;
    std::set<std::string> expanded;
    std::set<std::string> emitted;
    std::vector<BookEntry> entries;
    entries.reserve(static_cast<std::size_t>(options.maxPositions));

    std::uint64_t serial = 0;
    QueueItem root;
    root.prefix = options.rootMoves;
    root.serial = serial++;
    root.priority = 1.0;
    queued.insert(canonicalPrefixKey(root.prefix, rules.boardSize));
    queue.push(root);

    while (!queue.empty() && entries.size() < static_cast<std::size_t>(options.maxPositions)) {
        const QueueItem item = queue.top();
        queue.pop();

        const std::string prefixKey = canonicalPrefixKey(item.prefix, rules.boardSize);
        if (expanded.contains(prefixKey)) {
            continue;
        }
        expanded.insert(prefixKey);

        GameState state(rules);
        std::string error;
        if (!applyPrefix(state, item.prefix, error)) {
            std::cerr << "[book-build] skip invalid prefix: " << error << "\n";
            continue;
        }
        if (state.isGameOver() || state.isSwapDecisionPending()) {
            continue;
        }

        SearchEngine engine(searchConfig);
        const SearchResult result = engine.search(state);
        if (!result.bestMove.has_value() || !state.isLegalMove(*result.bestMove)) {
            continue;
        }

        const std::string entryKey = canonicalEntryKey(item.prefix, *result.bestMove, rules.boardSize);
        if (!emitted.contains(entryKey)) {
            emitted.insert(entryKey);
            entries.push_back(BookEntry {
                .prefix = item.prefix,
                .move = *result.bestMove,
                .score = result.summary.score,
                .depth = result.summary.depthReached,
                .elapsedMs = result.summary.elapsedMs,
                .nodes = result.summary.nodes,
            });
            std::cout << "[book-build] " << entries.size() << "/" << options.maxPositions
                      << " prefix=" << formatMoveList(item.prefix)
                      << " move=" << gomoku::moveToString(*result.bestMove)
                      << " score=" << result.summary.score
                      << " depth=" << result.summary.depthReached
                      << " nodes=" << result.summary.nodes
                      << "\n";
        }

        if (static_cast<int>(item.prefix.size()) >= options.maxPly) {
            continue;
        }

        std::vector<CandidateMove> candidates = expansionCandidates(state, state.sideToMove(), result.bestMove, options.maxCandidates);
        if (candidates.empty()) {
            continue;
        }

        const int bestStaticScore = candidates.front().score;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const CandidateMove& candidate = candidates[index];
            if (!state.isLegalMove(candidate.move)) {
                continue;
            }

            GameState child = state;
            if (!child.applyMove(candidate.move) || child.isGameOver()) {
                continue;
            }

            std::vector<Move> childPrefix = item.prefix;
            childPrefix.push_back(candidate.move);
            const std::string childKey = canonicalPrefixKey(childPrefix, rules.boardSize);
            if (expanded.contains(childKey) || queued.contains(childKey)) {
                continue;
            }

            QueueItem childItem;
            childItem.prefix = std::move(childPrefix);
            childItem.parentScore = result.summary.score;
            childItem.siblingRank = static_cast<int>(index);
            childItem.reach = item.reach / (1.0 + static_cast<double>(index) * 0.35);
            childItem.priority = linckeStylePriority(
                item, result.summary.score, bestStaticScore, candidate.score, static_cast<int>(index), static_cast<int>(childItem.prefix.size()));
            childItem.serial = serial++;
            queued.insert(childKey);
            queue.push(std::move(childItem));
        }
    }

    return entries;
}

void writeBook(const Options& options, const std::vector<BookEntry>& entries) {
    const std::filesystem::path parent = options.output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(options.output);
    if (!out) {
        throw std::runtime_error("failed to open output file: " + options.output.string());
    }

    out << "// Auto-generated by gomoku_book_build.\n";
    out << "// Do not edit by hand; re-run the builder to refresh.\n";
    out << "// Ruleset: " << gomoku::toString(options.ruleset) << "\n";
    out << "// Root: " << formatMoveList(options.rootMoves) << "\n";
    out << "// Positions: " << options.maxPositions << "\n";
    out << "// TimeMs: " << options.timeMs << "\n";
    out << "// Candidates: " << options.maxCandidates << "\n";
    out << "// Entries: " << entries.size() << "\n\n";

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const BookEntry& entry = entries[i];
        out << "    {Ruleset::" << (options.ruleset == Ruleset::Freestyle15 ? "Freestyle15" : "Standard15")
            << ", \"" << options.linePrefix << std::setfill('0') << std::setw(4) << (i + 1) << std::setfill(' ')
            << "\", " << formatPrefixLiteral(entry.prefix)
            << ", " << formatMoveLiteral(entry.move) << "},";
        out << "  // " << gomoku::moveToString(entry.move)
            << " score=" << entry.score
            << " depth=" << entry.depth
            << " nodes=" << entry.nodes
            << " time_ms=" << entry.elapsedMs
            << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage(argv[0]);
        return 2;
    }

    const auto& rules = gomoku::rulesFor(options.ruleset);
    for (Move move : options.rootMoves) {
        if (move.row < 0 || move.col < 0 || move.row >= rules.boardSize || move.col >= rules.boardSize) {
            std::cerr << "Root move out of bounds for " << rules.boardSize << "x" << rules.boardSize
                      << ": " << gomoku::moveToString(move) << "\n";
            return 2;
        }
    }

    GameState rootState(rules);
    std::string error;
    if (!applyPrefix(rootState, options.rootMoves, error)) {
        std::cerr << "Invalid root: " << error << "\n";
        return 2;
    }

    try {
        const std::vector<BookEntry> entries = buildBook(options);
        writeBook(options, entries);
        std::cout << "[book-build] wrote " << entries.size() << " entries to " << options.output << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "[book-build] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
