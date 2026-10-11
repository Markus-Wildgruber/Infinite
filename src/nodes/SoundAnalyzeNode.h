#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "audio/SoundAnalysis.h"
#include "core/INode.h"
#include "core/Modulation.h"

// --- Sound Analyze --------------------------------------------------------
// Load a reference sound (one note works best; start/length pick it out of a
// longer file) and it is analysed on a worker thread (audio/SoundAnalysis.h)
// into what a synth is set from: the fundamental, the level / frequency /
// decay of each harmonic partial, an ADSR fitted to its amplitude envelope,
// and its spectral envelope, brightness and noisiness. The body shows it;
// "Apply to" writes derived settings onto a synth in the patch (see
// app/bodies/SoundAnalyzeBody.cpp); the outputs below patch it anywhere.
//
// No audio graph: the file is decoded on the main thread (Platform decoders
// are main-thread only), mixed to mono, and analysed off it. Nothing but the
// path and the knobs is saved; the profile is recomputed on load.
//
// Outputs, all 0..1 (a pure read of the last profile - safe to call any number
// of times per frame; 0 while nothing is analysed):
//   f0       MIDI pitch / 127 of the fundamental (69 = A4 -> 0.543), fractional;
//            the same scale as Note to CV's default 0..127 range. 0 if unpitched.
//   attack   attack time / 4000 ms  } linear over 0..4000 ms - the range of the
//   decay    decay time / 4000 ms   } synths' envelope sliders, so a cable with
//   release  release time / 4000 ms } the default binding range lands the value.
//   sustain  sustain level (linear amplitude re the peak).
//   bright   spectral centroid on a log scale, 100 Hz -> 0, 10 kHz -> 1.
//   noise    noisiness: noise / (noise + harmonic) power.
//   p1..p8   linear amplitude of harmonic k at the onset, relative to the
//            strongest partial (1 = strongest, 0.5 = -6 dB, 0 = absent).
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
      kOutputCount = kPartial0 + kPartialOutputs
   };
   static constexpr float kEnvelopeRangeMs = 4000.0f; // time outputs: ms / this

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

   // Not saved: the body's line about the last "Apply to" (what was set where).
   std::string applyMessage;

   enum class State { Empty, Analyzing, Ready, Failed };
   State GetState() const { return mState; }
   const std::string& Status() const { return mStatus; }
   bool HasProfile() const { return mHaveProfile; }
   // The last good profile (valid() only when HasProfile()).
   const SoundAnalysis::SoundProfile& Profile() const { return mProfile; }
   // The last good profile as JSON (SoundAnalysis::ToJson plus "file"), or
   // nullptr while there is none - for the RPC/MCP layer.
   const nlohmann::json* ProfileJson() const { return mHaveProfile ? &mJson : nullptr; }
   double DurationSeconds() const { return mDurationSeconds; }
   // Whole-file min/max overview for the body's region strip (kOverviewColumns each).
   static constexpr int kOverviewColumns = 200;
   const std::vector<float>& OverviewMin() const { return mOverviewMin; }
   const std::vector<float>& OverviewMax() const { return mOverviewMax; }
   // Decode the file again on the next cook (e.g. it changed on disk).
   void Reload() { mLoadedPath.clear(); }

   SoundAnalysis::Options CurrentOptions() const;

   // INFINITE_SOUNDANALYSISTEST: synthesised tones with known partials,
   // decays and envelope must be recovered; output mapping; formula parses.
   static bool RunSelfTest();

private:
   struct Tap : public IModulator
   {
      SoundAnalyzeNode* owner = nullptr;
      int index = 0;
      float Value01() override { return owner ? owner->Value(index) : 0.0f; }
   };

   void LoadFile();
   void PollAnalysis();
   void StartAnalysisIfStale();
   void SetProfile(SoundAnalysis::SoundProfile profile);

   Tap mTaps[kOutputCount];
   int mLastCookFrame = -1;

   State mState = State::Empty;
   std::string mStatus;
   std::string mLoadedPath;
   double mDurationSeconds = 0.0;
   double mSampleRate = 0.0;
   std::shared_ptr<const std::vector<float>> mMono;
   std::vector<float> mOverviewMin, mOverviewMax;

   std::future<SoundAnalysis::SoundProfile> mJob;
   SoundAnalysis::Options mJobOptions;
   SoundAnalysis::Options mProfileOptions;
   bool mHaveResultForOptions = false; // the last finished job matches mProfileOptions (valid or not)

   bool mHaveProfile = false;
   SoundAnalysis::SoundProfile mProfile;
   nlohmann::json mJson;
};
