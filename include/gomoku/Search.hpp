#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gomoku/ClockState.hpp"
#include "gomoku/ProofSearch.hpp"
#include "gomoku/ThreatSearch.hpp"
#include "gomoku/Threats.hpp"

namespace gomoku {

struct SearchConfig {
    int maxDepth {4};
    std::uint64_t maxNodes {90000};
    int timeLimitMs {250};
    int softTimeLimitMs {0};
    std::size_t maxCandidateMoves {18};
    bool useAspirationWindows {true};
    bool useNullMovePruning {true};
    bool useDefensiveFiltering {true};
    bool useOpeningBook {false};
    bool useVcfAtLeaves {true};
    int vcfMaxDepth {10};
    std::uint64_t vcfNodeBudget {600};
    // Per-call snapshot of the match clock. Transitional home; the
    // governor (future work) consumes this as read-only input. Default
    // all-unknown so tests and in-process callers that do not care about
    // clocks pay no cost.
    ClockState clock {};
};

struct SearchSummary {
    int depthReached {0};
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
    std::optional<ProofAnalysisResult> proofAnalysis;
};

class SearchEngine {
public:
    explicit SearchEngine(SearchConfig config = {});

    SearchResult search(const GameState& state);

private:
    SearchConfig config_ {};
};

}  // namespace gomoku
