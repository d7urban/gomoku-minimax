# Project Status

Last updated: 2026-04-17

## Current Snapshot

- Current phase: Implementation
- Current checkpoint: Checkpoint 5 complete
- Current subtask: None
- Overall state: Checkpoint 5 is complete. The program now has proof-assisted tactical analysis, an `Analyst` opponent, annotated position save/load support, a proof benchmark tool/script, and GUI proof markers plus a dedicated proof-analysis pane on top of the existing Expert/threat-search engine.
- Next recommended action: Start the post-checkpoint cleanup pass with broader proof-analysis regression cases, larger position suites, and the deferred profiling work.
- Active blockers: None

## Status Legend

- `pending` - not started
- `in_progress` - currently being worked on
- `done` - completed
- `blocked` - cannot proceed yet

## Decisions Locked In

- Language/build: C++20 + CMake
- UI direction: SFML
- Engine/UI split: headless `engine/` plus graphical `ui/`
- Supported rulesets: 15x15 free-style, 15x15 standard, 16x16 swap-rule mode
- Engine roadmap: heuristic -> alpha-beta -> threat search -> PVS -> proof-assisted analysis

## Completed Work Log

| Date | Subtask | Status | Notes |
|---|---|---|---|
| 2026-04-16 | Project plan draft | done | Created `PLAN.md` with 5 playable checkpoints. |
| 2026-04-16 | Status tracker setup | done | Created `STATUS.md` for ongoing subtask-level tracking. |
| 2026-04-16 | C++20/CMake skeleton | done | Added engine library, CLI target, optional SFML UI target, and CTest smoke target. |
| 2026-04-16 | Core game-state implementation | done | Added rulesets, move generation, win/draw detection, swap-opening support, undo, and replay serialization. |
| 2026-04-16 | Rookie opponent | done | Added deterministic heuristic AI with immediate-win and immediate-danger handling. |
| 2026-04-16 | First playable interfaces | done | Added `gomoku_cli` text fallback and compiled `gomoku_ui` SFML window UI. |
| 2026-04-16 | Verification pass | done | Fresh build succeeded in `build-sfml`; smoke tests passed; `clang-tidy` run on all current source files. |
| 2026-04-16 | Threat-pattern evaluation pass | done | Added precomputed local pattern tables and a first threat-aware static evaluator. |
| 2026-04-16 | Club search bot | done | Added iterative-deepening alpha-beta search, candidate move generation, move ordering, transposition table, and `ClubAI`. |
| 2026-04-16 | Checkpoint 2 verification pass | done | Rebuilt in `build-sfml`, tests passed, and `clang-tidy` rerun on edited sources. |
| 2026-04-16 | Rotated cache pass | done | Added row/column/diagonal line bitboards and incremental local threat-cache updates in `GameState`. |
| 2026-04-16 | Cache regression coverage | done | Added smoke coverage for line caches, cached-vs-uncached threat analysis, undo restoration, and replay cache rebuild behavior. |
| 2026-04-16 | Checkpoint 2 overlay pass | done | Added SFML heatmap, threat labels, candidate markers, and top-move panel; rebuilt, tested, and reran `clang-tidy`. |
| 2026-04-16 | UI overlay layout fix | done | Split the right sidebar into separate status and candidate panels to prevent text overlap; rebuilt, tested, and reran `clang-tidy`. |
| 2026-04-16 | Responsive sidebar pass | done | Made board/sidebar layout respond to window size with resize-aware panel placement and sizing. |
| 2026-04-16 | GUI launcher script | done | Added root-level `run_gui.sh` to configure, build, and launch the SFML UI. |
| 2026-04-16 | Threat-sequence search core | done | Added `ThreatSequenceSearcher` with all-defenses pruning, dependency graph nodes, combination nodes, counter-threat-aware validation, and open-three required-empty handling. |
| 2026-04-16 | Threat-search integration | done | Wired the threat searcher into the main search as a root tactical oracle before alpha-beta. |
| 2026-04-16 | Threat-search regression coverage | done | Added smoke coverage for open-three threat metadata, immediate tactical wins, and refuted threat sequences. |
| 2026-04-16 | Tactical opponent | done | Added `TacticalAI`, parser/controller support, Match integration, and tactical-search summaries/results retention. |
| 2026-04-16 | Threat-analysis UI workflow | done | Added on-demand threat analysis, sequence stepping, and board overlays for forcing lines, defenses, and required empty squares. |
| 2026-04-16 | Tactical integration verification | done | Built, tested, and reran `clang-tidy` after wiring `TacticalAI`, CLI parsing, GUI defaults, and threat-analysis controls. |
| 2026-04-16 | Dedicated tactical regression suite | done | Added `gomoku_tactical_regressions` with curated open-three, required-empty, graph-metadata, and winning-line cases. |
| 2026-04-16 | Review-driven engine fixes | done | Fixed terminal turn convention for negamax, corrected threat-search open-three classification, removed unreachable rookie fallback, and added targeted regression coverage. |
| 2026-04-16 | Shared pattern infrastructure | done | Extracted common pattern classification into `PatternAnalysis` so evaluation and threat search use the same tables and logic. |
| 2026-04-16 | Incremental undo and hashing refactor | done | Replaced full-state undo snapshots with move-local undo records and added incremental Zobrist-style position hashing to `GameState`. |
| 2026-04-16 | Search hot-path cleanup | done | Switched TT keys to incremental position hashes and replaced the previous `vector<bool>` threat-update marker path. |
| 2026-04-17 | Search and rules follow-up fixes | done | Fixed TT abort pollution, normalized mate-distance TT scores, made exact-five pattern analysis reject overlines, removed the dead search fallback and unreachable rookie fallback, guarded `setSideToMoveForAnalysis` during swap-pending states, and reran `gomoku_smoke` plus `gomoku_tactical_regressions`. |
| 2026-04-17 | Tournament-style engine pass | done | Upgraded search to PVS with aspiration windows, stronger move ordering, guarded null-move pruning, defensive filtering, opening-book support, and time-aware threat-search limits; added `Expert`, self-play/benchmark/book tools, UI controller/time controls, and rebuilt `gomoku_ui` plus reran `gomoku_smoke` and `gomoku_tactical_regressions`. |
| 2026-04-17 | Checkpoint 4 search follow-up cleanup | done | Replaced null-move threat guards with cached `GameState` threat scans, widened aspiration bounds one-sided on retries, switched null-move cutoffs to fail-soft returns, reported static eval on opening-book hits, documented Expert config floors, hardened self-play turn validation, and reran `gomoku_smoke` plus `gomoku_tactical_regressions`. |
| 2026-04-17 | Proof-assisted analysis mode | done | Added a proof-number-style tactical analyzer, `AnalystAI`, annotated position import/export, `gomoku_proof_bench` plus `run_proof_bench.sh`, GUI proof markers/analysis pane/save-load hotkeys, and rebuilt `gomoku_ui` while rerunning `gomoku_smoke` and `gomoku_tactical_regressions`. |
| 2026-04-17 | Proof-analysis budget follow-up | done | Made `AnalystAI` respect combined move budgets across proof plus Expert fallback, bounded/counting threat enumeration inside proof candidate generation, added root/interior stop guards, versioned annotated-position files, documented the loaded-root threat-type limitation, and reran `gomoku_smoke` plus `gomoku_tactical_regressions`. |

## Checkpoint Status

### Checkpoint 1 - Playable Foundation

| Subtask | Status | Notes |
|---|---|---|
| C++20/CMake project setup | done | `gomoku_engine`, `gomoku_cli`, `gomoku_ui`, and `gomoku_smoke` targets created. |
| Core board state representation | done | `GameState` supports board storage, move history, and swap-opening state. |
| Legal move generation | done | Legal move enumeration implemented for all supported rulesets. |
| Win/draw detection | done | Free-style, exact-five, and draw detection verified in smoke tests. |
| Ruleset switching | done | 15x15 free-style, 15x15 standard, and 16x16 swap-rule modes supported. |
| Game serialization/replay logging | done | Replay export/import implemented as action logs. |
| First graphical board UI | done | Basic SFML board, stone rendering, hotkeys, and status pane implemented. |
| Mouse input/hover interaction | done | Click-to-place move input implemented in the UI. |
| Restart and undo controls | done | Available in both CLI and SFML UI. |
| Side-to-move and result display | done | Present in CLI output and UI status panel. |
| `Rookie` opponent | done | Basic playable AI integrated with CLI and UI turn flow. |

### Checkpoint 2 - Threat-Aware Search Bot

| Subtask | Status | Notes |
|---|---|---|
| Precomputed threat-pattern tables | done | Added local ternary pattern tables for 5/6/7-cell windows with target-aware threat classification. |
| Rotated bitboards | done | Added row/column/diagonal/anti-diagonal bitboard caches in `GameState`. |
| Incremental threat-board updates | done | Empty-cell threat scores are updated only near the last move instead of full-board rescans. |
| Static evaluation | done | Added threat-aware board evaluation based on best local move threats. |
| Iterative deepening | done | Added depth-by-depth search loop with node/time caps. |
| Negamax/alpha-beta search | done | Added first search backbone for the stronger bot. |
| Candidate move generation | done | Added near-stone filtering and move scoring. |
| Move ordering | done | Candidate ordering plus TT best-move promotion. |
| Transposition table | done | Added hashed position cache with bound types. |
| Search/eval UI overlays | done | UI now shows heatmap overlays, threat labels, candidate markers, top candidate text, and responsive sidebar layout alongside search stats. |
| `Club` opponent | done | Available in engine, CLI, and UI. |

### Checkpoint 3 - Tactical Threat Engine

| Subtask | Status | Notes |
|---|---|---|
| All-defenses trick | done | Used as an abstract pruning layer before validating actual defender branches. |
| Dependency-based search graph | done | Threat search now records dependency edges and explicit graph nodes. |
| Combination nodes | done | Multi-support dependencies are represented via explicit combination nodes in the graph. |
| Counter-threat/refutation search | done | Defender replies now include equal-or-stronger counter-threat moves during validation. |
| Open-three empty-square handling | done | Open-three threats track required empty squares and preserve them across recursive search. |
| Offensive threat-sequence detection | done | `ThreatSequenceSearcher` can now find forcing tactical lines for the attacker. |
| Defensive counter validation | done | Actual branch validation tests every defense/counter-threat reply before accepting a line. |
| Tactical regression test suite | done | Dedicated `gomoku_tactical_regressions` target covers curated open-three, required-empty, graph metadata, and winning-line cases. |
| Threat-analysis UI tools | done | GUI supports manual threat analysis, step navigation, and on-board sequence/defense overlays. |
| `Tactical` opponent | done | `TacticalAI` is selectable in engine, CLI, and GUI defaults. |

### Checkpoint 4 - Tournament-Style Engine

| Subtask | Status | Notes |
|---|---|---|
| Principal variation search | done | Main search now uses PVS at the root and interior nodes, and records a principal variation for completed iterations. |
| Time management | done | Search and threat-search both honor move budgets through hard/soft time limits and bounded tactical-oracle time slices. |
| Aspiration windows | done | Iterative deepening re-searches around the previous score with widening windows. |
| Stronger move ordering | done | Root-best, TT-best, killer, history, and threat-severity ordering are combined before search. |
| Guarded null-move pruning | done | Null-move forward pruning is enabled only in quiet enough positions with conservative tactical guards. |
| Defensive search filtering | done | Candidate generation now narrows to urgent blocks/counter-threats when the opponent has live forcing threats. |
| Self-play tooling | done | Added `gomoku_selfplay` for AI-vs-AI batches under configurable rules, controllers, and move times. |
| Benchmark/tuning suite | done | Added `gomoku_bench` with a small curated position suite and search-stat output. |
| Opening book generation | done | Added a built-in opening book plus `gomoku_book` for dumping the generated lines. |
| Time-control and AI-vs-AI UI | done | UI now exposes per-move time presets, controller cycling, and autoplay when both seats are AI-controlled. |
| `Expert` opponent | done | Added `ExpertAI`, controller parsing/string support, Match integration, and opening-book/time-aware move choice. |

### Checkpoint 5 - Proof-Assisted Analysis Mode

| Subtask | Status | Notes |
|---|---|---|
| Proof-number tactical analysis | done | Added `ProofAnalyzer` with proof/disproof-number propagation over a bounded tactical AND/OR tree plus threat-search shortcutting. |
| Selective proof-search integration | done | Added `AnalystAI`, which runs Expert search normally but attaches proof-assisted tactical confirmation in sharp positions and uses proven wins directly. |
| Position export/import | done | Added annotated position serialization/deserialization with proof metadata in `Replay`. |
| Reproducible benchmark scripts | done | Added `gomoku_proof_bench` and root-level `run_proof_bench.sh`. |
| Analysis UI pane and proof markers | done | GUI now supports manual proof analysis, root proof markers, PV overlays, and proof stats in the analysis pane. |
| Save/load annotated replay support | done | GUI can save/load the current analyzed position to `gomoku_analysis_position.txt` with proof annotations. |
| `Analyst` opponent | done | Added controller parsing, Match integration, UI selection, and smoke coverage for the new proof-assisted bot. |

## Cross-Cutting Tasks

| Subtask | Status | Notes |
|---|---|---|
| Rules unit tests | in_progress | Smoke coverage exists for freestyle wins, standard overlines, swap flow, replay load, and rookie defense. |
| Threat-recognition unit tests | in_progress | Smoke coverage now checks cached-vs-uncached threat analysis and line-cache correctness, but a broader pattern catalog is still missing. |
| Threat-sequence correctness tests | done | Dedicated tactical regression target now supplements smoke coverage with curated open-three, required-empty, graph-metadata, and winning-line cases. |
| Transposition consistency tests | in_progress | Search now uses incremental position hashes; smoke coverage checks undo/replay hash stability, but broader TT-specific cases are still pending. |
| Position regression suite | in_progress | Tactical positions now have a dedicated regression target; broader engine-position suites beyond threat search are still pending. |
| Profiling pass after Checkpoint 2 | pending | |
| Deterministic default behavior | done | Current rookie move choice and smoke coverage are deterministic. |

## Update Rules

- Update this file whenever a subtask starts, completes, or becomes blocked.
- Keep `Current Snapshot` accurate before stopping work.
- Add one row to `Completed Work Log` for every finished subtask.
- If a task is partially done, mark it `in_progress` and record what remains in `Notes`.
- If implementation changes the plan materially, update `PLAN.md` and mention that change here.
