# gomoku-minimax

Threat-aware C++20 Gomoku engine with an SFML GUI, CLI tools, opening book support, and tournament-style search improvements.

## Features

- Supported rulesets:
  - `freestyle15`
  - `standard15`
- Controllers:
  - `rookie`
  - `club`
  - `tactical`
  - `expert`
- Interfaces:
  - `gomoku_ui` SFML desktop app
  - `gomoku_cli` text interface
- Engine features:
  - threat-aware static evaluation with compound-threat scoring
  - alpha-beta / PVS search
  - clustered transposition table with cross-move reuse
  - staged threat move generation for defense and forcing lines
  - threat-sequence defense filtering with counter-threat refutations
  - null-move pruning guarded by bounded opponent threat search
  - selective VCF probing with forcing-only candidates
  - bounded quiescence search at noisy tactical leaves
  - cautious win-verification re-search for mate-like tactical scores
  - tactical threat-sequence search with all-defenses continuation support
  - opening book
  - iterative deepening with live-clock time governor
  - panic mode that can ignore soft time limits in losing tactical positions
  - bounded parallel root search
  - live search progress reporting in the GUI
  - AI-only OTB-style clock presets in the GUI
- Tooling:
  - self-play runner
  - search benchmark runner
  - opening-book dump tool
  - opening-book builder
  - Gomocup protocol adapter
  - tournament runner

## Build

Default build:

```bash
cmake -S . -B build
cmake --build build
```

Headless-only build:

```bash
cmake -S . -B build -DGOMOKU_BUILD_SFML_UI=OFF
cmake --build build
```

Quick GUI launcher:

```bash
./run_gui.sh
```

## Requirements

- CMake 3.20+
- C++20 compiler
- SFML 2.5+ if you want `gomoku_ui`

## Running

GUI:

```bash
./build/gomoku_ui
```

The GUI uses AI-only clock presets instead of a fixed per-move limit:
- `blitz`: `5:00 / 40`
- `fast`: `15:00 / 60`
- `slow`: `60:00 / 60`

These presets are allocated by the time governor from the live game clock;
they are not converted into a fixed per-move ceiling.

The search panel shows both:
- `Depth`: deepest ply actually visited, including forcing extensions
- `Completed depth`: last fully completed iterative-deepening pass

CLI:

```bash
./build/gomoku_cli
./build/gomoku_cli freestyle15 human expert 500
```

In the CLI and protocol tools, the final numeric argument is still a search time budget in milliseconds.

CLI usage:

```text
gomoku_cli [freestyle15|standard15] [human|rookie|club|tactical|expert|ai] [human|rookie|club|tactical|expert|ai] [move_time_ms]
```

## GUI Controls

- `1` / `2`: switch ruleset
- `O` / `P`: cycle opener / chooser controller
- `[` / `]` or `,` / `.`: cycle AI clock preset
- `Space`: toggle AI-vs-AI autoplay when both seats are AI
- `R`: restart
- `U`: undo
- `X`: save game to `gomoku_saved_game.txt`
- `L`: load game from `gomoku_saved_game.txt`
- `H`: heatmap overlay
- `T`: threat labels
- `C`: top-candidate markers
- `A`: analyze threats
- `Esc`: clear analysis overlays
- `K` / `S`: keep or swap colors when a swap decision is pending

## Tools

Self-play:

```bash
./build/gomoku_selfplay 4 freestyle15 expert expert 500
```

Search benchmark:

```bash
./build/gomoku_bench expert 500
```

Opening book dump:

```bash
./build/gomoku_book
```

Opening book builder:

```bash
./build/gomoku_book_build \
  --positions 1379 \
  --time-ms 10000 \
  --candidates 8 \
  --output src/engine/OpeningBookData.inc
```

The builder emits `OpeningBookData.inc` initializer entries. By default it writes
`OpeningBookData.generated.inc` so experiments do not overwrite the active book
unless `--output src/engine/OpeningBookData.inc` is passed explicitly.

Gomocup adapter:

```bash
./build/gomoku_gomocup --controller expert --threads 0
```

Use `--threads 0` for automatic root parallelism, `--threads 1` to force
single-threaded search, and `--no-strict-defense` to disable the hard
threat-sequence defense filter for triage. Tactical quiescence search is
enabled by default.

When the Gomocup adapter receives live `INFO time_left` and
`INFO moves_to_reset` updates, the engine uses the time governor to derive
per-move soft and hard budgets. This is the path used by the time-control
round-robin harness:

```bash
python3 ../gomoku-harness/harness/time_control_round_robin.py \
  --engine-cmd "/home/urban/Code/C++/gomoku-minimax/build/gomoku_gomocup --threads 16" \
  --presets "blitz,fast" \
  --games-per-pair 20 \
  --openings-file "../gomoku-harness/results/crazy_sensei_openings_balanced_rr_20260424.json" \
  --label "semanticfix_blitz_vs_fast_Qfix-TMfix-20g"
```

Tournament runner:

```bash
./build/gomoku_tournament \
  -n 32 \
  -t 500 \
  -o tournaments/openings.txt \
  ./build/gomoku_gomocup \
  ./build/gomoku_gomocup
```

Recent benchmark against the recorded `v0-baseline` ref:
- `500 ms/move`: `20-12` for current `expert` vs `v0` expert
- `1000 ms/move`: `27-5` for current `expert` vs `v0` expert

## Tests

Build and run the regression targets:

```bash
cmake --build build --target \
  gomoku_smoke \
  gomoku_tactical_regressions \
  gomoku_search_enhancements \
  gomoku_seeded_regressions \
  gomoku_time_governor \
  gomoku_book_build

ctest --test-dir build --output-on-failure
```

## Project Layout

- `include/gomoku/`: public engine headers
- `src/engine/`: engine implementation
- `src/ui/`: SFML GUI
- `src/cli/`: text interface
- `src/tools/`: self-play and benchmark tools
- `tests/`: smoke and tactical regression suites
- `Book/`: opening-book source files

## Current State

Current branch highlights:
- GUI play with human or AI seats
- opening-book play in the main engine
- threat-aware search with strict tactical defense filtering, VCF support, and win verification
- bounded tactical quiescence search for noisy leaf positions
- AI-only repeating time controls in the GUI and Gomocup live-clock time-governor support
- Gomocup root parallelism and richer search logging
- generated opening-book construction via `gomoku_book_build`
- save/load support for GUI games
