#include "SoundAnalyzeNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "audio/dsp/EquationDsp.h"
#include "core/AudioDecodeCache.h"
#include "platform/PlatformAudio.h"

namespace
{
   const char* const kOutputLabels[SoundAnalyzeNode::kOutputCount] = {
      "f0", "attack", "decay", "sustain", "release", "bright", "noise",
      "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8"
   };
}

SoundAnalyzeNode::SoundAnalyzeNode()
{
   for (int i = 0; i < kOutputCount; i++)
   {
      mTaps[i].owner = this;
      mTaps[i].index = i;
   }
}

SoundAnalyzeNode::~SoundAnalyzeNode()
{
   // The worker only holds its own copies (mono buffer, options), but a
   // std::async future blocks in its destructor anyway - say so explicitly.
   if (mJob.valid())
      mJob.wait();
}

const char* SoundAnalyzeNode::OutputLabel(int index) const
{
   return (index >= 0 && index < kOutputCount) ? kOutputLabels[index] : "out";
}

IModulator* SoundAnalyzeNode::ModulatorOutput(int index)
{
   return (index >= 0 && index < kOutputCount) ? &mTaps[index] : nullptr;
}

SoundAnalysis::Options SoundAnalyzeNode::CurrentOptions() const
{
   SoundAnalysis::Options o;
   o.startSeconds = std::max(0.0f, startSec);
   o.lengthSeconds = std::max(0.0f, lengthSec);
   o.f0HintHz = std::max(0.0f, f0Hint);
   o.partials = std::clamp(partials, 1, SoundAnalysis::kMaxPartials);
   return o;
}

float SoundAnalyzeNode::Value(int index) const
{
   if (!mHaveProfile || index < 0 || index >= kOutputCount)
      return 0.0f;
   const SoundAnalysis::SoundProfile& p = mProfile;
   switch (index)
   {
   case kF0:
   {
      if (!p.pitched || !(p.f0Hz > 0.0f))
         return 0.0f;
      const double midi = 69.0 + 12.0 * std::log2((double)p.f0Hz / 440.0);
      return std::clamp((float)(midi / 127.0), 0.0f, 1.0f);
   }
   case kAttack:
      return std::clamp(p.attackMs / kEnvelopeRangeMs, 0.0f, 1.0f);
   case kDecay:
      return std::clamp(p.decayMs / kEnvelopeRangeMs, 0.0f, 1.0f);
   case kSustain:
      return std::clamp(p.sustain, 0.0f, 1.0f);
   case kRelease:
      return std::clamp(p.releaseMs / kEnvelopeRangeMs, 0.0f, 1.0f);
   case kBrightness:
      return std::clamp(p.brightness, 0.0f, 1.0f);
   case kNoisiness:
      return std::clamp(p.noisiness, 0.0f, 1.0f);
   default:
   {
      const size_t k = (size_t)(index - kPartial0);
      if (k < p.partials.size() && p.partials[k].present)
         return std::clamp(p.partials[k].amp, 0.0f, 1.0f);
      return 0.0f;
   }
   }
}

void SoundAnalyzeNode::SetProfile(SoundAnalysis::SoundProfile profile)
{
   mProfile = std::move(profile);
   mJson = SoundAnalysis::ToJson(mProfile);
   mJson["file"] = path;
   mHaveProfile = mProfile.valid;
}

void SoundAnalyzeNode::LoadFile()
{
   // A run on the previous file must not land on this one. Analysis takes a
   // fraction of a second, so waiting here is cheaper than a cancel path.
   if (mJob.valid())
      mJob.wait();
   mJob = std::future<SoundAnalysis::SoundProfile>();
   mLoadedPath = path;
   mMono.reset();
   mOverviewMin.clear();
   mOverviewMax.clear();
   mDurationSeconds = 0.0;
   mSampleRate = 0.0;
   mHaveResultForOptions = false;
   mHaveProfile = false;
   mProfile = SoundAnalysis::SoundProfile();
   mJson = nlohmann::json();

   if (path.empty())
   {
      mState = State::Empty;
      mStatus.clear();
      return;
   }

   // Decoding stays on the main thread (Platform's decoders are main-thread
   // only); the analysis runs on the worker.
   Platform::SampleBuffer buffer;
   std::string err;
   if (!AudioDecodeCache::DecodeCached(path, buffer, err) || buffer.numFrames <= 0 || buffer.channels <= 0 ||
       buffer.sampleRate <= 0.0 || buffer.channelData.size() < (size_t)buffer.channels * (size_t)buffer.numFrames)
   {
      mState = State::Failed;
      mStatus = err.empty() ? "could not read the audio file" : err;
      return;
   }

   const size_t frames = (size_t)buffer.numFrames;
   std::vector<float> mono(frames, 0.0f);
   for (int ch = 0; ch < buffer.channels; ch++)
   {
      const float* src = buffer.channelData.data() + (size_t)ch * frames;
      for (size_t i = 0; i < frames; i++)
         mono[i] += src[i];
   }
   const float inv = 1.0f / (float)buffer.channels;
   for (float& v : mono)
      v *= inv;

   mOverviewMin.assign(kOverviewColumns, 0.0f);
   mOverviewMax.assign(kOverviewColumns, 0.0f);
   for (int c = 0; c < kOverviewColumns; c++)
   {
      const size_t a = (size_t)c * frames / kOverviewColumns;
      const size_t b = std::max(a + 1, (size_t)(c + 1) * frames / kOverviewColumns);
      float lo = 0.0f, hi = 0.0f;
      for (size_t i = a; i < b && i < frames; i++)
      {
         lo = std::min(lo, mono[i]);
         hi = std::max(hi, mono[i]);
      }
      mOverviewMin[(size_t)c] = lo;
      mOverviewMax[(size_t)c] = hi;
   }

   mSampleRate = buffer.sampleRate;
   mDurationSeconds = (double)frames / buffer.sampleRate;
   mMono = std::make_shared<const std::vector<float>>(std::move(mono));
   mState = State::Analyzing;
   mStatus = "analyzing...";
}

void SoundAnalyzeNode::PollAnalysis()
{
   if (!mJob.valid() || mJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
   SoundAnalysis::SoundProfile r = mJob.get();
   mProfileOptions = mJobOptions;
   mHaveResultForOptions = true;
   if (r.valid)
   {
      SetProfile(std::move(r));
      mState = State::Ready;
      mStatus.clear();
   }
   else
   {
      // Outputs must not keep reporting a region that no longer analyses.
      mHaveProfile = false;
      mState = State::Failed;
      mStatus = r.error.empty() ? "analysis failed" : r.error;
   }
}

void SoundAnalyzeNode::StartAnalysisIfStale()
{
   if (!mMono || mJob.valid())
      return;
   const SoundAnalysis::Options want = CurrentOptions();
   if (mHaveResultForOptions && want == mProfileOptions)
      return;
   // One run at a time; a knob that kept moving is picked up by the next one.
   mJobOptions = want;
   std::shared_ptr<const std::vector<float>> mono = mMono;
   const double sr = mSampleRate;
   mJob = std::async(std::launch::async, [mono, sr, want]()
                     { return SoundAnalysis::AnalyzeSound(mono->data(), mono->size(), sr, want); });
   mState = State::Analyzing;
   mStatus = "analyzing...";
}

void SoundAnalyzeNode::CookIfNeeded(int frameId)
{
   if (frameId == mLastCookFrame)
      return;
   mLastCookFrame = frameId;
   // A path set by a patch load, the file picker or a drop is read here, once per change.
   if (path != mLoadedPath)
      LoadFile();
   PollAnalysis();
   StartAnalysisIfStale();
}

void SoundAnalyzeNode::VisitParams(ParamVisitor& v)
{
   v.Text("path", path);
   v.Float("start", startSec);
   v.Float("length", lengthSec);
   v.Float("f0Hint", f0Hint);
   v.Int("partials", partials);
}

// ---------------------------------------------------------------- self-test

bool SoundAnalyzeNode::RunSelfTest()
{
   bool ok = true;
   auto check = [&ok](bool cond, const char* what)
   {
      if (!cond)
      {
         printf("SOUNDANALYSISTEST FAIL: %s\n", what);
         ok = false;
      }
   };
   auto within = [](double v, double want, double tol) { return std::fabs(v - want) <= tol; };
   const double kPi = 3.14159265358979323846;
   const double sr = 44100.0;
   uint32_t seed = 12345u;
   auto noise = [&seed]()
   {
      seed = seed * 1664525u + 1013904223u;
      return (double)(seed >> 8) / 16777216.0 - 0.5;
   };
   auto printPartials = [](const char* tag, const SoundAnalysis::SoundProfile& p)
   {
      printf("SOUNDANALYSISTEST %s: valid=%d f0=%.2f B=%.2e att=%.1fms dec=%.1fms sus=%.2f rel=%.1fms%s nhr=%.1fdB\n", tag,
             (int)p.valid, p.f0Hz, p.inharmonicity, p.attackMs, p.decayMs, p.sustain, p.releaseMs,
             p.releaseMeasured ? "(measured)" : "", p.noiseToHarmonicDb);
      for (const SoundAnalysis::Partial& q : p.partials)
         if (q.present)
            printf("   k=%d %.2f Hz %.2f dB T60 %.3f s%s\n", q.harmonic, q.freqHz, q.ampDb, q.decaySec,
                   q.sustains ? " (sustains)" : "");
   };

   // 1. A struck 220 Hz tone: harmonics 1, 1/2, 1/3 with T60s of 1.5, 1.0 and
   // 0.6 s, a 10 ms linear attack and a -80 dB noise floor, 2 s long.
   const double amps1[3] = { 1.0, 0.5, 1.0 / 3.0 };
   const double t60s[3] = { 1.5, 1.0, 0.6 };
   std::vector<float> tone1((size_t)(2.0 * sr));
   for (size_t i = 0; i < tone1.size(); i++)
   {
      const double t = (double)i / sr;
      const double att = std::min(1.0, t / 0.01);
      double s = 0.0;
      for (int h = 0; h < 3; h++)
         s += amps1[h] * std::pow(10.0, -3.0 * t / t60s[h]) * std::sin(2.0 * kPi * 220.0 * (h + 1) * t);
      tone1[i] = (float)(0.5 * att * s + 1e-4 * noise());
   }
   SoundAnalysis::SoundProfile p1;
   {
      SoundAnalysis::Options o;
      o.partials = 8;
      const auto t0 = std::chrono::steady_clock::now();
      p1 = SoundAnalysis::AnalyzeSound(tone1.data(), tone1.size(), sr, o);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      printPartials("struck", p1);
      printf("SOUNDANALYSISTEST struck analysed in %.0f ms\n", ms);
      check(p1.valid && p1.pitched, "struck tone: valid and pitched");
      check(within(p1.f0Hz, 220.0, 1.0), "struck tone: f0 220 Hz +-1");
      if (p1.partials.size() >= 4)
      {
         const double wantDb[3] = { 0.0, -6.02, -9.54 };
         for (int h = 0; h < 3; h++)
         {
            const SoundAnalysis::Partial& q = p1.partials[(size_t)h];
            char what[96];
            snprintf(what, sizeof(what), "struck tone: partial %d present, %.2f dB +-1.5", h + 1, wantDb[h]);
            check(q.present && within(q.ampDb, wantDb[h], 1.5), what);
            snprintf(what, sizeof(what), "struck tone: partial %d T60 %.1f s +-20%%", h + 1, t60s[h]);
            check(q.present && within(q.decaySec, t60s[h], 0.2 * t60s[h]), what);
            snprintf(what, sizeof(what), "struck tone: partial %d frequency", h + 1);
            check(q.present && within(q.freqHz, 220.0 * (h + 1), 1.5), what);
         }
         check(!p1.partials[3].present || p1.partials[3].ampDb < -40.0f, "struck tone: no 4th harmonic");
      }
      check(p1.inharmonicity < 2e-4f, "struck tone: harmonic (B ~ 0)");
      check(p1.attackMs >= 4.0f && p1.attackMs <= 25.0f, "struck tone: attack ~10 ms");
      check(!p1.sustained && p1.sustain < 0.05f, "struck tone: no sustain plateau");
      check(p1.decayMs > 300.0f && p1.decayMs < 900.0f, "struck tone: decay to -20 dB ~0.55 s");
      check(p1.noisiness < 0.1f, "struck tone: harmonic, not noisy");
      // Release builds take a small fraction of this; the bound only catches a
      // gross regression (and leaves room for an unoptimised Debug build).
      check(ms < 4000.0, "struck tone: analysis time");
   }

   // 2. The same tone from 0.5 s for 1 s with an f0 hint: levels are those at
   // the region start (p2 is 16 dB under p1 there), decays unchanged.
   {
      SoundAnalysis::Options o;
      o.startSeconds = 0.5;
      o.lengthSeconds = 1.0;
      o.f0HintHz = 220.0;
      o.partials = 4;
      const SoundAnalysis::SoundProfile p = SoundAnalysis::AnalyzeSound(tone1.data(), tone1.size(), sr, o);
      printPartials("region", p);
      check(p.valid && within(p.startSeconds, 0.5, 1e-3) && within(p.seconds, 1.0, 1e-3), "region: start/length honoured");
      // At 0.5 s p1 (T60 1.5 s) is down 20 dB and p2 (T60 1 s, -6 dB at onset) 36 dB: 16 dB apart.
      const double want2 = 20.0 * std::log10(0.5 * std::pow(10.0, -1.5)) - 20.0 * std::log10(std::pow(10.0, -1.0)); // -16.02
      check(p.partials.size() >= 2 && p.partials[1].present && within(p.partials[1].ampDb, want2, 1.5),
            "region: partial 2 level at the region start");
      check(!p.partials.empty() && p.partials[0].present && within(p.partials[0].decaySec, 1.5, 0.3),
            "region: partial 1 T60 unchanged");
   }

   // 3. A sustained stiff string: F = 110 Hz, B = 3e-4, 8 partials at 1/k, a
   // 50 ms attack, held to 1.4 s, then a 200 ms linear release.
   {
      const double F = 110.0, B = 3e-4;
      std::vector<float> tone((size_t)(2.0 * sr));
      for (size_t i = 0; i < tone.size(); i++)
      {
         const double t = (double)i / sr;
         double env = std::min(1.0, t / 0.05);
         if (t > 1.4)
            env = std::max(0.0, 1.0 - (t - 1.4) / 0.2);
         double s = 0.0;
         for (int k = 1; k <= 8; k++)
            s += (1.0 / k) * std::sin(2.0 * kPi * k * F * std::sqrt(1.0 + B * k * k) * t);
         tone[i] = (float)(0.3 * env * s + 1e-4 * noise());
      }
      SoundAnalysis::Options o;
      o.partials = 12;
      const SoundAnalysis::SoundProfile p = SoundAnalysis::AnalyzeSound(tone.data(), tone.size(), sr, o);
      printPartials("string", p);
      check(p.valid && p.pitched, "string: valid and pitched");
      check(within(p.f0Hz, F * std::sqrt(1.0 + B), 0.5), "string: f0 = F sqrt(1+B) +-0.5 Hz");
      check(within(p.inharmonicity, B, 1e-4), "string: inharmonicity 3e-4 +-1e-4");
      if (p.partials.size() >= 8)
      {
         check(p.partials[1].present && within(p.partials[1].ampDb, -6.02, 1.5), "string: partial 2 at -6 dB");
         check(p.partials[3].present && within(p.partials[3].ampDb, -12.04, 1.5), "string: partial 4 at -12 dB");
         check(p.partials[7].present && within(p.partials[7].freqHz, 8.0 * F * std::sqrt(1.0 + B * 64.0), 1.0),
               "string: partial 8 stretched");
         check(p.partials[0].sustains, "string: partial 1 sustains (fit stops at the release)");
      }
      check(p.sustained && p.sustain > 0.8f, "string: sustain plateau ~1");
      check(p.attackMs >= 30.0f && p.attackMs <= 75.0f, "string: attack ~50 ms");
      check(p.releaseMeasured && within(p.releaseMs, 200.0, 50.0), "string: release ~200 ms");
   }

   // 4. Silence and an out-of-range region are errors, not crashes.
   {
      std::vector<float> silence((size_t)sr, 0.0f);
      check(!SoundAnalysis::AnalyzeSound(silence.data(), silence.size(), sr, SoundAnalysis::Options()).valid,
            "silence is reported, not analysed");
      SoundAnalysis::Options o;
      o.startSeconds = 5.0;
      check(!SoundAnalysis::AnalyzeSound(tone1.data(), tone1.size(), sr, o).valid, "start past the end is an error");
   }

   // 5. Synth helpers: the additive formula parses as an Equation Synth
   // formula, and a saw spectrum matches a saw-like profile better than a sine.
   {
      const std::string formula = SoundAnalysis::AdditiveFormula(p1, 16, -48.0f);
      printf("SOUNDANALYSISTEST formula: %s\n", formula.c_str());
      EquationDsp::AstNodePtr ast;
      std::string err;
      check(EquationDsp::Parser::Parse(formula, ast, err) && ast != nullptr, "additive formula parses");
      check(formula.find("sin(2*pi*x)") != std::string::npos && formula.find("sin(6*pi*x)") != std::string::npos,
            "additive formula has harmonics 1 and 3");
      float saw[8], sine[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
      for (int k = 0; k < 8; k++)
         saw[k] = 1.0f / (float)(k + 1);
      check(SoundAnalysis::HarmonicMismatchDb(p1, saw, 3) < SoundAnalysis::HarmonicMismatchDb(p1, sine, 3),
            "1, 1/2, 1/3 is closer to a saw than to a sine");
      check(!SoundAnalysis::ToJson(p1)["partials"].empty(), "ToJson lists the partials");
   }

   // 6. Outputs: the documented 0..1 mappings, read from a node holding p1.
   {
      SoundAnalyzeNode node;
      node.path = "selftest";
      node.SetProfile(p1);
      check(within(node.Value(kF0), (69.0 + 12.0 * std::log2(p1.f0Hz / 440.0)) / 127.0, 1e-4), "f0 output = midi / 127");
      check(within(node.Value(kF0), 57.0 / 127.0, 0.01), "f0 output for 220 Hz ~ 57/127");
      check(within(node.Value(kAttack), p1.attackMs / 4000.0, 1e-5), "attack output = ms / 4000");
      check(within(node.Value(kPartial0), 1.0, 0.02), "p1 output = 1 (strongest)");
      check(within(node.Value(kPartial0 + 1), 0.5, 0.1), "p2 output ~ 0.5");
      check(node.Value(kPartial0 + 3) < 0.02f, "p4 output ~ 0 (absent)");
      check(node.Value(kPartial0) == node.Value(kPartial0), "outputs are a pure read");
      check(node.ProfileJson() != nullptr && (*node.ProfileJson())["file"] == "selftest", "ProfileJson carries the file");
      SoundAnalyzeNode empty;
      check(empty.ProfileJson() == nullptr && empty.Value(kF0) == 0.0f, "no profile: null json, zero outputs");
   }

   printf("SOUNDANALYSISTEST %s\n", ok ? "OK" : "FAIL");
   return ok;
}
