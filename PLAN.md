# Gomoku Program Plan

## Goal
Build a C++ gomoku program with a graphical desktop UI and a staged engine roadmap that combines:

- Victor Allis' ideas from the thesis: threat taxonomy, all-defenses, dependency-based search, and proof-number-style tactical proving.
- Tomek Czajka's practical engine ideas: precomputed patterns, rotated bitboards, threat boards, static evaluation, principal variation search, defensive filtering, and opening book support.

## Scope

- Primary game mode: 15x15 free-style gomoku.
- Also support: 15x15 standard gomoku (exactly five) and 16x16 swap-rule mode, since the PDF and article use different rulesets.
- Human vs AI, AI vs AI, undo, restart, move history, last-move highlight, and optional threat/analysis overlays.
- Clean split between `engine/` and `ui/` so the AI can be tested headlessly.

## Technical Direction

- Language/build: C++20 + CMake.
- UI: SFML for board rendering, mouse input, overlays, and simple panels.
- Board representation: 16x16 padded internal board so the same code can serve 15x15 and 16x16 modes.
- Engine data structures: rotated bitboards plus incremental threat boards once the stronger bots arrive.
- Search progression: heuristic bot -> alpha-beta bot -> threat-search bot -> PVS bot -> proof-assisted analysis bot.

## Checkpoint 1 - Playable Foundation

- Implement board state, legal move generation, win/draw detection, ruleset switching, and game serialization/replay logging.
- Build the first graphical UI: board, stones, hover/click input, restart, undo, side-to-move, and result banner.
- Add `Rookie` opponent:
  - wins immediately if possible,
  - blocks an immediate opponent win,
  - otherwise prefers moves near existing stones,
  - falls back to random legal play.
- Deliverable: a complete playable game in all supported rulesets with a basic but functional AI.

## Checkpoint 2 - Threat-Aware Search Bot

- Add precomputed line/pattern tables for fives, open fours, simple fours, open threes, broken threes, and non-forcing build-up patterns.
- Introduce rotated bitboards and incremental threat-board updates.
- Implement a static evaluation inspired by the article: score empty intersections by their best local threats in the four directions.
- Add iterative-deepening negamax/alpha-beta, candidate move generation, move ordering, and a transposition table.
- Improve the UI with search depth, evaluation, principal variation, and optional move heatmap overlays.
- Add `Club` opponent:
  - alpha-beta search,
  - threat-based evaluation,
  - consistent immediate tactical play.
- Deliverable: a clearly stronger playable opponent that already feels intentional rather than random.

## Checkpoint 3 - Tactical Threat Engine

- Implement threat-sequence search using the thesis/article overlap:
  - all-defenses trick,
  - dependency-based search graph,
  - combination nodes,
  - counter-threat/refutation search,
  - special handling for open-three required empty squares.
- Use the threat engine both offensively and defensively: detect winning sequences for the side to move and valid defenses against opponent forcing lines.
- Add tactical regression tests from hand-built positions and examples derived from the thesis/article.
- Extend the UI with:
  - threat overlay,
  - "analyze threats" action,
  - step-through display of a found forcing sequence.
- Add `Tactical` opponent:
  - alpha-beta backbone,
  - threat-sequence search as tactical oracle,
  - restricted defensive move generation based on valid counters.
- Deliverable: a playable bot that can find and defend longer forcing lines, not just short tactical shots.

## Checkpoint 4 - Tournament-Style Engine

- Upgrade the main search to principal variation search.
- Add time management, aspiration windows, stronger move ordering, and guarded null-move forward pruning.
- Implement the defensive filtering ideas from the article so search focuses on moves that actually answer live threats.
- Add self-play tooling and benchmark suites for tuning search parameters and evaluation weights.
- Build a small opening book:
  - central openings for 15x15 modes,
  - swap-aware opening set for 16x16 mode.
- Improve the UI with time controls, AI-vs-AI autoplay, bot strength selector, and opening-book indicators.
- Add `Expert` opponent:
  - PVS,
  - threat search,
  - transposition table,
  - opening book,
  - time-aware move choice.
- Deliverable: a polished playable opponent suitable for long games and repeated self-play.

## Checkpoint 5 - Proof-Assisted Analysis Mode

- Add a proof-number-search-based tactical analysis mode for forcing positions, following the thesis' solving-oriented ideas.
- Use proof search selectively as an analysis/fallback tool rather than the only gameplay search.
- Add export/import of analyzed positions and benchmark scripts for reproducible engine comparisons.
- Extend the UI with:
  - analysis pane,
  - proven win/loss markers,
  - node counts and search stats,
  - save/load positions and replay annotated lines.
- Add `Analyst` opponent:
  - `Expert` play in normal positions,
  - proof-assisted tactical confirmation in sharp forcing positions.
- Deliverable: the final program is still a normal playable gomoku app, but now also doubles as an analysis tool inspired by the PDF.

## Cross-Cutting Requirements

- Unit tests for rules, threat recognition, threat-sequence correctness, and transposition-table consistency.
- Position test suite with known wins, defenses, and false-positive traps.
- Profiling on every checkpoint after Checkpoint 2.
- Keep the engine deterministic by default for test reproducibility, with optional randomness only in opening variety.

## Recommended Build Order

- Finish the UI and rules first so every later engine milestone is immediately playable.
- Add evaluation before deep tactical search so there is always a usable opponent.
- Add dependency-based threat search before PVS tuning; the tactical oracle is more valuable than extra pruning early on.
- Treat proof-number search as the final analysis feature, not the first gameplay engine.
