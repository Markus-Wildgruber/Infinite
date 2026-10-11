#include "AudioCompareNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio/AudioNode.h"
#include "audio/DspMath.h"
#include "audio/dsp/FluxOnsetDetector.h"

namespace
{
   constexpr int kBands = AudioCompareNode::kBands;
   constexpr int kInputs = 2;

   // Band layout: kBands log-spaced bands whose edges run kLowHz..kHighHz,
   // each centred (geometrically) inside its own edges.
   constexpr float kLowHz = 40.0f;
   constexpr float kHighHz = 16000.0f;

   // Two cascaded RBJ band-passes per band (4th order, ~12 dB/octave skirts)
   // so a tone in one band leaks well below `range` into the far ones.
   constexpr int kStages = 2;

   // Silence floor, RMS dBFS. Any level below it reads as exactly this, so
   // two silences compare as identical and nothing below it gets a vote in
   // the overall similarity.
   constexpr float kFloorDb = -80.0f;
   constexpr double kFloorPower = 1.0e-8; // 10^(kFloorDb / 10)

   // --- Envelope: lag search and onsets ----------------------------------
   // Each input's mean square is followed by a short one-pole and sampled
   // once per ~1 ms "bin". Short enough to keep onsets sharp, long enough
   // that the ripple of a bass tone (twice its frequency) stays out of both
   // the onset detector and the lag search.
   constexpr double kEnvSmoothMs = 8.0;
   constexpr int kEnvRing = 4096;        // bins kept, power of two, > kEnvAnalysis
   constexpr int kEnvAnalysis = 3000;    // bins cross-correlated per lag estimate (~3 s)
   constexpr int kEnvMinAnalysis = 1000; // bins of history before the first estimate
   constexpr int kMaxLagBins = 250;      // lag search span either way (~250 ms)
   // A lag estimate counts only when the envelopes actually move (coefficient
   // of variation) and line up well at the peak; anything else (a steady
   // tone, silence, two unrelated sounds) holds the previous lag.
   constexpr float kLagMinCorr = 0.5f;
   constexpr float kLagMinCv = 0.1f;
   // Tie-breaker towards the lag already in use, so a periodic envelope (all
   // of its peaks equally good) never walks the lag to another period.
   constexpr float kLagPenaltyPerBin = 0.0005f;
   // A jump bigger than this has to be seen twice in a row before it is used.
   constexpr double kLagAgreeMs = 2.0;

   // --- Decimated signal: pitch and the fine lag -------------------------
   constexpr double kDecTargetHz = 16000.0; // rate = sampleRate / round(sampleRate / 16k), < 24 kHz
   constexpr int kDecRing = 32768;          // decimated samples kept, power of two
   constexpr int kYinWindow = 1024;         // integration window, ~64 ms at 16 kHz
   constexpr int kYinMaxTauCap = 512;       // >= 24 kHz / kYinMinHz
   constexpr double kYinMinHz = 60.0;
   constexpr double kYinMaxHz = 2000.0;
   constexpr float kYinThreshold = 0.2f;    // CMNDF dip that counts as pitched
   constexpr float kPitchSilenceDb = -60.0f;
   constexpr double kPitchHoldSec = 0.3;    // unpitched this long -> pitch diff relaxes to 0.5
   constexpr int kRefineWindow = 2048;      // waveform cross-correlation around the envelope lag
   constexpr double kRefineSpanMs = 1.5;
   constexpr int kRefineMaxHalf = 64;
   constexpr float kRefineMinCorr = 0.6f;   // |r| needed to trust the waveform over the envelope
   constexpr int kRefineRingMargin = 4096;  // decimated samples that may arrive while refining

   // --- Onsets ------------------------------------------------------------
   // Onsets themselves come from FluxOnsetDetector (spectral flux, shared
   // with the offline measurement); these only size the bookkeeping.
   constexpr int kOnsetRing = 64;
   constexpr int kOnsetDbHist = 16;          // power of two
   constexpr int kOnsetTolBins = 50;         // "coincide" = within +-50 ms (= AudioAnalysis::Compare)
   constexpr int kOnsetWindowBins = 3000;    // onsets older than ~3 s drop out

   // --- Cost --------------------------------------------------------------
   // The lag search and the pitch estimate run as resumable jobs, a bounded
   // number of multiply-adds per input sample, so no single block pays for a
   // whole cross-correlation (~1.8M MACs) or difference function.
   constexpr long long kLagBudget = 192;
   constexpr long long kPitchBudget = 128;

   const char* const kOutputNames[AudioCompareNode::kOutputCount] = {
      "similarity", "level diff", "correlation",
      "band 1", "band 2", "band 3", "band 4", "band 5", "band 6", "band 7", "band 8",
      "pitch diff", "onset match", "timing",
      "band 1 diff", "band 2 diff", "band 3 diff", "band 4 diff",
      "band 5 diff", "band 6 diff", "band 7 diff", "band 8 diff"
   };

   float BandCentre(int band)
   {
      return kLowHz * powf(kHighHz / kLowHz, ((float)band + 0.5f) / (float)kBands);
   }

   float PowerToDb(double power)
   {
      if (!(power > kFloorPower))
         return kFloorDb;
      return (float)(10.0 * std::log10(power));
   }

   // NaN-safe: a NaN fails both comparisons and lands on 0.
   float Clamp01(float v)
   {
      if (!(v > 0.0f))
         return 0.0f;
      return v < 1.0f ? v : 1.0f;
   }

   // NaN-safe clamp for params read off the main thread.
   float ClampParam(float v, float lo, float hi)
   {
      if (!(v >= lo))
         return lo;
      return v > hi ? hi : v;
   }

   double FlushTiny(double v)
   {
      return std::fabs(v) < 1.0e-30 ? 0.0 : v;
   }

   // Ring index of an absolute (possibly negative) sample/bin counter.
   int Wrap(int64_t i, int size)
   {
      return (int)((uint64_t)i & (uint64_t)(size - 1));
   }

   void FlushBiquad(DspMath::Biquad& f)
   {
      f.x1 = DspMath::FlushDenormal(f.x1);
      f.x2 = DspMath::FlushDenormal(f.x2);
      f.y1 = DspMath::FlushDenormal(f.y1);
      f.y2 = DspMath::FlushDenormal(f.y2);
   }
}

// ------------------------------------------------- audio-thread half
// Owns every piece of DSP state. ProcessBlock: no allocation, no locks, no
// strings - filters, followers, histories and job scratch are fixed-size
// members (the alignment delay lines are sized once in PrepareToPlay),
// results are published through relaxed atomics read by the main thread's
// taps/body.
//
// Per sample: the RAW inputs feed the analysis histories (1 ms envelope bins
// with onset detection, a ~16 kHz decimated copy); the ALIGNED inputs (the
// earlier one delayed by the current lag when `align` is on) feed the level /
// band / correlation followers. Per block: the lag job and the pitch job each
// advance by a bounded amount of work, onset match is recounted, everything
// is published.
class AudioCompareAudioNode : public AudioNode
{
public:
   AudioCompareAudioNode()
   {
      mTimeMs.store(150.0f, std::memory_order_relaxed);
      mRangeDb.store(24.0f, std::memory_order_relaxed);
      mPitchRange.store(100.0f, std::memory_order_relaxed);
      mLagRange.store(50.0f, std::memory_order_relaxed);
      mAlign.store(true, std::memory_order_relaxed);
      // Before the first block, report "two identical silences" - the same
      // thing an unpatched node computes once it runs.
      for (int i = 0; i < AudioCompareNode::kOutputCount; i++)
      {
         const bool centred = i == AudioCompareNode::kLevelDiff || i == AudioCompareNode::kPitchDiff ||
                              i == AudioCompareNode::kTiming || i >= AudioCompareNode::kBandDiff0;
         mOut[i].store(centred ? 0.5f : 1.0f, std::memory_order_relaxed);
      }
      for (int in = 0; in < kInputs; in++)
      {
         mLevelDb[in].store(kFloorDb, std::memory_order_relaxed);
         mPitchHz[in].store(0.0f, std::memory_order_relaxed);
         for (int b = 0; b < kBands; b++)
            mBandDb[in][b].store(kFloorDb, std::memory_order_relaxed);
      }
      mCorr.store(1.0f, std::memory_order_relaxed);
      mLevelDiffDb.store(0.0f, std::memory_order_relaxed);
      mCentsOut.store(0.0f, std::memory_order_relaxed);
      mLagMsOut.store(0.0f, std::memory_order_relaxed);
      mLagLockedOut.store(false, std::memory_order_relaxed);
      ConfigureRate(48000.0);
      ResetState();
   }

   void PrepareToPlay(double sampleRate, int /*maxBlockSize*/) override
   {
      ConfigureRate(sampleRate > 0.0 ? sampleRate : 48000.0);
      // Alignment delay lines: the longest lag AcceptLag can ever set, in
      // samples at this rate, rounded up to a power of two.
      const int maxLagSamples = (int)std::ceil(MaxLagMsTotal() * 0.001 * mSampleRate) + 2;
      int size = 1;
      while (size <= maxLagSamples)
         size <<= 1;
      for (int in = 0; in < kInputs; in++)
      {
         mDelay[in].assign((size_t)size, 0.0f);
         mOnsetDet[in].Prepare(mSampleRate);
      }
      mDelayMask = size - 1;
      ResetState();
      mCoeffForMs = -1.0f; // recompute the follower coefficient at the new rate
   }

   void Reset() override { ResetState(); }

   // Main thread (CookIfNeeded). Plain atomics - each is read once per block
   // and clamped on the audio side, so a torn set is harmless.
   void SetParams(float timeMs, float rangeDb, float pitchRangeCents, float lagRangeMs, bool align)
   {
      mTimeMs.store(timeMs, std::memory_order_relaxed);
      mRangeDb.store(rangeDb, std::memory_order_relaxed);
      mPitchRange.store(pitchRangeCents, std::memory_order_relaxed);
      mLagRange.store(lagRangeMs, std::memory_order_relaxed);
      mAlign.store(align, std::memory_order_relaxed);
   }

   void ProcessBlock(const AudioBuffer* const* inputs, int numInputs, AudioBuffer& output) override
   {
      const int numFrames = output.numFrames;

      // Nothing listens to this buffer (the outputs are modulator values),
      // but the AudioNode contract is "never skip the write".
      if (output.channels != nullptr)
         for (int ch = 0; ch < output.numChannels; ch++)
            if (output.channels[ch] != nullptr)
               std::fill(output.channels[ch], output.channels[ch] + numFrames, 0.0f);
      if (numFrames <= 0)
         return;

      const float timeMs = ClampParam(mTimeMs.load(std::memory_order_relaxed), 10.0f, 2000.0f);
      if (timeMs != mCoeffForMs)
      {
         // One-pole mean-square follower, time constant `timeMs`:
         // p += (1 - k) * (x^2 - p), k = exp(-1 / (tau * fs)).
         mCoeff = std::exp(-1.0 / (0.001 * (double)timeMs * mSampleRate));
         mCoeffForMs = timeMs;
      }
      const double g = 1.0 - mCoeff;
      const bool align = mAlign.load(std::memory_order_relaxed);

      const AudioBuffer* bufs[kInputs] = {
         (inputs != nullptr && numInputs > 0) ? inputs[0] : nullptr,
         (inputs != nullptr && numInputs > 1) ? inputs[1] : nullptr
      };

      const bool haveDelay = !mDelay[0].empty() && !mDelay[1].empty();
      const int delayA = (align && haveDelay) ? std::min(mDelaySamples[0], mDelayMask) : 0;
      const int delayB = (align && haveDelay) ? std::min(mDelaySamples[1], mDelayMask) : 0;

      for (int i = 0; i < numFrames; i++)
      {
         float x[kInputs];
         for (int in = 0; in < kInputs; in++)
            x[in] = MonoSample(bufs[in], i);

         FeedAnalysis(x[0], x[1]);

         // Alignment: the earlier input runs through its delay line.
         float a = x[0], b = x[1];
         if (haveDelay)
         {
            const int64_t w = mDelayWrite;
            mDelay[0][(size_t)Wrap(w, mDelayMask + 1)] = x[0];
            mDelay[1][(size_t)Wrap(w, mDelayMask + 1)] = x[1];
            a = mDelay[0][(size_t)Wrap(w - delayA, mDelayMask + 1)];
            b = mDelay[1][(size_t)Wrap(w - delayB, mDelayMask + 1)];
            mDelayWrite = w + 1;
         }
         const float y[kInputs] = { a, b };

         mPow[0] = FlushTiny(mPow[0] + g * ((double)a * a - mPow[0]));
         mPow[1] = FlushTiny(mPow[1] + g * ((double)b * b - mPow[1]));
         mCross = FlushTiny(mCross + g * ((double)a * b - mCross));

         for (int in = 0; in < kInputs; in++)
         {
            for (int band = 0; band < kBands; band++)
            {
               float v = y[in];
               for (int s = 0; s < kStages; s++)
                  v = mFilter[in][band][s].Process(v);
               double& p = mBandPow[in][band];
               p = FlushTiny(p + g * ((double)v * v - p));
            }
         }
      }
      mSampleCount += numFrames;

      RunLagJob((long long)numFrames * kLagBudget);
      RunPitchJob((long long)numFrames * kPitchBudget, align, timeMs);
      UpdateOnsetMatch(align, numFrames, timeMs);

      // Filter state can decay into the denormal range during silence; the
      // engine's FTZ/DAZ guard covers it on device threads, this covers the
      // headless/no-device pump too.
      for (int in = 0; in < kInputs; in++)
      {
         for (int band = 0; band < kBands; band++)
            for (int s = 0; s < kStages; s++)
               FlushBiquad(mFilter[in][band][s]);
         FlushBiquad(mDecFilter[in][0]);
         FlushBiquad(mDecFilter[in][1]);
      }

      Publish();
   }

   float Output(int index) const
   {
      if (index < 0 || index >= AudioCompareNode::kOutputCount)
         return 0.0f;
      return mOut[index].load(std::memory_order_relaxed);
   }
   float LevelDb(int input) const
   {
      return (input >= 0 && input < kInputs) ? mLevelDb[input].load(std::memory_order_relaxed) : kFloorDb;
   }
   float BandDb(int input, int band) const
   {
      if (input < 0 || input >= kInputs || band < 0 || band >= kBands)
         return kFloorDb;
      return mBandDb[input][band].load(std::memory_order_relaxed);
   }
   float PitchHz(int input) const
   {
      return (input >= 0 && input < kInputs) ? mPitchHz[input].load(std::memory_order_relaxed) : 0.0f;
   }
   float Correlation() const { return mCorr.load(std::memory_order_relaxed); }
   float LevelDiffDb() const { return mLevelDiffDb.load(std::memory_order_relaxed); }
   float PitchDiffCents() const { return mCentsOut.load(std::memory_order_relaxed); }
   float LagMs() const { return mLagMsOut.load(std::memory_order_relaxed); }
   bool LagLocked() const { return mLagLockedOut.load(std::memory_order_relaxed); }

private:
   // Mean of every channel at frame i; silence for a missing/short buffer.
   static float MonoSample(const AudioBuffer* buf, int i)
   {
      if (buf == nullptr || buf->channels == nullptr || buf->numChannels <= 0 || i >= buf->numFrames)
         return 0.0f;
      float sum = 0.0f;
      int count = 0;
      for (int ch = 0; ch < buf->numChannels; ch++)
      {
         if (buf->channels[ch] != nullptr)
         {
            sum += buf->channels[ch][i];
            count++;
         }
      }
      return count > 0 ? sum / (float)count : 0.0f;
   }

   // Everything that depends on the sample rate except the delay lines. No
   // allocation, so the constructor can run it too.
   void ConfigureRate(double sampleRate)
   {
      mSampleRate = sampleRate;
      mBinLen = std::max(1, (int)std::lround(mSampleRate / 1000.0));
      mEnvRate = mSampleRate / (double)mBinLen;
      mEnvG = 1.0 - std::exp(-1.0 / (0.001 * kEnvSmoothMs * mSampleRate));
      mDecFactor = std::max(1, (int)std::lround(mSampleRate / kDecTargetHz));
      mDecRate = mSampleRate / (double)mDecFactor;
      mYinMaxTau = std::min(kYinMaxTauCap, (int)std::ceil(mDecRate / kYinMinHz));
      mYinMinTau = std::max(2, (int)std::floor(mDecRate / kYinMaxHz));
      mRefineHalf = std::min(kRefineMaxHalf, (int)std::ceil(kRefineSpanMs * 0.001 * mDecRate) + 1);
      // Anti-alias for the decimated copy: 4th-order Butterworth low-pass at
      // 0.4 x the decimated rate (two RBJ stages, Q = 0.541 and 1.307).
      for (int in = 0; in < kInputs; in++)
      {
         mDecFilter[in][0].SetLowpass(0.4 * mDecRate, 0.5412, mSampleRate);
         mDecFilter[in][1].SetLowpass(0.4 * mDecRate, 1.3066, mSampleRate);
      }
      DesignBands();
   }

   // Longest |lag| an estimate can produce, ms: the envelope search span
   // plus the waveform refinement's reach past it.
   double MaxLagMsTotal() const
   {
      return (double)(kMaxLagBins + 1) * 1000.0 / mEnvRate + kRefineSpanMs + 1.0;
   }

   void DesignBands()
   {
      // Per-stage Q chosen so the CASCADE is -3 dB exactly at the band edges:
      // each 2nd-order RBJ band-pass has |H|^2 = 1 / (1 + Q^2 (f/f0 - f0/f)^2),
      // the band edges sit at f0 * r^(+-1/2) with r the edge ratio, and two
      // stages in series need |H|^2 = 1/sqrt(2) per stage there.
      const double r = std::pow((double)kHighHz / (double)kLowHz, 1.0 / (double)kBands);
      const double x = std::sqrt(r) - 1.0 / std::sqrt(r);
      const double q = std::sqrt(std::sqrt(2.0) - 1.0) / x;
      const double nyquistGuard = 0.45 * mSampleRate;
      for (int b = 0; b < kBands; b++)
      {
         const double fc = std::min((double)BandCentre(b), nyquistGuard);
         for (int in = 0; in < kInputs; in++)
            for (int s = 0; s < kStages; s++)
               mFilter[in][b][s].SetBandpass(fc, q, mSampleRate);
      }
   }

   void ResetState()
   {
      for (int in = 0; in < kInputs; in++)
      {
         mPow[in] = 0.0;
         for (int b = 0; b < kBands; b++)
         {
            mBandPow[in][b] = 0.0;
            for (int s = 0; s < kStages; s++)
               mFilter[in][b][s].Reset();
         }
         mDecFilter[in][0].Reset();
         mDecFilter[in][1].Reset();
         mEnvMs[in] = 0.0;
         std::fill(mEnvRing[in], mEnvRing[in] + kEnvRing, 0.0f);
         std::fill(mEnvDb[in], mEnvDb[in] + kOnsetDbHist, -100.0f);
         std::fill(mDecRing[in], mDecRing[in] + kDecRing, 0.0f);
         std::fill(mDelay[in].begin(), mDelay[in].end(), 0.0f);
         mOnsetHead[in] = 0;
         mOnsetFill[in] = 0;
         mLastOnset[in] = INT64_MIN / 2;
         mOnsetDet[in].Reset();
         mDelaySamples[in] = 0;
         mPitchLoud[in] = false;
      }
      mCross = 0.0;
      mBinPhase = 0;
      mBinCount = 0;
      mDecPhase = 0;
      mDecCount = 0;
      mDelayWrite = 0;
      mSampleCount = 0;

      mLagStage = kLagSnap;
      mLagMs = 0.0;
      mLagLocked = false;
      mHasPending = false;
      mPendingMs = 0.0;
      mLagMsOut.store(0.0f, std::memory_order_relaxed);
      mLagLockedOut.store(false, std::memory_order_relaxed);

      mPitchStage = kPitchSnap;
      mPitchLastSample = 0;
      mCents = 0.0;
      mUnvoicedSec = 0.0;
      mPitchHz[0].store(0.0f, std::memory_order_relaxed);
      mPitchHz[1].store(0.0f, std::memory_order_relaxed);
      mCentsOut.store(0.0f, std::memory_order_relaxed);

      mOnsetMatch = 1.0;
   }

   // ---- per-sample analysis feed (raw, unaligned inputs) -------------------
   void FeedAnalysis(float x0, float x1)
   {
      const float x[kInputs] = { x0, x1 };
      for (int in = 0; in < kInputs; in++)
         mEnvMs[in] = FlushTiny(mEnvMs[in] + mEnvG * ((double)x[in] * x[in] - mEnvMs[in]));
      // Onsets: the spectral-flux detector the offline measurement uses too
      // (audio/dsp/FluxOnsetDetector.h), so "onset match" agrees with it.
      // Times come back in samples since the reset; stored in envelope bins.
      for (int in = 0; in < kInputs; in++)
      {
         mOnsetDet[in].Push(x[in], [this, in](double sample)
         {
            mOnsets[in][mOnsetHead[in]] = (int64_t)(sample / (double)mBinLen);
            mOnsetHead[in] = (mOnsetHead[in] + 1) % kOnsetRing;
            mOnsetFill[in] = std::min(mOnsetFill[in] + 1, kOnsetRing);
         });
      }
      if (++mBinPhase >= mBinLen)
      {
         mBinPhase = 0;
         PushEnvelopeBin();
      }

      if (mDecFactor > 1)
      {
         float y[kInputs];
         for (int in = 0; in < kInputs; in++)
            y[in] = mDecFilter[in][1].Process(mDecFilter[in][0].Process(x[in]));
         if (++mDecPhase >= mDecFactor)
         {
            mDecPhase = 0;
            const int slot = Wrap(mDecCount, kDecRing);
            mDecRing[0][slot] = y[0];
            mDecRing[1][slot] = y[1];
            mDecCount++;
         }
      }
      else
      {
         const int slot = Wrap(mDecCount, kDecRing);
         mDecRing[0][slot] = x0;
         mDecRing[1][slot] = x1;
         mDecCount++;
      }
   }

   // One ~1 ms bin: store the envelope, run the onset detector.
   void PushEnvelopeBin()
   {
      const int slot = Wrap(mBinCount, kEnvRing);
      for (int in = 0; in < kInputs; in++)
      {
         const double ms = mEnvMs[in] > 0.0 ? mEnvMs[in] : 0.0;
         mEnvRing[in][slot] = (float)std::sqrt(ms);
         const float db = ms > 1.0e-10 ? (float)(10.0 * std::log10(ms)) : -100.0f;
         mEnvDb[in][Wrap(mBinCount, kOnsetDbHist)] = db;
      }
      mBinCount++;
   }

   // ---- lag job --------------------------------------------------------------
   // snapshot the last <= 3 s of both envelopes -> normalised cross-correlation
   // at every lag in +-kMaxLagBins (one lag per step) -> pick the peak ->
   // refine it on the decimated waveform (one lag per step) -> accept.
   void RunLagJob(long long budget)
   {
      while (budget > 0)
      {
         switch (mLagStage)
         {
         case kLagSnap:
         {
            const int n = (int)std::min<int64_t>(kEnvAnalysis, mBinCount);
            if (n < kEnvMinAnalysis)
               return;
            double sumA = 0.0, sumB = 0.0;
            const int64_t first = mBinCount - n;
            for (int k = 0; k < n; k++)
            {
               const int slot = Wrap(first + k, kEnvRing);
               mSnapA[k] = mEnvRing[0][slot];
               mSnapB[k] = mEnvRing[1][slot];
               sumA += mSnapA[k];
               sumB += mSnapB[k];
            }
            const double meanA = sumA / n, meanB = sumB / n;
            double varA = 0.0, varB = 0.0;
            for (int k = 0; k < n; k++)
            {
               mSnapA[k] = (float)(mSnapA[k] - meanA);
               mSnapB[k] = (float)(mSnapB[k] - meanB);
               varA += (double)mSnapA[k] * mSnapA[k];
               varB += (double)mSnapB[k] * mSnapB[k];
            }
            mSnapSdA = std::sqrt(varA / n);
            mSnapSdB = std::sqrt(varB / n);
            mSnapCvOk = meanA > 1.0e-5 && meanB > 1.0e-5 && mSnapSdA / meanA >= kLagMinCv &&
                        mSnapSdB / meanB >= kLagMinCv;
            mSnapN = n;
            mLagIdx = -kMaxLagBins;
            mLagStage = kLagCorr;
            budget -= 3LL * n;
            break;
         }
         case kLagCorr:
         {
            // r(l) = mean over the overlap of a[k] * b[k + l] / (sdA * sdB):
            // peaks at l = L when B is A delayed by L bins.
            const int l = mLagIdx;
            const int lo = std::max(0, -l);
            const int hi = std::min(mSnapN, mSnapN - l);
            double s = 0.0;
            for (int k = lo; k < hi; k++)
               s += (double)mSnapA[k] * mSnapB[k + l];
            const double denom = (double)std::max(1, hi - lo) * mSnapSdA * mSnapSdB;
            mLagR[l + kMaxLagBins] = denom > 1.0e-20 ? (float)(s / denom) : 0.0f;
            budget -= std::max(1, hi - lo);
            if (++mLagIdx > kMaxLagBins)
               mLagStage = kLagPick;
            break;
         }
         case kLagPick:
         {
            const double curBins = mLagMs * 0.001 * mEnvRate;
            int best = 0;
            double bestScore = -1.0e30;
            for (int idx = 0; idx <= 2 * kMaxLagBins; idx++)
            {
               const double l = (double)(idx - kMaxLagBins);
               const double score = (double)mLagR[idx] - kLagPenaltyPerBin * std::fabs(l - curBins);
               if (score > bestScore)
               {
                  bestScore = score;
                  best = idx;
               }
            }
            if (!mSnapCvOk || !(mLagR[best] >= kLagMinCorr))
            {
               mLagStage = kLagSnap; // nothing to go on: hold the current lag
               return;
            }
            double frac = 0.0;
            if (best > 0 && best < 2 * kMaxLagBins)
            {
               const double r0 = mLagR[best - 1], r1 = mLagR[best], r2 = mLagR[best + 1];
               const double den = r0 - 2.0 * r1 + r2;
               if (den < 0.0)
                  frac = std::clamp(0.5 * (r0 - r2) / den, -0.5, 0.5);
            }
            mEnvLagSec = ((double)(best - kMaxLagBins) + frac) / mEnvRate;

            // Waveform refinement window: decimated samples [t0, t0 + W) of A
            // against [t0 + l, t0 + l + W) of B, every l within the span, all
            // already written and not about to be overwritten.
            mRefCenter = (int)std::lround(mEnvLagSec * mDecRate);
            mRefT0 = mDecCount - kRefineWindow - std::max(0, mRefCenter + mRefineHalf);
            const int64_t oldest = mRefT0 + std::min(0, mRefCenter - mRefineHalf);
            if (oldest < 0 || oldest < mDecCount - (kDecRing - kRefineRingMargin))
            {
               AcceptLag(mEnvLagSec * 1000.0);
               mLagStage = kLagSnap;
               return;
            }
            mLagIdx = -mRefineHalf;
            mLagStage = kLagRefine;
            break;
         }
         case kLagRefine:
         {
            const int l = mRefCenter + mLagIdx;
            double dot = 0.0, ea = 0.0, eb = 0.0;
            for (int j = 0; j < kRefineWindow; j++)
            {
               const double av = mDecRing[0][Wrap(mRefT0 + j, kDecRing)];
               const double bv = mDecRing[1][Wrap(mRefT0 + j + l, kDecRing)];
               dot += av * bv;
               ea += av * av;
               eb += bv * bv;
            }
            mRefR[mLagIdx + mRefineHalf] = (ea > 1.0e-20 && eb > 1.0e-20) ? (float)(dot / std::sqrt(ea * eb)) : 0.0f;
            budget -= 3LL * kRefineWindow;
            if (++mLagIdx > mRefineHalf)
               mLagStage = kLagFinish;
            break;
         }
         case kLagFinish:
         {
            // |r|, so a polarity-inverted B refines too. A peak on the edge of
            // the span is not a peak - keep the envelope's answer then.
            int best = 0;
            for (int idx = 1; idx <= 2 * mRefineHalf; idx++)
               if (std::fabs(mRefR[idx]) > std::fabs(mRefR[best]))
                  best = idx;
            double lagSec = mEnvLagSec;
            if (best > 0 && best < 2 * mRefineHalf && std::fabs(mRefR[best]) >= kRefineMinCorr)
            {
               const double r0 = std::fabs(mRefR[best - 1]), r1 = std::fabs(mRefR[best]),
                            r2 = std::fabs(mRefR[best + 1]);
               const double den = r0 - 2.0 * r1 + r2;
               const double frac = den < 0.0 ? std::clamp(0.5 * (r0 - r2) / den, -0.5, 0.5) : 0.0;
               lagSec = ((double)(mRefCenter + best - mRefineHalf) + frac) / mDecRate;
            }
            AcceptLag(lagSec * 1000.0);
            mLagStage = kLagSnap;
            return;
         }
         default:
            mLagStage = kLagSnap;
            return;
         }
      }
   }

   // First confident estimate is taken as is; after that a small move is
   // tracked at once and a jump must repeat before it is believed.
   void AcceptLag(double ms)
   {
      const double maxMs = MaxLagMsTotal();
      ms = std::clamp(ms, -maxMs, maxMs);
      if (!mLagLocked || std::fabs(ms - mLagMs) <= kLagAgreeMs)
      {
         SetLag(ms);
      }
      else if (mHasPending && std::fabs(ms - mPendingMs) <= kLagAgreeMs)
      {
         SetLag(ms);
      }
      else
      {
         mPendingMs = ms;
         mHasPending = true;
      }
   }

   void SetLag(double ms)
   {
      mLagMs = ms;
      mLagLocked = true;
      mHasPending = false;
      // Positive lag = B late: delay A by it. Negative = B early: delay B.
      const int samples = (int)std::lround(ms * 0.001 * mSampleRate);
      mDelaySamples[0] = std::clamp(samples, 0, mDelayMask);
      mDelaySamples[1] = std::clamp(-samples, 0, mDelayMask);
      mLagMsOut.store((float)ms, std::memory_order_relaxed);
      mLagLockedOut.store(true, std::memory_order_relaxed);
   }

   // ---- pitch job --------------------------------------------------------------
   // snapshot a window of each decimated input (A taken `lag` earlier when
   // aligning) -> YIN difference function, one tau per step, A then B ->
   // pick both f0s, update the smoothed cents.
   void RunPitchJob(long long budget, bool align, float timeMs)
   {
      const int len = kYinWindow + mYinMaxTau + 1;
      while (budget > 0)
      {
         switch (mPitchStage)
         {
         case kPitchSnap:
         {
            const int lagDec = align ? (int)std::lround(mLagMs * 0.001 * mDecRate) : 0;
            const int off[kInputs] = { std::max(0, lagDec), std::max(0, -lagDec) };
            if (mDecCount < (int64_t)len + std::max(off[0], off[1]))
               return;
            for (int in = 0; in < kInputs; in++)
            {
               const int64_t start = mDecCount - off[in] - len;
               double sq = 0.0;
               for (int k = 0; k < len; k++)
               {
                  const float v = mDecRing[in][Wrap(start + k, kDecRing)];
                  mPitchBuf[in][k] = v;
                  sq += (double)v * v;
               }
               mPitchLoud[in] = PowerToDb(sq / len) >= kPitchSilenceDb;
            }
            mPitchIn = 0;
            mPitchTau = 1;
            mPitchStage = kPitchDiff;
            budget -= 2LL * len;
            break;
         }
         case kPitchDiff:
         {
            if (mPitchIn >= kInputs)
            {
               mPitchStage = kPitchFinish;
               break;
            }
            if (!mPitchLoud[mPitchIn])
            {
               mPitchIn++;
               mPitchTau = 1;
               break;
            }
            // d(tau) = sum_j (x[j] - x[j + tau])^2 over the window.
            const float* x = mPitchBuf[mPitchIn];
            const int tau = mPitchTau;
            double d = 0.0;
            for (int j = 0; j < kYinWindow; j++)
            {
               const double diff = (double)x[j] - x[j + tau];
               d += diff * diff;
            }
            mYinD[mPitchIn][tau] = (float)d;
            budget -= kYinWindow;
            if (++mPitchTau > mYinMaxTau)
            {
               mPitchIn++;
               mPitchTau = 1;
            }
            break;
         }
         case kPitchFinish:
         {
            const float fA = YinHz(0);
            const float fB = YinHz(1);
            mPitchHz[0].store(fA, std::memory_order_relaxed);
            mPitchHz[1].store(fB, std::memory_order_relaxed);
            const double dt = (double)(mSampleCount - mPitchLastSample) / mSampleRate;
            mPitchLastSample = mSampleCount;
            double target;
            if (fA > 0.0f && fB > 0.0f)
            {
               target = 1200.0 * std::log2((double)fB / (double)fA);
               mUnvoicedSec = 0.0;
            }
            else
            {
               mUnvoicedSec += dt;
               target = mUnvoicedSec < kPitchHoldSec ? mCents : 0.0;
            }
            const double k = std::exp(-dt / (0.001 * (double)timeMs));
            mCents += (1.0 - k) * (target - mCents);
            if (!std::isfinite(mCents))
               mCents = 0.0;
            mCentsOut.store((float)mCents, std::memory_order_relaxed);
            mPitchStage = kPitchSnap;
            return;
         }
         default:
            mPitchStage = kPitchSnap;
            return;
         }
      }
   }

   // YIN (de Cheveigne & Kawahara 2002): cumulative-mean-normalised
   // difference, first dip under the threshold, walked to its minimum,
   // parabolic interpolation on d. 0 = silent or unpitched.
   float YinHz(int in) const
   {
      if (!mPitchLoud[in])
         return 0.0f;
      const float* d = mYinD[in];
      float cm[kYinMaxTauCap + 2];
      cm[0] = 1.0f;
      double running = 0.0;
      for (int tau = 1; tau <= mYinMaxTau; tau++)
      {
         running += d[tau];
         cm[tau] = running > 1.0e-20 ? (float)((double)d[tau] * tau / running) : 1.0f;
      }
      int found = -1;
      for (int tau = mYinMinTau; tau < mYinMaxTau; tau++)
      {
         if (cm[tau] < kYinThreshold)
         {
            while (tau + 1 < mYinMaxTau && cm[tau + 1] < cm[tau])
               tau++;
            found = tau;
            break;
         }
      }
      if (found < 0)
         return 0.0f;
      double better = (double)found;
      {
         const double s0 = d[found - 1], s1 = d[found], s2 = d[found + 1];
         const double den = s0 - 2.0 * s1 + s2;
         if (den > 1.0e-20)
            better += std::clamp(0.5 * (s0 - s2) / den, -1.0, 1.0);
      }
      return better > 0.5 ? (float)(mDecRate / better) : 0.0f;
   }

   // ---- onset match ----------------------------------------------------------
   // Over the last ~3 s (in A's time frame, B shifted back by the lag when
   // aligning): (A onsets with a B partner within +-30 ms + B onsets with an
   // A partner) / (all of them). Onsets newer than the tolerance plus the lag
   // wait until their partner could have arrived.
   void UpdateOnsetMatch(bool align, int numFrames, float timeMs)
   {
      const double lagBins = align ? mLagMs * 0.001 * mEnvRate : 0.0;
      const double now = (double)mBinCount;
      const double lo = now - (double)kOnsetWindowBins;
      // The detector decides an onset ~85 ms after it happened; wait that
      // long too, or the partner of a fresh onset is not in the ring yet.
      const double latencyBins = (double)mOnsetDet[0].LatencySamples() / (double)mBinLen;
      const double hi = now - (double)kOnsetTolBins - std::fabs(lagBins) - latencyBins - 2.0;
      int total = 0, matched = 0;
      for (int side = 0; side < kInputs; side++)
      {
         const int other = 1 - side;
         // Aligned time of an onset: A as is, B minus the lag.
         const double shiftSelf = side == 1 ? lagBins : 0.0;
         const double shiftOther = other == 1 ? lagBins : 0.0;
         for (int i = 0; i < mOnsetFill[side]; i++)
         {
            const double t = (double)mOnsets[side][i] - shiftSelf;
            if (t < lo || t > hi)
               continue;
            total++;
            for (int j = 0; j < mOnsetFill[other]; j++)
            {
               if (std::fabs((double)mOnsets[other][j] - shiftOther - t) <= (double)kOnsetTolBins)
               {
                  matched++;
                  break;
               }
            }
         }
      }
      const double target = total > 0 ? (double)matched / (double)total : 1.0;
      const double k = std::exp(-(double)numFrames / (0.001 * (double)timeMs * mSampleRate));
      mOnsetMatch += (1.0 - k) * (target - mOnsetMatch);
   }

   // Block-rate: turns follower state into the published 0..1 outputs.
   void Publish()
   {
      const float rangeDb = ClampParam(mRangeDb.load(std::memory_order_relaxed), 3.0f, 60.0f);
      const float pitchRange = ClampParam(mPitchRange.load(std::memory_order_relaxed), 10.0f, 1200.0f);
      const float lagRange = ClampParam(mLagRange.load(std::memory_order_relaxed), 1.0f, 250.0f);

      // Level diff: full-band RMS dB, B minus A.
      const float dbA = PowerToDb(mPow[0]);
      const float dbB = PowerToDb(mPow[1]);
      const float diffDb = dbB - dbA;
      mLevelDb[0].store(dbA, std::memory_order_relaxed);
      mLevelDb[1].store(dbB, std::memory_order_relaxed);
      mLevelDiffDb.store(diffDb, std::memory_order_relaxed);
      mOut[AudioCompareNode::kLevelDiff].store(Clamp01(0.5f + diffDb / (2.0f * rangeDb)), std::memory_order_relaxed);

      // Correlation: normalised exponentially-weighted inner product. Two
      // silences are the same signal (+1); silence against sound is
      // unrelated (0).
      const bool silentA = !(mPow[0] > kFloorPower);
      const bool silentB = !(mPow[1] > kFloorPower);
      float corr;
      if (silentA && silentB)
         corr = 1.0f;
      else if (silentA || silentB)
         corr = 0.0f;
      else
         corr = (float)(mCross / std::sqrt(mPow[0] * mPow[1]));
      if (!(corr >= -1.0f))
         corr = -1.0f; // also catches NaN
      if (corr > 1.0f)
         corr = 1.0f;
      mCorr.store(corr, std::memory_order_relaxed);
      mOut[AudioCompareNode::kCorrelation].store(Clamp01(0.5f * (corr + 1.0f)), std::memory_order_relaxed);

      // Bands, and the overall similarity as their weighted mean:
      //   s_b = clamp(1 - |dB_b - dA_b| / range, 0, 1)
      //   w_b = max(dA_b, dB_b) - floor      (louder side, dB above floor)
      //   similarity = sum(w_b * s_b) / sum(w_b), or 1 when every band is silent
      double weighted = 0.0, weightSum = 0.0;
      for (int b = 0; b < kBands; b++)
      {
         const float da = PowerToDb(mBandPow[0][b]);
         const float db = PowerToDb(mBandPow[1][b]);
         mBandDb[0][b].store(da, std::memory_order_relaxed);
         mBandDb[1][b].store(db, std::memory_order_relaxed);
         const float s = Clamp01(1.0f - std::fabs(db - da) / rangeDb);
         mOut[AudioCompareNode::kBand0 + b].store(s, std::memory_order_relaxed);
         mOut[AudioCompareNode::kBandDiff0 + b].store(Clamp01(0.5f + (db - da) / (2.0f * rangeDb)), std::memory_order_relaxed);
         const double w = (double)(std::max(da, db) - kFloorDb);
         weighted += w * (double)s;
         weightSum += w;
      }
      const float similarity = weightSum > 1.0e-3 ? (float)(weighted / weightSum) : 1.0f;
      mOut[AudioCompareNode::kSimilarity].store(Clamp01(similarity), std::memory_order_relaxed);

      mOut[AudioCompareNode::kPitchDiff].store(Clamp01(0.5f + (float)mCents / (2.0f * pitchRange)),
                                               std::memory_order_relaxed);
      mOut[AudioCompareNode::kOnsetMatch].store(Clamp01((float)mOnsetMatch), std::memory_order_relaxed);
      mOut[AudioCompareNode::kTiming].store(Clamp01(0.5f + (float)mLagMs / (2.0f * lagRange)),
                                            std::memory_order_relaxed);
   }

   enum LagStage { kLagSnap, kLagCorr, kLagPick, kLagRefine, kLagFinish };
   enum PitchStage { kPitchSnap, kPitchDiff, kPitchFinish };

   // Rate-derived (ConfigureRate).
   double mSampleRate = 48000.0;
   int mBinLen = 48;
   double mEnvRate = 1000.0;
   double mEnvG = 0.0;
   int mDecFactor = 3;
   double mDecRate = 16000.0;
   int mYinMinTau = 8;
   int mYinMaxTau = 267;
   int mRefineHalf = 25;

   // Followers (aligned signals).
   double mCoeff = 0.0;
   float mCoeffForMs = -1.0f;
   DspMath::Biquad mFilter[kInputs][kBands][kStages];
   double mBandPow[kInputs][kBands] = {};
   double mPow[kInputs] = {};
   double mCross = 0.0;

   // Alignment delay lines (sized in PrepareToPlay).
   std::vector<float> mDelay[kInputs];
   int mDelayMask = 0;
   int64_t mDelayWrite = 0;
   int mDelaySamples[kInputs] = {};
   int64_t mSampleCount = 0;

   // Envelope bins + onsets (raw signals).
   double mEnvMs[kInputs] = {};
   int mBinPhase = 0;
   int64_t mBinCount = 0;
   float mEnvRing[kInputs][kEnvRing] = {};
   float mEnvDb[kInputs][kOnsetDbHist] = {};
   int64_t mOnsets[kInputs][kOnsetRing] = {};
   FluxOnsetDetector mOnsetDet[kInputs];
   int mOnsetHead[kInputs] = {};
   int mOnsetFill[kInputs] = {};
   int64_t mLastOnset[kInputs] = {};
   double mOnsetMatch = 1.0;

   // Decimated copy (raw signals).
   DspMath::Biquad mDecFilter[kInputs][2];
   int mDecPhase = 0;
   int64_t mDecCount = 0;
   float mDecRing[kInputs][kDecRing] = {};

   // Lag job.
   int mLagStage = kLagSnap;
   int mLagIdx = 0;
   int mSnapN = 0;
   float mSnapA[kEnvAnalysis] = {};
   float mSnapB[kEnvAnalysis] = {};
   double mSnapSdA = 0.0, mSnapSdB = 0.0;
   bool mSnapCvOk = false;
   float mLagR[2 * kMaxLagBins + 1] = {};
   double mEnvLagSec = 0.0;
   int mRefCenter = 0;
   int64_t mRefT0 = 0;
   float mRefR[2 * kRefineMaxHalf + 1] = {};
   double mLagMs = 0.0;
   bool mLagLocked = false;
   bool mHasPending = false;
   double mPendingMs = 0.0;

   // Pitch job.
   int mPitchStage = kPitchSnap;
   int mPitchIn = 0;
   int mPitchTau = 1;
   float mPitchBuf[kInputs][kYinWindow + kYinMaxTauCap + 2] = {};
   float mYinD[kInputs][kYinMaxTauCap + 2] = {};
   bool mPitchLoud[kInputs] = {};
   int64_t mPitchLastSample = 0;
   double mCents = 0.0;
   double mUnvoicedSec = 0.0;

   std::atomic<float> mTimeMs;
   std::atomic<float> mRangeDb;
   std::atomic<float> mPitchRange;
   std::atomic<float> mLagRange;
   std::atomic<bool> mAlign;

   std::atomic<float> mOut[AudioCompareNode::kOutputCount];
   std::atomic<float> mBandDb[kInputs][kBands];
   std::atomic<float> mLevelDb[kInputs];
   std::atomic<float> mPitchHz[kInputs];
   std::atomic<float> mCorr;
   std::atomic<float> mLevelDiffDb;
   std::atomic<float> mCentsOut;
   std::atomic<float> mLagMsOut;
   std::atomic<bool> mLagLockedOut;
};

// ------------------------------------------------- main-thread half

AudioCompareNode::AudioCompareNode()
   : mAudioNode(std::make_unique<AudioCompareAudioNode>())
{
   for (int i = 0; i < kOutputCount; i++)
   {
      mTaps[i].owner = this;
      mTaps[i].index = i;
   }
}

AudioCompareNode::~AudioCompareNode() = default;

void AudioCompareNode::CookIfNeeded(int frameId)
{
   // No DSP here - just hand the params to the audio thread.
   if (frameId == mLastCookFrame)
      return;
   mLastCookFrame = frameId;
   mAudioNode->SetParams(timeMs, rangeDb, pitchRangeCents, lagRangeMs, align);
}

const char* AudioCompareNode::OutputLabel(int index) const
{
   if (index < 0 || index >= kOutputCount)
      return "out";
   return kOutputNames[index];
}

IModulator* AudioCompareNode::ModulatorOutput(int index)
{
   if (index < 0 || index >= kOutputCount)
      return nullptr;
   return &mTaps[index];
}

AudioNode* AudioCompareNode::AudioNodeForNotePorts()
{
   return mAudioNode.get();
}

float AudioCompareNode::Value(int index) const
{
   return mAudioNode->Output(index);
}

float AudioCompareNode::LevelDiffDb() const { return mAudioNode->LevelDiffDb(); }
float AudioCompareNode::Correlation() const { return mAudioNode->Correlation(); }
float AudioCompareNode::LevelDb(int input) const { return mAudioNode->LevelDb(input); }
float AudioCompareNode::BandDb(int input, int band) const { return mAudioNode->BandDb(input, band); }
float AudioCompareNode::PitchHz(int input) const { return mAudioNode->PitchHz(input); }
float AudioCompareNode::PitchDiffCents() const { return mAudioNode->PitchDiffCents(); }
float AudioCompareNode::LagMs() const { return mAudioNode->LagMs(); }
bool AudioCompareNode::LagLocked() const { return mAudioNode->LagLocked(); }
float AudioCompareNode::BandCentreHz(int band) { return BandCentre(band); }
float AudioCompareNode::SilenceFloorDb() { return kFloorDb; }
float AudioCompareNode::MaxLagMs() { return (float)kMaxLagBins; } // ~1 ms bins

// ------------------------------------------------- INFINITE_AUDIOCOMPARETEST

namespace
{
   struct NoObserver
   {
      void operator()() const {}
   };

   // Runs `seconds` of audio through `node`, block by block, starting at
   // `startSec` (so consecutive calls continue one timeline) and returns the
   // time reached. gen(t, a, b) writes input A's and B's sample at time t
   // (seconds); obs() runs after every block. passA/passB false leaves that
   // pin unconnected (a null input = silence).
   template <typename Gen, typename Obs = NoObserver>
   double DriveCompare(AudioCompareAudioNode& node, double sampleRate, int blockSize, double startSec,
                       double seconds, Gen gen, Obs obs = Obs(), bool passA = true, bool passB = true)
   {
      std::vector<float> aL(blockSize), aR(blockSize), bL(blockSize), bR(blockSize), oL(blockSize), oR(blockSize);
      float* aCh[2] = { aL.data(), aR.data() };
      float* bCh[2] = { bL.data(), bR.data() };
      float* oCh[2] = { oL.data(), oR.data() };
      AudioBuffer bufA, bufB, out;
      bufA.channels = aCh; bufA.numChannels = 2; bufA.numFrames = blockSize;
      bufB.channels = bCh; bufB.numChannels = 2; bufB.numFrames = blockSize;
      out.channels = oCh; out.numChannels = 2; out.numFrames = blockSize;
      const AudioBuffer* ins[2] = { passA ? &bufA : nullptr, passB ? &bufB : nullptr };

      const long long first = (long long)std::llround(startSec * sampleRate);
      const long long total = (long long)(seconds * sampleRate);
      long long frame = 0;
      while (frame < total)
      {
         for (int i = 0; i < blockSize; i++)
         {
            const double t = (double)(first + frame + i) / sampleRate;
            float a = 0.0f, b = 0.0f;
            gen(t, a, b);
            aL[i] = aR[i] = a;
            bL[i] = bR[i] = b;
         }
         node.ProcessBlock(ins, 2, out);
         obs();
         frame += blockSize;
      }
      return (double)(first + frame) / sampleRate;
   }

   // Which band a frequency's edges put it in.
   int BandOf(float hz)
   {
      const double r = std::pow((double)kHighHz / (double)kLowHz, 1.0 / (double)kBands);
      const int b = (int)std::floor(std::log((double)hz / (double)kLowHz) / std::log(r));
      return std::clamp(b, 0, kBands - 1);
   }

   float Broadband(double t)
   {
      const double twoPi = 2.0 * M_PI;
      return 0.2f * (float)(std::sin(twoPi * 100.0 * t) + std::sin(twoPi * 440.0 * t) +
                            std::sin(twoPi * 2500.0 * t) + std::sin(twoPi * 7000.0 * t));
   }

   // A harmonic tone (fundamental + 2 overtones) - something YIN can lock to.
   float Harmonic(double t, double f0)
   {
      const double w = 2.0 * M_PI * f0 * t;
      return 0.3f * (float)(std::sin(w) + 0.5 * std::sin(2.0 * w) + 0.25 * std::sin(3.0 * w));
   }

   // Deterministic white noise in [-1, 1], indexed by sample.
   float NoiseAt(long long n)
   {
      uint64_t z = (uint64_t)n * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      z ^= z >> 31;
      return (float)((double)(z >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0);
   }

   // Irregular train of noise "plucks" (fast attack, 60 ms decay) - an
   // envelope the lag search can lock to, a carrier that decorrelates the
   // moment it is shifted. 0 before t = 0, so a delayed copy starts silent.
   struct PluckTrain
   {
      std::vector<double> onsets;
      double sampleRate = 48000.0;

      PluckTrain(double sr, double seconds) : sampleRate(sr)
      {
         uint32_t s = 12345u;
         double t = 0.05;
         while (t < seconds)
         {
            onsets.push_back(t);
            s = s * 1664525u + 1013904223u;
            t += 0.15 + 0.17 * (double)(s >> 8) / 16777216.0; // 150..320 ms apart
         }
      }

      float At(double t) const
      {
         if (t < 0.0)
            return 0.0f;
         double env = 0.0;
         for (double o : onsets)
         {
            const double age = t - o;
            if (age >= 0.0 && age < 0.5)
               env += std::exp(-age / 0.06) * std::min(1.0, age / 0.001);
         }
         return 0.5f * (float)env * NoiseAt(std::llround(t * sampleRate));
      }
   };
}

bool AudioCompareNode::RunSelfTest()
{
   const double sr = 48000.0;
   const int block = 256;
   const double seconds = 3.0; // >= 20 time constants at the 150 ms default, and enough lag/pitch history
   const float range = 24.0f;
   bool ok = true;

   auto check = [&ok](bool cond, const char* what) {
      if (!cond)
      {
         ok = false;
         printf("AUDIOCOMPARETEST FAIL: %s\n", what);
      }
   };

   auto fresh = [&](std::unique_ptr<AudioCompareAudioNode>& n, float timeMs = 150.0f, float pitchRange = 100.0f,
                    float lagRange = 50.0f, bool align = true) {
      n = std::make_unique<AudioCompareAudioNode>();
      n->SetParams(timeMs, range, pitchRange, lagRange, align);
      n->PrepareToPlay(sr, block);
   };

   std::unique_ptr<AudioCompareAudioNode> n;

   // 1. Identical signals: alike, level equal, same pitch, aligned.
   fresh(n);
   DriveCompare(*n, sr, block, 0.0, seconds, [](double t, float& a, float& b) { a = b = Broadband(t); });
   printf("AUDIOCOMPARETEST identical: similarity %.3f correlation %.3f level diff %.3f pitch diff %.3f "
          "timing %.3f (lag %.2f ms)\n",
          n->Output(kSimilarity), n->Output(kCorrelation), n->Output(kLevelDiff), n->Output(kPitchDiff),
          n->Output(kTiming), n->LagMs());
   check(n->Output(kSimilarity) > 0.95f, "identical: similarity <= 0.95");
   check(n->Output(kCorrelation) > 0.95f, "identical: correlation output <= 0.95");
   check(std::fabs(n->Output(kLevelDiff) - 0.5f) < 0.01f, "identical: level diff not 0.5");
   check(std::fabs(n->Output(kPitchDiff) - 0.5f) < 0.01f, "identical: pitch diff not 0.5");
   check(std::fabs(n->Output(kTiming) - 0.5f) < 0.01f, "identical: timing not 0.5 (aligned)");

   // 1b. Identical pluck trains: onsets coincide, lag found at 0.
   {
      const PluckTrain train(sr, 5.0);
      fresh(n);
      DriveCompare(*n, sr, block, 0.0, 4.0, [&train](double t, float& a, float& b) { a = b = train.At(t); });
      printf("AUDIOCOMPARETEST identical plucks: onset match %.3f timing %.3f (lag %.2f ms, locked %d) "
             "similarity %.3f\n",
             n->Output(kOnsetMatch), n->Output(kTiming), n->LagMs(), n->LagLocked() ? 1 : 0, n->Output(kSimilarity));
      check(n->Output(kOnsetMatch) > 0.95f, "identical plucks: onset match <= 0.95");
      check(n->LagLocked() && std::fabs(n->LagMs()) < 0.5f, "identical plucks: lag not locked at 0");
   }

   // 2. B = -A: same spectrum, inverted waveform.
   fresh(n);
   DriveCompare(*n, sr, block, 0.0, seconds, [](double t, float& a, float& b) { a = Broadband(t); b = -a; });
   float minBand = 1.0f;
   for (int b = 0; b < kBands; b++)
      minBand = std::min(minBand, n->Output(kBand0 + b));
   printf("AUDIOCOMPARETEST inverted: similarity %.3f correlation %.3f min band %.3f\n",
          n->Output(kSimilarity), n->Output(kCorrelation), minBand);
   check(n->Output(kCorrelation) < 0.05f, "inverted: correlation output >= 0.05");
   check(n->Output(kSimilarity) > 0.95f, "inverted: similarity <= 0.95 (should ignore polarity)");
   check(minBand > 0.95f, "inverted: a band similarity <= 0.95");

   // 3. A = 100 Hz, B = 5 kHz: different spectra.
   fresh(n);
   DriveCompare(*n, sr, block, 0.0, seconds, [](double t, float& a, float& b) {
      a = 0.5f * (float)std::sin(2.0 * M_PI * 100.0 * t);
      b = 0.5f * (float)std::sin(2.0 * M_PI * 5000.0 * t);
   });
   const int bandLo = BandOf(100.0f);
   const int bandHi = BandOf(5000.0f);
   printf("AUDIOCOMPARETEST tones: similarity %.3f correlation %.3f band %d (100 Hz) A %.1f dB B %.1f dB sim %.3f, "
          "band %d (5 kHz) A %.1f dB B %.1f dB sim %.3f\n",
          n->Output(kSimilarity), n->Output(kCorrelation),
          bandLo + 1, n->BandDb(0, bandLo), n->BandDb(1, bandLo), n->Output(kBand0 + bandLo),
          bandHi + 1, n->BandDb(0, bandHi), n->BandDb(1, bandHi), n->Output(kBand0 + bandHi));
   check(bandLo != bandHi, "tones: 100 Hz and 5 kHz landed in the same band");
   check(n->Output(kSimilarity) < 0.3f, "tones: similarity >= 0.3");
   check(n->Output(kBand0 + bandLo) < 0.2f, "tones: 100 Hz band similarity >= 0.2");
   check(n->Output(kBand0 + bandHi) < 0.2f, "tones: 5 kHz band similarity >= 0.2");
   check(n->BandDb(0, bandLo) - n->BandDb(1, bandLo) > 20.0f, "tones: A not > 20 dB above B in the 100 Hz band");
   check(n->BandDb(1, bandHi) - n->BandDb(0, bandHi) > 20.0f, "tones: B not > 20 dB above A in the 5 kHz band");
   check(std::fabs(n->Output(kCorrelation) - 0.5f) < 0.1f, "tones: correlation output not near 0.5 (unrelated)");

   // 4. B louder by 6 dB: level diff above 0.5 by exactly 6 / (2 * range).
   fresh(n);
   const float gain6 = DspMath::DbToLinear(6.0f);
   DriveCompare(*n, sr, block, 0.0, seconds, [gain6](double t, float& a, float& b) {
      a = Broadband(t);
      b = gain6 * a;
   });
   const float expected = 0.5f + 6.0f / (2.0f * range);
   printf("AUDIOCOMPARETEST B louder: level diff %.4f (expected %.4f) %+.2f dB, correlation %.3f, similarity %.3f\n",
          n->Output(kLevelDiff), expected, n->LevelDiffDb(), n->Output(kCorrelation), n->Output(kSimilarity));
   check(std::fabs(n->Output(kLevelDiff) - expected) < 0.01f, "B louder: level diff off the expected 0.5 + 6/(2*range)");
   check(std::fabs(n->LevelDiffDb() - 6.0f) < 0.25f, "B louder: level diff dB not ~+6 (B - A)");
   check(n->Output(kCorrelation) > 0.95f, "B louder: correlation output <= 0.95 (gain only)");

   // 5. B a semitone above A: pitch diff ~ +100 cents, mapped through pitch range.
   {
      const float pitchRange = 200.0f;
      fresh(n, 150.0f, pitchRange);
      const double f0 = 220.0;
      const double f1 = 220.0 * std::pow(2.0, 1.0 / 12.0);
      DriveCompare(*n, sr, block, 0.0, seconds, [f0, f1](double t, float& a, float& b) {
         a = Harmonic(t, f0);
         b = Harmonic(t, f1);
      });
      const float expectedOut = 0.5f + 100.0f / (2.0f * pitchRange);
      printf("AUDIOCOMPARETEST semitone: A %.2f Hz B %.2f Hz, %+.1f cents, pitch diff %.3f (expected %.3f)\n",
             n->PitchHz(0), n->PitchHz(1), n->PitchDiffCents(), n->Output(kPitchDiff), expectedOut);
      check(std::fabs(n->PitchHz(0) - (float)f0) < 2.0f, "semitone: A's f0 not ~220 Hz");
      check(std::fabs(n->PitchDiffCents() - 100.0f) < 10.0f, "semitone: pitch diff not ~+100 cents");
      check(std::fabs(n->Output(kPitchDiff) - expectedOut) < 0.03f, "semitone: pitch diff output off 0.5 + 100/(2*range)");
   }

   // 6. B = A delayed 20 ms. Timing reports it whether or not align is on;
   //    with align on, the waveform lines up again (correlation) and the
   //    level/band followers stop seeing the offset (similarity).
   {
      const PluckTrain train(sr, 7.0);
      const double delay = 0.020;
      const float lagRange = 50.0f;
      auto gen = [&train, delay](double t, float& a, float& b) {
         a = train.At(t);
         b = train.At(t - delay);
      };
      double simSum[2] = {}, corrSum[2] = {};
      float lag[2] = {}, timing[2] = {};
      for (int pass = 0; pass < 2; pass++)
      {
         const bool alignOn = pass == 0;
         fresh(n, 40.0f, 100.0f, lagRange, alignOn);
         const double t = DriveCompare(*n, sr, block, 0.0, 4.0, gen);
         int count = 0;
         AudioCompareAudioNode* node = n.get();
         DriveCompare(*n, sr, block, t, 1.5, gen, [&, node]() {
            simSum[pass] += node->Output(kSimilarity);
            corrSum[pass] += node->Output(kCorrelation);
            count++;
         });
         simSum[pass] /= std::max(1, count);
         corrSum[pass] /= std::max(1, count);
         lag[pass] = n->LagMs();
         timing[pass] = n->Output(kTiming);
      }
      const float expectedTiming = 0.5f + 20.0f / (2.0f * lagRange);
      printf("AUDIOCOMPARETEST delayed 20 ms: align on lag %.2f ms timing %.3f (expected %.3f) mean similarity %.3f "
             "mean correlation %.3f | align off lag %.2f ms timing %.3f mean similarity %.3f mean correlation %.3f\n",
             lag[0], timing[0], expectedTiming, simSum[0], corrSum[0], lag[1], timing[1], simSum[1], corrSum[1]);
      check(std::fabs(lag[0] - 20.0f) < 1.0f, "delayed: lag not ~20 ms (align on)");
      check(std::fabs(lag[1] - 20.0f) < 1.0f, "delayed: lag not ~20 ms (align off - timing must still measure)");
      check(std::fabs(timing[0] - expectedTiming) < 0.02f, "delayed: timing output off 0.5 + 20/(2*lag range)");
      check(simSum[0] > 0.95f, "delayed: aligned similarity <= 0.95");
      check(corrSum[0] > 0.9f, "delayed: aligned correlation output <= 0.9");
      check(corrSum[1] < 0.7f, "delayed: unaligned correlation output >= 0.7 (noise shifted 20 ms should be unrelated)");
      check(simSum[0] > simSum[1], "delayed: alignment did not raise similarity");
   }

   // 7. Nothing patched: finite, and two silences read as identical.
   fresh(n);
   DriveCompare(*n, sr, block, 0.0, 0.5, [](double, float& a, float& b) { a = b = 0.0f; }, NoObserver(), false, false);
   bool finite = true;
   for (int i = 0; i < kOutputCount; i++)
      finite = finite && std::isfinite(n->Output(i)) && n->Output(i) >= 0.0f && n->Output(i) <= 1.0f;
   printf("AUDIOCOMPARETEST unpatched: similarity %.3f level diff %.3f correlation %.3f pitch diff %.3f "
          "onset match %.3f timing %.3f\n",
          n->Output(kSimilarity), n->Output(kLevelDiff), n->Output(kCorrelation), n->Output(kPitchDiff),
          n->Output(kOnsetMatch), n->Output(kTiming));
   check(finite, "unpatched: an output is non-finite or outside 0..1");
   check(n->Output(kSimilarity) == 1.0f && n->Output(kLevelDiff) == 0.5f, "unpatched: not identical silence");
   check(n->Output(kPitchDiff) == 0.5f && n->Output(kTiming) == 0.5f && n->Output(kOnsetMatch) == 1.0f,
         "unpatched: pitch/timing/onset not at their 'same' values");
   check(n->Output(kBandDiff0) == 0.5f && n->Output(kBandDiff0 + 7) == 0.5f, "unpatched: band diffs not at 0.5");

   printf("AUDIOCOMPARETEST %s\n", ok ? "OK" : "FAIL");
   fflush(stdout);
   return ok;
}
