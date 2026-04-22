#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gomoku/ClockState.hpp"
#include "gomoku/ThreatSearch.hpp"
#include "gomoku/Threats.hpp"

namespace gomoku {

struct SearchSummary;

struct SearchConfig {
    int maxDepth {64};
    std::uint64_t maxNodes {0};
    int timeLimitMs {250};
    int softTimeLimitMs {0};
    std::size_t maxCandidateMoves {18};
    bool useAspirationWindows {true};
    bool useNullMovePruning {true};
    bool useDefensiveFiltering {true};
    bool useRootThreatSearch {true};
    bool useOpeningBook {false};
    bool useVcfAtLeaves {true};
    int vcfMaxDepth {10};
    std::uint64_t vcfNodeBudget {600};
    // Per-call snapshot of the match clock. Transitional home; the
    // governor (future work) consumes this as read-only input. Default
    // all-unknown so tests and in-process callers that do not care about
    // clocks pay no cost.
    ClockState clock {};
    std::function<void(const SearchSummary&)> progressCallback {};
};

struct SearchSummary {
    int depthReached {0};
    int maxDepthVisited {0};
    int score {0};
    int elapsedMs {0};
    std::uint64_t nodes {0};
    std::uint64_t ttHits {0};
    std::uint64_t threatNodes {0};
    std::uint64_t vcfNodes {0};
    int vcfHits {0};
    int rootCandidateCount {0};
    int threatSequenceLength {0};
    bool completedLastDepth {false};
    bool usedThreatSequence {false};
    bool usedOpeningBook {false};
    std::string openingBookName;
    std::vector<Move> principalVariation;
};

struct SearchResult {
    std::optional<Move> bestMove;
    SearchSummary summary {};
    std::optional<ThreatSearchResult> threatSequence;
};

// Counter-attacks that are strong enough to survive the defensive filter
// when the opponent is already threatening. A double-open-three fork is a
// valid counter to an opponent OpenThree even though its primary component
// is still only OpenThree.
bool isDefensiveCounterMove(const MoveThreatInfo& info, bool opponentFourOnBoard);

// Analysis helper for tests and diagnostics: returns the current VCF move
// candidates the engine would consider for `attacker` from this position.
// `childStage=false` uses the root VCF generator; `childStage=true` uses the
// localized child-stage generator.
std::vector<Move> vcfCandidateMovesForAnalysis(const GameState& state, Player attacker, bool childStage);

class SearchEngine {
public:
    explicit SearchEngine(SearchConfig config = {});

    SearchResult search(const GameState& state);

private:
    SearchConfig config_ {};
};

}  // namespace gomoku
