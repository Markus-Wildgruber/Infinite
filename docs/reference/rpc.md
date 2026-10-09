# Live control (JSON-RPC)

A running Infinite listens on `127.0.0.1:7777` for newline-delimited JSON-RPC 2.0, so a local tool
(a script, the `Infinite --mcp` bridge, a chat assistant) can build and inspect the patch on the
canvas while you watch. The authoritative code is `src/app/graph/Rpc.cpp` (methods) and
`src/core/RemoteControl.cpp` (transport). For writing patches as files instead, see
[patch-format.md](patch-format.md).

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

A parameter key or label only resolves once a node of that type has drawn a frame. A node created
in the same `batch` has not, so address its parameters by number there, or bind them in a later
request. `get_bindings {index}` lists a drawn node's parameters with their numbers and keys.

## Methods

### Reading

| Method | Params | Result |
|---|---|---|
| `list_node_types` | | `{<category>: [type, ...]}` |
| `describe` | `type?` | without `type`: `{types}`. With it: `{type, category, params[{key, kind, default}], inputs[{slot, name, kind, label}], outputs[{label, kind, modulator}], canBypass, hardwareDriven}` |
| `get_graph` | | the patch as JSON (nodes, cables, `modulation`, `expressions`, `globals`) plus `links[{id, srcIndex, srcOutput, dstIndex, dstSlot \| dstParamPin \| dstColorPin}]` |
| `get_params` | `index` | `{"<kind> <key>": "<value>", ...}`, every saved value of the node |
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
| `set_param` | `index`, `name` (saved key), `value` | `{}` |
| `bind_modulator` | `dst`, `param`, `src`, `srcOutput?`, `lo?` + `hi?` (destination units; both or neither, neither = full range), `curve?` (-1..1), `enabled?` | `{dst, param}`; `src`'s output must be a modulator |
| `unbind_modulator` | `dst`, `param` | `{}` |
| `set_expression` | `dst`, `param`, `text` (leading `=` optional; empty removes it), `curve?` | `{dst, param, shadowedByModulator?}` (a wired modulator wins over an expression) |
| `set_global` | `name`, `expr` | `{}`; replaces a global of that name in place, or appends one |
| `remove_global` | `name` | `{}` |
| `auto_wire_inputs` | `index` | `{wiredSlots, feederIndices}`: feeds every empty input with a default source |
| `set_node_position` | `index`, `x`, `y` | `{}` |
| `expand_node` | `index` | `{}` |

Each building call is one undo step.

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
| `render_frame` | `path`, `output?` | `{file, width, height, output, frame_stats, warning?}`: the Output's current image |
| `screenshot_node` | `index`, `path`, `padding?` | `{width, height}`: PNG of the node as drawn |
