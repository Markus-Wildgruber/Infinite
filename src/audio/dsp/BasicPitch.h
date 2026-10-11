#pragma once

#include <atomic>
#include <string>
#include <vector>

// Audio-to-notes transcription with Spotify's Basic Pitch model
// (Bittner et al., "A Lightweight Instrument-Agnostic Model for Polyphonic
// Note Transcription and Multipitch Estimation", ICASSP 2022; model weights
// and reference post-processing Apache-2.0, see assets/models/NOTICE.txt).
//
// Two stages, split so the expensive one runs once per file:
//   1. RunModel: mono audio at 22050 Hz -> per-frame note and onset
//      activations (86 frames/s, 88 piano keys). Seconds of CPU per minute of
//      audio; run it on a worker thread. Needs ONNX Runtime, which this build
//      links on Windows and Linux only (INFINITE_HAS_ORT); elsewhere it fails
//      with a "not available" error.
//   2. NotesFromActivations: activations -> notes, using the three
//      user-facing thresholds. Milliseconds, so knob changes re-run only this.
//
// Main-thread/worker only; nothing here is real-time safe.
namespace BasicPitch
{
   constexpr int kSampleRate = 22050;
   constexpr int kKeys = 88;       // MIDI 21 (A0) .. 108 (C8)
   constexpr int kLowestMidi = 21;

   struct Activations
   {
      int frames = 0;
      std::vector<float> note;  // frames * kKeys, row-major by frame
      std::vector<float> onset; // frames * kKeys
   };

   struct Note
   {
      float start = 0.0f; // seconds
      float end = 0.0f;   // seconds
      int pitch = 60;     // MIDI
      float amplitude = 0.0f; // 0..1, mean note activation (maps to velocity)
   };

   struct Settings
   {
      float onsetThreshold = 0.5f; // how strong an onset must be to start a note
      float frameThreshold = 0.3f; // how strong a note must stay to be held
      float minNoteMs = 127.7f;    // shorter notes are dropped
      int lowMidi = 21, highMidi = 108;
      int maxPolyphony = 0; // > 0: at most this many notes at once, the quietest give way
      bool operator==(const Settings& o) const
      {
         return onsetThreshold == o.onsetThreshold && frameThreshold == o.frameThreshold && minNoteMs == o.minNoteMs &&
                lowMidi == o.lowMidi && highMidi == o.highMidi && maxPolyphony == o.maxPolyphony;
      }
      bool operator!=(const Settings& o) const { return !(*this == o); }
   };

   // Where the bundled model lives: <exe dir>/assets/models/basic-pitch.onnx.
   std::string ModelPath();

   // Mixes `channels` planar channels (channel 0 then 1, each `frames` long,
   // the Platform::SampleBuffer layout) to mono and resamples to kSampleRate
   // with a windowed-sinc low-pass. `cancel` may be null.
   bool ToModelInput(const float* planar, int channels, int frames, double sampleRate, std::vector<float>& out,
                     const std::atomic<bool>* cancel);

   // Runs the model over the whole signal. `progress` (0..1) and `cancel` may
   // be null. Returns false with `error` filled on failure or cancel.
   bool RunModel(const std::vector<float>& audio22k, Activations& out, std::string& error,
                 std::atomic<float>* progress, const std::atomic<bool>* cancel);

   // Notes sorted by start time.
   std::vector<Note> NotesFromActivations(const Activations& a, const Settings& s);
}
