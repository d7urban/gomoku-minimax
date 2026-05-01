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
    bool useStrictDefenseFiltering {true};
    bool useRootThreatSearch {true};
    bool useOpeningBook {false};
    bool useVcfAtLeaves {true};
    bool useWinVerificationResearch {true};
    bool disableOpeningBook {false};
    bool disableNullMovePruning {false};
    bool disableDefensiveFiltering {false};
    bool compareNoDefFilterSearch {false};
    int maxRootThreads {0};
    double nextIterBranchingEstimate {0.0};
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
    int softLimitMs {0};
    int hardLimitMs {0};
    std::uint64_t nodes {0};
    std::uint64_t maxNodes {0};
    std::uint64_t ttHits {0};
    std::uint64_t threatNodes {0};
    std::uint64_t vcfNodes {0};
    std::uint64_t winVerificationNodes {0};
    int vcfHits {0};
    int winVerifications {0};
    int rootCandidateCount {0};
    int rootCandidateCountBeforeDefFilter {0};
    int rootCandidateCountAfterDefFilter {0};
    int threatSequenceLength {0};
    int requestedRootThreads {0};
    int lastIterationMs {0};
    int nextIterationEstimateMs {0};
    bool completedLastDepth {false};
    bool usedThreatSequence {false};
    bool usedOpeningBook {false};
    bool panicModeEntered {false};
    bool defFilterApplied {false};
    bool filteredOutBestMoveFromWiderSearch {false};
    bool noDefFilterBestMoveDiffers {false};
    bool noDefFilterBestMoveWasInBeforeDefFilter {false};
    std::string decisionSource {"search"};
    std::string openingBookName;
    std::string defFilterReason {"none"};
    std::string stopReason {"not_started"};
    std::optional<Move> noDefFilterBestMove;
    std::vector<Move> principalVariation;
    std::vector<Move> rootMovesBeforeDefFilter;
    std::vector<Move> rootMovesAfterDefFilter;
    std::vector<Move> rootMovesRemovedByDefFilter;
};

struct SearchResult {
    std::optional<Move> bestMove;
    SearchSummary summary {};
    std::optional<ThreatSearchResult> threatSequence;
};

// Counter-attacks that are strong enough to survive the defensive filter
// when the opponent is already threatening. The non-four cases are explicit
// compound threats; do not infer this from threatSeverityEnhanced(), which is
// only an ordering key.
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
