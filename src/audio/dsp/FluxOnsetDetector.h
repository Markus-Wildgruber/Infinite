#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "PortableFft.h"

// Streaming spectral-flux onset detector, shared by the live Audio Compare
// node and the offline analysis (AudioAnalysis::Analyze) so that the onset
// match the node outputs and the one a measurement reports count the same
// onsets. They used to disagree badly: the node wanted an 8 dB jump of the
// whole-signal level within 10 ms, which overlapping, ringing notes (a
// handpan, a piano with pedal) rarely make, while the measurement used
// spectral flux and found them.
//
// Recipe: frames of 1024 samples (512 below 30 kHz), hop half that, Hann
// window; flux = sum over bins of the rise in log(1 + 100 |X|). A frame is an
// onset when it is the largest within +-3 frames, clears the mean of the
// +-8 frames around it by 0.08 of the running peak flux, and is at least
// 50 ms after the previous onset. Causal: the running peak decays (halves in
// about 3 s) instead of being the whole file's maximum, and a frame is
// decided 8 frames (~85 ms at 48 kHz) after it arrives. Onset times are frame
// centres, in samples since Reset().
//
// Prepare() allocates (main thread); Reset/Push/Flush never allocate, so the
// audio thread may call them.
class FluxOnsetDetector
{
public:
   static constexpr int kHalf = 8;  // frames each side for the local mean
   static constexpr int kPeak = 3;  // frames each side for the local maximum
   static constexpr int kHist = 32; // > 2 * kHalf + 1

   void Prepare(double sampleRate)
   {
      mRate = sampleRate > 0.0 ? sampleRate : 48000.0;
      mLog2 = mRate > 30000.0 ? 10 : 9;
      mN = 1 << mLog2;
      mHop = mN / 2;
      mFft.Prepare(mLog2);
      mWindow.resize((size_t)mN);
      const double kTwoPi = 6.283185307179586;
      for (int i = 0; i < mN; i++)
         mWindow[(size_t)i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / mN));
      mIn.assign((size_t)mN, 0.0f);
      mFrame.assign((size_t)mN, 0.0f);
      mRe.assign((size_t)mN / 2, 0.0f);
      mIm.assign((size_t)mN / 2, 0.0f);
      mPrev.assign((size_t)mN / 2 + 1, 0.0f);
      mDecay = std::pow(0.5, (double)mHop / (mRate * 3.0));
      // A floor for the running peak, about the flux of a -60 dBFS tone
      // starting, so dither and room noise never count as onsets.
      mPeakFloor = 10.0 * (double)mN / 1024.0;
      mPrepared = true;
      Reset();
   }

   bool Prepared() const { return mPrepared; }

   // Latency of a decision, in samples: an onset is reported this long after
   // its own time at the latest.
   int LatencySamples() const { return kHalf * mHop + mN / 2; }

   void Reset()
   {
      if (!mPrepared)
         return;
      std::fill(mIn.begin(), mIn.end(), 0.0f);
      std::fill(mPrev.begin(), mPrev.end(), 0.0f);
      mFill = mN / 2; // frame 0 is centred on sample 0: half a window of silence before it
      mFrames = 0;
      mPeakFlux = 0.0;
      mLastOnset = -1.0e18;
      for (int i = 0; i < kHist; i++)
         mFlux[i] = 0.0;
   }

   // Feeds samples; calls onOnset(double sampleTime) for each onset decided.
   template <typename F>
   void Push(const float* x, int count, F&& onOnset)
   {
      if (!mPrepared)
         return;
      for (int i = 0; i < count; i++)
      {
         mIn[(size_t)mFill++] = x[i];
         if (mFill == mN)
         {
            Frame(onOnset);
            std::copy(mIn.begin() + mHop, mIn.end(), mIn.begin());
            mFill = mN - mHop;
         }
      }
   }

   template <typename F>
   void Push(float x, F&& onOnset) { Push(&x, 1, onOnset); }

   // End of a finite signal: pushes enough silence to decide its last frames.
   template <typename F>
   void Flush(F&& onOnset)
   {
      const float zero = 0.0f;
      for (int i = 0; i < LatencySamples() + mHop; i++)
         Push(&zero, 1, onOnset);
   }

private:
   template <typename F>
   void Frame(F& onOnset)
   {
      for (int i = 0; i < mN; i++)
         mFrame[(size_t)i] = mIn[(size_t)i] * mWindow[(size_t)i];
      mFft.Forward(mFrame.data(), mLog2, mRe.data(), mIm.data());
      // Forward() is x2-scaled; halve to the plain DFT magnitude.
      double sum = 0.0;
      for (int k = 1; k <= mN / 2; k++)
      {
         const float re = k < mN / 2 ? mRe[(size_t)k] : mIm[0];
         const float im = k < mN / 2 ? mIm[(size_t)k] : 0.0f;
         const float mag = 0.5f * std::sqrt(re * re + im * im);
         const float v = std::log1p(100.0f * mag);
         sum += std::max(0.0f, v - mPrev[(size_t)k]);
         mPrev[(size_t)k] = v;
      }
      mFlux[mFrames % kHist] = sum;
      mPeakFlux = std::max(sum, std::max(mPeakFloor, mPeakFlux * mDecay));
      const long long newest = mFrames++;
      const long long c = newest - kHalf;
      if (c < 1)
         return;

      const double norm = 1.0 / mPeakFlux;
      const double v = mFlux[c % kHist] * norm;
      double mean = 0.0;
      int n = 0;
      for (long long j = c - kHalf; j <= c + kHalf; j++)
         if (j >= 0)
         {
            mean += mFlux[j % kHist] * norm;
            n++;
         }
      mean /= std::max(1, n);
      if (!(v > mean + 0.08))
         return;
      for (long long j = c - kPeak; j <= c + kPeak; j++)
         if (j >= 0 && j != c && mFlux[j % kHist] > mFlux[c % kHist])
            return;
      const double t = (double)c * mHop;
      if (t - mLastOnset < 0.05 * mRate)
         return;
      mLastOnset = t;
      onOnset(t);
   }

   PortableFft::RealFft mFft;
   std::vector<float> mWindow, mIn, mFrame, mRe, mIm, mPrev;
   double mFlux[kHist] = {};
   double mRate = 48000.0;
   double mDecay = 1.0;
   double mPeakFloor = 10.0;
   double mPeakFlux = 0.0;
   double mLastOnset = -1.0e18;
   long long mFrames = 0;
   int mLog2 = 10, mN = 1024, mHop = 512, mFill = 512;
   bool mPrepared = false;
};
