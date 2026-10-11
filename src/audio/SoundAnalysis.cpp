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
         for (int k = 0; k < K; k++)
         {
            Partial& q = p.partials[(size_t)k];
            if (!q.present)
               continue;
            q.ampDb = onsetDb[(size_t)k] - strongest;
            q.amp = (float)std::pow(10.0, q.ampDb / 20.0);
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
      return j;
   }

   std::string AdditiveFormula(const SoundProfile& p, int maxTerms, float floorDb)
   {
      std::vector<const Partial*> terms;
      for (const Partial& q : p.partials)
         if (q.present && q.ampDb >= floorDb && q.amp > 0.0f)
            terms.push_back(&q);
      std::sort(terms.begin(), terms.end(), [](const Partial* a, const Partial* b) { return a->amp > b->amp; });
      if ((int)terms.size() > std::max(1, maxTerms))
         terms.resize((size_t)std::max(1, maxTerms));
      std::sort(terms.begin(), terms.end(), [](const Partial* a, const Partial* b) { return a->harmonic < b->harmonic; });
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
