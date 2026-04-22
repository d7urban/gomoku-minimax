# Upgrade Ideas

This file captures the most promising engine-strength ideas identified from a comparison with [PentaZen](https://github.com/sun-yuliang/PentaZen/tree/master), ranked by estimated practical impact on this codebase.

The ranking is intentionally pragmatic:
- expected playing-strength gain first
- implementation cost and architecture fit second
- ideas we already substantially have are ranked lower

## 1. Stronger Transposition Table

Estimated impact: high

Why it matters:
- Our current TT is a single direct-mapped slot with shallow replacement logic: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:52)
- That makes collisions expensive and throws away useful search information too aggressively.
- PentaZen uses clustered entries with age-aware replacement, which is materially stronger under real search pressure: <https://github.com/sun-yuliang/PentaZen/blob/master/src/tt.cpp>

What to take:
- clustered buckets instead of one slot per index
- age/generation tracking
- replacement based on depth plus age, not just depth alone

Why this is ranked first:
- It should improve move ordering, cutoff rate, and search stability across the whole engine.
- It fits our current architecture without forcing a board rewrite.

## 2. Real Staged Threat Move Generation

Estimated impact: high

Why it matters:
- PentaZen does not rely on one generic candidate generator. It switches generation mode based on tactical state: `DEFEND_B4`, `DEFEND_F3`, `VCF_ROOT`, `VCF_CHILD`, `DEFAULT`, `LARGE`: <https://github.com/sun-yuliang/PentaZen/blob/master/src/movegen.cpp>
- We already do some of this implicitly through defensive filtering and threat-aware ordering: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:657), [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:709)
- But we still start from a generic candidate list and then prune/order it. That is weaker than generating the right tactical subset up front.

What to take:
- explicit generation stages for:
  - defend opponent live four
  - defend opponent open three / double-threat pressure
  - VCF root
  - VCF child
  - early-opening wider neighborhood
- smaller tactical candidate sets before sorting

Why this is ranked second:
- It should reduce tactical misses and increase depth in exactly the sharp positions where we have seen errors.
- It is a better fit than importing PentaZen's full board internals.

## 3. Win Verification Re-search

Estimated impact: medium-high

Why it matters:
- PentaZen verifies claimed winning lines with a cautious re-search before trusting them: <https://github.com/sun-yuliang/PentaZen/blob/master/src/search.cpp>
- Inference: this likely reduces false tactical wins and unstable PVs in forcing positions.
- We already verify some null-move mate-like results and have dedicated threat search, but we do not have a broad "verify win before trusting it" policy at the search level: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:1012), [src/engine/ThreatSearch.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/ThreatSearch.cpp:546)

What to take:
- if a PV node finds a win score in a forcing line, re-search it under stricter conditions before locking it in
- only pay this cost for near-mate tactical results

Why this is ranked third:
- It is narrower than TT or move generation improvements, but it directly targets tactical false positives.

## 4. Multithreaded Root Search

Estimated impact: medium-high

Why it matters:
- PentaZen spreads iterative deepening work across helper threads and keeps a shared best result: <https://github.com/sun-yuliang/PentaZen/blob/master/src/search.cpp>
- We currently run search on one search thread and only use a worker thread in the GUI to keep the UI responsive.

What to take:
- parallel root move search or split-depth iterative search
- shared TT and shared stop conditions
- per-thread stats merged into the final summary

Why this is ranked fourth:
- On modern CPUs this can give a real strength jump.
- But it is riskier than the first three items and touches correctness, determinism, and reporting.

## 5. Better Root Time Allocation Based on Best-Move Stability

Estimated impact: medium

Why it matters:
- PentaZen stretches or shrinks turn time depending on whether the root best move keeps changing across iterations: <https://github.com/sun-yuliang/PentaZen/blob/master/src/search.cpp>
- We already have a version of this idea in the global time governor: [src/engine/TimeGovernor.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/TimeGovernor.cpp:118), [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:450)
- So this is overlap, not a missing feature.

What to take:
- tune our current governor with stronger root-instability reactions
- make the next-iteration affordability estimate depend more on tactical volatility

Why this is ranked fifth:
- Good tuning opportunity, but not a major missing capability.

## 6. More Aggressive VCF-Specific Move Generation

Estimated impact: medium

Why it matters:
- PentaZen uses dedicated VCF root/child generators that only keep B4-forming tactical moves meeting extra conditions: <https://github.com/sun-yuliang/PentaZen/blob/master/src/movegen.cpp>
- We already run a VCF probe at leaves and have separate threat-sequence machinery: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:962), [src/engine/ThreatSearch.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/ThreatSearch.cpp:253)

What to take:
- make VCF entry conditions and move generation more selective
- avoid spending VCF budget on weak pseudo-forcing moves

Why this is ranked sixth:
- It should help tactical sharpness, but we already cover part of the same space.

## 7. Internal Tactical State Caches Similar to F3/B4 Packs

Estimated impact: medium, but expensive

Why it matters:
- PentaZen keeps explicit tactical packs and incremental material-style counters for patterns like `F3` and `B4`: <https://github.com/sun-yuliang/PentaZen/blob/master/src/board.cpp>
- We already maintain incremental threat info, near-stone counts, and hashes: [src/engine/GameState.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/GameState.cpp:159), [src/engine/GameState.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/GameState.cpp:178), [src/engine/GameState.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/GameState.cpp:611)

What to take:
- only the smallest targeted caches if profiling shows a real hotspot
- avoid wholesale adoption of their board representation

Why this is ranked seventh:
- It might help speed, but it pushes toward a board-architecture rewrite if taken too far.

## 8. Full Board Representation Rewrite Toward PentaZen's Model

Estimated impact: unclear

Why it matters:
- PentaZen's strength is tied to a heavily specialized board, pattern-table, and incremental-update design: <https://github.com/sun-yuliang/PentaZen/blob/master/src/board.cpp>
- That system is coherent inside PentaZen, but adopting it here would mean replacing core assumptions in `GameState`, `Threats`, and candidate generation.

Why this is ranked last:
- This is not a clean upgrade. It is effectively a new engine.
- The risk-to-reward ratio is poor compared with focused upgrades above.

## Things We Already Largely Have

These are present enough that they are not strong upgrade candidates by themselves:
- LMR: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:895)
- aspiration windows: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:409)
- null-move pruning: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:1012)
- internal iterative deepening: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:1036)
- killer/history/counter ordering: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:613), [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:722)
- leaf VCF probing: [src/engine/Search.cpp](/home/urban/Code/C++/gomoku-minimax/src/engine/Search.cpp:962)

## Recommended Order

If the goal is practical strength gain with reasonable risk:

1. stronger TT
2. staged threat move generation
3. win verification re-search
4. multithreaded root search
5. VCF generator tightening

