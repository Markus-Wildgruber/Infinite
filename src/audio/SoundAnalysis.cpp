#include "SoundAnalysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>

#include "AudioAnalysis.h" // NoteName

namespace SoundAnalysis
{
   namespace
   {
      using json = nlohmann::json;
      using cfloat = std::complex<float>;
      const double kPi = 3.14159265358979323846;

      double R1(double v) { return std::round(v * 10.0) / 10.0; }
      double R2(double v) { return std::round(v * 100.0) / 100.0; }
      double R3(double v) { return std::round(v * 1000.0) / 1000.0; }

      size_t NextPow2(size_t n)
      {
         size_t p = 1;
         while (p < n)
            p <<= 1;
         return p;
      }

      float Median(std::vector<float> v)
      {
         if (v.empty())
            return 0.0f;
         const size_t mid = v.size() / 2;
         std::nth_element(v.begin(), v.begin() + (long)mid, v.end());
         return v[mid];
      }

      float AmpToDb(double a) { return (float)(20.0 * std::log10(std::max(a, 1e-9))); }

      // Radix-2 complex FFT with a precomputed twiddle table, so one plan
      // serves every frame of an STFT.
      class Fft
      {
      public:
         explicit Fft(size_t n) : mN(n), mTw(n / 2), mRev(n)
         {
            for (size_t k = 0; k < n / 2; k++)
            {
               const double a = 2.0 * kPi * (double)k / (double)n;
               mTw[k] = cfloat((float)std::cos(a), (float)-std::sin(a));
            }
            int bits = 0;
            while (((size_t)1 << bits) < n)
               bits++;
            for (size_t i = 0; i < n; i++)
            {
               size_t r = 0;
               for (int b = 0; b < bits; b++)
                  if ((i >> b) & 1)
                     r |= (size_t)1 << (bits - 1 - b);
               mRev[i] = r;
            }
         }

         size_t Size() const { return mN; }

         void Forward(cfloat* a) const
         {
            for (size_t i = 0; i < mN; i++)
               if (i < mRev[i])
                  std::swap(a[i], a[mRev[i]]);
            for (size_t len = 2; len <= mN; len <<= 1)
            {
               const size_t half = len / 2, step = mN / len;
               for (size_t i = 0; i < mN; i += len)
                  for (size_t k = 0; k < half; k++)
                  {
                     const cfloat v = a[i + k + half] * mTw[k * step];
                     a[i + k + half] = a[i + k] - v;
                     a[i + k] += v;
                  }
            }
         }

         // Scaled by 1/n.
         void Inverse(cfloat* a) const
         {
            for (size_t i = 0; i < mN; i++)
               a[i] = std::conj(a[i]);
            Forward(a);
            const float s = 1.0f / (float)mN;
            for (size_t i = 0; i < mN; i++)
               a[i] = std::conj(a[i]) * s;
         }

      private:
         size_t mN;
         std::vector<cfloat> mTw;
         std::vector<size_t> mRev;
      };

      struct Peak
      {
         double bin;
         float db;
      };

      // Parabolic interpolation of a log-magnitude peak at bin b.
      Peak Interp(const std::vector<float>& db, size_t b)
      {
         Peak p{ (double)b, db[b] };
         if (b == 0 || b + 1 >= db.size())
            return p;
         const double a = db[b - 1], c = db[b], d = db[b + 1];
         const double den = a - 2.0 * c + d;
         if (den < -1e-9)
         {
            const double delta = std::clamp(0.5 * (a - d) / den, -0.5, 0.5);
            p.bin = (double)b + delta;
            p.db = (float)(c - 0.25 * (a - d) * delta);
         }
         return p;
      }

      // YIN on x[start .. start + W + tauMax). Period in samples, 0 when
      // nothing periodic was found; `clarity` = 1 - d'(period).
      double YinPeriod(const std::vector<float>& x, size_t start, int W, int tauMin, int tauMax, const Fft& fft,
                       std::vector<cfloat>& fa, std::vector<cfloat>& fb, float& clarity)
      {
         clarity = 0.0f;
         const size_t m = fft.Size();
         const int span = W + tauMax;
         fa.assign(m, cfloat(0.0f, 0.0f));
         fb.assign(m, cfloat(0.0f, 0.0f));
         for (int i = 0; i < W; i++)
            fa[(size_t)i] = cfloat(x[start + (size_t)i], 0.0f);
         for (int i = 0; i < span; i++)
            fb[(size_t)i] = cfloat(x[start + (size_t)i], 0.0f);
         fft.Forward(fa.data());
         fft.Forward(fb.data());
         for (size_t i = 0; i < m; i++)
            fa[i] = std::conj(fa[i]) * fb[i];
         fft.Inverse(fa.data()); // fa[tau] = sum_j x_j x_{j+tau}

         std::vector<double> prefix((size_t)span + 1, 0.0);
         for (int i = 0; i < span; i++)
         {
            const double v = x[start + (size_t)i];
            prefix[(size_t)i + 1] = prefix[(size_t)i] + v * v;
         }
         const double e0 = prefix[(size_t)W];
         if (e0 < 1e-9)
            return 0.0;

         std::vector<float> d((size_t)tauMax + 1, 1.0f);
         double running = 0.0;
         for (int tau = 1; tau <= tauMax; tau++)
         {
            const double et = prefix[(size_t)(tau + W)] - prefix[(size_t)tau];
            const double diff = std::max(0.0, e0 + et - 2.0 * (double)fa[(size_t)tau].real());
            running += diff;
            d[(size_t)tau] = running > 0.0 ? (float)(diff * tau / running) : 1.0f;
         }

         int best = -1;
         for (int tau = tauMin; tau < tauMax; tau++)
            if (d[(size_t)tau] < 0.2f)
            {
               while (tau + 1 < tauMax && d[(size_t)tau + 1] < d[(size_t)tau])
                  tau++;
               best = tau;
               break;
            }
         if (best < 0)
         {
            // No dip under the absolute threshold: take the global minimum if it is still clearly periodic.
            int g = tauMin;
            for (int tau = tauMin; tau < tauMax; tau++)
               if (d[(size_t)tau] < d[(size_t)g])
                  g = tau;
            if (d[(size_t)g] > 0.35f)
               return 0.0;
            best = g;
         }
         double period = best;
         if (best > 1 && best < tauMax)
         {
            const double a = d[(size_t)best - 1], b = d[(size_t)best], c = d[(size_t)best + 1];
            const double den = a - 2.0 * b + c;
            if (std::fabs(den) > 1e-12)
               period = best + std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
         }
         clarity = std::clamp(1.0f - d[(size_t)best], 0.0f, 1.0f);
         return period;
      }

      double BandEdge(int i)
      {
         return (double)kBandLowHz * std::pow((double)kBandHighHz / kBandLowHz, (double)i / kEnvBands);
      }

      const double kLn1000 = 6.907755278982137; // -60 dB in nepers

      struct TransientResult
      {
         bool measured = false;
         float level = 0.0f, decayMs = 0.0f;         // full band
         float highLevel = 0.0f, highDecayMs = 0.0f; // through the 12 dB/oct highpass
      };

      // In-place 2-pole Butterworth highpass (RBJ cookbook biquad, Q = 1/sqrt 2).
      void HighpassInPlace(std::vector<float>& s, double sr, double fc)
      {
         const double w0 = 2.0 * kPi * std::clamp(fc, 10.0, 0.45 * sr) / sr;
         const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
         const double c = std::cos(w0), a0 = 1.0 + alpha;
         const double b0 = (1.0 + c) * 0.5 / a0, b1 = -(1.0 + c) / a0, b2 = b0;
         const double a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
         double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
         for (float& v : s)
         {
            const double in = v;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = (float)out;
         }
      }

      // The attack residual of the peak-normalised region `x`: the sinusoids
      // at `freqs` (Hz) are fitted and taken out of the stretch from 2 ms
      // after `onset` (a guard against an onset estimate a sample early) to
      // ~150 ms after it, and what is left above its own later level is the
      // burst. See the header for the method. With `highpassHz` > 0 the burst
      // is measured a second time through a 12 dB/oct highpass there.
      TransientResult MeasureTransient(const std::vector<float>& x, double sr, size_t onset, const std::vector<double>& freqs,
                                       double f0, double highpassHz)
      {
         TransientResult r;
         const size_t len = x.size();
         const size_t S = freqs.size();
         if (S == 0 || !(f0 > 0.0) || !(sr > 0.0))
            return r;
         const size_t guard = (size_t)std::lround(0.002 * sr);
         const size_t a0 = onset + guard;
         // Frames of 2.5 periods (8..50 ms): long enough that neighbouring
         // harmonics are separated by the window (coupling ~3 %), short
         // enough to follow the attack. Hop half a frame.
         const size_t T = std::clamp<size_t>((size_t)std::lround(std::max(2.5 / f0, 0.008) * sr), 64,
                                             std::max<size_t>(64, (size_t)std::lround(0.05 * sr)));
         const size_t hopF = std::max<size_t>(1, T / 2);
         const size_t ms = std::max<size_t>(1, (size_t)std::lround(0.001 * sr));
         if (a0 >= len)
            return r;
         const size_t span = std::min(len - a0, (size_t)std::lround(0.15 * sr));
         if (span < T + hopF)
            return r;
         const size_t nFr = 1 + (span - T) / hopF;
         // Samples [a0, a0 + useEnd) are modelled: up to the last frame centre
         // (holding the last amplitudes beyond it would read a decay as residual).
         const size_t useEnd = (nFr - 1) * hopF + T / 2;
         const size_t nb = useEnd / ms;
         if (nFr < 2 || nb < 20)
            return r;

         std::vector<float> w(T);
         double sumw = 0.0;
         for (size_t n = 0; n < T; n++)
         {
            w[n] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)T));
            sumw += w[n];
         }
         std::vector<double> omega(S);
         for (size_t k = 0; k < S; k++)
            omega[k] = 2.0 * kPi * freqs[k] / sr;
         auto phasorAt = [&](size_t k, size_t sample)
         { return std::polar(1.0, std::fmod(omega[k] * (double)sample, 2.0 * kPi)); };

         // Complex amplitude c of each sinusoid per frame, x ~ Re(c e^{j w n})
         // with n the absolute sample index, so frames are comparable.
         std::vector<std::complex<double>> C(nFr * S, std::complex<double>(0.0, 0.0));
         std::vector<float> res(T);
         std::vector<cfloat> ph(S * T);
         for (size_t j = 0; j < nFr; j++)
         {
            const size_t s = a0 + j * hopF;
            for (size_t n = 0; n < T; n++)
               res[n] = x[s + n];
            for (size_t k = 0; k < S; k++)
            {
               std::complex<double> z = phasorAt(k, s);
               const std::complex<double> step = std::polar(1.0, omega[k]);
               for (size_t n = 0; n < T; n++)
               {
                  ph[k * T + n] = cfloat((float)z.real(), (float)z.imag());
                  z *= step;
               }
            }
            // Gauss-Seidel on the windowed least-squares problem: project the
            // residual on each sinusoid in turn, three sweeps.
            for (int sweep = 0; sweep < 3; sweep++)
               for (size_t k = 0; k < S; k++)
               {
                  const cfloat* e = &ph[k * T];
                  double re = 0.0, im = 0.0;
                  for (size_t n = 0; n < T; n++)
                  {
                     const double v = (double)w[n] * res[n];
                     re += v * e[n].real();
                     im -= v * e[n].imag();
                  }
                  const double dr = 2.0 * re / sumw, di = 2.0 * im / sumw;
                  C[j * S + k] += std::complex<double>(dr, di);
                  for (size_t n = 0; n < T; n++)
                     res[n] -= (float)(dr * e[n].real() - di * e[n].imag());
               }
         }

         // The partials' model over [a0, a0 + useEnd): amplitudes linear
         // between frame centres, extrapolated from the first two before the
         // first centre (a decay is near-linear over half a frame).
         std::vector<float> model(useEnd, 0.0f);
         for (size_t k = 0; k < S; k++)
         {
            std::complex<double> z = phasorAt(k, a0);
            const std::complex<double> step = std::polar(1.0, omega[k]);
            for (size_t i = 0; i < useEnd; i++)
            {
               const double tpos = ((double)i - 0.5 * (double)T) / (double)hopF;
               const size_t jj = (size_t)std::clamp(std::floor(tpos), 0.0, (double)(nFr - 2));
               const double fr = tpos - (double)jj;
               const std::complex<double> c = C[jj * S + k] + (C[(jj + 1) * S + k] - C[jj * S + k]) * fr;
               model[i] += (float)(c.real() * z.real() - c.imag() * z.imag());
               z *= step;
            }
         }

         // Harmonic level as the RMS over one period, in 1 ms steps (a 1 ms
         // window on a low note would read the waveform, not its level).
         std::vector<double> m2(useEnd + 1, 0.0);
         for (size_t i = 0; i < useEnd; i++)
            m2[i + 1] = m2[i] + (double)model[i] * model[i];
         const size_t P = std::max(ms, (size_t)std::lround(sr / f0));
         double hPeak2 = 0.0;
         for (size_t b = 0; b < nb; b++)
         {
            const size_t e = std::min(useEnd, b * ms + P);
            if (e > b * ms)
               hPeak2 = std::max(hPeak2, (m2[e] - m2[b * ms]) / (double)(e - b * ms));
         }
         r.measured = true;
         if (hPeak2 <= 1e-12)
            return r;

         std::vector<float> resid(useEnd);
         for (size_t i = 0; i < useEnd; i++)
            resid[i] = x[a0 + i] - model[i];

         // Level (re the harmonic RMS peak) and decay of the burst in a residual.
         auto burst = [&](const std::vector<float>& rs, float& levelOut, float& decayOut)
         {
            levelOut = 0.0f;
            decayOut = 0.0f;
            std::vector<double> rb2(nb, 0.0); // 1 ms mean squares
            for (size_t b = 0; b < nb; b++)
            {
               double acc = 0.0;
               for (size_t i = b * ms; i < (b + 1) * ms; i++)
                  acc += (double)rs[i] * rs[i];
               rb2[b] = acc / (double)ms;
            }
            // What the residual settles to (sustained noise / unmodelled
            // partials) is not the burst: measured after the first 100 ms (or
            // 2/3 of what there is) and subtracted.
            const size_t Sb = std::max<size_t>(1, std::min<size_t>(100, (nb * 2) / 3));
            const size_t bgEnd = std::min(nb, Sb + 50);
            double bg2 = 0.0;
            if (bgEnd >= Sb + 10)
            {
               for (size_t b = Sb; b < bgEnd; b++)
                  bg2 += rb2[b];
               bg2 /= (double)(bgEnd - Sb);
            }
            double E = 0.0, Et = 0.0;
            for (size_t b = 0; b < Sb; b++)
            {
               const double e = std::max(0.0, rb2[b] - bg2) * (double)ms;
               E += e;
               Et += ((double)b + 0.5) * (double)ms * e;
            }
            if (E <= 1e-12 * hPeak2 * (double)ms)
               return;

            // Decay rate (per sample) from the energy's mean time: for
            // exp(-2 a t) on [0, Sw) it is 1/(2a) - Sw q / (1 - q),
            // q = exp(-2 a Sw), falling in a; solved by bisection in log a
            // over 1..200 ms to -60 dB.
            const double Sw = (double)(Sb * ms);
            const double tbar = Et / E;
            auto meanTime = [Sw](double a)
            {
               const double q = std::exp(-2.0 * a * Sw);
               return 1.0 / (2.0 * a) - Sw * q / std::max(1e-300, 1.0 - q);
            };
            const double aMin = kLn1000 / (0.2 * sr), aMax = kLn1000 / (0.001 * sr);
            double a;
            if (tbar >= meanTime(aMin))
               a = aMin;
            else if (tbar <= meanTime(aMax))
               a = aMax;
            else
            {
               double lo = std::log(aMin), hi = std::log(aMax);
               for (int it = 0; it < 50; it++)
               {
                  const double mid = 0.5 * (lo + hi);
                  if (meanTime(std::exp(mid)) > tbar)
                     lo = mid;
                  else
                     hi = mid;
               }
               a = std::exp(0.5 * (lo + hi));
            }
            decayOut = (float)(kLn1000 / a / sr * 1000.0);
            // Level at the start of the measured stretch from its energy,
            // then back to the onset along the same decay (at most +12 dB:
            // the guard hides anything shorter).
            const double I = (1.0 - std::exp(-2.0 * a * Sw)) / (2.0 * a);
            const double rg = std::sqrt(E / I);
            const double r0 = rg * std::min(4.0, std::exp(a * (double)guard));
            levelOut = (float)std::clamp(r0 / std::sqrt(hPeak2), 0.0, 1.0);
         };

         burst(resid, r.level, r.decayMs);
         if (highpassHz > 0.0)
         {
            HighpassInPlace(resid, sr, highpassHz);
            burst(resid, r.highLevel, r.highDecayMs);
         }
         return r;
      }
   }

   float BandCentreHz(int band)
   {
      band = std::clamp(band, 0, kEnvBands - 1);
      return (float)std::sqrt(BandEdge(band) * BandEdge(band + 1));
   }

   SoundProfile AnalyzeSound(const float* mono, size_t frames, double sr, const Options& opt)
   {
      SoundProfile p;
      p.sampleRate = sr;
      const int K = std::clamp(opt.partials, 1, kMaxPartials);
      p.partials.resize((size_t)K);
      for (int k = 1; k <= K; k++)
         p.partials[(size_t)k - 1].harmonic = k;
      for (int b = 0; b < kEnvBands; b++)
      {
         p.bandHz[b] = BandCentreHz(b);
         p.bandDb[b] = -90.0f;
      }
      if (mono == nullptr || frames == 0 || !(sr > 0.0))
      {
         p.error = "no audio";
         return p;
      }

      // ---- region ----------------------------------------------------------
      const size_t minRegion = (size_t)std::ceil(0.02 * sr);
      const size_t s0 = (size_t)std::clamp(std::llround(std::max(0.0, opt.startSeconds) * sr), 0LL, (long long)frames);
      if (s0 + minRegion > frames)
      {
         p.error = "the region starts past the end of the audio";
         return p;
      }
      size_t len = frames - s0;
      if (opt.lengthSeconds > 0.0)
         len = std::min(len, (size_t)std::llround(opt.lengthSeconds * sr));
      len = std::min(len, (size_t)(kMaxSeconds * sr));
      if (len < minRegion)
      {
         p.error = "the region is shorter than 20 ms";
         return p;
      }
      p.startSeconds = (double)s0 / sr;
      p.seconds = (double)len / sr;

      std::vector<float> x(mono + s0, mono + s0 + len);
      float peak = 0.0f;
      for (float& v : x)
      {
         if (!std::isfinite(v))
            v = 0.0f;
         peak = std::max(peak, std::fabs(v));
      }
      if (peak < 1e-6f)
      {
         p.error = "the region is silent";
         return p;
      }
      p.peakDb = AmpToDb(peak);
      // Everything below runs on a peak-normalised copy, so thresholds are relative.
      for (float& v : x)
         v /= peak;

      // ---- RMS envelope (1 ms hop) -------------------------------------------
      // Trailing RMS over one period of f0 (exact to the sample, so a harmonic
      // tone reads ripple-free). RMS rather than peak: the waveform peak of an
      // inharmonic tone swings by several dB as its partials drift in phase,
      // its power does not.
      const int hop = std::max(1, (int)std::lround(0.001 * sr));
      const double hopSec = (double)hop / sr;
      const size_t nEnv = (len + (size_t)hop - 1) / (size_t)hop;
      std::vector<double> sq(len + 1, 0.0);
      for (size_t i = 0; i < len; i++)
         sq[i + 1] = sq[i] + (double)x[i] * x[i];
      size_t envWin = 1;
      auto rmsEnvelope = [&](double windowSec)
      {
         const size_t wn = std::clamp<size_t>((size_t)std::lround(windowSec * sr), 1, len);
         envWin = wn;
         std::vector<float> env(nEnv);
         for (size_t i = 0; i < nEnv; i++)
         {
            const size_t e = std::min(len, (i + 1) * (size_t)hop);
            const size_t a = e >= wn ? e - wn : 0; // before the region counts as silence
            env[i] = (float)std::sqrt(std::max(0.0, (sq[e] - sq[a]) / (double)wn));
         }
         return env;
      };
      // First index within 2 % of the maximum: on a plateau the literal argmax
      // could land anywhere.
      auto peakIndex = [](const std::vector<float>& env)
      {
         const float m = *std::max_element(env.begin(), env.end());
         for (size_t i = 0; i < env.size(); i++)
            if (env[i] >= 0.98f * m)
               return i;
         return (size_t)0;
      };
      // Centre of the window that envelope point `idx` measured.
      auto peakSampleOf = [&](size_t idx)
      {
         const size_t e = std::min(len, (idx + 1) * (size_t)hop);
         return std::min(len - 1, e > envWin / 2 ? e - envWin / 2 : (size_t)0);
      };

      std::vector<float> env = rmsEnvelope(0.005);
      size_t pk = peakIndex(env);
      size_t pkSample = peakSampleOf(pk);

      // ---- coarse f0 (YIN over the body) -------------------------------------
      double coarseF0 = 0.0;
      float clarity = 0.0f;
      if (opt.f0HintHz > 0.0)
      {
         coarseF0 = std::clamp(opt.f0HintHz, 20.0, 0.45 * sr);
      }
      else
      {
         int W = (int)std::lround(0.05 * sr);
         const int tauMin = std::max(2, (int)(sr / 5000.0));
         int tauMax = std::min(W - 1, (int)(sr / 35.0));
         if ((size_t)(W + tauMax) >= len)
         {
            W = (int)(len / 2);
            tauMax = std::min(tauMax, W - 1);
         }
         if (tauMax > tauMin + 2 && W > 16)
         {
            const Fft fft(NextPow2((size_t)(2 * W + tauMax)));
            const size_t span = (size_t)(W + tauMax);
            const size_t step = std::max<size_t>(1, (size_t)std::lround(0.02 * sr));
            const size_t a0 = std::min(pkSample, len - span);
            const size_t a1 = std::min(len - span, a0 + (size_t)sr);
            std::vector<size_t> starts;
            std::vector<double> energies;
            double maxE = 0.0;
            for (size_t a = a0; a <= a1; a += step)
            {
               double e = 0.0;
               for (int i = 0; i < W; i++)
                  e += (double)x[a + (size_t)i] * x[a + (size_t)i];
               starts.push_back(a);
               energies.push_back(e);
               maxE = std::max(maxE, e);
            }
            std::vector<float> periods, clar;
            std::vector<cfloat> fa, fb;
            for (size_t i = 0; i < starts.size(); i++)
            {
               if (energies[i] < 0.01 * maxE)
                  continue;
               float c = 0.0f;
               const double per = YinPeriod(x, starts[i], W, tauMin, tauMax, fft, fa, fb, c);
               if (per > 0.0)
               {
                  periods.push_back((float)per);
                  clar.push_back(c);
               }
            }
            if (!periods.empty())
            {
               coarseF0 = sr / (double)Median(periods);
               clarity = Median(clar);
            }
         }
      }
      p.f0Clarity = clarity;
      const bool tryPitched = coarseF0 > 0.0 && (opt.f0HintHz > 0.0 || clarity >= 0.5f);

      if (tryPitched)
      {
         env = rmsEnvelope(std::clamp(1.0 / coarseF0, 0.002, 0.03));
         pk = peakIndex(env);
         pkSample = peakSampleOf(pk);
      }
      {
         const float m = std::max(1e-9f, *std::max_element(env.begin(), env.end()));
         for (float& v : env)
            v /= m;
      }
      p.peakTime = (float)((double)pkSample / sr);

      // ---- onset and attack crest --------------------------------------------
      // The onset is where the envelope last rose through 10 % of the way from
      // its pre-peak minimum to the peak: on silence that is the 10 % point,
      // after a decaying earlier sound (a previous note's tail) the start of
      // the rise out of it.
      size_t onsetSample = 0;
      {
         float lowest = 1.0f;
         for (size_t i = 0; i <= pk && i < env.size(); i++)
            lowest = std::min(lowest, env[i]);
         const float thr = lowest + 0.1f * (1.0f - lowest);
         size_t i = std::min(pk, env.size() - 1);
         while (i > 0 && env[i - 1] >= thr)
            i--;
         onsetSample = std::min(pkSample, i * (size_t)hop + (size_t)hop / 2);
         p.onsetTime = (float)((double)onsetSample / sr);
         const size_t e = std::min(len, onsetSample + (size_t)std::lround(0.08 * sr));
         if (e > onsetSample + 16)
         {
            double mx = 0.0, ss = 0.0;
            for (size_t k = onsetSample; k < e; k++)
            {
               mx = std::max(mx, (double)std::fabs(x[k]));
               ss += (double)x[k] * x[k];
            }
            const double rms = std::sqrt(ss / (double)(e - onsetSample));
            if (rms > 1e-9)
               p.attackCrestDb = (float)(20.0 * std::log10(mx / rms));
         }
      }

      // ---- long spectrum of the body -----------------------------------------
      const size_t maxL = std::min<size_t>(32768, (size_t)std::lround(0.75 * sr));
      const size_t minL = std::min(len, std::max<size_t>(2048, (size_t)(0.05 * sr)));
      size_t bodyStart = pkSample;
      if (len - bodyStart < minL)
         bodyStart = len - minL;
      const size_t L = std::max<size_t>(16, std::min(maxL, len - bodyStart));
      const size_t N = NextPow2(L) * 2; // x2 zero padding: finer peak picking
      const size_t nb = N / 2 + 1;
      const double binHz = sr / (double)N;
      std::vector<float> P(nb), Pdb(nb);
      {
         const Fft fftN(N);
         std::vector<cfloat> buf(N, cfloat(0.0f, 0.0f));
         for (size_t i = 0; i < L && bodyStart + i < len; i++)
         {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)L);
            buf[i] = cfloat((float)(x[bodyStart + i] * w), 0.0f);
         }
         fftN.Forward(buf.data());
         // A sinusoid of amplitude A peaks at |X| = A L / 4 under a Hann window.
         const double ampScale = 4.0 / (double)L;
         for (size_t k = 0; k < nb; k++)
         {
            const double mag = std::abs(buf[k]) * ampScale;
            P[k] = (float)(mag * mag);
            Pdb[k] = AmpToDb(mag);
         }
      }
      const double topHz = std::min(0.5 * sr, 20000.0);
      const size_t kLo = std::max<size_t>(1, (size_t)std::ceil(20.0 / binHz));
      const size_t kHi = std::min(nb - 2, (size_t)std::floor(topHz / binHz));
      float floorDb = -200.0f;
      if (kHi > kLo)
         floorDb = Median(std::vector<float>(Pdb.begin() + (long)kLo, Pdb.begin() + (long)kHi + 1));

      // ---- partials, harmonic by harmonic ------------------------------------
      std::vector<float> bodyDb((size_t)K, -200.0f);
      if (tryPitched)
      {
         double fLocal = coarseF0; // f_j / j of the last partial found
         for (int k = 1; k <= K; k++)
         {
            const double expected = k * fLocal;
            if (expected > 0.45 * sr)
               break;
            const double halfW = std::max(k == 1 ? (opt.f0HintHz > 0.0 ? 0.06 : 0.03) * expected : 0.12 * fLocal, 3.0 * binHz);
            const long b0 = std::max(1L, (long)std::floor((expected - halfW) / binHz));
            const long b1 = std::min((long)nb - 2, (long)std::ceil((expected + halfW) / binHz));
            if (b1 - b0 < 2)
               continue;
            long best = b0;
            for (long b = b0; b <= b1; b++)
               if (Pdb[(size_t)b] > Pdb[(size_t)best])
                  best = b;
            // An edge maximum is the skirt of a neighbouring peak, not a partial.
            const bool interior = best > b0 && best < b1;
            const Peak pkk = Interp(Pdb, (size_t)best);
            Partial& pa = p.partials[(size_t)k - 1];
            pa.freqHz = (float)(pkk.bin * binHz);
            bodyDb[(size_t)k - 1] = pkk.db;
            pa.present = interior && pkk.db > floorDb + 12.0f;
            if (pa.present)
               fLocal = pa.freqHz / k;
         }
         float loudest = -200.0f;
         for (int k = 0; k < K; k++)
            if (p.partials[(size_t)k].present)
               loudest = std::max(loudest, bodyDb[(size_t)k]);
         for (int k = 0; k < K; k++)
            if (p.partials[(size_t)k].present && bodyDb[(size_t)k] < loudest - 60.0f)
               p.partials[(size_t)k].present = false;
      }

      // ---- f0 and inharmonicity from the partials ----------------------------
      int nPresent = 0;
      double F = coarseF0, B = 0.0;
      {
         double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, sMean = 0.0;
         for (const Partial& q : p.partials)
         {
            if (!q.present)
               continue;
            const double k = q.harmonic;
            const double w = std::pow(10.0, bodyDb[(size_t)q.harmonic - 1] / 20.0);
            const double xv = k * k;
            const double yv = (q.freqHz / k) * (q.freqHz / k);
            sw += w;
            sx += w * xv;
            sy += w * yv;
            sxx += w * xv * xv;
            sxy += w * xv * yv;
            sMean += w * (q.freqHz / k);
            nPresent++;
         }
         bool fitted = false;
         if (nPresent >= 3)
         {
            const double den = sw * sxx - sx * sx;
            if (den > 0.0)
            {
               const double slope = (sw * sxy - sx * sy) / den;
               const double icpt = (sy - slope * sx) / sw;
               if (icpt > 0.0)
               {
                  const double b = slope / icpt;
                  if (b >= 0.0 && b <= 0.05)
                  {
                     B = b;
                     F = std::sqrt(icpt);
                     fitted = true;
                  }
               }
            }
         }
         if (!fitted && nPresent > 0)
         {
            B = 0.0;
            F = sMean / sw;
         }
      }
      p.pitched = nPresent > 0;
      if (p.pitched)
      {
         p.inharmonicity = (float)B;
         p.f0Hz = (float)(F * std::sqrt(1.0 + B));
      }
      else
      {
         p.f0Clarity = opt.f0HintHz > 0.0 ? 0.0f : p.f0Clarity;
         for (Partial& q : p.partials)
            q.freqHz = 0.0f;
      }

      // ---- ADSR from the RMS envelope ----------------------------------------
      // Where a held note's release starts (seconds from region start): the
      // partial decay fits below stop there, so a plateau followed by a
      // note-off does not read as a slow decay.
      double releaseStartSec = 1e30;
      {
         const size_t n = nEnv;
         const double ms = hopSec * 1000.0;
         // 10 ms centred moving average for the post-peak analysis.
         const size_t half = std::max<size_t>(1, (size_t)std::lround(0.005 / hopSec));
         std::vector<double> pre(n + 1, 0.0);
         for (size_t i = 0; i < n; i++)
            pre[i + 1] = pre[i] + env[i];
         std::vector<float> es(n);
         for (size_t i = 0; i < n; i++)
         {
            const size_t a = i >= half ? i - half : 0;
            const size_t b = std::min(n - 1, i + half);
            es[i] = (float)((pre[b + 1] - pre[a]) / (double)(b + 1 - a));
         }
         size_t endIdx = pk;
         for (size_t i = n; i-- > pk;)
            if (env[i] > 0.001f)
            {
               endIdx = i;
               break;
            }

         size_t i10 = 0;
         while (i10 < pk && env[i10] < 0.1f)
            i10++;
         size_t i90 = i10;
         while (i90 < pk && env[i90] < 0.9f)
            i90++;
         p.attackMs = (float)std::max(0.5, (double)(i90 - i10) * ms / 0.8);

         const size_t span = endIdx > pk ? endIdx - pk : 0;
         float S = 0.0f;
         bool sustained = false;
         if ((double)span * hopSec >= 0.1)
         {
            auto medianRange = [&](double f0, double f1)
            {
               const size_t a = pk + (size_t)(f0 * (double)span);
               const size_t b = std::max(a + 1, pk + (size_t)(f1 * (double)span));
               return Median(std::vector<float>(es.begin() + (long)a, es.begin() + (long)std::min(b, n)));
            };
            const float m1 = medianRange(0.2, 0.4), m2 = medianRange(0.4, 0.6);
            if (m1 > 0.1f && m2 > 0.1f && std::fabs(20.0 * std::log10(m2 / m1)) < 1.5)
            {
               sustained = true;
               S = std::clamp(0.5f * (m1 + m2), 0.0f, 1.0f);
            }
         }
         p.sustained = sustained;
         p.sustain = S;

         // Decay: to 90 % of the way from the peak down to the plateau.
         const float target = S + 0.1f * (1.0f - S);
         size_t id = 0;
         bool reached = false;
         for (size_t i = pk + 1; i <= endIdx && i < n; i++)
            if (es[i] <= target)
            {
               id = i;
               reached = true;
               break;
            }
         if (reached)
            p.decayMs = (float)((double)(id - pk) * ms / 0.9);
         else
         {
            // Never got there inside the region: extrapolate the dB slope.
            double sT = 0.0, sL = 0.0, sTT = 0.0, sTL = 0.0;
            int cnt = 0;
            for (size_t i = pk; i <= endIdx && i < n; i++)
            {
               if (es[i] <= 0.001f)
                  continue;
               const double t = (double)(i - pk) * hopSec;
               const double l = 20.0 * std::log10(es[i]);
               sT += t;
               sL += l;
               sTT += t * t;
               sTL += t * l;
               cnt++;
            }
            const double den = cnt * sTT - sT * sT;
            const double slope = (cnt >= 2 && den > 0.0) ? (cnt * sTL - sT * sL) / den : 0.0;
            if (slope < -0.1)
               p.decayMs = (float)((-20.0 * std::log10(std::max(target, 1e-4f))) / -slope * 1000.0 / 0.9);
            else
               p.decayMs = (float)((double)span * ms);
         }

         if (sustained)
         {
            const bool endsInside = endIdx + (size_t)std::ceil(0.01 / hopSec) < n - 1;
            if (endsInside)
            {
               size_t r0 = pk;
               for (size_t i = pk; i <= endIdx; i++)
                  if (es[i] >= 0.9f * S)
                     r0 = i;
               size_t r1 = endIdx;
               for (size_t i = r0 + 1; i <= endIdx; i++)
                  if (es[i] <= 0.1f * S)
                  {
                     r1 = i;
                     break;
                  }
               p.releaseMs = (float)((double)(r1 - r0) * ms / 0.8);
               p.releaseMeasured = true;
               releaseStartSec = (double)r0 * hopSec;
            }
            else
               p.releaseMs = 250.0f;
         }
         else
            p.releaseMs = p.decayMs; // a decaying sound keeps ringing the same way after note-off

         p.attackMs = std::clamp(p.attackMs, 0.5f, 20000.0f);
         p.decayMs = std::clamp(p.decayMs, 1.0f, 20000.0f);
         p.releaseMs = std::clamp(p.releaseMs, 1.0f, 20000.0f);

         const size_t points = std::min<size_t>(kEnvelopePoints, n);
         p.envelopeDb.resize(points);
         for (size_t i = 0; i < points; i++)
         {
            const size_t a = i * n / points, b = std::max(a + 1, (i + 1) * n / points);
            float m = 0.0f;
            for (size_t j = a; j < b && j < n; j++)
               m = std::max(m, env[j]);
            p.envelopeDb[i] = std::max(-90.0f, AmpToDb(m));
         }
         p.envHopSeconds = (float)((double)n * hopSec / (double)points);
      }

      // ---- partial amplitude tracks: onset level and decay -------------------
      if (p.pitched)
      {
         size_t M = std::clamp<size_t>(NextPow2((size_t)std::ceil(4.0 * sr / p.f0Hz)), 512, 16384);
         while (M > len && M > 256)
            M >>= 1;
         const size_t hopS = std::max<size_t>(M / 4, len / 400);
         const size_t nFr = len >= M ? 1 + (len - M) / hopS : 1;
         const Fft fftM(M);
         std::vector<float> win(M);
         double sumw = 0.0;
         for (size_t i = 0; i < M; i++)
         {
            win[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)M));
            sumw += win[i];
         }
         const double scale = 2.0 / sumw; // peak |X| -> sinusoid amplitude
         const double binM = sr / (double)M;
         std::vector<cfloat> fb(M);
         std::vector<std::vector<float>> track((size_t)K, std::vector<float>(nFr, -200.0f));
         std::vector<float> frameFloor(nFr, -200.0f), frameTime(nFr, 0.0f), mags(M / 2 + 1), tmp;
         for (size_t j = 0; j < nFr; j++)
         {
            const size_t a = j * hopS;
            for (size_t i = 0; i < M; i++)
               fb[i] = cfloat(a + i < len ? x[a + i] * win[i] : 0.0f, 0.0f);
            fftM.Forward(fb.data());
            for (size_t b = 0; b <= M / 2; b++)
               mags[b] = AmpToDb(std::abs(fb[b]) * scale);
            tmp.assign(mags.begin() + 1, mags.end());
            frameFloor[j] = Median(tmp);
            frameTime[j] = (float)(((double)a + 0.5 * (double)M) / sr);
            for (int k = 0; k < K; k++)
            {
               const Partial& q = p.partials[(size_t)k];
               if (!q.present)
                  continue;
               const long c = (long)std::lround(q.freqHz / binM);
               long best = -1;
               for (long b = std::max(1L, c - 1); b <= std::min((long)M / 2 - 1, c + 1); b++)
                  if (best < 0 || mags[(size_t)b] > mags[(size_t)best])
                     best = b;
               if (best > 0)
                  track[(size_t)k][j] = Interp(mags, (size_t)best).db;
            }
         }

         // First frame that starts at or after the envelope peak: everything
         // from there on is past the attack.
         const size_t jRef = std::min(nFr - 1, (pkSample + hopS - 1) / hopS);
         const double tRef = (double)pkSample / sr;
         std::vector<float> onsetDb((size_t)K, -200.0f);
         float strongest = -200.0f;
         for (int k = 0; k < K; k++)
         {
            Partial& q = p.partials[(size_t)k];
            if (!q.present)
               continue;
            const std::vector<float>& tr = track[(size_t)k];
            const float trMax = *std::max_element(tr.begin(), tr.end());
            // First frame within 1 dB of the track's maximum: near the onset
            // for a decaying partial, anywhere being equally good on a plateau.
            size_t jm = 0;
            while (jm + 1 < nFr && tr[jm] < trMax - 1.0f)
               jm++;
            const size_t js = std::min(nFr - 1, std::max(jm, jRef));
            std::vector<double> ts, ls;
            for (size_t j = js; j < nFr; j++)
            {
               if (tr[j] < trMax - 35.0f || tr[j] < frameFloor[j] + 6.0f)
                  break;
               if (j > js && (double)frameTime[j] + 0.5 * (double)M / sr > releaseStartSec)
                  break; // this frame already reaches into the release
               ts.push_back(frameTime[j]);
               ls.push_back(tr[j]);
            }
            double slope = 0.0, icpt = tr[js];
            if (ts.size() >= 2)
            {
               const double mt = std::accumulate(ts.begin(), ts.end(), 0.0) / (double)ts.size();
               const double ml = std::accumulate(ls.begin(), ls.end(), 0.0) / (double)ls.size();
               double num = 0.0, den = 0.0;
               for (size_t i = 0; i < ts.size(); i++)
               {
                  num += (ts[i] - mt) * (ls[i] - ml);
                  den += (ts[i] - mt) * (ts[i] - mt);
               }
               slope = den > 0.0 ? num / den : 0.0;
               icpt = ml - slope * mt;
               if (slope > -1.0)
               {
                  q.sustains = true;
                  q.decaySec = 0.0f;
               }
               else
                  q.decaySec = (float)std::clamp(-60.0 / slope, 0.005, 120.0);
            }
            // Level at the onset for a partial that starts with the note (the
            // fitted line extrapolated back to the envelope peak); at its own
            // later maximum for one that swells in afterwards.
            const double tEval = ((double)frameTime[js] - tRef <= 0.15 + (double)M / sr) ? tRef : (double)frameTime[js];
            double level = ts.size() >= 2 ? icpt + slope * tEval : (double)tr[js];
            level = std::min(level, (double)trMax + 12.0);
            onsetDb[(size_t)k] = (float)level;
            strongest = std::max(strongest, (float)level);
         }
         double harmonicPower = 0.0;
         for (int k = 0; k < K; k++)
         {
            Partial& q = p.partials[(size_t)k];
            if (!q.present)
               continue;
            q.ampDb = onsetDb[(size_t)k] - strongest;
            q.amp = (float)std::pow(10.0, q.ampDb / 20.0);
            harmonicPower += 0.5 * std::pow(10.0, onsetDb[(size_t)k] / 10.0); // A^2 / 2
         }
         // x was divided by the region peak: add it back for dBFS.
         if (harmonicPower > 0.0)
            p.levelDb = (float)(10.0 * std::log10(harmonicPower)) + p.peakDb;
      }

      // ---- sinusoids to take out of the attack: the partials, plus the
      // harmonics above the measured ones that show as peaks in the body ----
      std::vector<double> sinFreqs;
      if (p.pitched)
      {
         float loudest = -200.0f;
         for (const Partial& q : p.partials)
            if (q.present)
            {
               sinFreqs.push_back(q.freqHz);
               loudest = std::max(loudest, bodyDb[(size_t)q.harmonic - 1]);
            }
         const double topSin = std::min(0.45 * sr, 12000.0);
         for (int k = K + 1; k <= 64; k++)
         {
            const double expected = k * F * std::sqrt(1.0 + B * k * k);
            if (expected > topSin)
               break;
            const double halfW = std::max(0.12 * F, 3.0 * binHz);
            const long b0 = std::max(1L, (long)std::floor((expected - halfW) / binHz));
            const long b1 = std::min((long)nb - 2, (long)std::ceil((expected + halfW) / binHz));
            if (b1 - b0 < 2)
               continue;
            long best = b0;
            for (long b = b0; b <= b1; b++)
               if (Pdb[(size_t)b] > Pdb[(size_t)best])
                  best = b;
            const Peak pkk = Interp(Pdb, (size_t)best);
            if (best > b0 && best < b1 && pkk.db > floorDb + 12.0f && pkk.db > loudest - 60.0f)
               sinFreqs.push_back(pkk.bin * binHz);
         }
      }

      // ---- spectral envelope, centroid, rolloff ------------------------------
      {
         float raw[kEnvBands];
         for (int b = 0; b < kEnvBands; b++)
         {
            const double lo = BandEdge(b), hi = BandEdge(b + 1), fc = p.bandHz[b];
            float measured = -200.0f;
            if (fc < 0.5 * sr)
            {
               const size_t k0 = (size_t)std::ceil(lo / binHz);
               const size_t k1 = std::min(nb - 1, (size_t)std::floor(hi / binHz));
               if (k1 >= k0 && k0 < nb)
               {
                  for (size_t k = k0; k <= k1; k++)
                     measured = std::max(measured, Pdb[k]);
               }
               else
               {
                  const double pos = fc / binHz;
                  const size_t a = std::min(nb - 2, (size_t)pos);
                  const double t = pos - (double)a;
                  measured = (float)(Pdb[a] + (Pdb[a + 1] - Pdb[a]) * t);
               }
            }
            float harmonic = -200.0f;
            if (p.pitched && fc < 0.5 * sr)
            {
               // Peak envelope through the partials, linear in log frequency,
               // so the bands between harmonics do not read as holes.
               const Partial* prev = nullptr;
               for (const Partial& q : p.partials)
               {
                  if (!q.present)
                     continue;
                  const float qDb = bodyDb[(size_t)q.harmonic - 1];
                  if (prev == nullptr && fc < q.freqHz)
                  {
                     harmonic = qDb - 12.0f * (float)std::log2(q.freqHz / fc);
                     break;
                  }
                  if (prev != nullptr && fc <= q.freqHz)
                  {
                     const float pDb = bodyDb[(size_t)prev->harmonic - 1];
                     const double t = std::log(fc / prev->freqHz) / std::log((double)q.freqHz / prev->freqHz);
                     harmonic = (float)(pDb + (qDb - pDb) * t);
                     break;
                  }
                  prev = &q;
               }
            }
            raw[b] = std::max(measured, harmonic);
         }
         float smooth[kEnvBands];
         float top = -200.0f;
         for (int b = 0; b < kEnvBands; b++)
         {
            const float l = raw[std::max(0, b - 1)], c = raw[b], r = raw[std::min(kEnvBands - 1, b + 1)];
            smooth[b] = 0.25f * l + 0.5f * c + 0.25f * r;
            if (p.bandHz[b] >= 0.5 * sr)
               smooth[b] = -200.0f;
            top = std::max(top, smooth[b]);
         }
         for (int b = 0; b < kEnvBands; b++)
            p.bandDb[b] = std::max(-90.0f, smooth[b] - top);

         double tot = 0.0, num = 0.0;
         for (size_t k = kLo; k <= kHi; k++)
         {
            tot += P[k];
            num += (double)k * binHz * P[k];
         }
         if (tot > 0.0)
         {
            p.centroidHz = (float)(num / tot);
            double cum = 0.0;
            p.rolloffHz = (float)(kHi * binHz);
            for (size_t k = kLo; k <= kHi; k++)
            {
               cum += P[k];
               if (cum >= 0.85 * tot)
               {
                  p.rolloffHz = (float)(k * binHz);
                  break;
               }
            }
         }
         p.brightness = p.centroidHz > 0.0f
                           ? std::clamp((float)(std::log(p.centroidHz / 100.0) / std::log(100.0)), 0.0f, 1.0f)
                           : 0.0f;

         // ---- noise-to-harmonic ratio ----
         if (p.pitched && tot > 0.0)
         {
            std::vector<char> used(nb, 0);
            double harm = 0.0;
            const double lobeHz = 1.5 * 2.0 * sr / (double)L; // 1.5 x Hann main-lobe half width
            const double topH = std::min(0.45 * sr, 20000.0);
            for (int k = 1; k <= 512; k++)
            {
               double fp = k * F * std::sqrt(1.0 + B * k * k);
               if (fp > topH)
                  break;
               if (k <= K && p.partials[(size_t)k - 1].present)
                  fp = p.partials[(size_t)k - 1].freqHz;
               else
               {
                  // Unmeasured harmonic: snap to the local maximum near the prediction.
                  const double sw = std::max(3.0 * binHz, 0.01 * fp);
                  const size_t a = std::max(kLo, (size_t)std::floor((fp - sw) / binHz));
                  const size_t b = std::min(kHi, (size_t)std::ceil((fp + sw) / binHz));
                  size_t best = a;
                  for (size_t i = a; i <= b; i++)
                     if (Pdb[i] > Pdb[best])
                        best = i;
                  if (b >= a)
                     fp = (double)best * binHz;
               }
               const long a = std::max((long)kLo, (long)std::floor((fp - lobeHz) / binHz));
               const long b = std::min((long)kHi, (long)std::ceil((fp + lobeHz) / binHz));
               for (long i = a; i <= b; i++)
                  if (!used[(size_t)i])
                  {
                     used[(size_t)i] = 1;
                     harm += P[(size_t)i];
                  }
            }
            const double noise = std::max(tot - harm, tot * 1e-7);
            p.noiseToHarmonicDb = (float)std::clamp(10.0 * std::log10(noise / std::max(harm, 1e-20)), -60.0, 60.0);
            p.noisiness = (float)std::clamp(noise / tot, 0.0, 1.0);
         }
         else
         {
            p.noiseToHarmonicDb = 60.0f;
            p.noisiness = 1.0f;
         }
      }

      // ---- air: sustained residual from 1 kHz up -----------------------------
      if (kHi > kLo)
      {
         // Sum of P over a set of bins -> signal power. A sinusoid of
         // amplitude A peaks at P = A^2 and its bins sum to A^2 * 3N / (2L),
         // so its power A^2 / 2 is that sum * L / (3N); white noise of
         // variance s^2 has E[P] = 6 s^2 / L per bin over N / 2 bins, so the
         // same factor gives s^2 back (Hann: sum w^2 = 3L/8, sum w = L/2).
         const double toPow = (double)L / (3.0 * (double)N);
         std::vector<char> mask(nb, 0);
         if (p.pitched)
         {
            // Main lobe of every harmonic up to the top, widened for a fast
            // decay (its skirt) and, where only predicted, for inharmonicity.
            const double lobeHz = 1.5 * 2.0 * sr / (double)L;
            for (int k = 1; k <= 4096; k++)
            {
               const Partial* q = (k <= K && p.partials[(size_t)k - 1].present) ? &p.partials[(size_t)k - 1] : nullptr;
               double fp = q != nullptr ? (double)q->freqHz : k * F * std::sqrt(1.0 + B * k * k);
               if (fp > topHz)
                  break;
               double hw = std::max(lobeHz, 3.0 * binHz);
               if (q != nullptr && q->decaySec > 0.0f)
                  hw = std::max(hw, 10.0 / q->decaySec);
               if (q == nullptr)
               {
                  // A measured peak above the partials (sinFreqs) beats the prediction.
                  for (double f : sinFreqs)
                     if (std::fabs(f - fp) < 0.12 * F)
                     {
                        fp = f;
                        break;
                     }
                  hw = std::max(hw, 0.005 * fp);
               }
               hw = std::min(hw, 0.35 * F);
               const long a = std::max((long)kLo, (long)std::floor((fp - hw) / binHz));
               const long b = std::min((long)kHi, (long)std::ceil((fp + hw) / binHz));
               for (long i = a; i <= b; i++)
                  mask[(size_t)i] = 1;
            }
         }

         struct Band
         {
            double lo, hi, centre, noiseBin, noisePow, harmPow;
            size_t masked;
         };
         std::vector<Band> bands;
         const double topBand = std::min(0.45 * sr, 18000.0);
         std::vector<float> vals;
         for (int i = 0; i < 32; i++)
         {
            Band bd{};
            bd.lo = 1000.0 * std::pow(2.0, (i - 0.5) / 3.0);
            bd.hi = 1000.0 * std::pow(2.0, (i + 0.5) / 3.0);
            bd.centre = 1000.0 * std::pow(2.0, i / 3.0);
            if (bd.hi > topBand)
               break;
            const size_t k0 = std::max(kLo, (size_t)std::ceil(bd.lo / binHz));
            const size_t k1 = std::min(kHi, (size_t)std::floor(bd.hi / binHz));
            if (k1 < k0 + 4)
               continue;
            vals.clear();
            double maskedSum = 0.0;
            for (size_t k = k0; k <= k1; k++)
            {
               if (mask[k])
               {
                  maskedSum += P[k];
                  bd.masked++;
               }
               else
                  vals.push_back(P[k]);
            }
            if (vals.size() < 8)
               vals.assign(P.begin() + (long)k0, P.begin() + (long)k1 + 1);
            bd.noiseBin = std::max(1e-30, (double)Median(vals) / std::log(2.0)); // mean of an exponential = median / ln 2
            bd.noisePow = bd.noiseBin * (double)(k1 - k0 + 1) * toPow;
            bd.harmPow = std::max(0.0, (maskedSum - bd.noiseBin * (double)bd.masked) * toPow);
            bands.push_back(bd);
         }

         if (!bands.empty())
         {
            double harmonic = 0.0;
            if (p.pitched)
            {
               for (size_t k = kLo; k <= kHi; k++)
                  if (mask[k])
                     harmonic += P[k];
               harmonic *= toPow;
               for (const Band& bd : bands)
                  harmonic -= bd.noiseBin * (double)bd.masked * toPow;
               harmonic = std::max(0.0, harmonic);
            }
            // Crossover: the lowest band from which up the noise outweighs the harmonics.
            const int nB = (int)bands.size();
            int b1 = -1;
            double nCum = 0.0, hCum = 0.0;
            for (int b = nB - 1; b >= 0; b--)
            {
               nCum += bands[(size_t)b].noisePow;
               hCum += bands[(size_t)b].harmPow;
               if (nCum >= hCum)
                  b1 = b;
            }
            if (b1 < 0)
               b1 = nB - 1;
            // Then up to where the noise's own spectrum comes within 6 dB of its maximum.
            std::vector<double> D((size_t)nB);
            double dMax = -1e9;
            for (int b = 0; b < nB; b++)
            {
               D[(size_t)b] = 10.0 * std::log10(bands[(size_t)b].noiseBin);
               if (b >= b1)
                  dMax = std::max(dMax, D[(size_t)b]);
            }
            int bb = b1;
            while (bb < nB - 1 && D[(size_t)bb] < dMax - 6.0)
               bb++;
            double corner = bands[(size_t)b1].lo;
            if (bb > b1)
            {
               const double d0 = D[(size_t)bb - 1], d1 = D[(size_t)bb];
               const double fr = d1 > d0 ? std::clamp((dMax - 6.0 - d0) / (d1 - d0), 0.0, 1.0) : 1.0;
               corner = bands[(size_t)bb - 1].centre * std::pow(bands[(size_t)bb].centre / bands[(size_t)bb - 1].centre, fr);
            }
            corner = std::clamp(corner, 1000.0, 16000.0);
            double noise = 0.0;
            for (const Band& bd : bands)
               noise += bd.noisePow * std::clamp((bd.hi - std::max(bd.lo, corner)) / (bd.hi - bd.lo), 0.0, 1.0);
            p.airMeasured = true;
            p.noiseToneHz = (float)corner;
            p.noiseLevel = harmonic > 0.0 ? (float)std::clamp(std::sqrt(noise / harmonic), 0.0, 1.0) : 1.0f;
         }
      }

      // ---- attack transient (after air: its highpass corner is reused) -------
      if (p.pitched)
      {
         const TransientResult tr =
            MeasureTransient(x, sr, onsetSample, sinFreqs, p.f0Hz, p.airMeasured ? (double)p.noiseToneHz : 0.0);
         p.transientMeasured = tr.measured;
         p.transient = tr.level;
         p.transientDecayMs = tr.decayMs;
         p.transientHigh = tr.highLevel;
         p.transientHighDecayMs = tr.highDecayMs;
      }

      p.valid = true;
      return p;
   }

   json ToJson(const SoundProfile& p)
   {
      json j;
      j["valid"] = p.valid;
      if (!p.valid)
      {
         j["error"] = p.error;
         return j;
      }
      j["sampleRate"] = p.sampleRate;
      j["start"] = R3(p.startSeconds);
      j["seconds"] = R3(p.seconds);
      j["peakDb"] = R1(p.peakDb);
      j["peakTime"] = R3(p.peakTime);
      j["pitched"] = p.pitched;
      if (p.pitched)
      {
         j["f0Hz"] = R2(p.f0Hz);
         j["note"] = AudioAnalysis::NoteName(p.f0Hz);
         j["inharmonicity"] = p.inharmonicity;
      }
      j["clarity"] = R2(p.f0Clarity);

      json parts = json::array();
      for (const Partial& q : p.partials)
      {
         if (!q.present)
            continue;
         json e;
         e["k"] = q.harmonic;
         e["freqHz"] = R1(q.freqHz);
         e["ampDb"] = R1(q.ampDb);
         e["amp"] = R3(q.amp);
         if (q.decaySec > 0.0f)
            e["decaySec"] = R3(q.decaySec);
         else
            e["decaySec"] = nullptr;
         e["sustains"] = q.sustains;
         parts.push_back(e);
      }
      j["partials"] = parts;

      j["envelope"] = {
         { "attackMs", R1(p.attackMs) },
         { "decayMs", R1(p.decayMs) },
         { "sustain", R3(p.sustain) },
         { "releaseMs", R1(p.releaseMs) },
         { "sustained", p.sustained },
         { "releaseMeasured", p.releaseMeasured },
      };
      {
         const size_t n = p.envelopeDb.size();
         const size_t points = std::min<size_t>(64, n);
         json vals = json::array();
         for (size_t i = 0; i < points; i++)
         {
            const size_t a = i * n / points, b = std::max(a + 1, (i + 1) * n / points);
            float m = -120.0f;
            for (size_t k = a; k < b && k < n; k++)
               m = std::max(m, p.envelopeDb[k]);
            vals.push_back(R1(m));
         }
         j["envelopeDb"] = { { "hopSeconds", points > 0 ? R3(p.envHopSeconds * (double)n / (double)points) : 0.0 },
                             { "values", vals } };
      }

      json bandHz = json::array(), bandDb = json::array();
      for (int b = 0; b < kEnvBands; b++)
      {
         bandHz.push_back(std::round(p.bandHz[b]));
         bandDb.push_back(R1(p.bandDb[b]));
      }
      j["spectrum"] = {
         { "bandHz", bandHz },
         { "bandDb", bandDb },
         { "centroidHz", std::round(p.centroidHz) },
         { "rolloffHz", std::round(p.rolloffHz) },
         { "brightness", R3(p.brightness) },
         { "noiseToHarmonicDb", R1(p.noiseToHarmonicDb) },
         { "noisiness", R3(p.noisiness) },
      };
      if (p.pitched)
         j["levelDb"] = R1(p.levelDb);
      j["transient"] = {
         { "measured", p.transientMeasured },
         { "onset", R3(p.onsetTime) },
         { "level", R3(p.transient) },
         { "decayMs", R1(p.transientDecayMs) },
         { "crestDb", R1(p.attackCrestDb) },
         { "high", { { "level", R3(p.transientHigh) }, { "decayMs", R1(p.transientHighDecayMs) } } },
      };
      j["air"] = {
         { "measured", p.airMeasured },
         { "noiseLevel", R3(p.noiseLevel) },
         { "noiseDb", R1(AmpToDb(std::max(1e-4f, p.noiseLevel))) },
         { "noiseToneHz", std::round(p.noiseToneHz) },
      };
      return j;
   }

   // ---------------------------------------------------------------- multi-note

   namespace
   {
      // Least-squares line y = a + b x with x in octaves from the pivot, one
      // pass of 3-sigma outlier rejection (sigma from the median absolute
      // residual); needs >= 3 points spanning >= 2 semitones.
      KeyFit FitKey(const std::vector<double>& oct, const std::vector<double>& y)
      {
         KeyFit f;
         std::vector<char> use(oct.size(), 1);
         for (int pass = 0; pass < 2; pass++)
         {
            double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, lo = 1e9, hi = -1e9;
            for (size_t i = 0; i < oct.size(); i++)
               if (use[i])
               {
                  n += 1.0;
                  sx += oct[i];
                  sy += y[i];
                  sxx += oct[i] * oct[i];
                  sxy += oct[i] * y[i];
                  lo = std::min(lo, oct[i]);
                  hi = std::max(hi, oct[i]);
               }
            const double den = n * sxx - sx * sx;
            if (n < 3.0 || hi - lo < 2.0 / 12.0 - 1e-9 || den <= 0.0)
            {
               if (pass == 0)
                  return f;
               break; // the rejection left too little: keep the first fit
            }
            const double b = (n * sxy - sx * sy) / den;
            const double a = (sy - b * sx) / n;
            double ssTot = 0.0, ssRes = 0.0;
            const double my = sy / n;
            std::vector<float> absRes;
            for (size_t i = 0; i < oct.size(); i++)
               if (use[i])
               {
                  const double r = y[i] - (a + b * oct[i]);
                  ssRes += r * r;
                  ssTot += (y[i] - my) * (y[i] - my);
                  absRes.push_back((float)std::fabs(r));
               }
            f.valid = true;
            f.notes = (int)n;
            f.dbPerOct = (float)b;
            f.atRefDb = (float)a;
            f.r2 = ssTot > 1e-12 ? (float)std::clamp(1.0 - ssRes / ssTot, 0.0, 1.0) : 0.0f;
            if (pass == 1)
               break;
            const double sigma = 1.4826 * Median(absRes);
            if (sigma <= 1e-6)
               break;
            bool dropped = false;
            for (size_t i = 0; i < oct.size(); i++)
               if (use[i] && std::fabs(y[i] - (a + b * oct[i])) > 3.0 * sigma)
               {
                  use[i] = 0;
                  dropped = true;
               }
            if (!dropped)
               break;
         }
         return f;
      }

      double MidiToHz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

      bool MajorityTrue(const std::vector<char>& v)
      {
         size_t t = 0;
         for (char c : v)
            t += c ? 1 : 0;
         return !v.empty() && 2 * t > v.size();
      }
   }

   MultiNoteResult AnalyzeNotes(const float* mono, size_t frames, double sr, const std::vector<NoteEvent>& notes,
                                const MultiNoteOptions& o, const std::atomic<bool>* cancel)
   {
      MultiNoteResult r;
      r.refMidi = o.refMidi;
      r.notesGiven = (int)notes.size();
      if (mono == nullptr || frames == 0 || !(sr > 0.0))
      {
         r.error = "no audio";
         return r;
      }
      const double duration = (double)frames / sr;
      const int lowMidi = std::min(o.lowMidi, o.highMidi), highMidi = std::max(o.lowMidi, o.highMidi);

      // ---- isolation --------------------------------------------------------
      std::vector<size_t> order(notes.size());
      std::iota(order.begin(), order.end(), (size_t)0);
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return notes[a].start < notes[b].start; });

      std::vector<float> inRangeAmps;
      for (const NoteEvent& n : notes)
         if (n.midi >= lowMidi && n.midi <= highMidi)
            inRangeAmps.push_back(n.amplitude);
      r.notesInRange = (int)inRangeAmps.size();
      const float minAmp = o.minRelAmplitude * Median(inRangeAmps);

      // A note at a harmonic interval above another, starting with it, is the
      // transcriber hearing that note's partial: it does not break the lower
      // note's isolation (it is itself never isolated - its "fundamental" is right there).
      auto isOwnPartial = [](int base, int other)
      {
         const int d = other - base;
         return d == 12 || d == 19 || d == 24 || d == 28 || d == 31 || d == 36;
      };

      struct Candidate
      {
         size_t note;
         double regionStart, regionEnd;
      };
      std::vector<Candidate> candidates;
      for (size_t pos = 0; pos < order.size(); pos++)
      {
         const NoteEvent& n = notes[order[pos]];
         if (n.midi < lowMidi || n.midi > highMidi)
            continue;
         if (n.end - n.start < o.minNoteSeconds || n.amplitude < minAmp)
            continue;
         if (n.start < 0.0 || n.start >= duration - 0.1)
            continue;
         bool isolated = true;
         double nextOnset = 1e30;
         for (size_t q = 0; q < order.size() && isolated; q++)
         {
            if (q == pos)
               continue;
            const NoteEvent& m = notes[order[q]];
            const double dt = m.start - n.start;
            if (std::fabs(dt) <= 0.03 && isOwnPartial(n.midi, m.midi))
               continue;
            if (dt >= -o.isolationBefore && dt <= o.isolationAfter)
               isolated = false;
            else if (dt > 0.0)
               nextOnset = std::min(nextOnset, m.start);
         }
         if (!isolated)
            continue;
         Candidate c;
         c.note = order[pos];
         c.regionStart = std::max(0.0, n.start - o.preRoll);
         c.regionEnd = std::min({ nextOnset - 0.005, n.start + o.maxNoteSeconds, duration });
         if (c.regionEnd - c.regionStart < 0.1)
            continue;
         candidates.push_back(c);
      }
      r.notesIsolated = (int)candidates.size();
      if (candidates.empty())
      {
         r.error = "no isolated notes (" + std::to_string(r.notesGiven) + " notes, " + std::to_string(r.notesInRange) +
                   " in range) - every note has another starting within " + std::to_string((int)std::lround(o.isolationBefore * 1000.0)) +
                   " ms before or " + std::to_string((int)std::lround(o.isolationAfter * 1000.0)) + " ms after it";
         return r;
      }

      // ---- at most maxNotes, spread over the pitches, then over the file ------
      std::vector<Candidate> chosen;
      {
         std::vector<int> pitches;
         for (const Candidate& c : candidates)
            pitches.push_back(notes[c.note].midi);
         std::sort(pitches.begin(), pitches.end());
         pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
         std::vector<std::vector<size_t>> groups(pitches.size()); // candidate indices, by time
         for (size_t i = 0; i < candidates.size(); i++)
         {
            const size_t g = (size_t)(std::lower_bound(pitches.begin(), pitches.end(), notes[candidates[i].note].midi) - pitches.begin());
            groups[g].push_back(i);
         }
         // Round-robin quotas, then evenly spaced picks inside each pitch.
         const size_t cap = (size_t)std::max(1, o.maxNotes);
         std::vector<size_t> quota(groups.size(), 0);
         size_t total = 0;
         for (bool more = true; more && total < cap;)
         {
            more = false;
            for (size_t g = 0; g < groups.size() && total < cap; g++)
               if (quota[g] < groups[g].size())
               {
                  quota[g]++;
                  total++;
                  more = true;
               }
         }
         for (size_t g = 0; g < groups.size(); g++)
            for (size_t i = 0; i < quota[g]; i++)
            {
               const size_t idx = (size_t)(((double)i + 0.5) * (double)groups[g].size() / (double)quota[g]);
               chosen.push_back(candidates[groups[g][std::min(idx, groups[g].size() - 1)]]);
            }
         std::sort(chosen.begin(), chosen.end(), [&](const Candidate& a, const Candidate& b)
                   { return notes[a.note].start < notes[b.note].start; });
      }

      // ---- analyse each ----------------------------------------------------
      std::vector<SoundProfile> profiles;
      std::vector<int> midis;
      for (const Candidate& c : chosen)
      {
         if (cancel != nullptr && cancel->load())
         {
            r.error = "cancelled";
            return r;
         }
         const NoteEvent& n = notes[c.note];
         Options opt;
         opt.startSeconds = c.regionStart;
         opt.lengthSeconds = c.regionEnd - c.regionStart;
         opt.f0HintHz = MidiToHz(n.midi);
         opt.partials = o.partials;
         SoundProfile p = AnalyzeSound(mono, frames, sr, opt);
         r.notesAnalysed++;
         if (!p.valid || !p.pitched || !(p.f0Hz > 0.0f))
            continue;
         // The transcriber's pitch and the measured one must agree (an octave
         // error or a neighbour's partial would poison the medians).
         const double cents = 1200.0 * std::log2((double)p.f0Hz / opt.f0HintHz);
         if (std::fabs(cents) > 70.0)
            continue;
         NoteRow row;
         row.start = n.start;
         row.regionStart = p.startSeconds;
         row.regionSeconds = p.seconds;
         row.midi = n.midi;
         row.f0Hz = p.f0Hz;
         row.levelDb = p.levelDb;
         row.peakDb = p.peakDb;
         if (!p.partials.empty() && p.partials[0].present && p.levelDb > -119.0f)
         {
            // levelDb is the power sum of the partials: the fundamental's share of it.
            double sumAmp2 = 0.0;
            for (const Partial& q : p.partials)
               if (q.present)
                  sumAmp2 += (double)q.amp * q.amp;
            const double a1 = p.partials[0].amp;
            if (sumAmp2 > 0.0 && a1 > 0.0)
            {
               row.hasH1 = true;
               row.h1Db = p.levelDb + (float)(10.0 * std::log10(a1 * a1 / sumAmp2));
            }
         }
         if (p.partials.size() >= 2 && p.partials[0].present && p.partials[1].present)
         {
            row.hasH2 = true;
            row.h2h1Db = p.partials[1].ampDb - p.partials[0].ampDb;
         }
         if (p.partials.size() >= 3 && p.partials[0].present && p.partials[2].present)
         {
            row.hasH3 = true;
            row.h3h1Db = p.partials[2].ampDb - p.partials[0].ampDb;
         }
         row.transient = p.transient;
         row.noiseLevel = p.noiseLevel;
         r.rows.push_back(row);
         profiles.push_back(std::move(p));
         midis.push_back(n.midi);
      }
      r.notesUsed = (int)profiles.size();
      if (profiles.empty())
      {
         r.error = "none of the " + std::to_string(r.notesAnalysed) + " isolated notes analysed as the transcribed pitch";
         return r;
      }

      // ---- key tracking -------------------------------------------------------
      {
         std::vector<double> xl, yl, xt, yt, x2, y2, x3, y3;
         for (const NoteRow& row : r.rows)
         {
            const double oct = (double)(row.midi - o.refMidi) / 12.0;
            if (row.hasH1)
            {
               xl.push_back(oct);
               yl.push_back(row.h1Db);
            }
            if (row.levelDb > -119.0f)
            {
               xt.push_back(oct);
               yt.push_back(row.levelDb);
            }
            if (row.hasH2)
            {
               x2.push_back(oct);
               y2.push_back(row.h2h1Db);
            }
            if (row.hasH3)
            {
               x3.push_back(oct);
               y3.push_back(row.h3h1Db);
            }
         }
         r.keyLevelFromFundamental = xl.size() >= 3;
         r.keyLevel = r.keyLevelFromFundamental ? FitKey(xl, yl) : FitKey(xt, yt);
         r.keyTilt = FitKey(x2, y2);
         r.keyTilt3 = FitKey(x3, y3);
      }

      // ---- merged profile -----------------------------------------------------
      SoundProfile& m = r.profile;
      const size_t U = profiles.size();
      const int K = std::clamp(o.partials, 1, kMaxPartials);
      m.sampleRate = sr;
      m.pitched = true;
      m.f0Hz = (float)MidiToHz(o.refMidi);
      m.partials.resize((size_t)K);
      for (int k = 1; k <= K; k++)
         m.partials[(size_t)k - 1].harmonic = k;
      auto med = [&](auto get)
      {
         std::vector<float> v;
         v.reserve(U);
         for (const SoundProfile& p : profiles)
            v.push_back((float)get(p));
         return Median(v);
      };
      m.f0Clarity = med([](const SoundProfile& p) { return p.f0Clarity; });
      m.inharmonicity = med([](const SoundProfile& p) { return p.inharmonicity; });
      m.peakDb = med([](const SoundProfile& p) { return p.peakDb; });
      m.peakTime = med([](const SoundProfile& p) { return p.peakTime; });
      m.onsetTime = med([](const SoundProfile& p) { return p.onsetTime; });
      m.levelDb = med([](const SoundProfile& p) { return p.levelDb; });

      // Partials relative to each note's fundamental, key tilt removed (the
      // synth's keyTilt shelf moves every partial above the fundamental by the
      // same amount), median, then relative to the strongest.
      {
         const float tilt = r.keyTilt.valid ? r.keyTilt.dbPerOct : 0.0f;
         bool anyFundamental = false;
         for (const SoundProfile& p : profiles)
            anyFundamental = anyFundamental || (!p.partials.empty() && p.partials[0].present);
         std::vector<float> merged((size_t)K, -200.0f);
         for (int k = 0; k < K; k++)
         {
            std::vector<float> lv, ratio, t60;
            int sustains = 0, decays = 0, eligible = 0;
            for (size_t u = 0; u < U; u++)
            {
               const SoundProfile& p = profiles[u];
               if ((size_t)k >= p.partials.size())
                  continue;
               const bool hasRef = !p.partials.empty() && p.partials[0].present;
               if (anyFundamental && !hasRef)
                  continue;
               eligible++;
               const Partial& q = p.partials[(size_t)k];
               if (!q.present)
                  continue;
               const float ref = hasRef ? p.partials[0].ampDb : 0.0f;
               const float adj = k >= 1 ? tilt * (float)(midis[u] - o.refMidi) / 12.0f : 0.0f;
               lv.push_back(q.ampDb - ref - adj);
               if (p.f0Hz > 0.0f)
                  ratio.push_back(q.freqHz / p.f0Hz);
               if (q.decaySec > 0.0f)
               {
                  t60.push_back(q.decaySec);
                  decays++;
               }
               else if (q.sustains)
                  sustains++;
            }
            // Present in at least a third of the notes that could show it.
            if (lv.empty() || 3 * (int)lv.size() < eligible)
               continue;
            Partial& mq = m.partials[(size_t)k];
            mq.present = true;
            merged[(size_t)k] = Median(lv);
            mq.freqHz = ratio.empty() ? (float)(k + 1) * m.f0Hz : Median(ratio) * m.f0Hz;
            mq.sustains = sustains > decays;
            mq.decaySec = (!mq.sustains && !t60.empty()) ? Median(t60) : 0.0f;
         }
         const float top = *std::max_element(merged.begin(), merged.end());
         for (int k = 0; k < K; k++)
         {
            Partial& mq = m.partials[(size_t)k];
            if (!mq.present)
               continue;
            mq.ampDb = merged[(size_t)k] - top;
            mq.amp = (float)std::pow(10.0, mq.ampDb / 20.0);
         }
      }

      // Envelope: ADSR medians; the curve is the median of the notes'
      // envelopes on a common grid from their region starts.
      m.attackMs = med([](const SoundProfile& p) { return p.attackMs; });
      m.decayMs = med([](const SoundProfile& p) { return p.decayMs; });
      {
         std::vector<char> sus, rel;
         for (const SoundProfile& p : profiles)
         {
            sus.push_back(p.sustained ? 1 : 0);
            rel.push_back(p.releaseMeasured ? 1 : 0);
         }
         m.sustained = MajorityTrue(sus);
         m.releaseMeasured = m.sustained && MajorityTrue(rel);
      }
      m.sustain = m.sustained ? med([](const SoundProfile& p) { return p.sustain; }) : 0.0f;
      m.releaseMs = med([](const SoundProfile& p) { return p.releaseMs; });
      {
         double longest = 0.0;
         for (const SoundProfile& p : profiles)
            longest = std::max(longest, p.seconds);
         const double hopSec = std::max(0.004, longest / (double)kEnvelopePoints);
         const size_t points = std::max<size_t>(1, std::min<size_t>(kEnvelopePoints, (size_t)std::ceil(longest / hopSec)));
         const size_t need = std::max<size_t>(1, (U + 2) / 3);
         std::vector<float> v;
         for (size_t i = 0; i < points; i++)
         {
            const double t = ((double)i + 0.5) * hopSec;
            v.clear();
            for (const SoundProfile& p : profiles)
            {
               if (p.envelopeDb.empty() || !(p.envHopSeconds > 0.0f) || t >= p.seconds)
                  continue;
               const size_t j = std::min(p.envelopeDb.size() - 1, (size_t)(t / p.envHopSeconds));
               v.push_back(std::pow(10.0f, p.envelopeDb[j] / 20.0f));
            }
            if (v.size() < need)
               break;
            m.envelopeDb.push_back(std::max(-90.0f, AmpToDb(Median(v))));
         }
         m.envHopSeconds = (float)hopSec;
         m.seconds = (double)m.envelopeDb.size() * hopSec;
         if (!m.envelopeDb.empty())
         {
            const float top = *std::max_element(m.envelopeDb.begin(), m.envelopeDb.end());
            for (float& d : m.envelopeDb)
               d = std::max(-90.0f, d - top);
         }
      }
      m.startSeconds = notes[chosen.front().note].start;

      // Spectrum, transient, air: medians.
      for (int b = 0; b < kEnvBands; b++)
      {
         m.bandHz[b] = BandCentreHz(b);
         m.bandDb[b] = med([b](const SoundProfile& p) { return p.bandDb[b]; });
      }
      m.centroidHz = med([](const SoundProfile& p) { return p.centroidHz; });
      m.rolloffHz = med([](const SoundProfile& p) { return p.rolloffHz; });
      m.brightness = med([](const SoundProfile& p) { return p.brightness; });
      m.noiseToHarmonicDb = med([](const SoundProfile& p) { return p.noiseToHarmonicDb; });
      m.noisiness = med([](const SoundProfile& p) { return p.noisiness; });
      {
         std::vector<float> lv, dc, hl, hd, cr, nl, nt;
         for (const SoundProfile& p : profiles)
         {
            cr.push_back(p.attackCrestDb);
            if (p.transientMeasured)
            {
               lv.push_back(p.transient);
               if (p.transient > 0.0f)
                  dc.push_back(p.transientDecayMs);
               hl.push_back(p.transientHigh);
               if (p.transientHigh > 0.0f)
                  hd.push_back(p.transientHighDecayMs);
            }
            if (p.airMeasured)
            {
               nl.push_back(p.noiseLevel);
               nt.push_back(p.noiseToneHz);
            }
         }
         m.attackCrestDb = Median(cr);
         m.transientMeasured = !lv.empty();
         m.transient = Median(lv);
         m.transientDecayMs = Median(dc);
         m.transientHigh = Median(hl);
         m.transientHighDecayMs = Median(hd);
         m.airMeasured = !nl.empty();
         m.noiseLevel = Median(nl);
         m.noiseToneHz = Median(nt);
      }
      m.valid = true;
      r.valid = true;
      return r;
   }

   json ToJson(const MultiNoteResult& r, size_t maxRows)
   {
      json j;
      j["valid"] = r.valid;
      if (!r.error.empty())
         j["error"] = r.error;
      j["notesGiven"] = r.notesGiven;
      j["notesInRange"] = r.notesInRange;
      j["notesIsolated"] = r.notesIsolated;
      j["notesAnalysed"] = r.notesAnalysed;
      j["notesUsed"] = r.notesUsed;
      if (!r.valid)
         return j;
      auto fit = [](const KeyFit& f, const char* slope, const char* r2, const char* n, json& out)
      {
         out[slope] = f.valid ? json(R2(f.dbPerOct)) : json(nullptr);
         out[r2] = f.valid ? json(R2(f.r2)) : json(nullptr);
         out[n] = f.notes;
      };
      json kt;
      kt["refMidi"] = r.refMidi;
      fit(r.keyLevel, "levelDbPerOct", "levelR2", "levelNotes", kt);
      if (r.keyLevel.valid)
         kt["levelAtRefDb"] = R1(r.keyLevel.atRefDb);
      kt["levelOf"] = r.keyLevelFromFundamental ? "fundamental" : "all partials";
      fit(r.keyTilt, "tiltDbPerOct", "tiltR2", "tiltNotes", kt);
      fit(r.keyTilt3, "tilt3DbPerOct", "tilt3R2", "tilt3Notes", kt);
      j["keyTracking"] = kt;
      j["partialsAt"] = "MIDI " + std::to_string(r.refMidi) + " (levels re each note's fundamental, key tilt removed)";
      json rows = json::array();
      for (size_t i = 0; i < r.rows.size() && i < maxRows; i++)
      {
         const NoteRow& row = r.rows[i];
         json e;
         e["t"] = R3(row.start);
         e["note"] = AudioAnalysis::NoteName((float)MidiToHz(row.midi));
         e["midi"] = row.midi;
         e["f0Hz"] = R2(row.f0Hz);
         e["levelDb"] = R1(row.levelDb);
         e["h1Db"] = row.hasH1 ? json(R1(row.h1Db)) : json(nullptr);
         e["peakDb"] = R1(row.peakDb);
         e["h2h1Db"] = row.hasH2 ? json(R1(row.h2h1Db)) : json(nullptr);
         e["h3h1Db"] = row.hasH3 ? json(R1(row.h3h1Db)) : json(nullptr);
         e["transient"] = R3(row.transient);
         e["noiseLevel"] = R3(row.noiseLevel);
         rows.push_back(e);
      }
      j["notes"] = rows;
      if (r.rows.size() > maxRows)
         j["notesTruncated"] = "showing " + std::to_string(maxRows) + " of " + std::to_string(r.rows.size());
      return j;
   }

   namespace
   {
      // The partials AdditiveFormula keeps, by harmonic number.
      std::vector<const Partial*> AdditiveTerms(const SoundProfile& p, int maxTerms, float floorDb)
      {
         std::vector<const Partial*> terms;
         for (const Partial& q : p.partials)
            if (q.present && q.ampDb >= floorDb && q.amp > 0.0f)
               terms.push_back(&q);
         std::sort(terms.begin(), terms.end(), [](const Partial* a, const Partial* b) { return a->amp > b->amp; });
         if ((int)terms.size() > std::max(1, maxTerms))
            terms.resize((size_t)std::max(1, maxTerms));
         std::sort(terms.begin(), terms.end(), [](const Partial* a, const Partial* b) { return a->harmonic < b->harmonic; });
         return terms;
      }
   }

   float AdditiveRmsOverPeak(const SoundProfile& p, int maxTerms, float floorDb)
   {
      const std::vector<const Partial*> terms = AdditiveTerms(p, maxTerms, floorDb);
      if (terms.empty())
         return 0.70710678f;
      const int n = 2048;
      double peak = 0.0, ss = 0.0;
      for (int i = 0; i < n; i++)
      {
         const double xx = (double)i / n;
         double v = 0.0;
         for (const Partial* q : terms)
            v += q->amp * std::sin(2.0 * kPi * q->harmonic * xx);
         peak = std::max(peak, std::fabs(v));
         ss += v * v;
      }
      // Peak floored at 1: Equation Synth scales a wave down to peak 1, never up.
      return (float)(std::sqrt(ss / n) / std::max(1.0, peak));
   }

   std::string AdditiveFormula(const SoundProfile& p, int maxTerms, float floorDb)
   {
      const std::vector<const Partial*> terms = AdditiveTerms(p, maxTerms, floorDb);
      if (terms.empty())
         return "sin(2*pi*x)";
      std::string out;
      char buf[64];
      for (const Partial* q : terms)
      {
         if (!out.empty())
            out += " + ";
         snprintf(buf, sizeof(buf), "%.4f*sin(%d*pi*x)", q->amp, 2 * q->harmonic);
         out += buf;
      }
      return out;
   }

   float HarmonicMismatchDb(const SoundProfile& p, const float* amps, int count)
   {
      const int n = std::min(count, (int)p.partials.size());
      if (amps == nullptr || n <= 0)
         return 999.0f;
      float maxRef = 0.0f, maxP = 0.0f;
      for (int k = 0; k < n; k++)
      {
         maxRef = std::max(maxRef, amps[k]);
         if (p.partials[(size_t)k].present)
            maxP = std::max(maxP, p.partials[(size_t)k].amp);
      }
      if (maxRef <= 0.0f || maxP <= 0.0f)
         return 999.0f;
      double num = 0.0, den = 0.0;
      for (int k = 0; k < n; k++)
      {
         const float a = std::max(0.0f, amps[k]) / maxRef;
         const Partial& q = p.partials[(size_t)k];
         const float b = q.present ? q.amp / maxP : 0.0f;
         const float dbA = a > 0.0f ? std::max(-60.0f, AmpToDb(a)) : -60.0f;
         const float dbB = b > 0.0f ? std::max(-60.0f, AmpToDb(b)) : -60.0f;
         const double w = std::max(a, b) + 0.02;
         num += w * std::fabs(dbA - dbB);
         den += w;
      }
      return (float)(num / den);
   }
}
