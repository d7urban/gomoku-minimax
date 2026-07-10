# gomoku-minimax

C++20 Gomoku with a playable SFML GUI, a headless CLI, and a staged engine that grows from a simple heuristic bot into a proof-assisted analysis tool.

## Features

- Supported rulesets:
  - `freestyle15`
  - `standard15`
  - `swap16`
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
  - tactically tiered static evaluation
  - iterative-deepening alpha-beta / PVS search
  - transposition table
  - tactical threat-sequence search
  - opening book
  - proof-assisted tactical analysis with complete defender verification
- Tooling:
  - self-play runner
  - search benchmark runner
  - proof benchmark runner
  - opening-book dump tool

## Search Correctness

The static evaluator treats tactical classes as a strict hierarchy. Immediate Five, OpenFour, SimpleFour, and OpenThree opportunities dominate quiet positional terms. It evaluates the four strongest candidate threats with diminishing weight and uses the aggregate low-grade board heatmap only as a small tie-breaker.

Threat queries describe moves that can create a pattern; they are not treated as threats already placed on the board. Optional defensive filtering is limited to concrete one-ply winning squares. Null-move pruning, defensive filtering, razoring, and reverse futility pruning are disabled by default while their assumptions are being validated.

Bounded threat searches provide move-ordering hints rather than unverified mate scores. Proof analysis can report `ProvenWin` only after every legal defender reply has been searched. Expert search uses a fixed high depth target and lets the time and node budgets determine the last completed iterative-deepening depth.

## Build

Headless build:

```bash
cmake -S . -B build
cmake --build build
```

SFML GUI build:

```bash
cmake -S . -B build-sfml -DGOMOKU_BUILD_SFML_UI=ON
cmake --build build-sfml
```

Quick GUI launcher:

```bash
./run_gui.sh
```

## Requirements

- CMake 3.20+
- C++20 compiler
- SFML 2.5+ to build `gomoku_ui`

## Running

GUI:

```bash
./build-sfml/gomoku_ui
```

CLI:

```bash
./build/gomoku_cli
./build/gomoku_cli freestyle15 human analyst 500
```

CLI usage:

```text
gomoku_cli [freestyle15|standard15|swap16] [human|rookie|club|tactical|expert|analyst|ai] [human|rookie|club|tactical|expert|analyst|ai] [move_time_ms]
```

## GUI Controls

- `1` / `2` / `3`: switch ruleset
- `O` / `P`: cycle opener / chooser controller
- `[` / `]`: cycle AI move time
- `Space`: toggle AI-vs-AI autoplay when both seats are AI
- `R`: restart
- `U`: undo
- `H`: heatmap overlay
- `T`: threat labels
- `C`: top-candidate markers
- `A`: analyze threats
- `F`: analyze proof
- `X`: save annotated analysis position
- `L`: load annotated analysis position
- `Left` / `Right`: step through a threat line
- `Esc`: clear analysis overlays
- `K` / `S`: keep or swap colors when a swap decision is pending

Saved proof-analysis positions are written to `gomoku_analysis_position.txt` in the project directory.

## Tools

Self-play:

```bash
./build/gomoku_selfplay 4 freestyle15 expert analyst 500
```

Search benchmark:

```bash
./build/gomoku_bench expert 500
```

Proof benchmark:

```bash
./build/gomoku_proof_bench 500
./run_proof_bench.sh 500
```

Opening book dump:

```bash
./build/gomoku_book
```

## Tests

Build and run the regression targets:

```bash
cmake --build build --target \
  gomoku_smoke \
  gomoku_tactical_regressions \
  gomoku_search_enhancements \
  gomoku_seeded_regressions
ctest --test-dir build --output-on-failure
```

The seeded search regressions use deterministic node budgets rather than wall-clock limits, including short-versus-long budget checks for forcing replies.

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

The project is currently through Checkpoint 5 of the roadmap: playable game, multiple AI levels, tactical threat analysis, tournament-style search improvements, and proof-assisted analysis mode.
