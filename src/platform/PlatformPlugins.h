#pragma once
// AU/VST3 plugin scanning and hosting, executable paths, headless app setup.
// Split out of Platform.h (docs/plans/compile-times/include-split.md, Phase 4);
// every Platform:: function still has macOS, Windows and Linux sides.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace Platform
{
   // ---- audio plugin hosting -----------------------------------------------
   // Every Objective-C object involved in hosting a third-party plugin lives
   // behind this facade, for the same reason the rest of this header exists:
   // src/nodes/ and src/audio/ are pure C++ and the audio thread must never
   // send an Objective-C message or touch ARC. AudioPluginNode holds an opaque
   // PluginHandle* and calls PluginRender from ProcessBlock; that one function
   // is the only real-time-safe entry point here, and it does nothing but call
   // a render block cached on the main thread at prepare time.
   // Audio Unit and (optionally, build-time) VST3 plugin hosting backend.
   // VST3 support is gated behind INFINITE_ENABLE_VST3, off by default: the
   // Steinberg VST3 SDK is GPLv3-or-proprietary and this codebase is MIT, so
   // building with VST3 enabled changes the license of the distributed binary
   // - see LICENSE and docs/plans/audio/plugin-hosting.md. Every struct that
   // crosses this boundary carries a `format` string ("au" or "vst3") so both
   // backends share one surface.
   struct PluginDesc
   {
      std::string format = "au";
      std::string name;
      std::string manufacturer;
      // Stable identity, and what a patch file stores - NOT a filesystem path,
      // which moves when a plugin is reinstalled elsewhere. For AU this is
      // "au:<type>:<subtype>:<manufacturer>" built from the component's four-
      // char codes, e.g. "au:aufx:dely:appl". For VST3 this is the factory
      // class's FUID string, e.g. "vst3:<uid-hex>".
      std::string identifier;
      std::string path; // Bundle directory path (e.g. for VST3 bundles)

      // True for 'aumu' (instrument) and 'aumf' (music effect) components -
      // the two component types that can meaningfully consume MIDI. This is
      // what AudioPluginNode keys its note input pin's visibility off, and
      // what PluginConfigure below uses to decide whether to expect an input
      // bus at all.
      bool acceptsNotes = false;
   };

   // Every installed effect/music-effect/instrument Audio Unit, via
   // AVAudioUnitComponentManager (a registry query).
   void EnumerateAudioUnits(std::vector<PluginDesc>& out);

   // Resolves a dropped .component *bundle directory* back to the plugin(s) it
   // contains, by reading its Info.plist AudioComponents array. Returns false
   // if the path isn't a readable audio component bundle. One bundle can
   // declare several components, hence the vector.
   bool DescribeAudioUnitBundle(const std::string& bundlePath, std::vector<PluginDesc>& out);

   // Enumerates VST3 plugins (.vst3 bundles) found within the specified folders.
   // Gated by INFINITE_ENABLE_VST3 at build time (no-op when disabled). Each
   // bundle is probed out-of-process (see main.cpp's "--vst3-scan-bundle"
   // child mode) so a plugin that crashes or hangs while its factory is being
   // read only costs a dead/killed child, and the walk moves on to the next
   // bundle instead of taking the whole app down mid-scan.
   void EnumerateVST3Plugins(const std::vector<std::string>& folders, std::vector<PluginDesc>& out);

   // Absolute path to this process's own executable (argv[0] resolved via
   // _NSGetExecutablePath), used to re-exec ourselves for the VST3 scan's
   // "--vst3-scan-bundle" child mode.
   std::string ExecutablePath();
   std::string ScannerExecutablePath();


   // Claims the shared NSApplication singleton with the Prohibited activation
   // policy before anything else can - the Dock icon comes from activation
   // policy, not window visibility, so a hidden GLFW window (GLFW_VISIBLE =
   // false) still shows up in the Dock unless this runs first. Two callers:
   //
   //  - First thing in the "--vst3-scan-bundle" child process, before any
   //    plugin code runs. The child is a re-exec of this same app binary, so
   //    it shares our bundle identity - if a scanned plugin's Cocoa UI classes
   //    touch [NSApplication sharedApplication] during their static
   //    init/factory instantiation (common for VST3 bundles with a Cocoa
   //    editor), AppKit lazily creates a default-policy shared application for
   //    *this* process and it shows up as its own Dock icon/window, i.e. a
   //    spurious extra "Infinite" instance.
   //  - Before glfwInit() on a headless test/screenshot run (INFINITE_EXITAFTER
   //    / IMAGERESYNTH_SCREENSHOT), so the hygiene suite's fixtures don't flash
   //    an icon into the Dock even though their window is never shown.
   //
   // Creating the shared application ourselves first and setting it to
   // Prohibited claims that singleton before anything else can, so nothing
   // afterward - plugin or GLFW - can make the process visible.
   void SuppressAppUIForHeadlessProcess();

   // Headless CLI runs (--render, --frame, ...) print one JSON status line. On
   // Windows Infinite.exe is a GUI-subsystem binary, so its stdout is not the
   // terminal that launched it; this attaches to the parent's console (only
   // when stdout is not already redirected to a file or pipe) so the line is
   // visible. No-op on macOS and Linux.
   void AttachConsoleForHeadless();

   // Resolves a dropped .vst3 bundle back to the plugin description(s) it contains.
   bool DescribeVST3Bundle(const std::string& bundlePath, std::vector<PluginDesc>& out);

   // Caches bundle path for a plugin identifier.
   void CacheVST3BundlePath(const std::string& identifier, const std::string& bundlePath);

   // User-added VST3 folders (PluginScanner::Folders()), kept in sync so that
   // PluginVST3Create's on-demand bundle resolution can search them too, not
   // just the two OS-standard VST3 directories. No-op when VST3 is disabled.
   void SetVST3SearchFolders(const std::vector<std::string>& folders);

   // Crash-safety for the VST3 scan. Each bundle is probed in a disposable
   // child process (see EnumerateVST3Plugins); if that child is killed by a
   // signal, exits nonzero, or simply hangs, its bundle path is appended to a
   // persisted blocklist immediately - same scan, no relaunch needed - and
   // skipped by every future scan. A sentinel file backs this up for the rare
   // case where the child dies before even that: it is written immediately
   // before the in-process probe inside the child (CFBundleLoadExecutable /
   // bundleEntry / bundleExit) and cleared right after a clean return, so a
   // sentinel still non-empty at the next launch blocklists that bundle too.
   // Surface both lists in the Plugins panel, with a way to clear the
   // blocklist and retry.
   std::vector<std::string> VST3Blocklist();
   void ClearVST3Blocklist();

   // Bundles the most recent EnumerateVST3Plugins() call could not describe
   // (corrupt bundle, wrong CPU architecture, no usable audio-effect class) by
   // path. Reset at the start of every call; read immediately after it
   // returns. Distinct from the blocklist: a scan failure here is a clean,
   // reported miss, not evidence the bundle is dangerous to retry.
   std::vector<std::string> VST3ScanFailures();

   // Top-level scanned entries recognized as a plugin format Infinite does not
   // host - VST2, in practice: a ".vst" bundle (macOS) or a bare ".dll" sitting
   // directly in a scanned VST3 folder (almost certainly a Windows VST2 binary
   // dropped in by mistake). Filename/extension checking only - these files are
   // never opened, loaded, or executed. Populated by the same EnumerateVST3Plugins
   // walk that fills VST3ScanFailures, and only for entries at the top level of
   // the walk; a ".dll"/".so"/".vst" living inside a valid ".vst3" bundle is never
   // visited by that walk in the first place (see EnumerateVST3Plugins), so it is
   // never flagged here. Reset at the start of every call; read immediately after
   // it returns, same contract as VST3ScanFailures. Infinite hosts AU and VST3
   // only - VST2 hosting is out of scope (Steinberg stopped issuing VST2 SDK
   // licenses in Oct 2018), this exists purely to explain a shorter list.
   std::vector<std::string> UnsupportedPluginsSeen();

   struct PluginHandle;

   enum class PluginLoadState
   {
      Pending, // instantiation still in flight - keep polling, keep the UI live
      Ready,   // render block cached, safe to publish to the audio thread
      Failed   // see the outError from PluginPoll
   };

   // Starts instantiation and returns immediately. AUAudioUnit instantiation
   // is asynchronous (out-of-process AUv3s in particular can take seconds), so
   // this never blocks: the handle exists straight away in the Pending state
   // and the node polls it from CookIfNeeded. `sampleRate` may be 0 if the
   // device isn't open yet; PluginPrepare below re-does the format/render-
   // resource setup once a real rate is known.
   PluginHandle* PluginCreate(const PluginDesc& desc, double sampleRate, int maxBlockFrames);

   // Main thread only, call once per frame while Pending. The transition to
   // Ready happens inside this call: the completion handler runs on an
   // arbitrary queue and does nothing but stash the instantiated unit, and all
   // of the actual setup (bus formats, maximumFramesToRender,
   // allocateRenderResources, caching the render block) is done here, on the
   // main thread, where AUAudioUnit expects it.
   PluginLoadState PluginPoll(PluginHandle* handle, std::string& outError);

   // Re-negotiates bus formats and re-allocates render resources at a new
   // sample rate / block size. Main thread only, and NOT safe while the handle
   // is reachable from a published topology - AudioPluginNode calls it before
   // publishing. A no-op if nothing changed. Returns false and fills outError
   // if the plugin rejects the format.
   bool PluginPrepare(PluginHandle* handle, double sampleRate, int maxBlockFrames, std::string& outError);

   // Main thread only, and never while the handle is still reachable from a
   // published audio topology - see AudioPluginNode's retire discipline. Closes
   // the editor window first, removes the learn observer, and briefly spins
   // (main thread, bounded) if a render is in flight before tearing the unit
   // down.
   void PluginDestroy(PluginHandle* handle);

   PluginDesc PluginDescriptionOf(PluginHandle* handle);

   // Samples of processing latency the loaded plugin itself reports
   // (lookahead, oversampling, internal FFT windows, ...) at its currently
   // prepared sample rate. 0 for a plugin that reports none, hasn't finished
   // loading, or is null - never a special sentinel. Main thread only; the
   // value is refreshed each time render resources are (re)allocated (see
   // PluginConfigure/PluginVST3Configure) and read here, not recomputed, so
   // this is cheap enough to call every RebuildAudioTopology.
   int PluginLatencySamples(PluginHandle* handle);

   // THE real-time-safe entry point. Calls the cached AUAudioUnitRenderBlock
   // with a stack-allocated pull-input block that copies from `in`. No
   // Objective-C message send, no ARC retain/release, no allocation, no lock.
   // `in` may be null (treated as silence). Output is planar, one pointer per
   // channel, numFrames long.
   void PluginRender(PluginHandle* handle, const float* const* in, int inChannels,
                     float* const* out, int outChannels, int numFrames);

   // ALSO real-time-safe, same discipline as PluginRender: schedules a MIDI
   // event through a block cached on the main thread at prepare time (into
   // AUAudioUnit.scheduleMIDIEventBlock), no Objective-C message send, no ARC,
   // no allocation. `bytes` is a standard 1-3 byte MIDI message (note on/off,
   // channel 0). A no-op if the plugin never published a schedule block (an
   // effect with no MIDI input, or one that doesn't support it) - the node
   // degrades to rendering with no note input rather than crashing.
   void PluginScheduleMIDIEvent(PluginHandle* handle, int frameOffset, const unsigned char* bytes,
                                int byteCount);

   struct PluginParamInfo
   {
      unsigned long long address = 0;
      std::string displayName;
      float minValue = 0.0f;
      float maxValue = 1.0f;
      float defaultValue = 0.0f;
      std::string unit;
   };

   // The plugin's AUParameterTree, flattened (allParameters). Main thread only.
   int PluginParameterCount(PluginHandle* handle);
   bool PluginParameterInfo(PluginHandle* handle, int index, PluginParamInfo& out);
   bool PluginParameterInfoByAddress(PluginHandle* handle, unsigned long long address, PluginParamInfo& out);

   // Main thread only - AUParameter's setter is non-blocking and is the
   // supported way to drive a parameter from a host's own UI, so mapped params
   // go straight down this path from CookIfNeeded rather than through the
   // node's ParamMailbox (the plugin does its own smoothing). This is the one
   // deliberate exception to "every param reaches the audio thread through the
   // mailbox", and is why AudioPluginNode carries a documented
   // AUDIOPARAMSWEEPTEST baseline.
   void PluginSetParameter(PluginHandle* handle, unsigned long long address, float value);
   bool PluginGetParameter(PluginHandle* handle, unsigned long long address, float& outValue);

   // "Configure" / learn mode, the Ableton-style mapping gesture: while learn
   // is on, touching a control in the plugin's OWN editor window reports that
   // parameter's address here, and the node fills its next free mapping slot.
   //
   // Implemented with AUParameterTree's parameter observer, whose block is
   // called on an arbitrary thread - so it does nothing but store the address
   // into an atomic that PluginPollLearned drains from the main thread.
   // Parameter writes this host makes itself are tagged with the observer's own
   // token and therefore never echo back as a learned touch.
   void PluginBeginLearn(PluginHandle* handle);
   void PluginEndLearn(PluginHandle* handle);
   bool PluginPollLearned(PluginHandle* handle, unsigned long long& outAddress);

   // The plugin's own editor, in a separate native window (the app itself is
   // GLFW/ImGui/OpenGL and has no other NSWindow). Verified: a plain NSWindow
   // is created, displayed and dispatched to correctly with glfwPollEvents as
   // the only run-loop pump. The view comes from
   // -requestViewControllerWithCompletionHandler:, which is asynchronous, so
   // PluginOpenEditor returns true meaning "the request went out" and the
   // window appears a beat later.
   //
   // Closing the window (red button or PluginDestroy) routes back through
   // PluginCloseEditor so PluginEditorIsOpen never lies to the node.
   bool PluginOpenEditor(PluginHandle* handle, std::string& outError);
   void PluginCloseEditor(PluginHandle* handle);
   bool PluginEditorIsOpen(PluginHandle* handle);

   // True if any hosted plugin currently has an editor window on screen.
   bool AnyPluginEditorOpen();

   // Drains and dispatches pending main-run-loop work (AppKit events, plugin
   // editor redraws, XPC replies from an out-of-process AUv3) without blocking.
   // Main thread only. No-op if no editor window is open. Returns true if it
   // actually dispatched something.
   bool PumpPluginEditorEvents();

   // AUAudioUnit.fullState, keyed-archived and base64'd so it round-trips
   // through Patch.cpp's Text param (one backslash-escaped line, no length
   // cap). Returns false if the plugin publishes no state.
   bool PluginSaveState(PluginHandle* handle, std::string& outBase64);
   bool PluginRestoreState(PluginHandle* handle, const std::string& base64);

}
