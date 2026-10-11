#pragma once
// Audio levels input, file playback, device callback bridge, recovery,
// device list, sample library decoding, audio-in capture.
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
   // ---- audio input ----
   // Taps the default input device and keeps a running spectrum. Everything is
   // computed on the audio thread and read lock-free-ish by the render thread;
   // the graph only ever reads smoothed magnitudes, so a torn read is harmless.
   const int kAudioBands = 16;

   struct AudioLevels
   {
      float rms = 0.0f;
      float peak = 0.0f;
      float low = 0.0f;   // ~20-250 Hz
      float mid = 0.0f;   // ~250-2k
      float high = 0.0f;  // ~2k-16k
      float bands[kAudioBands] = { 0 };
      bool onset = false; // transient detected since the last read
   };

   // ---- audio file playback ----
   // File playback moved into the DSP graph (AudioFileNode/AudioFilePlayerAudioNode
   // in src/nodes/AnalyzeNodes.cpp) - it decodes via DecodeAudioFileToBuffer below
   // and runs its own AudioNode like SamplerNode, rather than owning a private
   // AVAudioEngine here. OpenAudioDialog is still shared with the file picker.
   std::string OpenAudioDialog();
   // Standard MIDI files (.mid / .midi), for the MIDI File node.
   std::string OpenMidiDialog();
   // Save-as for a .mid (Audio to MIDI's export). Returns the chosen path with
   // the .mid extension, or "" if cancelled.
   std::string SaveMidiDialog(const std::string& suggestedName = "Untitled.mid");

   bool AudioStart(std::string& outError);
   void AudioStop();
   bool AudioIsRunning();
   std::string AudioDeviceName();
   // Returns false if audio is not running; `out` is then left at zero.
   bool AudioRead(AudioLevels& out);
   void AudioSetSmoothing(float attack, float release);
   void AudioSetGain(float gain);

   // ---- audio synthesis spike (P0 feasibility test, throwaway) ----
   // Raw AVAudioSourceNode generating a 440 Hz sine directly in its render
   // block, to measure whether a live synthesis callback coexists with the
   // render loop without glitches or a measurable FPS hit. Not a real node
   // type - see docs/plans/audio/P0-feasibility-prompt.md.
   struct AudioSpikeStats
   {
      double sampleRate = 0.0;
      int blockSize = 0;
      double maxJitterMs = 0.0;
      uint64_t callbackCount = 0;
   };

   bool AudioSpikeStart(std::string& outError);
   void AudioSpikeStop();
   AudioSpikeStats AudioSpikeGetStats();

   // ---- audio engine render callback bridge ----
   // Render callback bridge: called on the real-time audio thread. Must obey
   // every audio-thread prohibition in docs/plans/audio/P1a-engine-prompt.md -
   // no allocation, no locks, no std::function/map/string, no GL/ImGui/file I/O.
   // buffers is planar: buffers[ch][0..numFrames) for channel ch.
   typedef void (*AudioRenderCallback)(float** buffers, int numChannels, int numFrames, void* userData);

   // requestedDeviceId == 0 means "system default output device" (existing
   // behavior). requestedSampleRate == 0.0 / requestedBufferFrames == 0 mean
   // "use whatever the device is already running at" (also existing
   // behavior). A requested value the device rejects or clamps is not an
   // error - outSampleRate (and Platform::AudioDeviceBufferFrames below)
   // reflect what was actually negotiated, the same way the sample rate
   // readback already worked before this was added.
   bool AudioDeviceOpen(AudioRenderCallback callback, void* userData, double& outSampleRate, std::string& outError,
                        uint32_t requestedDeviceId = 0, double requestedSampleRate = 0.0,
                        int requestedBufferFrames = 0);
   void AudioDeviceClose();

   // Actual buffer frame size CoreAudio reports for the given device right
   // now (0 = system default output device). Buffer size is a device-wide
   // property, not something AVAudioEngine hands back per-open, so this
   // queries the hardware directly - lets the UI show what was really
   // negotiated after a requestedBufferFrames the device may have clamped.
   uint32_t AudioDeviceBufferFrames(uint32_t deviceId = 0);

   // Best available estimate of the audio round-trip latency (input to
   // output: what a Looper/overdub has to compensate for) in FRAMES at the
   // output device's current rate: output-side (device latency + safety
   // offset + stream latency + buffer) plus input-side of the input device
   // Infinite captures from (AudioInputCaptureGetDevice(), 0 = system default).
   // 0 means unknown. `outputDeviceId` follows AudioDeviceBufferFrames (0 =
   // system default output). Main-thread only; cheap enough for a per-frame
   // UI readout (a handful of property reads on macOS, cached atomics on
   // Windows, a cached value on Linux) - never call from the audio thread.
   uint32_t AudioRoundTripLatencyFrames(uint32_t outputDeviceId = 0);

   // Output-stream mode preference for the render device, applied on the next
   // AudioDeviceOpen. 0 = standard (default, the platform's normal path),
   // 1 = low-latency shared, 2 = exclusive. Only Windows acts on this
   // (IAudioClient3 minimum-period shared stream / WASAPI exclusive); every
   // request degrades toward 0 rather than failing the open. macOS and Linux
   // ignore it and always run mode 0.
   void AudioSetOutputMode(int mode);
   // The mode the currently open render device is actually running (after any
   // fallback); 0 when no device is open or the platform has no such modes.
   int AudioOutputModeActive();

   // Headless round-trip check for the WASAPI PCM<->float conversion helpers
   // (docs/plans/windows-render/FIX_BRIEF.md addendum A1: WASAPI shared mode
   // can hand back a non-float mix format, and the render/capture paths must
   // convert rather than refuse). Prints "AUDIOPCMTEST OK"/"AUDIOPCMTEST
   // FAIL" and returns the same verdict as a bool - this is the one piece of
   // A1 that's actually CI-reachable (no device needed), so it's wired into
   // main.cpp's env-var test dispatch next to the other INFINITE_*TEST
   // fixtures. Trivially true on macOS, which never negotiates PCM here.
   bool AudioPcmConversionSelfTest();

   // ---- audio device recovery (config-change / sleep-wake) ----
   // docs/plans/optimization/prompts/02-device-change-and-wake-recovery.md.
   //
   // AVAudioEngineConfigurationChangeNotification can be posted on an
   // arbitrary thread, and NSWorkspace's sleep/wake notifications are not
   // guaranteed main-thread either by anything this file controls - nothing
   // downstream of them may touch the audio graph directly
   // (src/audio/AudioNode.h's RT-safety list covers the render thread, but
   // main-thread-only graph state like gNodes/AudioCable is just as
   // unsafe from a random notification thread). So these three functions
   // are the entire cross-thread surface: the notification handlers
   // installed by AudioDeviceOpen do nothing but latch an atomic flag, and
   // each accessor here consumes (clears) its flag on read. main.cpp's
   // per-frame loop polls all three once a frame and does the actual
   // Stop/Start work itself, through the same choke point Apply-audio-
   // settings already uses - see main.cpp's PollAudioRecovery.
   //
   // Consuming means a burst of N notifications between two polls collapses
   // to a single true - the poller does its own additional rate-limiting on
   // top of that (see PollAudioRecovery's comment), this alone does not
   // bound retry frequency.
   bool AudioDeviceConfigDidChange();
   bool AudioWillSleep();
   bool AudioDidWake();

   // Test-only: pokes the same flag AudioDeviceConfigDidChange() consumes,
   // without a real CoreAudio notification - INFINITE_AUDIORECOVERYTEST's
   // way of exercising PollAudioRecovery's restart/rate-limit logic
   // deterministically, since real device unplug/sleep-wake can't be
   // scripted headlessly.
   void AudioDeviceDebugSimulateConfigChange();

   // ---- audio device enumeration (CoreAudio HAL) ----
   struct AudioDeviceInfo
   {
      std::string name;
      uint32_t deviceId = 0;
      bool isInput = false;
      bool isOutput = false;
      int inputChannels = 0;
      int outputChannels = 0;
   };

   std::vector<AudioDeviceInfo> AudioListDevices();

   // ---- sample library ----
   // Native open panel restricted to picking a single directory (no files).
   // Returns "" if cancelled. `title` sets the panel's title (defaults to
   // the Samples search panel's "Add sample folder" wording); `initialDir`
   // seeds the panel's starting folder when non-empty.
   std::string OpenFolderDialog(const char* title = "Add sample folder",
                                const std::string& initialDir = std::string());

   // A whole audio file decoded to planar float PCM, ready for an audio-
   // thread node to read directly (no further per-block decoding). Decoding
   // itself only ever happens on the main thread, at load time.
   struct SampleBuffer
   {
      std::vector<float> channelData; // interleaved-free: channel 0 then channel 1, each numFrames long
      int channels = 0;
      int numFrames = 0;
      double sampleRate = 0.0;
   };

   // Decodes whatever AVFoundation reads natively (wav/aiff/caf/m4a/mp3/alac).
   // Returns false and fills outError on failure (missing file, unsupported
   // codec, etc). Main-thread only - not real-time safe.
   bool DecodeAudioFileToBuffer(const std::string& path, SampleBuffer& outBuffer, std::string& outError);

   // ---- audio input capture (for the node graph's Audio In node) ----
   // Taps the render device's own AVAudioEngine input node (the same engine
   // AudioDeviceOpen already runs) and buffers raw samples into a lock-free
   // ring, so AudioInputNode's audio-thread ProcessBlock can drain them each
   // block without touching Objective-C, taking a lock, or allocating.
   // Independent of AudioStart/AudioRead above, which is a separate node
   // (Audio Analyze) on its own separate engine instance.
   //
   // AddRef/RemoveRef just count how many AudioInputNode instances are alive
   // (call from the node's constructor/destructor - cheap, no Objective-C).
   // Pump (call every CookIfNeeded, main thread only) installs the tap once
   // the want-count is > 0 and the output device is open, and tears it down
   // once it drops to 0. Pumping every frame rather than once is what makes
   // this self-healing across the user stopping/restarting the output device
   // from the menu's audio toggle: AudioDeviceClose tears the tap down with
   // the rest of that engine, and the next Pump reinstalls it on the new one
   // without any node needing to notice the device came back.
   void AudioInputCaptureAddRef();
   void AudioInputCaptureRemoveRef();
   void AudioInputCaptureSetDevice(uint32_t deviceId);
   uint32_t AudioInputCaptureGetDevice();
   void AudioInputCapturePump(std::string& outError);
   bool AudioInputCaptureIsRunning();

   // Reads up to numFrames of the most recently captured audio into planar
   // per-channel buffers (outChannels[ch][0..numFrames)). Underrun frames are
   // zero-filled. Returns the channel count actually captured (0 if capture
   // isn't running yet, e.g. still waiting on the device to open or on mic
   // permission).
   // `readerCursor` is per-reader state maintained across calls so multiple
   // Audio In nodes can read concurrently without starving each other.
   // `channelOffset`: 0-indexed channel to read (0 = input 1, 1 = input 2, etc.)
   // `isMono`: when true, copies channelOffset to both outChannels[0] and outChannels[1] (center mono).
   //           when false, reads stereo pair (channelOffset and channelOffset + 1).
   // Audio-thread safe: lock-free, no allocation.
   int AudioInputCaptureRead(float* const* outChannels, int numFrames, int maxChannels,
                             uint64_t& readerCursor, int channelOffset = 0, bool isMono = false);
   int AudioInputCaptureRead(float* const* outChannels, int numFrames, int maxChannels);

}
