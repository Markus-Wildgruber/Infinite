#include "McpBridge.h"

#include "Base64.h"
#include "json.hpp"
#include "platform/AppPaths.h"
#include "platform/NetCompat.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
   #include <fcntl.h>
   #include <io.h>
#endif

#ifndef INFINITE_VERSION_STRING
#define INFINITE_VERSION_STRING "0.0.0"
#endif

namespace McpBridge
{
   namespace
   {
      using json = nlohmann::json;

      // Read by the client once, at initialize: how to work with the tools as a
      // whole. Per-tool rules live in each tool's description.
      const char* kInstructions =
         "These tools edit the node patch open in the running Infinite app while the user watches. "
         "Work in this order: (1) get_graph to see what is already there - never rebuild what exists; "
         "(2) describe every node type before using it - never guess type names, slot names, outputs or "
         "parameter keys; (3) make changes with batch so each request is one undo step and fails as a whole; "
         "(4) check the result with get_graph / get_bindings, for pictures render_frame, and for sound analyze_audio "
         "(numbers: levels, bands, pitch, onsets - compare several nodes in one call) or analyze_file (whole files: "
         "tempo, key, spectrum). Use transport to play/pause/seek, e.g. play from 0 before comparing sources; for "
         "repeatable comparisons use analyze_audio with offline:true (renders the patch itself, sample-accurate). "
         "Tidy or adjust with move_node, set_bypass (instead of zeroing a mix), rename_node and duplicate_node. "
         "Groups (create_group, set_group) mute/solo what leaves them and can be locked; a locked group refuses "
         "every edit, from you too - say so and ask the user before unlocking. The timeline: list_clips, add_clip "
         "(a node or group plays during its clip), move_clip, set_clip, delete_clip, set_loop; render_to_track "
         "freezes a node's sound into a WAV clip. "
         "Node indices change after undo, redo, reload or a failed batch; every node also has a stable uid "
         "(get_graph: \"u123\"), which any tool accepts wherever it takes a node index. "
         "An image only shows if it reaches an Output node; sound only if it reaches Audio Out or an Output's audio input. "
         "Prefer modulators (LFO, Envelope, Audio Analyze, ...) bound with bind_modulator over per-frame expressions "
         "when the user asks for movement. Keep changes minimal and explain what you changed.";

      struct Tool
      {
         const char* name;
         const char* description;
         const char* schema; // JSON Schema of the arguments
      };

      // Each tool is one RemoteControl method of the same name (rpc.md), except
      // get_graph (trimmed here) and render_frame (returns the image itself).
      const Tool kTools[] = {
         { "list_node_types", "All node types by category. Use describe for one type's details.",
           R"({"type":"object","properties":{}})" },
         { "describe",
           "One node type's schema: params (saved keys, kinds, defaults), inputs (slot number, slot name, kind), "
           "outputs (label, kind, whether it is a modulator) and help - the node's reference text, which documents "
           "units, ranges and what each output's value means (e.g. Audio Compare's scaling and direction). Call before "
           "creating, wiring or interpreting a type.",
           R"({"type":"object","properties":{"type":{"type":"string","description":"Node type name, e.g. Shape"}},"required":["type"]})" },
         { "get_graph",
           "The current patch, compact: nodes (index, type, category, bypassed), cables (image/geometry/audio/notes), "
           "modulation, expressions and globals. Parameter values are not included - use get_params for one node.",
           R"({"type":"object","properties":{}})" },
         { "get_params",
           "Every saved parameter value of one node, as {\"<kind> <key>\": value}; kind is f (float), i (int), b (bool, "
           "\"1\"/\"0\"), c (colour) or s (text, given literally - a Windows path has single backslashes). Numbered keys "
           "count from 0: a Mixer's mute0 is the channel labelled 1. Dropdowns are listed again under _options with "
           "their option names in index order (e.g. an EQ band type 0 = low shelf); _label is the node's custom name; "
           "_modulated lists the parameters a modulator or expression drives right now, with their current value.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]}},"required":["index"]})" },
         { "get_bindings",
           "Modulation bindings, expressions and globals with parameter keys and ranges. With index, also lists that "
           "node's modulatable parameters (number, key, label, min, max, current = this frame's value after "
           "modulation) - only known once the node has drawn a frame.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]}}})" },
         { "explain", "The patch as a readable signal-flow description, as `Infinite --explain` prints it.",
           R"({"type":"object","properties":{"all":{"type":"boolean"}}})" },
         { "create_node",
           "Create one node. Returns its index. Category is looked up when omitted.",
           R"({"type":"object","properties":{"typeName":{"type":"string"},"category":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"}},"required":["typeName"]})" },
         { "delete_node", "Delete a node with its cables and bindings.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]}},"required":["index"]})" },
         { "connect",
           "Patch a cable from src's output into dst's input slot. srcOutput and dstSlot take a number or the name "
           "describe lists (dstSlot: inputs[].name like \"input_2\"; srcOutput: outputs[].label). The same rules as "
           "dragging a cable apply.",
           R"({"type":"object","properties":{"src":{"type":["integer","string"]},"dst":{"type":["integer","string"]},"srcOutput":{"type":["integer","string"]},"dstSlot":{"type":["integer","string"]}},"required":["src","dst","dstSlot"]})" },
         { "disconnect", "Remove a cable by its link id (the id field of a get_graph cable; it appears once the canvas has drawn).",
           R"({"type":"object","properties":{"linkId":{"type":"integer"}},"required":["linkId"]})" },
         { "set_param",
           "Set one saved parameter by its key (from describe / get_params). Bools take true/false (or 1/0), numbers a "
           "number, text a plain string (no escaping: a path exactly as written), colours [r,g,b] 0..1. Dropdowns "
           "take the option number or its name (get_params _options lists them). A wrong type is an error, not a "
           "silent 0. Setting a file path loads the file.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"name":{"type":"string"},"value":{}},"required":["index","name","value"]})" },
         { "bind_modulator",
           "Drive a parameter from a modulator output (LFO, Envelope, Random, Macro, Audio Analyze bands, ...). param is "
           "the parameter number, key or label (get_bindings with index lists them). lo/hi: the swing in the "
           "parameter's own units, both or neither (neither = its full range).",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]},"src":{"type":["integer","string"]},"srcOutput":{"type":["integer","string"]},"lo":{"type":"number"},"hi":{"type":"number"},"curve":{"type":"number","minimum":-1,"maximum":1},"enabled":{"type":"boolean"}},"required":["dst","param","src"]})" },
         { "unbind_modulator", "Remove the modulator binding from a parameter.",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]}},"required":["dst","param"]})" },
         { "set_expression",
           "Drive a parameter from an expression evaluated every frame (t = seconds, plus globals). Empty text removes "
           "it. A modulator bound to the same parameter wins over the expression.",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]},"text":{"type":"string"},"curve":{"type":"number"}},"required":["dst","param","text"]})" },
         { "set_global", "Create or replace a named global expression that parameter expressions can read.",
           R"({"type":"object","properties":{"name":{"type":"string"},"expr":{"type":"string"}},"required":["name","expr"]})" },
         { "remove_global", "Delete a global expression.",
           R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})" },
         { "batch",
           "Run several of these tools as ONE undo step, all or nothing: if any call fails, nothing is applied. "
           "Give create_node an \"id\" (letters, digits, _) and refer to that node as \"$id\" in later calls of the "
           "same batch. Parameters of a node created in the same batch must be given by number. "
           "Example: {\"calls\":[{\"method\":\"create_node\",\"params\":{\"typeName\":\"Shape\",\"id\":\"pic\"}},"
           "{\"method\":\"connect\",\"params\":{\"src\":\"$pic\",\"dst\":3,\"dstSlot\":\"in\"}}]}",
           R"({"type":"object","properties":{"calls":{"type":"array","items":{"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method"]}}},"required":["calls"]})" },
         { "undo", "Undo the last change (a whole batch counts as one).", R"({"type":"object","properties":{}})" },
         { "redo", "Redo the last undone change.", R"({"type":"object","properties":{}})" },
         { "validate_patch_text",
           "Check patch-file source (the .inf text format) without touching the canvas. Returns errors and warnings "
           "with line numbers and hints.",
           R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" },
         { "load_patch_text",
           "Replace the WHOLE patch with patch-file source (one undo step). Only when the user asks for a new patch; "
           "validate it first.",
           R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" },
         { "transport",
           "Play, pause or move the timeline. action: play (starts the audio engine too; fromStart:true plays from 0), "
           "pause (keeps the position), resume (plays on from there), stop (= pause; rewind:true also goes back to 0), "
           "toggle, rewind, seek (seconds or beats), start_audio, state (default). bpm sets the tempo. Returns playing, "
           "seconds, beats, bpm, audioRunning and the loop. An Audio File with followTransport on (the default) plays "
           "exactly at the transport time minus its offset, so seeks and restarts land on the same sample; Audio to "
           "MIDI's notes follow the transport the same way.",
           R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","resume","stop","toggle","rewind","seek","start_audio","state"]},"fromStart":{"type":"boolean"},"rewind":{"type":"boolean"},"seconds":{"type":"number"},"beats":{"type":"number"},"bpm":{"type":"number"}}})" },
         { "move_node",
           "Move a node on the canvas: x/y place it, dx/dy move it by that much (canvas units, as get_graph's x/y). "
           "One undo step.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"x":{"type":"number"},"y":{"type":"number"},"dx":{"type":"number"},"dy":{"type":"number"}},"required":["index"]})" },
         { "set_bypass",
           "Bypass a node (it passes its input straight through, or goes silent / stops cooking when it has none) or "
           "enable it again - the same as the B key. Omit bypassed to toggle. Use this to take an effect out instead "
           "of zeroing its mix.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"bypassed":{"type":"boolean"}},"required":["index"]})" },
         { "rename_node",
           "Give a node a title of its own (shown on the canvas and saved with the patch); an empty name goes back to "
           "the type's name. get_graph lists it as label.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"name":{"type":"string"}},"required":["index","name"]})" },
         { "duplicate_node",
           "Copy one node: same type, parameters and label, placed dx/dy (default 40/40) from it, with the same "
           "inputs connected unless connectInputs:false. Outgoing cables and modulation bindings are not copied. "
           "Returns the new index.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"dx":{"type":"number"},"dy":{"type":"number"},"connectInputs":{"type":"boolean"}},"required":["index"]})" },
         { "create_group",
           "Group nodes (indices) into a new Group with a label - the canvas box around them. A node can be in one "
           "group; groups do not nest. Returns the group (index, label, members, mute, solo, locked).",
           R"({"type":"object","properties":{"nodes":{"type":"array","items":{"type":["integer","string"]}},"label":{"type":"string"}},"required":["nodes"]})" },
         { "group_add", "Add nodes to an existing group (group = its index).",
           R"({"type":"object","properties":{"group":{"type":["integer","string"]},"nodes":{"type":"array","items":{"type":["integer","string"]}}},"required":["group","nodes"]})" },
         { "group_remove", "Take nodes out of a group (they stay in the patch).",
           R"({"type":"object","properties":{"group":{"type":["integer","string"]},"nodes":{"type":"array","items":{"type":["integer","string"]}}},"required":["group","nodes"]})" },
         { "set_group",
           "Set a group's mute, solo, locked and/or label. mute silences everything that LEAVES the group (audio fades "
           "out ~10 ms, notes stop, images go black, modulators hold) while its members keep running in time; solo "
           "mutes every other group (ungrouped nodes keep playing); locked forbids moving, rewiring, deleting and "
           "changing values of its members by hand and by these tools (mute/solo/timeline still work). Never "
           "unlock without the user's say-so.",
           R"({"type":"object","properties":{"group":{"type":["integer","string"]},"mute":{"type":"boolean"},"solo":{"type":"boolean"},"locked":{"type":"boolean"},"label":{"type":"string"}},"required":["group"]})" },
         { "list_groups", "Every group with its members and mute/solo/locked state.",
           R"({"type":"object","properties":{}})" },
         { "apply_sound_profile",
           "Press a Sound Analyze node's Apply: write its last analysis onto a synth (target = Equation Synth, "
           "Wavetable, Oscillator, Analog, Metallic or Sampler). Returns what was set. One undo step.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"target":{"type":["integer","string"]}},"required":["index","target"]})" },
         { "list_clips",
           "The timeline: lanes (index, name, kind, mute, solo, enabled, gain) and clips (id, lane, startBeats, "
           "lengthBeats, source node index/type, kind node|group|sample|video, enabled, gain, fades).",
           R"({"type":"object","properties":{}})" },
         { "add_clip",
           "Put a node or group on the timeline: it plays during [startBeats, startBeats+lengthBeats). A group with "
           "clips is audible only inside them. lane -1 (default) = a new lane; clips on one lane may not overlap.",
           R"({"type":"object","properties":{"node":{"type":["integer","string"]},"output":{"type":"integer"},"lane":{"type":"integer"},"startBeats":{"type":"number"},"lengthBeats":{"type":"number"}},"required":["node","startBeats","lengthBeats"]})" },
         { "move_clip", "Move/resize a clip: lane, startBeats, lengthBeats (omitted = unchanged).",
           R"({"type":"object","properties":{"clipId":{"type":"integer"},"lane":{"type":"integer"},"startBeats":{"type":"number"},"lengthBeats":{"type":"number"}},"required":["clipId"]})" },
         { "set_clip", "Change a clip: enabled, gainDb, fadeInBeats, fadeOutBeats, name.",
           R"({"type":"object","properties":{"clipId":{"type":"integer"},"enabled":{"type":"boolean"},"gainDb":{"type":"number"},"fadeInBeats":{"type":"number"},"fadeOutBeats":{"type":"number"},"name":{"type":"string"}},"required":["clipId"]})" },
         { "delete_clip", "Remove a clip from the timeline (its node stays).",
           R"({"type":"object","properties":{"clipId":{"type":"integer"}},"required":["clipId"]})" },
         { "set_loop", "Set the timeline loop region in beats, or enabled:false to clear it.",
           R"({"type":"object","properties":{"enabled":{"type":"boolean"},"startBeats":{"type":"number"},"endBeats":{"type":"number"}}})" },
         { "add_midi_track",
           "Add a MIDI lane that plays into one instrument (a node with a note input, e.g. Equation Synth). Then "
           "add_clip on that lane makes an empty MIDI clip; set_midi_notes fills it. MIDI clips play with the "
           "timeline's audio source set to Timeline.",
           R"({"type":"object","properties":{"instrument":{"type":["integer","string"]},"name":{"type":"string"}},"required":["instrument"]})" },
         { "set_midi_notes",
           "Replace a MIDI clip's notes: [{t: beats from the clip start, dur: beats, midi: 0-127, vel: 0-1}]. One undo "
           "step. The user can edit them in the piano roll (double-click the clip).",
           R"({"type":"object","properties":{"clipId":{"type":"integer"},"notes":{"type":"array","items":{"type":"object","properties":{"t":{"type":"number"},"dur":{"type":"number"},"midi":{"type":"integer"},"vel":{"type":"number"}},"required":["t","dur","midi"]}}},"required":["clipId","notes"]})" },
         { "get_midi_notes", "A MIDI clip's notes ({t, dur, midi, vel} in beats from the clip start) and its lane/instrument.",
           R"({"type":"object","properties":{"clipId":{"type":"integer"}},"required":["clipId"]})" },
         { "capture_notes",
           "Record what a note source plays (Note Sequencer, Note Switcher, Arpeggiator, MIDI File, Audio to MIDI, "
           "...) from startBeats to endBeats (default: the loop, else 4 bars from the playhead) into a MIDI clip on a "
           "new MIDI lane playing into instrument (default: the instrument it is cabled into). Renders offline, so "
           "it is exact and repeatable. Waits until done; returns lane, clipId and noteCount. One undo step.",
           R"({"type":"object","properties":{"node":{"type":["integer","string"]},"instrument":{"type":["integer","string"]},"startBeats":{"type":"number"},"endBeats":{"type":"number"}},"required":["node"]})" },
         { "render_audio_to_file",
           "Render a node's sound (any audio node, a Mixer, or an Audio Out = what feeds it) offline into a WAV: "
           "from start for seconds (transport seconds) plus tail (seconds, to ring out), from a clean start (voices, "
           "envelopes, effect tails and sequencer phases as on a fresh transport start), faster than real time, "
           "sample-accurate and identical every run. bitDepth 16 | 24 (default) | 32 (float); sampleRate default "
           "48000; path absolute, or relative = the recordings folder (empty = named after the node). No tempo "
           "chunks are written. Waits until done; returns path, seconds, sampleRate, channels, peakDb, rmsDb.",
           R"({"type":"object","properties":{"node":{"type":["integer","string"]},"start":{"type":"number"},"seconds":{"type":"number"},"tail":{"type":"number"},"path":{"type":"string"},"bitDepth":{"type":"integer","enum":[16,24,32]},"sampleRate":{"type":"number"}},"required":["node","seconds"]})" },
         { "press_button",
           "Press one of a node's own buttons, exactly like clicking it: Looper rec/play/dub/clear/undo, Audio Out "
           "record/stop, Output record/render, Audio to MIDI record/stop, players' play/pause/restart, ... Omit "
           "button to list the node's buttons. Use this instead of modulator tricks.",
           R"({"type":"object","properties":{"node":{"type":["integer","string"]},"button":{"type":"string"}},"required":["node"]})" },
         { "render_to_track",
           "Freeze a node's sound: renders it offline (sample-accurate) from startBeats to endBeats plus tailSeconds "
           "(default 2, for ringing out) into a WAV in the recordings folder and places it as an audio clip on a new "
           "lane at startBeats. Default range: the loop, else 4 bars from the playhead. normalize optional. Waits "
           "until done and returns the file, lane and clip id. One undo step.",
           R"({"type":"object","properties":{"node":{"type":["integer","string"]},"startBeats":{"type":"number"},"endBeats":{"type":"number"},"tailSeconds":{"type":"number"},"normalize":{"type":"boolean"}},"required":["node"]})" },
         { "get_notes",
           "The notes an Audio to MIDI node transcribed (times in seconds of the file) or a MIDI File node plays (in "
           "beats): t, dur, note name, midi, level, plus count, range and pitch-class counts. Filter with from/to "
           "(time), low/high (MIDI numbers); limit (default 200). Use it to check which notes were detected, e.g. "
           "whether high notes are there.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]},"from":{"type":"number"},"to":{"type":"number"},"low":{"type":"integer"},"high":{"type":"integer"},"limit":{"type":"integer"}},"required":["index"]})" },
         { "analyze_audio",
           "Listen to nodes as NUMBERS: records `seconds` of each node's live audio output (up to 4 nodes, captured over "
           "exactly the same time span) and returns per node: rms/peak dB, 8 band levels, spectral centroid, onsets, "
           "pitch (median note, range, a melody of notes) and a 50-row timeline; with 2+ nodes also a comparison against "
           "the first (level and band differences, waveform correlation and time offset, pitch agreement, onset "
           "match). Measure the node itself (an Audio Out measures what feeds it); it must be playing and lead to an "
           "Audio Out. play:true rewinds and starts the transport first. offline:true instead renders the patch "
           "itself from start (seconds on the transport, default the current position) with the device paused: "
           "sample-accurate and identical every run, independent of live timing - best for A/B comparisons such as "
           "a synth played by Audio to MIDI against the original Audio File.",
           R"({"type":"object","properties":{"nodes":{"type":"array","items":{"type":"integer"},"maxItems":4},"node":{"type":["integer","string"]},"seconds":{"type":"number","minimum":0.2,"maximum":30},"frameSeconds":{"type":"number"},"play":{"type":"boolean"},"offline":{"type":"boolean"},"start":{"type":"number","minimum":0}}})" },
         { "analyze_file",
           "Analyse a whole audio file offline (path, or node = a node with a file such as Audio File): duration, "
           "levels, average spectrum, onset rate, tempo (bpm + confidence), key (+ chroma), pitch summary and a "
           "timeline. notes:true also transcribes it (note count, density, polyphony, range, first notes) - slower. "
           "sound:true adds what a synth needs to recreate it: f0, inharmonicity, partials (frequency, level, T60 "
           "decay), ADSR, spectral envelope, brightness, noisiness, the strike's attack noise (transient level, decay, "
           "crest) and the air above the harmonics (noiseLevel, noiseToneHz). soundMode \"region\" (default) analyses "
           "one note picked with start/length (seconds), f0 (Hz hint); soundMode \"notes\" transcribes the file, "
           "keeps the isolated strikes within low..high (MIDI), merges them (spectrum given at C4) and fits key "
           "tracking: keyLevel and keyTilt in dB per octave, as Equation Synth's keyLevel/keyTilt take them. "
           "partials 1-32. A Sound Analyze node can apply such a profile to a synth.",
           R"({"type":"object","properties":{"path":{"type":"string"},"node":{"type":["integer","string"]},"notes":{"type":"boolean"},"sound":{"type":"boolean"},"soundMode":{"type":"string","enum":["region","notes"]},"start":{"type":"number"},"length":{"type":"number"},"f0":{"type":"number"},"partials":{"type":"integer"},"low":{"type":"integer"},"high":{"type":"integer"}}})" },
         { "get_sound_profile",
           "A Sound Analyze node's last result as numbers: f0, partials (frequency, level dB, T60), ADSR, spectral "
           "envelope, brightness, noisiness. Set its path/start/length with set_param first; its body's Apply button "
           "(or the values) configure a synth.",
           R"({"type":"object","properties":{"index":{"type":["integer","string"]}},"required":["index"]})" },
         { "render_frame",
           "Look at the picture: returns the Output node's current image plus brightness/colour statistics.",
           R"({"type":"object","properties":{"output":{"type":"integer","description":"Output node index when there are several"}}})" },
      };

      // ---- the connection to the running app ---------------------------------

      class Link
      {
      public:
         ~Link() { Close(); }

         // One request/reply round trip. False (with err) when the app is not
         // reachable; an RPC-level error comes back as `reply.error`.
         bool Call(const std::string& method, const json& params, json& reply, std::string& err)
         {
            for (int attempt = 0; attempt < 2; attempt++)
            {
               if (mFd < 0 && !Open(err))
                  return false;
               const json req = { {"jsonrpc", "2.0"}, {"id", ++mNextId}, {"token", mToken},
                                  {"method", method}, {"params", params} };
               const std::string line = req.dump() + "\n";
               std::string got;
               if (SendAll(line) && ReadLine(got))
               {
                  reply = json::parse(got, nullptr, false);
                  if (reply.is_discarded())
                  {
                     err = "unreadable reply from Infinite";
                     return false;
                  }
                  // A token from a previous launch: the app restarted, re-read it.
                  if (reply.contains("error") && reply["error"].value("message", "").find("auth token") != std::string::npos &&
                      attempt == 0)
                  {
                     Close();
                     continue;
                  }
                  return true;
               }
               Close(); // the app went away or restarted; reconnect once
            }
            err = "lost the connection to Infinite";
            return false;
         }

      private:
         int mFd = -1;
         int mNextId = 0;
         std::string mToken;
         std::string mBuf;

         bool Open(std::string& err)
         {
            // u8path: the same UTF-8-safe path RemoteControl writes it to.
            std::ifstream tok(std::filesystem::u8path(AppPaths::AppSupportDir() + "/control_token"));
            std::getline(tok, mToken);
            int port = 7777;
            if (const char* p = std::getenv("INFINITE_CONTROL_PORT"))
               port = std::atoi(p);
            const int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fd < 0 || ::connect(fd, (const sockaddr*)&addr, sizeof(addr)) != 0)
            {
               if (fd >= 0)
                  NetCompat::NetClose(fd);
               err = "Infinite is not running (nothing listens on 127.0.0.1:" + std::to_string(port) +
                     "). Ask the user to open Infinite, then try again.";
               return false;
            }
            mFd = fd;
            mBuf.clear();
            return true;
         }

         void Close()
         {
            if (mFd >= 0)
               NetCompat::NetClose(mFd);
            mFd = -1;
         }

         bool SendAll(const std::string& s)
         {
            size_t sent = 0;
            while (sent < s.size())
            {
               const NetSsize n = NetCompat::NetSend(mFd, s.data() + sent, s.size() - sent, 0);
               if (n <= 0)
                  return false;
               sent += (size_t)n;
            }
            return true;
         }

         bool ReadLine(std::string& out)
         {
            for (;;)
            {
               const size_t nl = mBuf.find('\n');
               if (nl != std::string::npos)
               {
                  out = mBuf.substr(0, nl);
                  mBuf.erase(0, nl + 1);
                  return true;
               }
               char chunk[65536];
               const NetSsize n = NetCompat::NetRecv(mFd, chunk, sizeof(chunk), 0);
               if (n <= 0)
                  return false;
               mBuf.append(chunk, (size_t)n);
            }
         }
      };

      // ---- tool calls -----------------------------------------------------------

      json TextResult(const std::string& text, bool isError)
      {
         return { {"content", json::array({ { {"type", "text"}, {"text", text} } })}, {"isError", isError} };
      }

      // get_graph without every node's full parameter dump, which would cost the
      // model thousands of tokens per call; get_params fetches one node's.
      json CompactGraph(const json& g)
      {
         json out = json::object();
         json nodes = json::array();
         for (const json& n : g.value("nodes", json::array()))
         {
            json c = { {"index", n.value("index", -1)}, {"type", n.value("typeName", "")}, {"category", n.value("category", "")} };
            // Stable id: any node parameter of any tool also takes "u<uid>",
            // which keeps naming the same node after undo, redo or reload.
            if (n.contains("uid") && n["uid"].is_number_unsigned() && n["uid"].get<uint64_t>() != 0)
               c["uid"] = "u" + std::to_string(n["uid"].get<uint64_t>());
            if (n.value("bypassed", false))
               c["bypassed"] = true;
            if (n.contains("label") && n["label"].is_string())
               c["label"] = n["label"];
            // Positions, so a layout can be tidied with move_node.
            c["x"] = std::round(n.value("x", 0.0));
            c["y"] = std::round(n.value("y", 0.0));
            nodes.push_back(c);
         }
         out["nodes"] = nodes;
         // Cables from the patch records, which are always complete; the live
         // `links` list is rebuilt when a frame draws (empty straight after a
         // batch) and is only used here for the id that disconnect takes.
         json cables = json::array();
         for (const char* kind : { "cables", "geometry", "audio", "notes" })
            for (const json& c : g.value(kind, json::array()))
            {
               json rec = { {"kind", std::string(kind) == "cables" ? "image" : kind},
                            {"src", c.value("srcIndex", -1)}, {"srcOutput", c.value("srcOutput", 0)},
                            {"dst", c.value("dstIndex", -1)}, {"dstSlot", c.value("dstSlot", -1)} };
               for (const json& l : g.value("links", json::array()))
                  if (l.value("srcIndex", -2) == rec["src"] && l.value("srcOutput", -2) == rec["srcOutput"] &&
                      l.value("dstIndex", -2) == rec["dst"] && l.value("dstSlot", -2) == rec["dstSlot"])
                     rec["id"] = l["id"];
               cables.push_back(rec);
            }
         out["cables"] = cables;
         for (const char* key : { "modulation", "expressions", "globals" })
            if (g.contains(key) && !g[key].empty())
               out[key] = g[key];
         return out;
      }

      // analyze_audio / analyze_file: the app only starts a job (its RPC
      // handlers must not block its UI thread); this process does the waiting.
      json RunAnalysis(Link& link, const std::string& name, json params)
      {
         json reply;
         std::string err;
         auto call = [&](const char* method, const json& p) -> bool
         {
            if (!link.Call(method, p, reply, err))
               return false;
            if (reply.contains("error"))
            {
               err = reply["error"].value("message", "error");
               return false;
            }
            return true;
         };

         double budget = 180.0; // a long file with notes:true takes a while
         if (name == "analyze_audio")
         {
            const bool offline = params.value("offline", false);
            if (!offline && params.value("play", false) && !call("transport", { { "action", "play" }, { "fromStart", true } }))
               return TextResult(err, true);
            params.erase("play");
            // Offline renders as fast as the graph allows, which for a heavy patch can be slower than real time.
            const double seconds = std::clamp(params.value("seconds", 3.0), 0.2, 30.0);
            budget = offline ? seconds * 20.0 + 40.0 : seconds * 2.0 + 30.0;
            if (!call("capture_audio", params))
               return TextResult(err, true);
         }
         else if (!call("analyze_file", params))
            return TextResult(err, true);
         const json id = reply.value("result", json::object()).value("id", json());
         if (!id.is_number_integer())
            return TextResult("Infinite did not return an analysis id (is this Infinite older than the tool?)", true);

         const auto start = std::chrono::steady_clock::now();
         for (;;)
         {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!call("analysis_result", { { "id", id } }))
               return TextResult(err, true);
            const json r = reply.value("result", json::object());
            const std::string status = r.value("status", "");
            if (status == "done")
            {
               json out = r.value("result", json::object());
               if (r.contains("note"))
                  out["note"] = r["note"];
               return TextResult(out.dump(), false);
            }
            if (status == "error")
               return TextResult(r.value("error", std::string("analysis failed")), true);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budget)
               return TextResult("analysis timed out", true);
         }
      }

      json CallTool(Link& link, const std::string& name, const json& args)
      {
         bool known = false;
         for (const Tool& t : kTools)
            known = known || name == t.name;
         if (!known)
            return TextResult("unknown tool '" + name + "'", true);

         json params = args.is_object() ? args : json::object();
         if (name == "analyze_audio" || name == "analyze_file")
            return RunAnalysis(link, name, params);
         if (name == "render_to_track" || name == "capture_notes" || name == "render_audio_to_file")
         {
            // The app renders in slices between frames; this process waits.
            const char* statusMethod = name == "render_to_track" ? "render_status"
                                     : name == "capture_notes"   ? "capture_status"
                                                                 : "render_file_status";
            json reply;
            std::string err;
            if (!link.Call(name, params, reply, err))
               return TextResult(err, true);
            if (reply.contains("error"))
               return TextResult(reply["error"].value("message", "error"), true);
            const auto start = std::chrono::steady_clock::now();
            for (;;)
            {
               const json r = reply.value("result", json::object());
               if (r.value("done", false))
                  return TextResult(r.dump(), !r.value("ok", false));
               if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > 600.0)
                  return TextResult("render timed out", true);
               std::this_thread::sleep_for(std::chrono::milliseconds(250));
               if (!link.Call(statusMethod, json::object(), reply, err))
                  return TextResult(err, true);
               if (reply.contains("error"))
                  return TextResult(reply["error"].value("message", "error"), true);
            }
         }
         std::string tmpImage;
         if (name == "render_frame")
         {
            std::error_code ec;
            tmpImage = (std::filesystem::temp_directory_path(ec) / "infinite_mcp_frame.jpg").string();
            params["path"] = tmpImage;
            params["maxSize"] = 1024; // enough to judge the picture; a 4K frame would blow the tool-output budget
         }

         json reply;
         std::string err;
         if (!link.Call(name, params, reply, err))
            return TextResult(err, true);
         if (reply.contains("error"))
            return TextResult(reply["error"].value("message", "error"), true);
         json result = reply.value("result", json::object());

         if (name == "get_graph")
            return TextResult(CompactGraph(result).dump(), false);
         if (name == "render_frame")
         {
            std::ifstream f(tmpImage, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            result.erase("file");
            json content = json::array({ { {"type", "text"}, {"text", result.dump()} } });
            if (!bytes.empty())
               content.push_back({ {"type", "image"}, {"mimeType", "image/jpeg"},
                                   {"data", Base64::Encode((const uint8_t*)bytes.data(), bytes.size())} });
            return { {"content", content}, {"isError", false} };
         }
         return TextResult(result.dump(), false);
      }

      // ---- the MCP side ---------------------------------------------------------

      void Send(const json& msg)
      {
         const std::string s = msg.dump() + "\n";
         std::fwrite(s.data(), 1, s.size(), stdout);
         std::fflush(stdout);
      }

      void Reply(const json& id, const json& result) { Send({ {"jsonrpc", "2.0"}, {"id", id}, {"result", result} }); }

      void ReplyError(const json& id, int code, const std::string& message)
      {
         Send({ {"jsonrpc", "2.0"}, {"id", id}, {"error", { {"code", code}, {"message", message} }} });
      }
   }

   int Run()
   {
#if defined(_WIN32)
      // No CRLF translation on the protocol stream.
      _setmode(_fileno(stdin), _O_BINARY);
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      Link link;
      std::string line;
      while (std::getline(std::cin, line))
      {
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         if (line.empty())
            continue;
         const json msg = json::parse(line, nullptr, false);
         if (msg.is_discarded() || !msg.is_object())
         {
            ReplyError(nullptr, -32700, "parse error");
            continue;
         }
         const std::string method = msg.value("method", "");
         // Notifications (no id) need no answer: initialized, cancelled, ...
         if (!msg.contains("id"))
            continue;
         const json id = msg["id"];
         const json params = msg.value("params", json::object());

         if (method == "initialize")
         {
            Reply(id, { {"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                        {"capabilities", { {"tools", json::object()} }},
                        {"serverInfo", { {"name", "infinite"}, {"version", INFINITE_VERSION_STRING} }},
                        {"instructions", kInstructions} });
         }
         else if (method == "ping")
            Reply(id, json::object());
         else if (method == "tools/list")
         {
            json tools = json::array();
            for (const Tool& t : kTools)
               tools.push_back({ {"name", t.name}, {"description", t.description}, {"inputSchema", json::parse(t.schema)} });
            Reply(id, { {"tools", tools} });
         }
         else if (method == "tools/call")
         {
            try
            {
               Reply(id, CallTool(link, params.value("name", ""), params.value("arguments", json::object())));
            }
            catch (const std::exception& e)
            {
               Reply(id, TextResult(std::string("bridge error: ") + e.what(), true));
            }
         }
         else
            ReplyError(id, -32601, "method not found: " + method);
      }
      return 0;
   }
}
