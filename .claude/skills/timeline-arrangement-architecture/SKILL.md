---
name: timeline-arrangement-architecture
description: "Map of the Timeline/Arrangement system: Clip/Lane/TrackGroup model, drawing/compositing, selection settings, sample vs clip, grouping, audio/video playback paths, retrigger, stretch/BPM, live-drawn waveform. Use before planning/reviewing src/arrange/ or the Arrange panel, or for \"how does the timeline work\"."
---

## When to use (full scope)

Map of how Infinite's Timeline/Arrangement system actually works today - the single Clip/Lane/TrackGroup data model, how clips are drawn and composited, what settings each selection kind exposes, real vs. informal type distinctions (sample vs clip), grouping/nesting, the playback/playhead call paths for audio and video, retriggering, per-sample stretch/BPM, signal application order, and why the waveform is always live-drawn rather than cached from disk. Use before planning or reviewing any change to src/arrange/ or the Arrange panel in main.cpp, when asked "how does the timeline work", "what does a clip/track/group actually store", "does X exist for the timeline yet", or when deciding where a new timeline feature belongs.

Paths below are relative to the repo root (`/Users/namansoni/infinte`). Everything here was
verified against code (file:line) as of commit `83fd442` (post "Arrangement overhaul",
`101c874`), then updated for the `timeline-arrangement-improvements` pass (branch
`claude/timeline-arrangement-improvements-81c7c7`, see below). Where something the codebase's
own docs/UI implies does **not** actually exist, it's called out explicitly — don't assume
otherwise. Line numbers throughout may have drifted a bit further since (they've already
shifted by ~340 lines from the bounce-removal alone) — treat them as approximate anchors, not
exact addresses; grep the function/symbol name first.

**What changed in the `timeline-arrangement-improvements` pass**, since the sections below
still describe the *post*-change state as current fact rather than calling out the diff:
- Individual-clip "Bounce to Sample" / "Bounce / Render Clip" is **gone entirely** (menu items,
  inspector button, `ArrangeBuildClipScopedRenderJob`/`ArrangeBuildClipBounceRenderJob`/
  `ArrangeApplyClipBounceResult`, the `bounceClipId`/`clipScope` job fields,
  `gArrangeRenderActiveClipScope`). "Render Track" and "Render Group"
  (`laneScope`/`gArrangeRenderActiveLaneScope`/`ArrangeCommitLaneScopedRenderJob`) are untouched
  and still the only render/export entry points.
- The per-clip "Trigger mode (Timeline / Retrigger)" UI row is **removed** from the inspector.
  `Clip::retrigger` (`ArrangeModel.h:119`) still exists and still defaults `true`, but nothing
  in the UI writes `false` to it anymore — every clip retriggers.
- Audio Sample tempo (redesigned 2026-09-16, `bugfix/arrange-sample-bpm-redesign`): one rule
  everywhere (RunTopology, static waveform, split/trim offsets) - `effBpm = syncToTempo ?
  sampleBpm : liveTempo`, source second at beat b = `sourceOffsetSeconds + (b - start) * 60 /
  effBpm`. Synced = time-stretched (Signalsmith Stretch via `ClipTimeStretch`) by tempo/sampleBpm and follows tempo changes live; unsynced
  = native speed, box fixed in ticks (tempo change reveals/hides the tail). Sample BPM is always
  editable but only audible while synced; toggling sync or editing it while synced rescales the
  box (`ArrangeRescaleSampleBox` via `TrimEdge`). `origBpm` = detected BPM, display only (<= 0 =
  none); drop turns sync on only when a BPM was detected. Audio thread: `SetClipSamplePosition`
  per block, position-locked in `AudioFilePlayerAudioNode::ProcessClipBlock`
  (`gClipSampleDriftReseeks` must stay 0). Measured by `INFINITE_ARRANGESAMPLETEST=<dir>` and
  `INFINITE_ARRANGESAMPLEEXPORTTEST=<dir>` (WAV + MP4 through the real render queue).
- The track header row's inline Opacity/Solo/Mute/Gain/Pan mix strip was briefly moved to the
  docked inspector only, then **restored** to the header row on user request — it lives in both
  places now (the header strip for quick access, §3's "Track (lane)" inspector row for the full
  set). The strip is vertically centered on the row's *effective* (possibly resized) height, not
  a fixed one, and is skipped entirely if a row is shrunk below the strip's own height.
- Track rows are user-resizable (drag a row's bottom border); waveform rendering scales to the
  resized row height instead of a fixed pixel height. Drag-resize is floored at the default
  30px row height (`kMinLaneHeight == kLaneHeight`) — rows can only grow, not shrink below
  default — capped at 160px. A toolbar toggle next to the viewport button resets every track
  back to default height in one click.
- Below the last track, the vertical beat/bar gridlines now continue into the empty scroll area
  instead of stopping dead at the last row, so the timeline reads as one continuous grid
  regardless of track count.
- The Arrange panel's monitor/viewport preview (`gArrangeShowViewport`) now **defaults off**.
- Double-clicking an already-open clip/track/group inspector now **closes** it (toggle) instead
  of just re-opening the same selection.
- Clip-source routing (assign/reassign/clear, gain/pan/pitch, audio topology) was audited
  end-to-end and confirmed **already fully generic** via `IAudioSource`/virtual dispatch — see
  §6's note on retrigger/pitch/rate being deliberately opt-in per node type, which is the only
  place behavior legitimately varies by concrete node type.

**Group clips, render to track, helper API (branch `feature/groups-timeline-tools`):**
- **Group as clip source.** A canvas `GroupNode` can be a clip's `srcUid` on ANY lane type
  (`ArrangeClipSourceFits`, `src/app/panels/ArrangeTools.cpp`). Gestures: canvas group
  right-click > Add to Timeline (`AddNodeToArrangeTimeline`, natural lane =
  `ArrangeNaturalLaneType`: audio if a member makes sound, else video if one makes pictures),
  and every "Assign Node..." path (the canvas picker in `StageSidePanels.cpp` tries nodes
  first, groups second, since a group's frame covers its members). There is no canvas-node
  drag onto the timeline - those two are the only gestures. A group with >= 1 clip anywhere
  is audible/visible ONLY inside its clips; `GroupTimelineWindows(groupUid, out)` returns
  false when it has no clip, else true + sorted, merged `[startBeat, endBeat)` windows of
  its playing clips (clip enabled, `LaneEffectivelyEnabled`, audio lane not muted / not
  silenced by another lane's solo, inside `gArrangeRenderActiveLaneScope`). true + empty =
  gated and silent. Cached on `gArrange.revision` + a lane-scope signature, model-only (no
  node lookups). The gate itself (audio/notes/images at the group boundary, group
  mute/solo winning) lives in the group code (UtilityNodes/PreviewBodies/AudioTopology/
  AudioEngine), not here. A group clip never becomes an audio terminal
  (`ArrangeIsGroupSource` skip in `RebuildAudioTopology`'s scheduled loop) nor a video
  layer (`CollectArrangeVideoLayers`), and does not count as video for the Render dialog
  (`ArrangeRenderVideoClipsInRange`). Drawn in the group's colour with a small frame glyph
  and no waveform; the inspector says "Group Clip" and shows the gate instead of
  gain/pan/pitch/fades/grade. Deleting the group goes through the generic
  `Arrange::ClearSource` (clip goes offline). `INFINITE_ARRANGEGROUPTEST` covers it.
- **Render to track** (`src/app/panels/ArrangeRenderToTrack.cpp`) is the canvas-side
  bounce (per-clip bounce stays removed). Node/Audio Out/Group right-click > "Render to
  track..." (popup: start/end via `ArrangeTickField`, tail s, normalize). Offline,
  sample-accurate, an `OfflineTake` (`app/graph/OfflineTake.cpp`, shared by every audio-only
  offline render): device stopped, transport `SeekBeats` + offline mode (loop suspended),
  `gHeadlessAudioRate` carries the take rate so `RebuildAudioTopology` prepares at it
  (no device is opened), `ForceAudioRepare`, tapped nodes forced into the topology via
  `OfflineTakeForceNodes` -> `RenderToTrackForcedNodes()` (seeded in `RebuildAudioTopology`),
  `AudioProbe` armed in offline mode on each tap's first output, `OfflineTakeRender` ~60 ms
  per frame (modulation + audio-node cook stepped every 1/60 s of rendered audio, then
  `ProcessOffline`). A group taps every member whose audio leaves the group (cable to a
  non-member or into an Audio Out/Output), max 4 (probe slots), summed - pre-gate
  (`RenderTapsForNode`). `OfflineTakeRestore` gives everything back and re-prepares the graph
  so live playback does not inherit the take's voices. Then: 16-bit WAV (kept at 16-bit: it is
  an intermediate the timeline decodes again) in `AudioRecordings::GenerateFilePath`, ONE
  `PushUndoCheckpoint` (SpawnNode/ArrangeEdit suppressed) for a new bottom audio lane +
  `ArrangeImportMediaFile` at the range start with `ArrangePendingImport::keepNativeSpeed`
  (BPM detection never turns Sync on). Job stays active until the decode lands.
  Pumped from `ArrangeRenderQueueTick`; dialogs drawn from
  `DrawArrangeWavRenderProgressWindow`; `ArrangeRenderBusy()` includes it.
  API: `StartRenderNodeToTrack`, `RenderToTrackState()`.
- **Render to file / Export audio** (`src/app/panels/ArrangeRenderToFile.cpp`): the same
  taps and `OfflineTake`, but into a WAV the user names (16/24-bit PCM or 32-bit float, any
  rate - the take renders at `OfflineTakeRate()` and `audio/dsp/StreamResampler.h` converts),
  written block by block (no buffer of the take), no timeline change, no undo. Start/length in
  seconds (default: the loop, else 0..60 s). Node right-click "Render to file..." (Mixer and
  Audio Out: "Export audio..."). Exact WAV layout in `audio/WavFormat.h` (fmt + data, float
  adds fact; never acid/smpl/bext/LIST). API: `StartRenderToFile`, `RenderToFileState()`
  (peak/RMS dBFS of what was written); `ArrangeRenderBusy()` includes it.
  `INFINITE_RENDERFILETEST`.
- **Helper API for the assistant** (`ArrangeTools.cpp`, declared in `AppShared.h`):
  `ArrangeClipsJson`, `ArrangeAddClip`, `ArrangeMoveClip`, `ArrangeDeleteClip`,
  `ArrangeSetClipProps`, `ArrangeSetLoop(bool,double,double,err)`. They edit a COPY of
  `gArrange` via `Arrange::` ops, `Validate`, then swap in (revision moves, topology
  rebuilds, patch dirty). They NEVER push undo - the RPC caller pushes
  `PushUndoCheckpoint()` first. Add/move refuse overlaps with an error (the panel's
  `PlaceOverwrite` would carve instead); move lifts the clip out and re-places it (no trim
  semantics, id/group kept; only group clips may change lane type).

**MIDI lanes, piano roll, capture (branch `feature/groups-timeline-tools`):**
- **Model.** `Arrange::kLaneMidi` (= 2). The lane names ONE instrument:
  `Lane::instrumentUid` (any node with a note input - `ArrangeIsNoteInstrument`, i.e. some
  `NoteInputSlot(slot) != nullptr`). Its clips keep every common field but `srcUid` stays 0
  (Normalize zeroes it) and hold `Clip::notes` (`Arrange::MidiNote {start, length, note,
  velocity}`, ticks relative to the clip start, sorted by (start, note)). Notes past the clip
  end are kept but not played/drawn (trim-end is lossless); `Split`, `CarveRange`
  (overwrite/paste/duplicate landing inside), start-trims and `TrimGroupEdge` crop them
  (`CropMidiNotesLeft/Right`: a note crossing a cut ends there on the left and starts at 0 on
  the right). `ClearSource(uid)` also zeroes a lane's `instrumentUid`. `LaneHasMixer(type)`
  (audio + MIDI) is the solo set: a soloed MIDI lane silences unsoloed audio lanes and vice
  versa (AudioTopology, ArrangeTools group windows, the panel). MIDI clips only move between
  MIDI lanes; `ArrangeClipSourceFits(*, kLaneMidi)` is false (no group clips there).
- **Persistence.** `stream 2 ...` (an older reader clamps it to video) + `streammidi <i>
  <instrumentUid>` + `clipnotes <i> <clipId> <count> [<start> <len> <note> <vel>]*` (own
  append-only tags, Patch.cpp; JSON export carries `instrumentUid` / `notes`).
  `ArrangeContentEqual` compares notes and instrumentUid, so piano-roll gestures undo.
- **Playback** (`src/audio/TimelineNoteMerger.h`, `ArrangeMidiAugmentTopology` in
  `src/app/panels/ArrangeMidi.cpp`, called from `RebuildAudioTopology` right after the note
  wiring is described). One `TimelineNoteMerger` (an `AudioNode`, `noteOnly`) per instrument
  uid, inserted into `order` just before the instrument; it takes over the instrument's
  `NoteWire` (same producer outbox, same cursor, same group gate) and the instrument's wire is
  pointed at the merger's outbox, so canvas notes and timeline notes merge, sorted by
  `frameOffset` (offs first on a tie). Notes play only under timeline routing (like audio
  clips) and only from enabled clips on playing lanes (scope, `LaneEffectivelyEnabled`,
  mute/solo). They are published as an immutable `TimelineNoteSnapshot` (absolute beats,
  note-off clipped to the clip end, a key per note) by one atomic pointer store, only when the
  content changed; a replaced snapshot (and an unused merger) is freed once
  `CompletedGeneration()` passes the generation that could still read it (immediately with no
  device). The merger tells snapshots apart by `serial`, never by address. Per block it reads
  `BlockStartBeats()`, places note-ons/offs on the exact sample, handles a loop wrap inside the
  block, and releases everything it holds on stop, on any jump (block start != last block's
  end), at the wrap, when a new snapshot no longer has the note (edit/mute/delete) and on
  re-prepare - a note that survives an unrelated edit keeps sounding. A merger no lane wants
  stays (empty) while it still holds a note. Under timeline routing each MIDI lane is also
  its instrument's way to the device: a windowed terminal over the whole timeline with
  `sourceNode = nullptr` (no position lock / pitch override pushed into an instrument), lane
  gain/pan/mute/solo, silent while paused; skipped when that instrument is already an
  audio-lane clip source. Piano-roll audition goes through the merger
  (`ArrangeMidiAudition`).
- **Editing UI** (`src/app/panels/ArrangePianoRoll.cpp`). MIDI clips draw blue with a mini
  piano roll (`ArrangeDrawMidiClipPreview`); double-click a MIDI clip = floating piano roll
  (`OpenArrangePianoRoll`, drawn from `DrawArrangeWavRenderProgressWindow`), double-click/
  pencil on an empty MIDI lane = empty one-bar clip (`ArrangeAddEmptyMidiClip`). Gestures are
  listed at the top of that file; drags are one `ArrangeGestureBegin/End` each
  (`ArrangePianoRollGestureOpen` keeps the panel's stray-gesture cleanup off them), Delete
  goes through `ArrangePianoRollOwnsKeys` so the canvas/timeline do not also delete. Grid =
  `MusicTime::QuantizeGridList`, snapped on the timeline's absolute grid. "Add MIDI Track"
  submenu (`ArrangeDrawAddMidiTrackMenu`) in every add-track menu; instrument picker in the
  track inspector (`ArrangeDrawMidiLaneInspector`); clip inspector section
  (`ArrangeDrawMidiClipInspector`); canvas instrument right-click > Add MIDI track for this.
- **Capture notes to timeline** (`ArrangeMidi.cpp`). Note producer right-click (popup: range,
  instrument = first one it is cabled into by default). MIDI File / Audio to MIDI notes are
  read directly (`GetSong()` beats with position/speed/loop/transpose; `Notes()` seconds,
  transport-locked with offset/loop). Anything else renders offline like render to track
  (device stopped, offline mode, `ProcessOffline` in 64-frame blocks, restore) with a
  `NoteCaptureTap` wired to the producer's outbox by its own cursor; on/off paired (voice id,
  else pitch+source), held notes end at the range end; a new MIDI lane + one clip at the range
  start, one undo step (UI path; the API path leaves undo to the caller).
  `CaptureNotesBusy()` is part of `ArrangeRenderBusy()`.
- **API** (AppShared.h): `ArrangeAddMidiLane`, `ArrangeSetMidiLaneInstrument`,
  `ArrangeSetMidiNotes` / `ArrangeMidiNotesJson` (`[{t, dur, midi, vel}]`, beats from the clip
  start), `StartCaptureNotesToTimeline` + `CaptureNotesState()`; `ArrangeAddClip` on a MIDI
  lane makes an empty MIDI clip; `ArrangeClipsJson` reports lane kind `midi` (+
  instrumentUid/Index/Type) and clip kind `midi` (+ noteCount). `INFINITE_ARRANGEMIDITEST`.

## The data model (read this first)

Everything below hangs off one file: `src/arrange/ArrangeModel.h` (~390 lines, read it whole
before touching Arrange code).

```
Arrange::Model
 ├─ lanes: vector<Lane>            ("tracks")
 │   └─ clips: vector<Clip>        (sorted, non-overlapping per lane)
 ├─ trackGroups: vector<TrackGroup>  (parentGroupId chains -> real recursive nesting)
 ├─ markers
 ├─ settings
 └─ revision                       (bumped on every edit; the dirty/rebuild signal)
```

- **`Clip`** (`ArrangeModel.h:57-104`) is the **only** clip struct. There is no `AudioClip`,
  `VideoClip`, `AudioSample`, or `VideoSample` type anywhere in the codebase. Fields are just
  commented "audio only" / "video only" (`pan`/`pitch`/`syncToTempo`/`retrigger` at 74-94 are
  audio-only; `blendMode`/`opacity`/`colorBrightness/Contrast/Saturation` at 71,90-93 are
  video-only). A clip's effective type comes transitively from its owning `Lane::type`.
- **`Lane`** ("track", `ArrangeModel.h:106-121`): id, `type` (`kLaneVideo`/`kLaneAudio`,
  line 52), per-type settings (`opacity`/`gainDb`/`pan`/`mute`/`solo`), `groupId`,
  `colorR/G/B`, `clips`.
- **`TrackGroup`** (`ArrangeModel.h:131-139`): named/colored container for lanes *and other
  track groups* via `parentGroupId` (0 = top level). **No collapsed-render suppression** —
  `collapsed` is a stored UI field but never hides a subtree from drawing (comment,
  `ArrangeModel.h:129-130`).
- Storage is **flat vectors** throughout (`Model::lanes`, `Model::trackGroups`) — nesting is
  expressed only through id links (`parentGroupId`, `Clip::groupId`), not a tree of pointers.
- Persisted in `Patch::Data` as `streams`/`markers`/`trackGroups`/`arrangeSettings`
  (`src/core/Patch.h:375-393`). `ArrangeSettingsRecord` (`Patch.h:355-373`) deliberately omits
  "audio mode" — the app always starts in Canvas mode; that flag must never round-trip.

## 1. Rendering

No dedicated `TimelineView`/`ArrangePanel` class — it's monolithic inside `main.cpp`:

| What | Function | Location |
|---|---|---|
| Whole panel (ruler, rows, clips, drag, marquee, keys) | `DrawArrangePanelContent()` | `main.cpp:29247` |
| Docking wrapper (4 orientation call sites) | `DrawArrangePanelDocked` | `main.cpp:32902` (called from `63452, 63470, 81786, 82111`) |
| Group header row | `DrawArrangeGroupHeaderRow` lambda | `main.cpp:31030` |
| Per-clip rect/waveform/thumbnail | inline in the panel loop | `main.cpp:~31600-31800` |
| Settings/inspector panel | `DrawArrangeClipSettingsChild(panelW)` | `main.cpp:37460-38148` |

**Video compositing is a separate concern from UI chrome** — it happens once per frame after
the normal cook loop, not inside the panel draw call:
`CollectArrangeVideoLayers` (`main.cpp:27247`) → `CompositeArrangeTimelineVideo`
(`main.cpp:27361`) → `ArrangeComposeShader()` (`main.cpp:27292-27350`, hand-written GLSL blend/
grade shader) → `CompositeArrangeMonitorIfRequested()` (`main.cpp:27522`, called from the main
loop at `main.cpp:83461`). This is a known "fans out across several places" hotspot — see
`codebase-navigation`.

**Color/render settings that actually exist:**
- Clip tint: `Clip::colorR/G/B` + `hasTint` flag, palette swatches in inspector
  (`main.cpp:37799-37810`, applied at draw time `31700-31706`).
- Type default palette (not user-editable): video = purple, audio = green
  (`main.cpp:31701-31706`, exact ARGB values there).
- Muted/offline: desaturated grey + diagonal hatch (`main.cpp:31707-31727`,
  `DrawArrangeHatch` at `28467`).
- Group accent stripe/border: `ArrangeGroupColor(groupId, alpha)` (`main.cpp:28455`).
- Track-group color: `TrackGroup::color`, set via `RecolorTrackGroup` (`main.cpp:38094-38099`).
- Waveform color: **hardcoded**, not a setting — `IM_COL32(255,255,255,115)` normal / `...,60`
  muted (`main.cpp:31741`).
- Video grade ("Bright"/"Contrast"/"Sat" in inspector) → `Clip::colorBrightness/Contrast/
  Saturation`, applied in-shader via `uGrade` (`main.cpp:27321-27323`).

**No "bounce color" exists, and no per-clip bounce/render exists anymore either.** The
`83fd442`/`cae4444` "clip-bounce" commits (clip-scoped render/export, via
`gArrangeRenderActiveClipScope`) were fully removed in the `timeline-arrangement-improvements`
pass — Bounce to Sample and Bounce/Render Clip are gone from both the context menu and the
inspector. "Render Track" and "Render Group" (`gArrangeRenderActiveLaneScope`) are the only
render/export scopes left.

## 2. Categorization — "sample" vs "clip" is informal, not a type

There is **one** clip struct for both audio and video. The one real enum,
`Arrange::LaneType { kLaneVideo, kLaneAudio }` (`ArrangeModel.h:52`), lives on the lane, not
the clip.

Drag-drop media import classifies by `Arrange::ImportMediaKind { Audio, Video, Image }`
(`src/arrange/ArrangeMediaImport.h:22`), used only to pick which **node type** to spawn:

| Drop kind | Spawned node | Class |
|---|---|---|
| Audio | `"Audio File"` | `AudioFileNode` (`src/nodes/AnalyzeNodes.h:193`) |
| Video | `"Video"` | `VideoSourceNode` (`src/nodes/VideoSourceNode.h:20`) |
| Image | `"Image Source"` | — |

`SamplerNode` (`src/nodes/SamplerNode.h:51`) exists and is used elsewhere in the graph, but
**drag-drop import never spawns it** — confirmed, every `ArrangeImportMediaFile` path only
calls `SpawnNode("Audio File", ...)` (`main.cpp:28984-28987`). The inspector's pitch control
does special-case both (`dynamic_cast<AudioFileNode*>` / `<SamplerNode*>`,
`main.cpp:37786-37789`), so a clip's `srcUid` can point at either.

**Conclusion:** "audio clip" and "audio sample" are the same object (an `Arrange::Clip` on an
audio lane). Same for video. Build any future UI/feature language around **lane type + clip**,
not a nonexistent sample/clip split — unless you're deliberately introducing that split, in
which case this is the place it needs to land.

**Rule: never gate clip *behaviour* on `sampleDropped`.** It only means "was created by a
file drop". Gating on it made the hand-assigned-source path a second, untested path: until
v0.4 such clips skipped the position lock and the retrigger, and Sampler-sourced clips got
no pitch at all. Gate on what the source node can do instead. Every audio clip now
position-locks. The engine offers both `SetClipSamplePosition` and `SetClipPitchOverride`,
because it can't tell which one a node consumes (`SamplerNode`/`WavetableSynthCore`
implement only the pitch override). Only the source-time *mapping* (`sampleBpm`,
`syncToTempo`) is Sample-specific. Cross-lane conflicts are recorded for audio and video
(`ArrangeVideoSourceConflictClips()`), warned in the inspector, never suppressed.
`INFINITE_ARRANGESAMPLETEST=<dir>` and `INFINITE_ARRANGESAMPLEEXPORTTEST=<dir>` cover this
(both in the hygiene driver).

## 3. Per-selection-kind settings

All defined in one function, branching on selection kind:
`DrawArrangeClipSettingsChild` (`main.cpp:37460-38148`).

| Selection | Fields | Where (approximate, re-grep before trusting) |
|---|---|---|
| Single clip (both) | Name, Active/Bypassed, Start, Length, Fade In/Out, Color Tint, Source Node assign/clear. **No Trigger mode row** — retrigger is implicit and always on, not user-facing. | `~37556-37840` |
| + video only | Blend mode, Opacity, Bright, Contrast, Sat | `~37669-37746` |
| + audio only | Gain (dB), Pan, Pitch (semitones, live into node), Tempo Sync (Sample only — Sync checkbox, always-editable "Sample BPM", detected-BPM/stretch readout, Reset to Detected) | `~37747-37795` |
| Multi-clip | Bulk rename, bulk Active/Bypassed, bulk tint, conditional Ungroup, Delete Selected | `~37841-37935` |
| Track (lane) | Name, Active/Bypassed, audio: Solo/Mute/Gain/Pan, video: Opacity, Track Tint, Duplicate/Delete. Duplicated by the header row's own mix strip (same fields, quick-access) — see §1's changelog note. | `~37936-38065` |
| Track Group | Name, Active/Bypassed, Group Color, Add Video/Audio Track, Ungroup (Keep Tracks), Delete Group+Tracks | `~38066-38131` |
| Nothing selected | placeholder text | `~38137-38145` |

A nested track group's own record is an identical `TrackGroup` — parent nesting adds no extra
fields.

## 4. Grouping / nesting — two independent mechanisms

```
Clip group (flat)              Track group (recursive)
  Clip.groupId ─┐                TrackGroup.parentGroupId
                ├─ dissolves        │
                │  at ≤1 member     ├── TrackGroup (child)
  Clip.groupId ─┘                   │     └── Lane
                                    └── Lane
```

- **Clip group**: flat, a clip belongs to ≤1 group, auto-dissolves at ≤1 remaining member
  (`ArrangeModel.h:258-260`). Ops: `Group`/`Ungroup`/`RemoveFromGroup`/`ClipsInGroup`/
  `TrimGroupEdge`/`ScaleGroup` (`ArrangeModel.h:261-272`).
- **Track group**: real recursive nesting, never auto-dissolves (only pruned when a whole
  subtree is empty, `ArrangeModel.h:127-129`). Traversal: `TrackGroupChildren(parentGroupId)`
  returns one level of interleaved lanes+child-groups (`ArrangeModel.h:349-358`); callers
  recurse manually — there's no single "give me the whole tree" call. `GroupAncestors`/
  `GroupDepth`/`LanesInTrackGroupRecursive` (`326-335`) support ancestor queries.
  `LaneEffectivelyEnabled` (`343-348`) is the **one place the whole ancestor chain is read
  together** — a disabled ancestor group must silence a lane even if the lane itself looks
  enabled. Any new "is this lane active" check must go through this, not re-derive it.

## 5. Playback / playhead engine

**Video**: per frame, `CollectArrangeVideoLayers(beat, out)` (`main.cpp:27247`) walks video
lanes back-to-front, finds the one clip per lane covering `beat` (from `Transport::Beats()`),
resolves `GraphNode* gn = FindNodeByUid(c.srcUid)`, then `CompositeArrangeTimelineVideo`
(`27361`) pulls either:
- an `IGeometrySource` → rendered into a pooled `NodeViewport` (`gArrangeGeomViewports`,
  `main.cpp:26925-26949`), or
- an ordinary image node → `gn->node->GetOutputTexture(...)`, whatever the node's own
  `CookIfNeeded` already produced this frame in the normal cook loop.

**Key fact:** the Arrange clip never drives the video source's position — `VideoSourceNode`
reads **global `Transport` time** directly (see `VideoSourceNode.h:12-14`). The clip only
decides *whether* that node's current frame is shown at this beat, not *which* frame the node
decodes.

**Audio**: `RebuildAudioTopology()` (`main.cpp:36409+`) turns every enabled audio-clip into a
`ClipWindow` (start/end beat, gain, fades, pan, retrigger flag — `src/audio/AudioEngine.h:69`)
grouped by `AudioTerminal` (one per lane+srcUid+srcOutput). Per block,
`AudioEngine::RunTopology` (`AudioEngine.cpp:171-559`) does:

```
1. Retrigger pass   (206-236)  — seek source node if a window onset is in this block
2. Node cook loop   (257-304)  — source node free-runs, writes its own output buffer
3. Terminal summation (325-558) — gain × fade × declick envelope, then pan, then sum
```

The source node has **no idea** it's being played by a timeline clip unless retriggered —
gating/mixing all happens downstream, in step 3.

## 6. Retriggering

Real, and **not restricted to a "Sample" type** in the data model — `Clip::retrigger`
(`ArrangeModel.h:94`) is a plain per-clip bool on any audio-lane clip. It only does something
useful when the source node can be seeked (`AudioFileNode`/`SamplerNode` respond to
`RequestRetrigger()`); for a generative/live source it's a harmless no-op.

- **Conflict guard**: two lanes sharing one source node can't retrigger independently (one
  playback position, two consumers). `RebuildAudioTopology` detects
  `lanesPerSrc[srcUid].size() > 1` and force-disables `retrigger` on those windows, surfacing
  the affected clips via `gArrangeRetriggerConflictClipIds` (`main.cpp:36508-36532`) as an
  inline inspector warning (`37656-37664`). **Video has no equivalent conflict guard** — see
  Known gaps below.
- **Engine mechanics**: `AudioEngine.cpp:193-236`, retrigger fires only when a window's
  `startBeat` falls **inside the current block** — see Known gaps for what this means on a
  hard seek.

## 7. Per-sample stretch / BPM

| Question | Answer |
|---|---|
| Can a video be time-stretched? | Only a flat-rate speed multiplier, `VideoSourceNode::speed` (`VideoSourceNode.h:68`, default 1.0, serialized via `VisitParams`). **Per-node only** — the Arrange clip inspector doesn't expose it; must select the node on the canvas. No pitch-preserving/optical-flow stretch exists. |
| Can an audio sample sync its internal BPM to project tempo? | **Yes** (2026-09-16 redesign, see the top-of-file note): synced Samples time-stretch (Signalsmith Stretch, `src/audio/dsp/ClipTimeStretch.h`) by `tempo / sampleBpm` every block and follow live tempo changes; BPM is estimated at drop (`ArrangeEstimateSampleBpm`). |
| What audio pitch control *does* exist? | `Clip::pitch` (±24 semitones) pushed live into `AudioFileNode::pitch`/`SamplerNode::pitch`. This is **varispeed** (rate changes with pitch, turntable-style), not independent pitch-shift — applied in `AudioFilePlayerAudioNode`'s per-sample read: `pitchRatio = 2^(pitch/12); mPos += mPlaybackRate * pitchRatio` (`src/nodes/AnalyzeNodes.cpp:944-956`). |

## 8. Settings application order

**Audio** (`AudioEngine::RunTopology`, `AudioEngine.cpp:171-559`):

```
node's own source DSP (incl. varispeed pitch, baked in during cook)
  -> clip gain x fade x declick envelope        (w.gain, 436-450)
  -> clip pan combined with lane pan             (equal-power, built at topology time, main.cpp:36499-36504)
  -> lane/terminal flat gain                     (346, 508-509)
  -> summed into device buffer
```

**Video** (`CompositeArrangeTimelineVideo` / `ArrangeComposeShader`,
`main.cpp:27361-27424, 27297-27350`):

```
node's own render (any internal grading it does itself)
  -> clip color grade: brightness -> contrast -> saturation  (uGrade, 27321-27323)
  -> blend against accumulated base via Clip::blendMode, opacity = lane.opacity * clip.opacity
     (lane-by-lane, back to front)
```

## 9. Waveform display — always live, never cached from disk

There is **no static/precomputed peak-file path at all**, even for a fully-decoded on-disk
sample. `ArrangeClipWave` (`main.cpp:26962-26992`) is filled exclusively by the audio thread
as the clip actually plays:

```
AudioEngine::RunTopology writes peaks per block (AudioEngine.cpp:454-479)
  -> lock-free ring, AudioEngine::ClipPeaks()
  -> drained once per frame by ArrangeSyncClipVisuals() (main.cpp:27128-27222)
  -> gArrangeClipWaves (main.cpp:26986)
```

Explicit design intent, from the header comment (`main.cpp:26958-26961`): "Never saved and
never pre-decoded: a clip's source is a live node, not a file, so there is nothing to read
ahead of the playhead. A clip that has not been played yet draws a flat centre line." This is
true regardless of whether the underlying `AudioFileNode` already has the whole file decoded
in memory — **the timeline never reads that buffer directly for drawing.** A freshly-dropped,
never-played clip shows a flat line until the transport has passed over it once.

**Scrubbing** is real but ruler-only (not click-on-clip): `ArrangeScrubBegin/Update/End/Cancel`
(`main.cpp:6020-6041`) move a ghost playhead during drag, then call `ArrangeSeekTick` →
`Transport::Instance().SeekBeats(...)` (`6013-6016`) once, on release. This only **relocates**
the playhead — it does not itself start playback; if already playing, `RunTopology` continues
naturally from the new beat next block. Applies uniformly to audio and video, since both read
from the same `Transport` beat.

If you're asked to add pre-decoded/static waveform rendering, this is the boundary to change:
either populate `gArrangeClipWaves` from the source node's already-decoded buffer up front
(for `AudioFileNode`/`SamplerNode` specifically — they're the only two with a full buffer
available ahead of the playhead), or add a second, source-agnostic "has cached peaks" path
that live-fills only fall back to when no cache exists.

## Known gaps (not bugs, but real holes worth flagging before building on top)

**Open test failure (unverified since 2026-09-14):** `INFINITE_ARRANGEWAVETEST`'s "filled by
playback" check failed 0/32 buckets the first time an audio device opened on this Mac, and
it isn't in `known-test-failures.txt`. Re-run it before trusting the live waveform fill.

0. **Clip-source routing is NOT gated by node type — confirmed, not a gap.** An audit of the
   full pipeline (compatibility gates `IsNodeAudioCompatible`/`IsNodeVideoCompatible`, manual +
   context-menu assignment, output-slot selection, gain/pan/pitch writes,
   `RebuildAudioTopology`, `AudioEngine::RunTopology`) found it is already fully general via
   `IAudioSource` and C++ virtual dispatch, with no hardcoded restriction to any concrete node
   type anywhere in that path. The only place behavior legitimately varies by node type is the
   optional `RequestRetrigger()`/`SetClipPitchOverride()`/`SetClipSamplePosition()`
   hooks on `AudioNode` (`src/audio/AudioNode.h:83-136`) — no-op by default,
   overridden only by `AudioFileNode`, `SamplerNode` (pitch only), and `WavetableSynthCore`
   (pitch only) — because a node with no internal playback position has nothing to retrigger or
   pitch-shift. That's deliberate, documented, opt-in architecture, not a routing bug. Don't
   re-investigate this from scratch; if a future report says "routing only works for node X",
   the actual bug is more likely in that specific node's own `IAudioSource`/compatibility
   plumbing, not the shared pipeline.
1. **Retrigger-on-hard-seek is unverified.** The retrigger pass only fires when a window's own
   `startBeat` is inside the *current* block (`AudioEngine.cpp:223-227`). A hard seek (Home/
   End/marker jump/ruler scrub) landing *inside* a `retrigger=true` clip, not at its start, may
   not retrigger it — the source node could keep stale position until the next natural window
   boundary. Confirm at runtime before relying on "retrigger always fires correctly after a
   seek."
2. **Video has no cross-lane conflict guard.** `VideoSourceNode` reads the global `Transport`
   position, not a clip-relative one. Two clips on different lanes (or the same clip moved
   without moving its node) pointing at the same video node would show the same absolute-time
   frame — audio has `gArrangeRetriggerConflictClipIds` to warn about this class of problem;
   video has no equivalent.
3. **Video decode internals not traced here** (`VideoSourceNode::CookIfNeeded`,
   `Platform::VideoFrameAt` on macOS/Windows) — only the Arrange-clip → node's-cooked-texture
   boundary is confirmed. Read `src/nodes/VideoSourceNode.cpp` +
   `src/platform/Platform.mm` / `src/platform/win/MediaWin.cpp` before touching decode timing.

## Where to look when...

| Task | Start here |
|---|---|
| Add a clip/lane/group field | `src/arrange/ArrangeModel.h` struct + `Patch.h` persistence record + `DrawArrangeClipSettingsChild` inspector UI |
| Change how a clip is drawn | `DrawArrangePanelContent`, `main.cpp:~31600-31800` |
| Change video compositing/blend | `ArrangeComposeShader`, `main.cpp:27292-27350` |
| Change audio clip envelope/pan/gain | `AudioEngine::RunTopology` terminal pass, `AudioEngine.cpp:325-558` |
| Add per-clip video speed control to the UI | expose `VideoSourceNode::speed` in `DrawArrangeClipSettingsChild`'s video branch, `main.cpp:37669-37746` |
| Change BPM-sync/time-stretch | keep the single effBpm rule in RunTopology, `ArrangeComputeSampleStaticWave` and `SampleSourceBpm` in ArrangeModel.cpp in lock-step; rerun `INFINITE_ARRANGESAMPLETEST` |
| Add static/cached waveform peaks | `ArrangeClipWave`/`gArrangeClipWaves` fill path, `main.cpp:26951-27222` |
