# gomoku-minimax

C++20 Gomoku with a playable SFML GUI, a headless CLI, and a staged engine that grows from a simple heuristic bot into a stronger threat-aware searcher.

## Features

- Supported rulesets:
  - `freestyle15`
  - `standard15`
- Controllers:
  - `rookie`
  - `club`
  - `tactical`
  - `expert`
  - `analyst`
- Interfaces:
  - `gomoku_ui` SFML desktop app
  - `gomoku_cli` text interface
- Engine features:
  - threat-aware static evaluation
  - alpha-beta / PVS search
  - transposition table
  - tactical threat-sequence search
  - opening book
  - time-governed iterative deepening
- Tooling:
  - self-play runner
  - search benchmark runner
  - opening-book dump tool
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

CLI:

```bash
./build/gomoku_cli
./build/gomoku_cli freestyle15 human analyst 500
```

CLI usage:

```text
gomoku_cli [freestyle15|standard15] [human|rookie|club|tactical|expert|analyst|ai] [human|rookie|club|tactical|expert|analyst|ai] [move_time_ms]
```

`analyst` currently uses the same move selection path as `expert`.

## GUI Controls

- `1` / `2`: switch ruleset
- `O` / `P`: cycle opener / chooser controller
- `[` / `]` or `,` / `.`: cycle AI move time
- `Space`: toggle AI-vs-AI autoplay when both seats are AI
- `R`: restart
- `U`: undo
- `H`: heatmap overlay
- `T`: threat labels
- `C`: top-candidate markers
- `A`: analyze threats
- `Esc`: clear analysis overlays
- `K` / `S`: keep or swap colors when a swap decision is pending

## Tools

Self-play:

```bash
./build/gomoku_selfplay 4 freestyle15 expert analyst 500
```

Search benchmark:

```bash
./build/gomoku_bench expert 500
```

Opening book dump:

```bash
./build/gomoku_book
```

Gomocup adapter:

```bash
./build/gomoku_gomocup --controller expert
```

Tournament runner:

```bash
./build/gomoku_tournament ./build/gomoku_gomocup ./build/gomoku_gomocup
```

## Tests

Build and run the regression targets:

```bash
cmake --build build --target \
  gomoku_smoke \
  gomoku_tactical_regressions \
  gomoku_search_enhancements \
  gomoku_seeded_regressions \
  gomoku_time_governor

ctest --test-dir build --output-on-failure
```

## Project Layout

- `include/gomoku/`: public engine headers
- `src/engine/`: engine implementation
- `src/ui/`: SFML GUI
- `src/cli/`: text interface
- `src/tools/`: self-play and benchmark tools
- `tests/`: smoke and tactical regression suites
- `PLAN.md`: checkpoint roadmap
- `STATUS.md`: implementation log and current status

## Current State

The project is currently through Checkpoint 5 of the roadmap: playable game, multiple AI levels, tactical threat analysis, opening-book support, tournament-style search improvements, and time-governed search.
