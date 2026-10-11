#include "AudioAnalysis.h"
#include "dsp/FluxOnsetDetector.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>

namespace AudioAnalysis
{
   namespace
   {
      using json = nlohmann::json;
      using cfloat = std::complex<float>;
      const double kPi = 3.14159265358979323846;

      float ToDb(double power) { return (float)(10.0 * std::log10(std::max(power, 1e-12))); }

      // One decimal is plenty for a reader and keeps the JSON small.
      double R1(double v) { return std::round(v * 10.0) / 10.0; }
      double R3(double v) { return std::round(v * 1000.0) / 1000.0; }

      int NextPow2(int n)
      {
         int p = 1;
         while (p < n)
            p <<= 1;
         return p;
      }

      // In-place iterative radix-2 FFT; `inverse` scales by 1/N.
      void Fft(std::vector<cfloat>& a, bool inverse)
      {
         const size_t n = a.size();
         for (size_t i = 1, j = 0; i < n; i++)
         {
            size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1)
               j ^= bit;
            j ^= bit;
            if (i < j)
               std::swap(a[i], a[j]);
         }
         for (size_t len = 2; len <= n; len <<= 1)
         {
            const double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
            const cfloat wlen((float)std::cos(ang), (float)std::sin(ang));
            for (size_t i = 0; i < n; i += len)
            {
               cfloat w(1.0f, 0.0f);
               for (size_t k = 0; k < len / 2; k++)
               {
                  const cfloat u = a[i + k];
                  const cfloat v = a[i + k + len / 2] * w;
                  a[i + k] = u + v;
                  a[i + k + len / 2] = u - v;
                  w *= wlen;
               }
            }
         }
         if (inverse)
            for (cfloat& x : a)
               x /= (float)n;
      }

      std::vector<float> Hann(int n)
      {
         std::vector<float> w((size_t)n);
         for (int i = 0; i < n; i++)
            w[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
         return w;
      }

      // Band edges, log-spaced over 20 Hz .. 20 kHz.
      double BandEdge(int i) { return 20.0 * std::pow(1000.0, (double)i / kBands); }

      // Windowed power spectrum of mono[start .. start+n) (zero outside the signal).
      void PowerSpectrum(const std::vector<float>& mono, long long start, const std::vector<float>& window,
                         std::vector<cfloat>& scratch, std::vector<float>& power)
      {
         const int n = (int)window.size();
         scratch.assign((size_t)n, cfloat(0.0f, 0.0f));
         for (int i = 0; i < n; i++)
         {
            const long long idx = start + i;
            if (idx >= 0 && idx < (long long)mono.size())
               scratch[(size_t)i] = cfloat(mono[(size_t)idx] * window[(size_t)i], 0.0f);
         }
         Fft(scratch, false);
         power.resize((size_t)n / 2 + 1);
         for (int k = 0; k <= n / 2; k++)
            power[(size_t)k] = std::norm(scratch[(size_t)k]);
      }

      // YIN on mono[start .. start + w + tauMax). Returns the period in samples
      // (0 when no periodicity clears the threshold) and the clarity.
      double YinPeriod(const std::vector<float>& mono, long long start, int w, int tauMin, int tauMax,
                       std::vector<cfloat>& fa, std::vector<cfloat>& fb, std::vector<float>& d, float& clarity)
      {
         clarity = 0.0f;
         const int len = w + tauMax;
         auto x = [&](int i) -> float
         {
            const long long idx = start + i;
            return (idx >= 0 && idx < (long long)mono.size()) ? mono[(size_t)idx] : 0.0f;
         };
         const int m = NextPow2(len + w);
         fa.assign((size_t)m, cfloat(0.0f, 0.0f));
         fb.assign((size_t)m, cfloat(0.0f, 0.0f));
         for (int i = 0; i < w; i++)
            fa[(size_t)i] = cfloat(x(i), 0.0f);
         for (int i = 0; i < len; i++)
            fb[(size_t)i] = cfloat(x(i), 0.0f);
         Fft(fa, false);
         Fft(fb, false);
         for (int i = 0; i < m; i++)
            fa[(size_t)i] = std::conj(fa[(size_t)i]) * fb[(size_t)i];
         Fft(fa, true); // fa[tau] = sum_j x_j * x_{j+tau}

         // Energies of the two windows via a running sum.
         std::vector<double> prefix((size_t)len + 1, 0.0);
         for (int i = 0; i < len; i++)
            prefix[(size_t)i + 1] = prefix[(size_t)i] + (double)x(i) * x(i);
         const double e0 = prefix[(size_t)w];
         if (e0 < 1e-8)
            return 0.0;

         // Cumulative mean normalized difference d'(tau).
         d.assign((size_t)tauMax + 1, 1.0f);
         double running = 0.0;
         for (int tau = 1; tau <= tauMax; tau++)
         {
            const double et = prefix[(size_t)tau + w] - prefix[(size_t)tau];
            const double diff = std::max(0.0, e0 + et - 2.0 * (double)fa[(size_t)tau].real());
            running += diff;
            d[(size_t)tau] = running > 0.0 ? (float)(diff * tau / running) : 1.0f;
         }

         const float threshold = 0.15f;
         int best = -1;
         for (int tau = std::max(2, tauMin); tau < tauMax; tau++)
            if (d[(size_t)tau] < threshold)
            {
               while (tau + 1 < tauMax && d[(size_t)tau + 1] < d[(size_t)tau])
                  tau++;
               best = tau;
               break;
            }
         if (best < 0)
            return 0.0;
         // Parabolic interpolation around the dip.
         double period = best;
         if (best > 1 && best < tauMax)
         {
            const double a = d[(size_t)best - 1], b = d[(size_t)best], c = d[(size_t)best + 1];
            const double den = a - 2.0 * b + c;
            if (std::fabs(den) > 1e-12)
               period = best + 0.5 * (a - c) / den;
         }
         clarity = std::clamp(1.0f - d[(size_t)best], 0.0f, 1.0f);
         return period;
      }

      float Percentile(std::vector<float> v, float p)
      {
         if (v.empty())
            return 0.0f;
         const size_t k = (size_t)std::clamp((double)p * (v.size() - 1), 0.0, (double)v.size() - 1);
         std::nth_element(v.begin(), v.begin() + (long)k, v.end());
         return v[k];
      }

      double Pearson(const double* a, const double* b, int n)
      {
         double ma = 0.0, mb = 0.0;
         for (int i = 0; i < n; i++)
         {
            ma += a[i];
            mb += b[i];
         }
         ma /= n;
         mb /= n;
         double num = 0.0, da = 0.0, db = 0.0;
         for (int i = 0; i < n; i++)
         {
            num += (a[i] - ma) * (b[i] - mb);
            da += (a[i] - ma) * (a[i] - ma);
            db += (b[i] - mb) * (b[i] - mb);
         }
         return (da > 0.0 && db > 0.0) ? num / std::sqrt(da * db) : 0.0;
      }

      const char* kPitchNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
   }

   std::string NoteName(float hz)
   {
      if (!(hz > 0.0f))
         return std::string();
      const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
      const int nearest = (int)std::lround(midi);
      const int cents = (int)std::lround((midi - nearest) * 100.0);
      char buf[32];
      const int pc = ((nearest % 12) + 12) % 12;
      const int octave = (int)std::floor(nearest / 12.0) - 1;
      if (std::abs(cents) >= 5)
         snprintf(buf, sizeof(buf), "%s%d %+dc", kPitchNames[pc], octave, cents);
      else
         snprintf(buf, sizeof(buf), "%s%d", kPitchNames[pc], octave);
      return buf;
   }

   std::vector<float> MixToMono(const std::vector<float>& s)
   {
      std::vector<float> out(s.size() / 2);
      for (size_t i = 0; i < out.size(); i++)
         out[i] = 0.5f * (s[2 * i] + s[2 * i + 1]);
      return out;
   }

   Result Analyze(const std::vector<float>& mono, double sr, const Options& opt)
   {
      Result r;
      r.sampleRate = sr;
      r.seconds = sr > 0.0 ? (double)mono.size() / sr : 0.0;
      if (mono.empty() || sr <= 0.0)
         return r;

      // ---- whole-signal level ----
      double sumSq = 0.0;
      float peak = 0.0f;
      for (float v : mono)
      {
         sumSq += (double)v * v;
         peak = std::max(peak, std::fabs(v));
      }
      r.rmsDb = ToDb(sumSq / (double)mono.size());
      r.peakDb = ToDb((double)peak * peak);

      // ---- per-frame levels, bands, centroid, pitch ----
      const int hop = std::max(1, (int)std::lround(opt.frameSeconds * sr));
      const int fftN = sr > 30000.0 ? 4096 : 2048;
      const std::vector<float> window = Hann(fftN);
      double windowPower = 0.0;
      for (float w : window)
         windowPower += (double)w * w;
      const double binHz = sr / fftN;
      const int yinW = sr > 30000.0 ? 2048 : 1024;
      const int tauMin = std::max(2, (int)(sr / 2000.0));
      const int tauMax = std::min(yinW - 1, (int)(sr / 50.0));

      std::vector<cfloat> scratch, fa, fb;
      std::vector<float> power, yinD;
      double bandPowerSum[kBands] = {};
      double centroidNum = 0.0, centroidDen = 0.0;
      double chroma[12] = {};
      std::vector<float> pitched, loud;
      int silent = 0;
      const size_t frameCount = (mono.size() + (size_t)hop - 1) / (size_t)hop;
      r.frames.reserve(frameCount);
      for (size_t fi = 0; fi < frameCount; fi++)
      {
         const long long s0 = (long long)fi * hop;
         const long long s1 = std::min((long long)mono.size(), s0 + hop);
         Frame f;
         f.time = (float)((double)s0 / sr);
         double fs = 0.0;
         float fp = 0.0f;
         for (long long i = s0; i < s1; i++)
         {
            fs += (double)mono[(size_t)i] * mono[(size_t)i];
            fp = std::max(fp, std::fabs(mono[(size_t)i]));
         }
         f.rmsDb = ToDb(fs / (double)std::max<long long>(1, s1 - s0));
         f.peakDb = ToDb((double)fp * fp);

         const long long center = s0 + hop / 2;
         PowerSpectrum(mono, center - fftN / 2, window, scratch, power);
         // Positive-frequency power scaled so a band's value is the mean
         // square of its share of the signal (see the header).
         const double norm = 2.0 / ((double)fftN * windowPower);
         double bandP[kBands] = {};
         double cNum = 0.0, cDen = 0.0;
         for (int k = 1; k <= fftN / 2; k++)
         {
            const double hz = k * binHz;
            const double p = power[(size_t)k] * norm;
            for (int b = 0; b < kBands; b++)
               if (hz >= BandEdge(b) && hz < BandEdge(b + 1))
               {
                  bandP[b] += p;
                  break;
               }
            if (hz >= 20.0 && hz <= 20000.0)
            {
               cNum += hz * p;
               cDen += p;
            }
            if (opt.rhythmAndKey && hz >= 55.0 && hz <= 2000.0)
            {
               const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
               const int pc = (((int)std::lround(midi)) % 12 + 12) % 12;
               chroma[pc] += std::sqrt(p);
            }
         }
         for (int b = 0; b < kBands; b++)
         {
            f.bandsDb[b] = ToDb(bandP[b]);
            bandPowerSum[b] += bandP[b];
         }
         f.centroidHz = cDen > 1e-12 ? (float)(cNum / cDen) : 0.0f;
         centroidNum += cNum;
         centroidDen += cDen;

         if (f.rmsDb < -60.0f)
            silent++;
         else
         {
            loud.push_back(f.rmsDb);
            const double period = YinPeriod(mono, center - (yinW + tauMax) / 2, yinW, tauMin, tauMax, fa, fb, yinD, f.pitchClarity);
            if (period > 0.0 && f.rmsDb > -50.0f)
            {
               f.pitchHz = (float)(sr / period);
               pitched.push_back(f.pitchHz);
            }
            else
               f.pitchClarity = 0.0f;
         }
         r.frames.push_back(f);
      }
      for (int b = 0; b < kBands; b++)
         r.bandsDb[b] = ToDb(bandPowerSum[b] / (double)std::max<size_t>(1, frameCount));
      r.centroidHz = centroidDen > 1e-12 ? (float)(centroidNum / centroidDen) : 0.0f;
      r.silentRatio = (float)silent / (float)std::max<size_t>(1, frameCount);
      if (!pitched.empty())
      {
         r.medianPitchHz = Percentile(pitched, 0.5f);
         r.lowPitchHz = Percentile(pitched, 0.05f);
         r.highPitchHz = Percentile(pitched, 0.95f);
      }
      r.pitchedRatio = loud.empty() ? 0.0f : (float)pitched.size() / (float)loud.size();

      // ---- onsets: spectral flux at ~11 ms resolution ----
      const int oN = sr > 30000.0 ? 1024 : 512;
      const int oHop = oN / 2;
      const std::vector<float> oWin = Hann(oN);
      std::vector<float> prevMag((size_t)oN / 2 + 1, 0.0f), flux;
      for (long long s = 0; s < (long long)mono.size(); s += oHop)
      {
         PowerSpectrum(mono, s - oN / 2, oWin, scratch, power);
         double sum = 0.0;
         for (size_t k = 1; k < power.size(); k++)
         {
            const float mag = std::log1p(100.0f * std::sqrt(power[k]));
            sum += std::max(0.0f, mag - prevMag[k]);
            prevMag[k] = mag;
         }
         flux.push_back((float)sum);
      }
      const float fluxMax = flux.empty() ? 0.0f : *std::max_element(flux.begin(), flux.end());
      const double fluxRate = sr / oHop;
      if (fluxMax > 0.0f)
      {
         for (float& v : flux)
            v /= fluxMax; // the tempo estimate below reads the normalised flux
         // Onsets come from the same streaming detector the live Audio Compare
         // node runs, so a measurement and the node count the same onsets.
         FluxOnsetDetector det;
         det.Prepare(sr);
         auto onOnset = [&](double sample) { r.onsets.push_back((float)(sample / sr)); };
         det.Push(mono.data(), (int)std::min<size_t>(mono.size(), (size_t)2147483647), onOnset);
         det.Flush(onOnset);
      }

      if (!opt.rhythmAndKey)
         return r;

      // ---- tempo: autocorrelation of the onset envelope over 60..200 bpm ----
      if (r.seconds >= 4.0 && fluxMax > 0.0f)
      {
         std::vector<float> env = flux;
         const float mean = std::accumulate(env.begin(), env.end(), 0.0f) / (float)env.size();
         for (float& v : env)
            v -= mean;
         const int lagLo = std::max(1, (int)std::floor(fluxRate * 60.0 / 200.0));
         const int lagHi = std::min((int)env.size() - 1, (int)std::ceil(fluxRate * 60.0 / 60.0));
         std::vector<double> score((size_t)lagHi + 2, 0.0);
         double best = -1e30, sumScore = 0.0;
         int bestLag = -1, counted = 0;
         for (int lag = lagLo; lag <= lagHi; lag++)
         {
            double acc = 0.0;
            for (size_t i = 0; i + (size_t)lag < env.size(); i++)
               acc += (double)env[i] * env[i + (size_t)lag];
            acc /= (double)(env.size() - (size_t)lag);
            // Prefer tempi near 120 bpm when octave-related lags score alike.
            const double bpm = 60.0 * fluxRate / lag;
            const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 0.9, 2.0));
            score[(size_t)lag] = acc * prior;
            sumScore += std::max(0.0, score[(size_t)lag]);
            counted++;
            if (score[(size_t)lag] > best)
            {
               best = score[(size_t)lag];
               bestLag = lag;
            }
         }
         if (bestLag > 0 && best > 0.0)
         {
            double lag = bestLag;
            if (bestLag > lagLo && bestLag < lagHi)
            {
               const double a = score[(size_t)bestLag - 1], b = score[(size_t)bestLag], c = score[(size_t)bestLag + 1];
               const double den = a - 2.0 * b + c;
               if (std::fabs(den) > 1e-12)
                  lag = bestLag + 0.5 * (a - c) / den;
            }
            r.tempoBpm = (float)(60.0 * fluxRate / lag);
            const double meanScore = sumScore / std::max(1, counted);
            r.tempoConfidence = (float)std::clamp(1.0 - meanScore / best, 0.0, 1.0);
         }
      }

      // ---- key: chroma vs Krumhansl-Kessler profiles ----
      {
         static const double kMajor[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
         static const double kMinor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
         const double total = std::accumulate(chroma, chroma + 12, 0.0);
         if (total > 1e-9)
         {
            for (int i = 0; i < 12; i++)
               r.chroma[i] = (float)(chroma[i] / total);
            double best = -2.0, second = -2.0;
            for (int minor = 0; minor < 2; minor++)
               for (int root = 0; root < 12; root++)
               {
                  double rotated[12];
                  for (int i = 0; i < 12; i++)
                     rotated[i] = chroma[(i + root) % 12];
                  const double c = Pearson(rotated, minor ? kMinor : kMajor, 12);
                  if (c > best)
                  {
                     second = best;
                     best = c;
                     r.keyRoot = root;
                     r.keyMinor = minor != 0;
                  }
                  else if (c > second)
                     second = c;
               }
            r.keyConfidence = (float)std::max(0.0, best - second);
         }
      }
      return r;
   }

   json ToJson(const Result& r, int maxFrames)
   {
      json bands = json::array();
      for (int b = 0; b < kBands; b++)
         bands.push_back({ { "hz", { (int)std::lround(BandEdge(b)), (int)std::lround(BandEdge(b + 1)) } }, { "db", R1(r.bandsDb[b]) } });
      json out = {
         { "seconds", R3(r.seconds) },
         { "sampleRate", (int)std::lround(r.sampleRate) },
         { "rmsDb", R1(r.rmsDb) },
         { "peakDb", R1(r.peakDb) },
         { "crestDb", R1(r.peakDb - r.rmsDb) },
         { "silentRatio", R3(r.silentRatio) },
         { "centroidHz", (int)std::lround(r.centroidHz) },
         { "bands", bands },
         { "onsetCount", (int)r.onsets.size() },
         { "onsetsPerSecond", R1(r.seconds > 0.0 ? r.onsets.size() / r.seconds : 0.0) },
      };
      json onsets = json::array();
      for (size_t i = 0; i < r.onsets.size() && i < 64; i++)
         onsets.push_back(R3(r.onsets[i]));
      out["onsets"] = onsets;

      json pitch = { { "pitchedRatio", R3(r.pitchedRatio) } };
      if (r.medianPitchHz > 0.0f)
      {
         pitch["medianHz"] = R1(r.medianPitchHz);
         pitch["median"] = NoteName(r.medianPitchHz);
         pitch["range"] = { NoteName(r.lowPitchHz), NoteName(r.highPitchHz) };
         // Consecutive frames on the same semitone, merged: the melody as notes.
         json melody = json::array();
         int curMidi = -1;
         double curStart = 0.0;
         auto flush = [&](double endTime)
         {
            if (curMidi >= 0 && melody.size() < 64)
               melody.push_back({ { "t", R3(curStart) }, { "note", NoteName((float)(440.0 * std::pow(2.0, (curMidi - 69) / 12.0))) },
                                  { "dur", R3(endTime - curStart) } });
         };
         for (const Frame& f : r.frames)
         {
            const int m = f.pitchHz > 0.0f ? (int)std::lround(69.0 + 12.0 * std::log2(f.pitchHz / 440.0)) : -1;
            if (m != curMidi)
            {
               flush(f.time);
               curMidi = m;
               curStart = f.time;
            }
         }
         flush(r.seconds);
         pitch["melody"] = melody;
      }
      out["pitch"] = pitch;

      if (r.tempoBpm > 0.0f)
         out["tempo"] = { { "bpm", R1(r.tempoBpm) }, { "confidence", R3(r.tempoConfidence) } };
      if (r.keyRoot >= 0)
      {
         json chroma = json::array();
         for (float c : r.chroma)
            chroma.push_back(R3(c));
         out["key"] = { { "name", std::string(kPitchNames[r.keyRoot]) + (r.keyMinor ? " minor" : " major") },
                        { "confidence", R3(r.keyConfidence) }, { "chroma", chroma } };
      }

      if (maxFrames > 0 && !r.frames.empty())
      {
         // Column-wise and decimated: far fewer tokens than an array of objects.
         const size_t n = r.frames.size();
         const size_t step = (n + (size_t)maxFrames - 1) / (size_t)maxFrames;
         json t = json::array(), rms = json::array(), pk = json::array(), cen = json::array(), hz = json::array(),
              bandRows = json::array();
         for (size_t i = 0; i < n; i += step)
         {
            const Frame& f = r.frames[i];
            t.push_back(R3(f.time));
            rms.push_back(R1(f.rmsDb));
            pk.push_back(R1(f.peakDb));
            cen.push_back((int)std::lround(f.centroidHz));
            hz.push_back(R1(f.pitchHz));
            json row = json::array();
            for (int b = 0; b < kBands; b++)
               row.push_back((int)std::lround(f.bandsDb[b]));
            bandRows.push_back(row);
         }
         out["timeline"] = { { "t", t }, { "rmsDb", rms }, { "peakDb", pk }, { "centroidHz", cen }, { "pitchHz", hz }, { "bandsDb", bandRows } };
      }
      return out;
   }

   json Compare(const Result& a, const Result& b, const std::vector<float>& monoA, const std::vector<float>& monoB, double sr)
   {
      json out;
      out["levelDiffDb"] = R1(a.rmsDb - b.rmsDb);
      json bandDiff = json::array();
      double absSum = 0.0;
      int counted = 0;
      for (int i = 0; i < kBands; i++)
      {
         const double d = a.bandsDb[i] - b.bandsDb[i];
         bandDiff.push_back(R1(d));
         if (a.bandsDb[i] > -70.0f || b.bandsDb[i] > -70.0f)
         {
            absSum += std::min(24.0, std::fabs(d));
            counted++;
         }
      }
      out["bandDiffDb"] = bandDiff;
      // Tonal balance after removing the overall level difference.
      double shapeSum = 0.0;
      for (int i = 0; i < kBands; i++)
         if (a.bandsDb[i] > -70.0f || b.bandsDb[i] > -70.0f)
            shapeSum += std::min(24.0, (double)std::fabs((a.bandsDb[i] - b.bandsDb[i]) - (a.rmsDb - b.rmsDb)));
      out["spectralSimilarity"] = R3(counted > 0 ? 1.0 - absSum / (24.0 * counted) : 1.0);
      out["spectralShapeSimilarity"] = R3(counted > 0 ? 1.0 - shapeSum / (24.0 * counted) : 1.0);

      // Waveform correlation at zero lag and at the best lag within +/-100 ms.
      const size_t n = std::min(monoA.size(), monoB.size());
      if (n > 0 && sr > 0.0)
      {
         const int maxLag = std::min((int)n - 1, (int)(0.1 * sr));
         const int m = NextPow2((int)n + maxLag + 1);
         std::vector<cfloat> fa((size_t)m, cfloat(0.0f, 0.0f)), fb((size_t)m, cfloat(0.0f, 0.0f));
         double ea = 0.0, eb = 0.0;
         for (size_t i = 0; i < n; i++)
         {
            fa[i] = cfloat(monoA[i], 0.0f);
            fb[i] = cfloat(monoB[i], 0.0f);
            ea += (double)monoA[i] * monoA[i];
            eb += (double)monoB[i] * monoB[i];
         }
         if (ea > 1e-9 && eb > 1e-9)
         {
            Fft(fa, false);
            Fft(fb, false);
            for (int i = 0; i < m; i++)
               fa[(size_t)i] = std::conj(fa[(size_t)i]) * fb[(size_t)i];
            Fft(fa, true); // fa[lag] = sum a_i * b_{i+lag} (negative lags wrap to the end)
            const double normE = std::sqrt(ea * eb);
            double best = -2.0;
            int bestLag = 0;
            for (int lag = -maxLag; lag <= maxLag; lag++)
            {
               const double c = fa[(size_t)(lag >= 0 ? lag : m + lag)].real() / normE;
               if (c > best)
               {
                  best = c;
                  bestLag = lag;
               }
            }
            out["correlation"] = R3(fa[0].real() / normE);
            out["bestLagMs"] = R1(1000.0 * bestLag / sr);
            out["correlationAtBestLag"] = R3(best);
         }
         else
            out["correlation"] = nullptr;
      }

      // Pitch agreement frame by frame (both pitched).
      if (a.medianPitchHz > 0.0f && b.medianPitchHz > 0.0f)
         out["medianPitchDiffCents"] = (int)std::lround(1200.0 * std::log2(a.medianPitchHz / b.medianPitchHz));
      int both = 0, agree = 0;
      for (size_t i = 0; i < std::min(a.frames.size(), b.frames.size()); i++)
         if (a.frames[i].pitchHz > 0.0f && b.frames[i].pitchHz > 0.0f)
         {
            both++;
            if (std::fabs(1200.0 * std::log2(a.frames[i].pitchHz / b.frames[i].pitchHz)) < 50.0)
               agree++;
         }
      if (both > 0)
         out["pitchAgreement"] = R3((double)agree / both);

      // Onsets, defined exactly like Audio Compare's "onset match" output:
      // the share of all onsets, on both sides, with a partner on the other
      // side within 50 ms (kOnsetToleranceSeconds). onsetRecall is the share
      // of A's onsets that B has; onsetOffsetMs how late B's partners are on
      // average. Times are compared as captured: a latency between the two
      // shows up in bestLagMs and onsetOffsetMs.
      if (!a.onsets.empty() || !b.onsets.empty())
      {
         constexpr float kOnsetToleranceSeconds = 0.05f;
         auto partnered = [&](const std::vector<float>& from, const std::vector<float>& to, double* offsetSum)
         {
            int matched = 0;
            for (float t : from)
            {
               float bestD = 1e9f;
               for (float u : to)
                  if (std::fabs(u - t) < std::fabs(bestD))
                     bestD = u - t;
               if (std::fabs(bestD) <= kOnsetToleranceSeconds)
               {
                  matched++;
                  if (offsetSum)
                     *offsetSum += bestD;
               }
            }
            return matched;
         };
         double offsetSum = 0.0;
         const int matchedA = partnered(a.onsets, b.onsets, &offsetSum);
         const int matchedB = partnered(b.onsets, a.onsets, nullptr);
         out["onsetMatch"] = R3((double)(matchedA + matchedB) / (double)(a.onsets.size() + b.onsets.size()));
         if (!a.onsets.empty())
            out["onsetRecall"] = R3((double)matchedA / a.onsets.size());
         if (matchedA > 0)
            out["onsetOffsetMs"] = R1(1000.0 * offsetSum / matchedA);
      }

      // Loudness contour agreement.
      const size_t fn = std::min(a.frames.size(), b.frames.size());
      if (fn >= 3)
      {
         std::vector<double> ea(fn), eb(fn);
         for (size_t i = 0; i < fn; i++)
         {
            ea[i] = std::max(-80.0f, a.frames[i].rmsDb);
            eb[i] = std::max(-80.0f, b.frames[i].rmsDb);
         }
         out["envelopeCorrelation"] = R3(Pearson(ea.data(), eb.data(), (int)fn));
      }
      return out;
   }
}
