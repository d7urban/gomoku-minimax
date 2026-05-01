# WORKFLOW: Ideas to Steal from sortingsearching.com Gomoku Article

Source: https://sortingsearching.com/2020/05/18/gomoku.html

This is a ranked, step-by-step plan for porting ideas from that article into
`gomoku-minimax`. Items are ordered by expected playing-strength gain first,
then by implementation cost. Each step lists what the article does, what we
have today, what to change, and how to validate. Each step is intended to be
landable on its own, with a regression run between steps.

Baseline gauge: `gomoku_tournament -n 64 -t 500 ./build/gomoku_gomocup ./build/gomoku_gomocup`
against the recorded `v0-baseline` (see README) before and after each step.
Also run `ctest --test-dir build --output-on-failure` and the tactical
regressions after each change.

---

## Step 1 — Hard defense filtering driven by the threat-sequence output (HIGH impact)

What the article does:
- When the opponent has a threat sequence, the main PVS restricts children to
  the *valid defensive moves* derived from that sequence:
  1. all intersections inside the threat sequence,
  2. plus moves that create counter-threats found in refutation searches,
  3. iteratively refined across all detected sequences.
- Only those moves become children. Everything else is pruned at the node.

What we have today:
- `SearchConfig::useDefensiveFiltering` filters candidates only when the
  opponent already *statically* has an `OpenThree` or `SimpleFour` on the
  board (see `Search.cpp:1117`).
- `ThreatSequenceSearcher::searchWinningSequence` already returns a graph
  with `defenseMoves`, `requiredEmpty`, and `refutations`, but the main PVS
  does not consume that structure — it is wired through
  `SearchResult::threatSequence` only at the root for reporting.

What to change:
1. In `SearchRunner::negamax`, when entering a node where the opponent has a
   discovered threat sequence (run `ThreatSequenceSearcher` shallowly on the
   opponent's perspective, gated by tactical-pressure heuristics already used
   to gate VCF), build a `defenseSet` = union of:
   - every `Move` in `step.defenseMoves` and `step.requiredEmpty` over the
     returned `sequence`,
   - every counter-threat move that itself produces an attacker `SimpleFour`
     or stronger (use `analyzeMove` + `isDefensiveCounterMove`).
2. Intersect candidate generation with `defenseSet` instead of (or in
   addition to) the current static filter. Fall back to the unrestricted set
   only if `defenseSet` is empty.
3. Cache the per-node `defenseSet` in the search stack so a repeated probe at
   the same hash from the move-ordering layer reuses it.

How to validate:
- `gomoku_tactical_regressions` must still pass.
- Add a regression in `tests/` for a known forced-win position where the
  defender has exactly one defensive cell — confirm the engine considers
  only that cell.
- Self-play `expert` vs `v0-baseline` at 500 ms/move, expect a positive
  delta beyond noise (target: +5 wins per 32 games or better).

Risk: false negatives if the threat searcher misses a sequence — keep the
unrestricted fallback path and a `--no-strict-defense` switch for triage.

---

## Step 2 — Null-move pruning gated on "no opponent threat sequence" (HIGH-MEDIUM impact)

What the article does:
- "If in a node there is no threat sequence for the opponent, there is no
  immediate danger. In that case, we first try a null move ... and run a
  shallower search."
- The condition is an *actual search*, not a pattern flag.

What we have today:
- Null-move pruning at `Search.cpp:1759`, gated by `completedDepth_` and a
  static condition on tactical pressure.
- We *do* run threat search opportunistically, but never specifically as the
  null-move guard.

What to change:
1. Before applying null-move at the current node, run a bounded
   `ThreatSequenceSearcher` (small `maxNodes`, `maxDepth` ≈ 4) for the
   opponent. If it finds *any* winning sequence, skip null-move (and prefer
   to extend instead).
2. If no sequence is found, allow null-move and use the article's aggressive
   reduction (R = depth/2 + 1 typical for non-chess; tune empirically).
3. Cache the threat-search result on the search stack so the defense
   filtering step (Step 1) reuses it without recomputation.

How to validate:
- Bench: `gomoku_bench expert 500` — expect higher node-rate and deeper
  completed depths in quiet positions, equal or better in tactical ones.
- Tactical regression suite must not regress; add a fixture that previously
  produced a null-move-induced blunder if any exist (mine the corpus).

Risk: zugzwang-style positions are rare in gomoku, but a missed sequence is
a tactical hole. The small bounded threat search is the safety net.

---

## Step 3 — All-Defenses Trick + dependency DAG with topological validation (HIGH impact, ThreatSearch core)

What the article does:
- Builds a directed acyclic graph of threat nodes (Allis-style).
- "Combination Nodes": threats on a single line are combined; topological
  sort confirms whether two threats can be ordered without reusing the same
  empty squares.
- "All-Defenses Trick": rather than branching on each individual defender
  reply, assume the defender plays *all* defensive squares for a single
  threat at once. Collapses combinatorial explosion.
- Open threes get special handling because they require specific empty
  intersections.

What we have today:
- `ThreatSequenceSearcher` already exposes `ThreatGraphNodeKind::Combination`
  in the API (`ThreatSearch.hpp:11`), but the implementation in
  `ThreatSearch.cpp` is depth-bounded threat enumeration without a real
  dependency DAG, no topological validation, and no all-defenses collapse.
- `ThreatStep` already carries `requiredEmpty` and `defenseMoves`, so the
  data shape is mostly there.

What to change (incremental):
1. Implement `allDefensesApplied(state, threatStep)` that places *all*
   defensive stones for the opponent in one virtual move (push to a stack of
   pseudo-edits in a `GameStateDelta` helper). This avoids touching `apply
   Move`'s game-end logic.
2. In the threat search recursion, replace the per-defense branching with a
   single `allDefensesApplied` step. Keep per-defense branching only for
   open threes where multiple completions exist.
3. Add a `Combination` node builder: when two `ThreatStep`s touch overlapping
   squares, attempt to topologically order them; only if a valid order
   exists, emit a single `Combination` node whose dependencies point to both
   parents.
4. Restrict the top-level recursion so combinations are explored before
   single-threat continuations — typical wins go through them first.

How to validate:
- New unit tests in `tests/` exercising textbook double-threat win patterns
  (4-3 fork, 3-3 fork, double-four). Confirm the searcher returns a
  `foundWin = true` with a `Combination` node in the graph.
- Self-play vs baseline; expect a tactical sharpness gain especially in
  middlegame fork positions.

Risk: this is the largest single change. Land it behind a config flag
(`useAllDefensesTrick`) and keep the current path as fallback for one
release cycle.

---

## Step 4 — Counter-Threat Refutation in threat-space search (HIGH impact for correctness)

What the article does:
- After finding a candidate threat sequence, search for *defender*
  counter-threats that either:
  - win independently for the defender, or
  - interfere with the original sequence by stealing required empty squares.
- Refutations are checked conservatively (no recursive refutation), which
  bounds cost.

What we have today:
- `ThreatSearchResult::refutations` exists in the API but the search itself
  does not actively enumerate defender counter-threats during sequence
  validation — it only collects what surfaces incidentally.

What to change:
1. After a candidate winning `sequence` is found, walk it forward; at each
   step, run a one-ply `enumerateThreats(state, defender)` call and check:
   - Does any defender threat reach `SimpleFour` or higher? If yes, see if
     it threatens a square in any later `step.requiredEmpty`. If so, mark
     the sequence as *refuted*.
   - Does the defender threat itself constitute a faster win? If so,
     refuted.
2. Only return `foundWin = true` if no refutation interferes.
3. Surface the refuting moves in `ThreatSearchResult::refutations` so Step 1
   (defense filtering) can use them as counter-threat candidates.

How to validate:
- Build a regression set of positions where the engine previously announced
  a forced win that the opponent refuted. Confirm those are now reported as
  `foundWin = false`.
- Tournament: false-positive forced-win blunders should drop.

Risk: cost. Bound it via a `refutationNodeBudget` field on
`ThreatSequenceConfig`.

---

## Step 5 — Exponential static evaluation per intersection (MEDIUM impact)

What the article does:
- For each empty intersection, take the two best threats over the four
  directions (`a`, `b` with `a ≥ b`) and accumulate
  `score += 1.5 * 1.8^a + 1.8^b` for each player; subtract the opponent's.
- Threat values map roughly to severity ranks (0–16 in the article).

What we have today:
- `threatWeight` in `Threats.hpp:22` is a hand-tuned linear table per
  threat type. Aggregation in `evaluatePlayerPotential` (in `Threats.cpp`)
  sums per-cell contributions but does not combine the two best threats per
  cell with exponential weighting.

What to change:
1. In `StaticEvaluator::evaluatePlayerPotential`, for each empty cell:
   - Collect the four per-direction threat severities for that player.
   - Sort and take the top two as `a`, `b`.
   - Accumulate `1.5 * pow(1.8, a) + pow(1.8, b)` (precompute the
     `pow(1.8, k)` table at startup; the severity space is tiny).
2. Keep the existing linear weights as a fallback under a config flag for
   A/B comparison.
3. Re-tune `kDefaultAspirationWindow` if the eval scale shifts materially.

How to validate:
- Self-play A vs B (new eval vs old eval) at 500 ms/move, target +score
  delta beyond noise.
- Spot-check that the top-candidate ordering on tricky middlegame positions
  prefers double-threat-creating moves more strongly than today.

Risk: aspiration windows and mate-score thresholds may need re-tuning.

---

## Step 6 — Panic mode: continue searching past the soft limit when losing (MEDIUM impact)

What the article does:
- "When time allocation is exceeded mid-search, the system abandons time
  constraints and continues searching alternative moves until finding a
  non-losing option or exhausting possibilities."

What we have today:
- `TimeGovernor` in `TimeGovernor.cpp` provides soft and hard limits and
  reacts to root-instability, but does not have a "best move is currently
  losing → keep searching" branch.

What to change:
1. In the iterative-deepening loop in `SearchRunner::run`, after each depth
   completes, inspect the score of the best move. If it is at or below
   `-kLosingThreshold` (e.g., a near-mate loss), promote the time limit:
   ignore the soft limit entirely and let search continue until either:
   - a non-losing alternative is found, or
   - the hard limit is hit, or
   - all root candidates are exhausted at the current depth.
2. Surface this in `SearchSummary` (`panicModeEntered` bool) so the GUI can
   show the state to the user.

How to validate:
- Construct test positions where the engine starts in a lost position; with
  panic mode, the engine should sometimes find swindle resources missed
  before. Compare results at fixed time control.

Risk: minor — only triggered in lost positions, and bounded by hard limit.

---

## Step 7 — Cache opponent threat-search across move-ordering and search (MEDIUM impact, plumbing)

What the article does (implicitly):
- Each node has *one* threat-state computation that drives both move
  generation (defense set) and pruning (null-move guard, extensions).

What we have today:
- We compute threat info incrementally, but defensive filtering, VCF
  probing, and null-move gating each consult the position separately.

What to change:
1. Introduce a `NodeTacticalState` struct on the search stack carrying:
   - opponent threat sequence (if any),
   - own threat sequence (if any),
   - the derived `defenseSet`,
   - the `panicMode` flag.
2. Compute it once per node, before move generation; reuse in:
   - candidate filtering (Step 1),
   - null-move gating (Step 2),
   - extension decisions,
   - VCF entry conditions.

How to validate:
- `gomoku_bench` should show fewer redundant threat-search calls per node
  via the profiling counters in `Threats.hpp:56`.
- Functional behavior must not change beyond what Steps 1–2 introduced.

Risk: low; this is refactoring for cost reduction.

---

## Step 8 — Adopt the article's precomputed pattern catalog (MEDIUM impact, larger refactor)

What the article does:
- Precomputes 65 distinct threat patterns indexed by (severity level, way
  to complete, occupied squares, empty squares, defense squares).
- For each line length 5–16 and each opponent stone configuration,
  precomputes how the line is subdivided into sub-lines where threats can
  fit. Pattern matching becomes O(1) table lookups per affected window.

What we have today:
- `PatternAnalysis::classifyPatternWindow` enumerates and classifies windows
  procedurally per-call (`ThreatSearch.cpp:112`). Correct, but not as fast
  as a pure lookup.

What to change:
1. Generate the pattern catalog at build time (or via a once-per-process
   static initializer) keyed by the bit pattern of own/opponent in a
   fixed-length window.
2. Replace `classifyPatternWindow` with the table lookup; keep the current
   logic as a verification harness in debug builds.
3. Add the line-subdivision table for length 5–16 used to pick which
   windows even need to be looked up.

How to validate:
- Unit tests must agree byte-for-byte across the old and new classifiers
  on every legal window.
- `gomoku_bench` should show measurably faster threat info computation.

Risk: medium. The catalog is well-defined, but the standard15 (renju) rules
forbidden moves complicate the table. Land freestyle15 first.

---

## Step 9 — Lincke's algorithm for automatic opening-book construction (LOW-MEDIUM impact)

What the article does:
- Manually picks the opening, then "automatically constructs an opening
  book" using Lincke's algorithm, ending with 1,379 analyzed positions.

What we have today:
- `OpeningBook.cpp` reads a static `OpeningBookData.inc`; book construction
  is not automated.

What to change:
1. Implement a Lincke-style book builder as a separate tool
   (`gomoku_book_build`):
   - Start from the configured opening.
   - Recursively expand positions using deep search (e.g., 10 s/move).
   - Use Lincke's selection criterion to decide which sibling positions to
     expand next, biased toward positions reachable in real play.
   - Cap at N positions (start with 1k–2k).
2. Emit a new `OpeningBookData.inc` and add a CI guard that the book builds
   in a bounded time.

How to validate:
- Tournament: book-on vs book-off at fast time controls. Book-on should
  show a measurable opening-phase edge.

Risk: low; isolated to tooling.

---

## Recommended landing order

1. Step 1 (hard defense filtering)
2. Step 2 (threat-search-gated null move)
3. Step 7 (cache the per-node tactical state — pays for Steps 1+2)
4. Step 4 (counter-threat refutation)
5. Step 3 (All-Defenses Trick + DAG)
6. Step 5 (exponential eval)
7. Step 6 (panic mode)
8. Step 8 (precomputed pattern catalog)
9. Step 9 (Lincke book builder)

Run the tournament gauge after every step. If two consecutive steps fail to
move the win-rate, stop and profile before continuing.

---

## Things from the article we deliberately skip

- Rotated bitboards in 4 directions: already implemented
  (`GameState.hpp:91-98`).
- Incremental threat updates within a small radius of placed stones: already
  implemented (`GameState.cpp` near-stone counts, `collectThreatUpdateIndices`).
- Cached game termination: already implemented (`GameState::result()`,
  `isGameOver()`).
- Generic PVS / alpha-beta machinery: already implemented.
