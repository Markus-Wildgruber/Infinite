# Live control (JSON-RPC)

A running Infinite listens on `127.0.0.1:7777` for newline-delimited JSON-RPC 2.0, so a local tool
(a script, the `Infinite --mcp` bridge, a chat assistant) can build and inspect the patch on the
canvas while you watch. The authoritative code is `src/app/graph/Rpc.cpp` (methods) and
`src/core/RemoteControl.cpp` (transport). For writing patches as files instead, see
[patch-format.md](patch-format.md).

## MCP bridge

`Infinite --mcp` serves these methods as Model Context Protocol tools on stdin/stdout, for an MCP
client such as Claude Code (`src/core/McpBridge.cpp`). It holds no state: each tool call is one
request to the running instance below, so start Infinite first. It reads the token itself,
honours `INFINITE_CONTROL_PORT`, trims `get_graph` to nodes, cables and bindings (no parameter
dump), and returns `render_frame` as a 1024 px JPEG the model can look at.

```json
{"mcpServers": {"infinite": {"command": "C:/path/to/Infinite.com", "args": ["--mcp"]}}}
```

## Connecting

- Loopback only. The port is `7777`, or `INFINITE_CONTROL_PORT` when set before launch. Headless
  and self-test runs do not listen.
- Every request carries the token the app writes at startup to `control_token` in the app support
  folder (`AppPaths::AppSupportDir()`: `%APPDATA%\Infinite` on Windows,
  `~/Library/Application Support/Infinite` on macOS). It changes on every launch.
- One JSON object per line, one reply per line:

```
{"jsonrpc":"2.0","id":1,"token":"<token>","method":"create_node","params":{"typeName":"Shape"}}
{"jsonrpc":"2.0","id":1,"result":{"index":1}}
{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"unknown node type 'Shap' (did you mean: Shape, ...)"}}
```

- Requests run on the main thread between frames, in the order received. A reply arrives within a
  frame or two. A wrong JSON type (a string where an index belongs) is an error reply, never a crash.

## Addressing

| What | Accepted forms |
|---|---|
| Node (`index`, `src`/`srcIndex`, `dst`/`dstIndex`) | the node index from `create_node` / `get_graph`, as a number or digit string; inside a `batch`, `"$<id>"` |
| Input slot (`dstSlot`) | slot number, or its name from `describe` `inputs[].name` (`input`, `input_2`, `audio`, ...) |
| Output (`srcOutput`) | output number, or its label from `describe` `outputs[].label` (case-insensitive) |
| Parameter (`param`) | the modulatable parameter index, or its saved key (`sizeX`), or its on-screen label |

Node indices are not stable across `undo`, `redo` or a rolled-back `batch` (the graph is rebuilt and
renumbered). Re-read `get_graph` after any of those.

A parameter key or label only resolves once the node has reported its controls, which happens on
the frame after `create_node` (or, for an existing collapsed node, after the first request that
asks for them: it answers "retry once" and the next request resolves). A node created in the same
`batch` has not reported yet, so address its parameters by number there, or bind them in a later
request. `get_bindings {index}` lists a node's parameters with their numbers, keys and ranges.

## Methods

### Reading

| Method | Params | Result |
|---|---|---|
| `list_node_types` | | `{<category>: [type, ...]}` |
| `describe` | `type?` | without `type`: `{types}`. With it: `{type, category, params[{key, kind, default}], inputs[{slot, name, kind, label}], outputs[{label, kind, modulator}], canBypass, hardwareDriven, help}`. `help` is the node's reference text, with units and what each output's value means |
| `get_graph` | | the patch as JSON (nodes with `label` when renamed, cables, `modulation`, `expressions`, `globals`) plus `links[{id, srcIndex, srcOutput, dstIndex, dstSlot \| dstParamPin \| dstColorPin}]` |
| `get_params` | `index` | `{"<kind> <key>": "<value>", ...}`, every saved value of the node. Kinds: `f` float, `i` int, `b` bool (`"1"`/`"0"`), `c` colour (`"r g b"`), `s` text - returned literally (a Windows path has single backslashes), not in the patch file's escaped form. Numbered keys count from 0 (`mute0` is the Mixer channel labelled 1). Dropdowns also appear in `_options` (`{"i band1Type": ["low shelf", "peak", ...]}`, index order) once the node has drawn; `_label` is the node's custom name; `_modulated` (`{key: {current, by, param}}`) lists the parameters a modulator or expression drives |
| `get_notes` | `index`, `from?`, `to?`, `low?`, `high?` (MIDI), `limit?` (200) | `{unit, count, range[lo, hi], pitchClasses{}, notes[{t, dur, note, midi, level, channel?}], durationSeconds?, truncated?, warning?}`: Audio to MIDI's transcription (seconds) or a MIDI File's notes (beats) |
| `get_bindings` | `index?` | `{modulation[{dst, param, src, srcOutput, lo, hi, hasRange, enabled, curve, dstKey?, dstLabel?, min?, max?}], expressions[{dst, param, text, curve, error?, shadowedByModulator?}], globals[{name, expr, value, error?}]}`; with `index`, also `params[{param, dstKey?, dstLabel?, min?, max?, options?}]` for that node |
| `explain` | `all?` | `{text, graph}`: the graph as `Infinite --explain` prints it |
| `get_node_position` | `index` | `{x, y}` |

### Building

| Method | Params | Result |
|---|---|---|
| `create_node` | `typeName`, `category?` (looked up when omitted), `x?`, `y?`, `id?` | `{index, id?}` |
| `delete_node` | `index` | `{}`; also drops its cables and every binding to or from it |
| `connect` | `src`, `dst`, `srcOutput?` (0), `dstSlot` | `{}`; the same compatibility rules as dragging a cable |
| `disconnect` | `linkId` (from `get_graph` `links`) | `{}` |
| `set_param` | `index`, `name` (saved key), `value` | `{}`. A dropdown also takes its option name (`"low shelf"`). The value is checked against the key's kind: bools take `true`/`false` (also `1`/`0`, `"on"`/`"off"`), numbers a number or numeric string, colours `[r, g, b]` or `"r g b"`, text a literal string (no escaping - `D:\new\file` stays as written). A mismatch is an error instead of a silent default. Setting a text key also reloads the node's derived state, so a new file `path` loads at once (Audio File, Sampler, Image, ...) |
| `bind_modulator` | `dst`, `param`, `src`, `srcOutput?`, `lo?` + `hi?` (destination units; both or neither, neither = full range), `curve?` (-1..1), `enabled?` | `{dst, param}`; `src`'s output must be a modulator |
| `unbind_modulator` | `dst`, `param` | `{}` |
| `set_expression` | `dst`, `param`, `text` (leading `=` optional; empty removes it), `curve?` | `{dst, param, shadowedByModulator?}` (a wired modulator wins over an expression) |
| `set_global` | `name`, `expr` | `{}`; replaces a global of that name in place, or appends one |
| `remove_global` | `name` | `{}` |
| `auto_wire_inputs` | `index` | `{wiredSlots, feederIndices}`: feeds every empty input with a default source |
| `set_node_position` / `move_node` | `index`, `x?`, `y?`, `dx?`, `dy?` | `{x, y}`; x/y place the node, dx/dy move it from where it is |
| `set_bypass` | `index`, `bypassed?` (omit to toggle) | `{bypassed}`; as the B key: the node passes its input through |
| `rename_node` | `index`, `name` (`""` clears) | `{title}`; saved with the patch (`label` line) |
| `expand_node` | `index` | `{}` |

Each building call is one undo step.

### Groups

A Group's members are an explicit, saved list (Ctrl+G, "Add to group", or these methods); dragging
a node over a group's box does not make it a member. Groups do not nest; a node is in at most one.

| Method | Params | Result |
|---|---|---|
| `duplicate_node` | `index`, `dx?`/`dy?` (40), `connectInputs?` (true), `id?` | `{index, inputsConnected, id?}`: same type, parameters and label, the same input cables; outgoing cables and modulation are not copied |
| `create_group` | `nodes` (indices), `label?` | the group `{index, label, members[], mute, solo, locked}` |
| `group_add` / `group_remove` | `group`, `nodes` | the group |
| `set_group` | `group`, `mute?`, `solo?`, `locked?`, `label?` | the group. `mute` silences everything leaving the group (audio fades over ~10 ms, notes stop, images go black, modulators hold) while members keep running; `solo` mutes every other group, ungrouped nodes are unaffected; `locked` refuses edits to members |
| `list_groups` | | `{groups[]}` |
| `apply_sound_profile` | `index` (Sound Analyze), `target` (synth) | `{applied}`: what Apply wrote |

While a group is locked, every editing method touching one of its members (`set_param`, `connect`,
`disconnect`, `delete_node`, `set_bypass`, `rename_node`, `move_node`, `bind_modulator`,
`unbind_modulator`, `set_expression`, `group_add`/`group_remove`, `apply_sound_profile`'s target)
fails with `Group '<label>' is locked`. Mute, solo, modulation and the timeline keep working.

### Timeline

| Method | Params | Result |
|---|---|---|
| `list_clips` | | `{lanes[], clips[]}` (see the MCP tool description for fields) |
| `add_clip` | `node` (a node or a group), `output?`, `lane?` (-1 = new lane), `startBeats`, `lengthBeats` | `{clipId}`. A node plays during its clips; a group that has clips is audible only inside them |
| `move_clip` | `clipId`, `lane?`, `startBeats?`, `lengthBeats?` | `{}` |
| `set_clip` | `clipId`, `enabled?`, `gainDb?`, `fadeInBeats?`, `fadeOutBeats?`, `name?` | `{}` |
| `delete_clip` | `clipId` | `{}` |
| `set_loop` | `enabled?` (true), `startBeats`, `endBeats` | `{enabled, startBeats, endBeats}` |
| `render_to_track` | `node`, `startBeats?`, `endBeats?` (default: the loop, else 4 bars from the playhead), `tailSeconds?` (2), `normalize?` | `{active, done, ok, progress, message, path, lane, clipId}`; renders offline into a WAV and adds it as an audio clip on a new lane. Poll `render_status` (the MCP tool waits) |
| `render_status` | | as above |
| `render_audio_to_file` | `node`, `start` (s), `seconds`, `tail?` (s), `path?` (absolute, or relative = recordings folder), `bitDepth?` (16, 24 default, 32 = float), `sampleRate?` (48000) | `{active, done, ok, progress, message, path, seconds, sampleRate, channels, peakDb, rmsDb}`; offline from a clean start, sample-accurate, repeatable. Poll `render_file_status` (the MCP tool waits) |
| `render_file_status` | | as above |
| `press_button` | `node`, `button?` | `{pressed}`, or without `button` `{buttons[]}`: the node's own momentary buttons, as clicking them |
| `add_midi_track` | `instrument` (a node with a note input), `name?` | `{lane}`; `add_clip` on it makes an empty MIDI clip |
| `set_midi_notes` | `clipId`, `notes[{t, dur, midi, vel?}]` (beats from the clip start) | `{notes}`; replaces the clip's notes |
| `get_midi_notes` | `clipId` | `{clipId, lane, startBeats, lengthBeats, instrumentIndex, noteCount, notes[]}` |
| `capture_notes` | `node` (a note source), `instrument?`, `startBeats?`, `endBeats?` | `{active, done, ok, progress, message, lane, clipId, noteCount}`; renders the source offline over the range into a MIDI clip on a new MIDI lane. Poll `capture_status` |
| `capture_status` | | as above |

### Transport

| Method | Params | Result |
|---|---|---|
| `transport` | `action?` (`play`, `pause`, `resume`, `stop`, `toggle`, `rewind`, `seek`, `start_audio`, `state` - the default), `fromStart?` (with `play`), `rewind?` (with `stop`), `seconds?` / `beats?` (with `seek`), `bpm?` | `{playing, seconds, beats, bpm, audioRunning, sampleRate, loop{enabled, startBeats, endBeats}, warning?}`. `pause` and `stop` keep the position (`stop` with `rewind: true` goes back to 0); `resume` and `play` carry on from it. `play`, `resume` and `toggle` also start the audio engine, which does not open at launch; `warning` says why when the device would not open |

### Measuring sound

Numbers instead of pictures (`graph/AudioProbeJobs.cpp`, `audio/AudioAnalysis.cpp`). Handlers
never block the UI thread, so measuring is start-then-poll; the MCP bridge's `analyze_audio` and
`analyze_file` tools do the polling and return the finished result in one call.

| Method | Params | Result |
|---|---|---|
| `capture_audio` | `node` or `nodes` (1-4 indices), `seconds?` (3, 0.2-30), `frameSeconds?` (0.1), `offline?`, `start?` (seconds, with `offline`) | `{id, seconds, offline?}`. Taps each node's audio output from the same block on, so several nodes are compared over exactly the same span. An Audio Out is measured through the node feeding it. Live, it fails when the engine is off or a node has no audio output. `offline: true` pauses the device (none needs to be open) and renders the graph itself from `start` (default: the current position) as an offline render does: from a clean start (every node re-prepared, sequencers on step 0 at their position), with modulation applied every 1/60 s of rendered audio, so the result is sample-accurate and identical every run; the measured nodes are processed even if nothing reaches an Audio Out; the device and transport are restored afterwards. One capture at a time |
| `analyze_file` | `path` or `node` (a node with a `path`), `notes?` (false), `sound?` (false), `start?`, `length?`, `f0?`, `partials?` | `{id, seconds}`. Decodes the file and analyses it whole, with tempo and key; `notes: true` adds a Basic Pitch transcription summary (Windows/Linux); `sound: true` adds a `sound` profile (f0, inharmonicity, partials with level and T60, ADSR, spectral envelope, `transient`, `air`): of the region `start`/`length` by default, or with `soundMode: "notes"` merged over the file's isolated transcribed notes in `low`..`high` (MIDI) with `multiNote.keyTracking` (level and harmonic tilt per octave) |
| `get_sound_profile` | `index` (a Sound Analyze node) | its last profile (as `sound` above, plus `file`), or `{status}` |
| `analysis_result` | `id` | `{status: "capturing" \| "analyzing", progress}` while running; `{status: "done", result, note?}`; `{status: "error", error}` |

A capture `result` is `{nodes[{index, type, seconds, rmsDb, peakDb, crestDb, silentRatio, centroidHz,
bands[{hz[lo, hi], db}], onsetCount, onsetsPerSecond, onsets[], pitch{pitchedRatio, medianHz?,
median?, range?, melody[{t, note, dur}]?}, timeline{t[], rmsDb[], peakDb[], centroidHz[], pitchHz[],
bandsDb[][]}, warning?}], compare?[{a, b, levelDiffDb, bandDiffDb[], spectralSimilarity,
spectralShapeSimilarity, correlation, bestLagMs, correlationAtBestLag, medianPitchDiffCents?,
pitchAgreement?, onsetMatch?, onsetRecall?, onsetOffsetMs?, envelopeCorrelation?}]}`. `onsetMatch` is the share of
all onsets on both sides with a partner on the other side within 50 ms - the same spectral-flux detector and
definition as Audio Compare's onset match output; `onsetRecall` is the share of A's onsets B has. Levels are dBFS RMS (a
full-scale sine reads -3 dB); differences are A minus B, and a positive `bestLagMs` means B lags A.
A file `result` has the same per-node fields plus `file`, `channels`, `tempo{bpm, confidence}`,
`key{name, confidence, chroma[12]}` and, with `notes`, `notes{count, perSecond, averagePolyphony,
range, pitchClassCounts[12], first[]}`.

### `batch`

`{"calls": [{"method": ..., "params": {...}}, ...]}` runs the calls in one frame as **one undo
step, all or nothing**: on the first failure everything already applied is undone and the error
names the failing call (`batch call 3 failed, nothing applied - connect: ...`).

A `create_node` with `"id": "<word>"` (a letter or `_`, then letters, digits, `_`) lets later calls
in the same batch use `"$<word>"` wherever a node goes (`index`, `src`, `dst`, `srcIndex`,
`dstIndex`, `output`). Labels last only for that batch.

```json
{"calls": [
  {"method": "create_node", "params": {"typeName": "Shape", "id": "pic"}},
  {"method": "create_node", "params": {"typeName": "Output", "id": "out"}},
  {"method": "create_node", "params": {"typeName": "LFO", "id": "lfo"}},
  {"method": "connect", "params": {"src": "$pic", "dst": "$out", "dstSlot": "in"}},
  {"method": "bind_modulator", "params": {"dst": "$pic", "param": 0, "src": "$lfo"}}
]}
```

Not allowed inside a batch: `batch`, `undo`, `redo`, `new_patch`, `load_patch`, `load_patch_text`.

### Patch, history and output

| Method | Params | Result |
|---|---|---|
| `new_patch` | | `{}` |
| `load_patch` / `save_patch` | `path` | `{}` |
| `load_patch_text` | `text` (patch source) | `{nodes, status}`; replaces the whole graph, one undo step |
| `validate_patch_text` | `text`, `strict?` (true) | `{ok, errors[], warnings[], nodes}`; the canvas is untouched |
| `undo` / `redo` | | `{}` |
| `fit_view` / `fit_view_node` | `index` for the latter | `{}` |
| `render_frame` | `path`, `output?`, `maxSize?` | `{file, width, height, output, frame_stats, previewWidth?, previewHeight?, warning?}`: the Output's current image; with `maxSize` the file is a preview no larger than that on its long side (stats stay full-frame) |
| `screenshot_node` | `index`, `path`, `padding?` | `{width, height}`: PNG of the node as drawn |
