# Include split: recompile less per edit

Status: Phase 0 done, Phase 1 in progress (2026-10-09).
Measure with `tools/include-fanin.py` before and after every phase.

## Problem

The `main.cpp` split (commits `81a6bb42`, `5e49b7d8`) moved main's 300-line
include preamble into `src/app/AppCommon.h` and made it the base of
`src/app/AppShared.h`. Every split-out TU inherited the whole preamble.

Baseline (Windows, MSVC Release, branch `feature/ai-assistant-sidebar`,
git window 120 days / 1415 commits):

| Measure | Value |
|---|---|
| TUs in the `Infinite` target | 312 |
| TUs that include `AppShared.h` / `AppCommon.h` ("app TUs") | 78 |
| Project headers every app TU reads (the umbrella) | 234 (89 `nodes/`, 48 `core/`, 37 `audio/dsp/`, 26 `audio/`, 25 `core/field/`) |
| Compile time of one app TU | ~2 min (8 in parallel) |
| Full rebuild, MultiToolTask x8 | ~21 min |
| TU recompiles caused by header edits in the window | 114,333 |
| Average TUs recompiled per header edit | 86 |
| Share of those caused by `AppShared.h`'s own edits | ~1% |

So the cost is the umbrella, not `AppShared.h` itself. Example: editing
`nodes/Geometry3DNodes.h` recompiles 98 TUs; about 33 use it.

Most expensive headers (TUs x edits):

| Header | TUs | Edits | Recompiles |
|---|---|---|---|
| `platform/Platform.h` | 131 | 69 | 9039 |
| `core/INode.h` | 208 | 23 | 4784 |
| `nodes/Geometry3DNodes.h` | 98 | 37 | 3626 |
| `core/Patch.h` | 83 | 41 | 3403 |
| `nodes/UtilityNodes.h` | 80 | 36 | 2880 |
| `nodes/GeometryOpNodes.h` | 86 | 31 | 2666 |
| `audio/AudioEngine.h` | 85 | 31 | 2635 |

Why the umbrella cannot simply be cut: `AppShared.h` itself names types from
~167 umbrella headers. About 85 of those only as pointer/reference (a forward
declaration is enough). About 30 are needed in full, mostly by inline bodies:
`MakeDelayNode` (~L5680), `StubPredictorNode` (~L2198), `MainGraphHost`
(~L4771), extern globals of value type (`Patch::Data`, `SampleScanner`,
`NodeViewport` map).

Estimate (heuristic, name-match per TU): with a ~20-header core and each app
TU including what it names, app-TU recompiles from header edits drop ~65%
(86.7k -> 30.6k over the window), and a median app TU reads ~40 project
headers instead of 234.

## Phases

### Phase 0 - measure (done)
- `tools/include-fanin.py`: reads MSBuild's
  `Infinite.tlog/Microsoft.Build.CPPTasks.CL.read.1.tlog` or `ninja -t deps`
  output, joins with git churn, prints the table above and the umbrella.
- Baseline recorded above.

### Phase 1 - precompiled header for outside code
- `target_precompile_headers` on `Infinite`: std, `imgui.h`,
  `imgui_internal.h`, `json.hpp`. No project header ever goes in.
- Only `src/**/*.cpp`; vendored sources, `.mm`, and the three `/Od` self-test
  files skip it. Option `INFINITE_PCH` (default ON) to compare or back out.
- Cuts per-TU time, not the number of TUs an edit recompiles.
- Exit: full build faster than baseline on Windows; macOS and Linux CI green
  (watch the CL.exe ICE history - `ci-failure-triage`).

### Phase 2 - slim `AppCommon.h` (the main win)
1. `src/app/AppForward.h`: forward declarations for the classes `AppShared.h`
   uses only by pointer/reference.
2. Move the inline bodies that need full node types out of `AppShared.h`, into
   the one `.cpp` that uses them or a small topic header.
3. Remove `nodes/*`, `audio/dsp/*`, `core/field/*` (and other non-core)
   includes from `AppCommon.h`. Keep a core of about 20: imgui, std, `GLUtil`,
   `INode`, `GraphNode`, `Patch`, `Modulation`, `Transport`, `NodeFactory`,
   the three cables, `ParamRoles`, `CategoryColors`, `NodeViewport`,
   `HeadlessJob`, `BenchReport`, `UiScale`, `AudioBuffer`, `AudioNode`,
   `MusicTime`, `ParamMailbox`.
4. Add explicit includes per TU, one directory per commit (`bodies/`,
   `frame/`, `panels/`, `selftest/`, `graph/`, `ui/`, rest). Every commit
   builds on all three platforms.
- Exit: `include-fanin.py --max-umbrella 40` passes; full self-test round
  trip (`run-infinite-hygiene`) unchanged.

### Phase 3 - split `AppShared.h` by topic
Sections already exist: offline render (~L419), arrangement render jobs
(~L510), audio recovery (~L607), deferred dropdown / columns widgets (~L1270,
~L2840), arrangement video / clip waveforms / thumbnails / terminal PDC
(~L4161-4677), inspector editing (~L4411), patch file watch (~L5419), audio
sweep discovery (~L5602). TUs include only the topic headers they use, so a
change like the assistant panel's 15 lines no longer rebuilds all 78.

### Phase 4 - split `platform/Platform.h`
Single most expensive header (131 TUs x 69 edits). Split by area (audio,
MIDI, window, media, child process, files/paths); implementations unchanged.
Every `Platform::` function keeps its macOS, Windows and Linux sides
(`windows-parity`, `linux-parity`).

### Phase 5 - guardrail
CI step: `include-fanin.py --max-umbrella <budget>` (needs a Ninja deps dump
on Linux), plus a grep that `AppCommon.h` includes nothing under `nodes/`,
`audio/dsp/` or `core/field/`. Without it the umbrella grows back.

## Not planned
- Splitting `core/INode.h` (208 TUs, 23 edits): every node needs it. Revisit
  with Phase 0 data after Phase 2.
- Unity/jumbo builds: worse for small-edit rebuilds.
- C++20 modules: too risky across three compilers.

## Risks
- Only Windows can be built locally here: an include removed for Windows can
  break a macOS/Linux path under `#ifdef`. CI must be green per commit.
- The `main.cpp` split is ongoing; include moves conflict with nearly
  everything. Small commits, landed quickly, coordinated with that work.
- No behaviour change is intended; run the self-test round trip after
  Phases 2 and 3 anyway.
