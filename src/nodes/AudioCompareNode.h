#pragma once

#include <memory>

#include "core/AudioCable.h"
#include "core/INode.h"
#include "core/Modulation.h"

class AudioCompareAudioNode; // defined in AudioCompareNode.cpp (two-object rule)

// --- Audio Compare ------------------------------------------------------
// Two audio inputs in (A, B), "how does B differ from A" out, as numbers - so
// a patch can react to one sound matching, drowning out, detuning from or
// lagging behind another.
//
// Same shape as Audio Analyze / Audio to CV: an INode that owns an AudioNode
// the topology builder runs (exposed through AudioNodeForNotePorts(), NOT as
// an IAudioSource - its graph-visible outputs are modulator values, not an
// audio buffer).
//
// CONVENTION: A is the reference, B is the sound being judged. Every output is
// 0..1, and every signed output reads "B relative to A": 0.5 = the same,
// above 0.5 = B louder / higher / later, below 0.5 = B quieter / lower /
// earlier. The output order is fixed - new outputs are only ever appended, so
// saved cables keep their meaning.
//
//   0 similarity   weighted mean of the eight band similarities, each band
//                  weighted by how far its LOUDER side sits above the -80 dB
//                  silence floor. 1 = same spectral balance and level, 0 =
//                  every audible band `range` dB or more apart. Ignores
//                  polarity (B = -A reads 1).
//   1 level diff   0.5 + (dB(B) - dB(A)) / (2 * range), full-band RMS dB.
//                  0.5 = equally loud, > 0.5 = B louder, 1 = B `range` dB or
//                  more louder, 0 = A `range` dB or more louder.
//   2 correlation  waveform correlation r = sum(a*b) / sqrt(sum(a^2) sum(b^2)),
//                  -1..1 mapped to 0..1: 1 = same waveform, 0.5 = unrelated,
//                  0 = polarity inverted. Measured on the aligned pair
//                  when `align` is on.
//   3..10 band 1..8  1 - |dB(B_band) - dB(A_band)| / range, eight log-spaced
//                  bands 40 Hz .. 16 kHz, low to high. 1 = equal, 0 = `range`
//                  dB or more apart in EITHER direction (the body shows which).
//   11 pitch diff  0.5 + cents / (2 * pitch range), cents = 1200 log2(f0(B) /
//                  f0(A)) from a YIN pitch estimate on each side. 0.5 = same
//                  pitch, > 0.5 = B sharp / higher. Falls back to 0.5 once
//                  either side has been unpitched or silent for 0.3 s.
//   12 onset match fraction of the onsets seen in the last 3 s (both sides)
//                  that have a partner on the other side within +-30 ms, after
//                  alignment. 1 = every onset matched (also: no onsets at all),
//                  0 = none.
//   13 timing      0.5 + lag / (2 * lag range), lag = ms B arrives after A,
//                  found by envelope cross-correlation over the last 3 s
//                  (search span +-250 ms) refined on the waveform. 0.5 =
//                  aligned, > 0.5 = B late, < 0.5 = B early. Measured whether
//                  or not `align` is on, and held while the material gives no
//                  confident estimate (steady tones, silence).
//
// Params: time = smoothing time constant (ms) for level, bands, similarity,
// correlation, pitch and onset match. range = dB span for level diff and the
// band similarities. pitch range = cents span for pitch diff. lag range = ms
// span for timing. align = delay the earlier input by the found lag before
// level, bands, similarity, correlation, pitch and onset match are computed,
// so a latency between the two paths does not read as a difference.
//
// Unconnected inputs are silence, so with nothing patched every output sits
// at "identical" (two silences are the same signal).
class AudioCompareNode : public INode
{
public:
   enum Output
   {
      kSimilarity = 0,
      kLevelDiff,
      kCorrelation,
      kBand0,
      kPitchDiff = kBand0 + 8,
      kOnsetMatch,
      kTiming,
      kOutputCount
   };
   static constexpr int kBands = 8;

   static INode* Create() { return new AudioCompareNode(); }

   AudioCompareNode(); // out-of-line: mAudioNode's pointee is forward-declared here
   ~AudioCompareNode() override;

   unsigned int GetOutputTexture() override { return 0; }
   int GetOutputWidth() const override { return 0; }
   int GetOutputHeight() const override { return 0; }
   void CookIfNeeded(int frameId) override;

   int OutputCount() const override { return kOutputCount; }
   const char* OutputLabel(int index) const override;
   IModulator* ModulatorOutput(int index) override;

   AudioCable inputA;
   AudioCable inputB;
   AudioCable* AudioInputSlot(int slot) override { return slot == 0 ? &inputA : (slot == 1 ? &inputB : nullptr); }
   const char* InputLabel(int slot) const override { return slot == 0 ? "A" : (slot == 1 ? "B" : nullptr); }

   // Owns an AudioNode but is not an audio source - see the class comment.
   AudioNode* AudioNodeForNotePorts() override;
   // Keep analysing while anything is patched in, even with nothing
   // downstream (there never is - the outputs are modulator values).
   bool RequiresAudioProcessing() const override { return inputA.IsConnected() || inputB.IsConnected(); }

   // 0..1 value of output `index` (see Output), as last published by the
   // audio thread. Pure read of atomics - safe to call any number of times
   // per frame (the modulator idempotency rule).
   float Value(int index) const;

   // Raw readouts for the body (main thread), all "B relative to A".
   float LevelDiffDb() const;            // dB(B) - dB(A), full band
   float Correlation() const;            // -1..1
   float LevelDb(int input) const;       // full-band RMS dB of input 0 (A) / 1 (B)
   float BandDb(int input, int band) const;
   float PitchHz(int input) const;       // last f0 estimate, 0 = unpitched / silent
   float PitchDiffCents() const;         // smoothed 1200 log2(f0(B) / f0(A))
   float LagMs() const;                  // ms B arrives after A (negative = B early)
   bool LagLocked() const;               // false until the first confident lag estimate
   static float BandCentreHz(int band);
   static float SilenceFloorDb();
   static float MaxLagMs();              // lag search span, either direction

   float timeMs = 150.0f;          // smoothing time constant, 10..2000 ms
   float rangeDb = 24.0f;          // dB span: level diff 0.5 -> 0/1, band similarity 1 -> 0, 3..60
   float pitchRangeCents = 100.0f; // cents span: pitch diff 0.5 -> 0/1, 10..1200
   float lagRangeMs = 50.0f;       // ms span: timing 0.5 -> 0/1, 1..250
   bool align = true;              // compensate the found lag before comparing

   void VisitParams(ParamVisitor& v) override
   {
      v.Float("time", timeMs);
      v.Float("range", rangeDb);
      v.Float("pitchRange", pitchRangeCents);
      v.Float("lagRange", lagRangeMs);
      v.Bool("align", align);
   }

   // INFINITE_AUDIOCOMPARETEST: drives the audio-thread half directly with
   // synthetic buffers and checks the outputs against analytic expectations.
   static bool RunSelfTest();

private:
   struct Tap : public IModulator
   {
      AudioCompareNode* owner = nullptr;
      int index = 0;
      float Value01() override { return owner ? owner->Value(index) : 0.0f; }
   };

   Tap mTaps[kOutputCount];
   std::unique_ptr<AudioCompareAudioNode> mAudioNode;
   int mLastCookFrame = -1;
};
