#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Streaming band-limited sample-rate converter for interleaved float audio,
// fed block by block (Render to file converts the take's rate to the rate the
// user asked for without ever holding the whole take).
//
// Windowed sinc: h(x) = fc * sinc(fc * x) * kaiser(x / halfWidth), with
//   fc        = 0.945 * min(1, outRate / inRate)   (cutoff, as a fraction of
//               the input Nyquist - just under the lower of the two Nyquists,
//               so downsampling folds nothing back and the passband is flat
//               to ~20.8 kHz at 44.1 kHz),
//   halfWidth = 32 zero crossings / fc input samples,
//   Kaiser beta 9 (side lobes ~ -90 dB).
// The kernel is tabulated at kOversample points per input sample and read
// with linear interpolation; the fractional phase is the same for every tap of
// one output sample, so one output costs 2 * halfWidth multiply-adds per
// channel and no transcendental calls. fc in front of the sinc makes the taps
// sum to ~1, so DC passes at unity gain.
//
// Output sample n sits at input position n * inRate / outRate (both streams
// start at time 0). Input before the first sample and after Finish() is
// zero. Pure arithmetic, so two runs over the same input match bit for bit.
// Same family as the one-shot converter in audio/dsp/BasicPitch.cpp, made
// incremental.
class StreamResampler
{
public:
   static constexpr int kZeroCrossings = 32;
   static constexpr int kOversample = 1024;

   void Prepare(double inRate, double outRate, int channels)
   {
      mChannels = std::max(1, channels);
      mStep = (inRate > 0.0 && outRate > 0.0) ? inRate / outRate : 1.0;
      mFc = 0.945 * std::min(1.0, 1.0 / mStep);
      mHalfWidth = (double)kZeroCrossings / mFc;
      mTableHalf = (int)std::ceil(mHalfWidth * kOversample);
      mTable.assign((size_t)(2 * mTableHalf + 2 * kOversample + 4), 0.0f);
      const double kPi = 3.14159265358979323846;
      const double beta = 9.0;
      const double i0Beta = BesselI0(beta);
      for (int i = 0; i <= 2 * mTableHalf; i++)
      {
         const double x = (double)(i - mTableHalf) / kOversample;
         const double r = x / mHalfWidth;
         if (std::fabs(r) >= 1.0)
            continue;
         const double a = kPi * mFc * x;
         const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
         const double win = BesselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta;
         mTable[(size_t)i] = (float)(mFc * sinc * win);
      }
      mBuf.assign((size_t)mChannels, std::vector<float>());
      mBufStart = 0;
      mInTotal = 0;
      mNextOut = 0;
   }

   // Input frames needed past an output sample's position before it can be
   // computed - render this many extra frames at the end of a take so the
   // last outputs see real audio instead of the zero padding.
   int Lookahead() const { return (int)std::ceil(mHalfWidth) + 2; }
   long long OutputFrames() const { return mNextOut; }

   // Appends `frames` interleaved input frames and appends every output frame
   // that is now computable (interleaved) to `out`, never more than
   // `maxOutFrames` in total (< 0 = no cap).
   void Push(const float* in, int frames, std::vector<float>& out, long long maxOutFrames = -1)
   {
      for (int c = 0; c < mChannels; c++)
      {
         std::vector<float>& b = mBuf[(size_t)c];
         const size_t base = b.size();
         b.resize(base + (size_t)frames);
         for (int i = 0; i < frames; i++)
            b[base + (size_t)i] = in[(size_t)i * (size_t)mChannels + (size_t)c];
      }
      mInTotal += frames;
      Produce(out, maxOutFrames, false);
   }

   // End of input: emits outputs up to `maxOutFrames` in total, reading zeros
   // past the last input frame.
   void Finish(std::vector<float>& out, long long maxOutFrames)
   {
      Produce(out, maxOutFrames, true);
   }

private:
   static double BesselI0(double x)
   {
      double sum = 1.0, term = 1.0;
      const double q = x * x * 0.25;
      for (int k = 1; k < 64; k++)
      {
         term *= q / ((double)k * (double)k);
         sum += term;
         if (term < sum * 1e-17)
            break;
      }
      return sum;
   }

   void Produce(std::vector<float>& out, long long maxOutFrames, bool final)
   {
      double acc[8];
      const int nch = std::min(mChannels, 8);
      for (;;)
      {
         if (maxOutFrames >= 0 && mNextOut >= maxOutFrames)
            break;
         const double t = (double)mNextOut * mStep;
         const long long first = (long long)std::ceil(t - mHalfWidth);
         const long long last = (long long)std::floor(t + mHalfWidth);
         if (!final && last >= mInTotal)
            break; // needs input that has not arrived yet
         if (final && maxOutFrames < 0 && first >= mInTotal)
            break; // nothing left that any input reaches
         for (int c = 0; c < nch; c++)
            acc[c] = 0.0;
         const long long kStart = std::max<long long>(first, std::max<long long>(0, mBufStart));
         const long long kEnd = std::min(last, mInTotal - 1);
         if (kStart <= kEnd)
         {
            // Table position of tap kStart; each next tap is one input sample
            // later, i.e. kOversample table points earlier. The fraction is
            // shared by every tap.
            // Table index i holds h((i - mTableHalf) / kOversample).
            const double pos0 = (t - (double)kStart) * kOversample + (double)mTableHalf;
            long long ip = (long long)std::floor(pos0);
            const double f = pos0 - (double)ip;
            const size_t off = (size_t)(kStart - mBufStart);
            const int taps = (int)(kEnd - kStart + 1);
            for (int j = 0; j < taps; j++, ip -= kOversample)
            {
               if (ip < 0 || ip + 1 >= (long long)mTable.size())
                  continue;
               const double h = (double)mTable[(size_t)ip] * (1.0 - f) + (double)mTable[(size_t)ip + 1] * f;
               for (int c = 0; c < nch; c++)
                  acc[c] += h * (double)mBuf[(size_t)c][off + (size_t)j];
            }
         }
         for (int c = 0; c < mChannels; c++)
            out.push_back(c < nch ? (float)acc[c] : 0.0f);
         mNextOut++;
      }
      // Drop input no future output reaches.
      const long long keepFrom = (long long)std::floor((double)mNextOut * mStep - mHalfWidth) - 1;
      if (keepFrom > mBufStart)
      {
         const long long drop = std::min<long long>(keepFrom - mBufStart, (long long)mBuf[0].size());
         if (drop > 0)
         {
            for (std::vector<float>& b : mBuf)
               b.erase(b.begin(), b.begin() + (std::ptrdiff_t)drop);
            mBufStart += drop;
         }
      }
   }

   int mChannels = 2;
   double mStep = 1.0;      // input samples per output sample
   double mFc = 0.945;      // cutoff, fraction of the input Nyquist
   double mHalfWidth = 32.0; // in input samples
   int mTableHalf = 0;
   std::vector<float> mTable;
   std::vector<std::vector<float>> mBuf; // per channel, input frames from mBufStart on
   long long mBufStart = 0;  // absolute input index of mBuf[c][0]
   long long mInTotal = 0;   // input frames pushed so far
   long long mNextOut = 0;   // absolute index of the next output frame
};
