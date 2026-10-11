#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "audio/SoundAnalysis.h"
#include "core/INode.h"
#include "core/Modulation.h"

// --- Sound Analyze --------------------------------------------------------
// Load a reference sound and it is analysed on a worker thread
// (audio/SoundAnalysis.h) into what a synth is set from: the fundamental, the
// level / frequency / decay of each harmonic partial, an ADSR fitted to its
// amplitude envelope, its spectral envelope, brightness and noisiness, the
// attack transient (noise burst level / decay, crest) and the sustained "air"
// noise. The body shows it; "Apply to" writes derived settings onto a synth in
// the patch (see app/bodies/SoundAnalyzeBody.cpp); the outputs below patch it
// anywhere.
//
// Two modes:
//   region  one note: start/length pick it out of a longer file.
//   notes   the whole file is transcribed with the Audio to MIDI model
//           (audio/dsp/BasicPitch.h, default settings; cached per file, so
//           knob changes only re-run the analysis), every reasonably isolated
//           note between lowNote and highNote is analysed and the results are
//           merged (medians), plus key tracking: how level and harmonic
//           balance change with pitch (SoundAnalysis::AnalyzeNotes).
//
// No audio graph: the file is decoded on the main thread (Platform decoders
// are main-thread only), mixed to mono, and analysed off it. Nothing but the
// path and the knobs is saved; the profile is recomputed on load.
//
// Outputs, all 0..1 (a pure read of the last profile - safe to call any number
// of times per frame; 0 while nothing is analysed):
//   f0       MIDI pitch / 127 of the fundamental (69 = A4 -> 0.543), fractional;
//            the same scale as Note to CV's default 0..127 range. 0 if unpitched.
//            (Mode notes: the merged profile is placed at MIDI 60 -> 0.472.)
//   attack   attack time / 4000 ms  } linear over 0..4000 ms - the range of the
//   decay    decay time / 4000 ms   } synths' envelope sliders, so a cable with
//   release  release time / 4000 ms } the default binding range lands the value.
//   sustain  sustain level (linear amplitude re the peak).
//   bright   spectral centroid on a log scale, 100 Hz -> 0, 10 kHz -> 1.
//   noise    noisiness: noise / (noise + harmonic) power.
//   p1..p8   linear amplitude of harmonic k at the onset, relative to the
//            strongest partial (1 = strongest, 0.5 = -6 dB, 0 = absent).
//   transient       attack-noise level: residual RMS at the onset / harmonic RMS peak (0..1 as measured).
//   transientDecay  its decay to -60 dB / 200 ms (Equation Synth's range 1..200 ms).
//   air             sustained noise above its highpass corner / harmonic RMS (noiseLevel, 0..1).
//   keyLevel        (dB per octave + 12) / 24: -12 -> 0, 0 -> 0.5, +12 -> 1 (0.5 in region mode).
//   keyTilt         (dB per octave + 12) / 24, same scale (0.5 in region mode).
class SoundAnalyzeNode : public INode
{
public:
   enum Output
   {
      kF0 = 0,
      kAttack,
      kDecay,
      kSustain,
      kRelease,
      kBrightness,
      kNoisiness,
      kPartial0,
      kPartialOutputs = 8,
      // Appended after p1..p8: saved cables keep their indices.
      kTransient = kPartial0 + kPartialOutputs,
      kTransientDecay,
      kAir,
      kKeyLevel,
      kKeyTilt,
      kOutputCount
   };
   static constexpr float kEnvelopeRangeMs = 4000.0f;      // time outputs: ms / this
   static constexpr float kTransientDecayRangeMs = 200.0f; // transientDecay output: ms / this
   static constexpr float kKeyRangeDbPerOct = 12.0f;       // key outputs: (dB/oct + this) / (2 * this)

   enum Mode
   {
      kModeRegion = 0,
      kModeNotes = 1
   };

   static INode* Create() { return new SoundAnalyzeNode(); }
   SoundAnalyzeNode();
   ~SoundAnalyzeNode() override;

   unsigned int GetOutputTexture() override { return 0; }
   int GetOutputWidth() const override { return 0; }
   int GetOutputHeight() const override { return 0; }
   void CookIfNeeded(int frameId) override;
   void VisitParams(ParamVisitor& v) override;

   int OutputCount() const override { return kOutputCount; }
   const char* OutputLabel(int index) const override;
   IModulator* ModulatorOutput(int index) override;

   // 0..1 value of output `index` (see Output).
   float Value(int index) const;

   // Saved params. Names are patch keys.
   std::string path;        // "path"     the reference audio file
   float startSec = 0.0f;   // "start"    region start, seconds into the file
   float lengthSec = 0.0f;  // "length"   region length, seconds; 0 = to the end (max 20 s)
   float f0Hint = 0.0f;     // "f0Hint"   Hz; 0 = detect the pitch
   int partials = 16;       // "partials" harmonics measured, 1..32
   int mode = kModeRegion;  // "mode"     0 = region (one note), 1 = notes (average over the detected notes)
   int lowNote = 21;        // "lowNote"  mode notes: MIDI range of the notes used (21 = A0 .. 108 = C8)
   int highNote = 108;      // "highNote"

   // Not saved: the body's line about the last "Apply to" (what was set where).
   std::string applyMessage;

   enum class State { Empty, Analyzing, Ready, Failed };
   State GetState() const { return mState; }
   const std::string& Status() const { return mStatus; }
   bool HasProfile() const { return mHaveProfile; }
   // The last good profile (valid() only when HasProfile()). In mode notes,
   // the merged one (SoundAnalysis::MultiNoteResult::profile).
   const SoundAnalysis::SoundProfile& Profile() const { return mProfile; }
   // The multi-note result behind Profile() when it came from mode notes, else null.
   const SoundAnalysis::MultiNoteResult* MultiNote() const { return mHaveProfile ? mMulti.get() : nullptr; }
   // The last good profile as JSON (SoundAnalysis::ToJson plus "file"; in
   // mode notes also "mode": "notes" and "multiNote": the counts, key
   // tracking and per-note rows), or nullptr while there is none - for the
   // RPC/MCP layer.
   const nlohmann::json* ProfileJson() const { return mHaveProfile ? &mJson : nullptr; }
   // Mode notes: the model is running on the file (Progress 0..1).
   bool IsTranscribing() const { return mTranscribe.valid(); }
   float Progress() const { return mProgress.load(std::memory_order_relaxed); }
   // Notes the model found in the file (mode notes; 0 before / without a transcription).
   int TranscribedNotes() const { return mNotes ? (int)mNotes->size() : 0; }
   double DurationSeconds() const { return mDurationSeconds; }
   // Whole-file min/max overview for the body's region strip (kOverviewColumns each).
   static constexpr int kOverviewColumns = 200;
   const std::vector<float>& OverviewMin() const { return mOverviewMin; }
   const std::vector<float>& OverviewMax() const { return mOverviewMax; }
   // Decode the file again on the next cook (e.g. it changed on disk).
   void Reload() { mLoadedPath.clear(); }

   SoundAnalysis::Options CurrentOptions() const;
   SoundAnalysis::MultiNoteOptions CurrentMultiOptions() const;

   // INFINITE_SOUNDANALYSISTEST: synthesised tones with known partials,
   // decays and envelope must be recovered; attack burst and air noise
   // measured, a clean tone reads none; key tracking over synthesised notes;
   // output mapping; formula parses.
   static bool RunSelfTest();

private:
   struct Tap : public IModulator
   {
      SoundAnalyzeNode* owner = nullptr;
      int index = 0;
      float Value01() override { return owner ? owner->Value(index) : 0.0f; }
   };

   // What an analysis run was asked for; a change re-runs it.
   struct Request
   {
      int mode = kModeRegion;
      SoundAnalysis::Options single;
      int lowNote = 21, highNote = 108; // mode notes only
      bool operator==(const Request& o) const
      {
         if (mode != o.mode)
            return false;
         if (mode == kModeNotes)
            return single.partials == o.single.partials && lowNote == o.lowNote && highNote == o.highNote;
         return single == o.single;
      }
      bool operator!=(const Request& o) const { return !(*this == o); }
   };
   struct JobResult
   {
      SoundAnalysis::SoundProfile profile;
      std::shared_ptr<const SoundAnalysis::MultiNoteResult> multi; // mode notes
   };
   struct TranscribeResult
   {
      std::shared_ptr<const std::vector<SoundAnalysis::NoteEvent>> notes;
      std::string error;
   };

   Request CurrentRequest() const;
   void LoadFile();
   void StopTranscription();
   void StartTranscription();
   void PollTranscription();
   void PollAnalysis();
   void StartAnalysisIfStale();
   void SetProfile(SoundAnalysis::SoundProfile profile, std::shared_ptr<const SoundAnalysis::MultiNoteResult> multi = nullptr);

   Tap mTaps[kOutputCount];
   int mLastCookFrame = -1;

   State mState = State::Empty;
   std::string mStatus;
   std::string mLoadedPath;
   double mDurationSeconds = 0.0;
   double mSampleRate = 0.0;
   std::shared_ptr<const std::vector<float>> mMono;
   std::vector<float> mOverviewMin, mOverviewMax;

   std::future<JobResult> mJob;
   std::shared_ptr<std::atomic<bool>> mJobCancel; // notes runs only
   Request mJobRequest;
   Request mProfileRequest;
   bool mHaveResultForOptions = false; // the last finished job (or transcription failure) matches mProfileRequest

   // Mode notes: the transcription of mLoadedPath, run once per file.
   std::future<TranscribeResult> mTranscribe;
   std::shared_ptr<std::atomic<bool>> mCancel;
   std::atomic<float> mProgress{ 0.0f };
   std::shared_ptr<const std::vector<SoundAnalysis::NoteEvent>> mNotes;
   bool mTranscribeFailed = false;
   std::string mTranscribeError;

   bool mHaveProfile = false;
   SoundAnalysis::SoundProfile mProfile;
   std::shared_ptr<const SoundAnalysis::MultiNoteResult> mMulti;
   nlohmann::json mJson;
};
