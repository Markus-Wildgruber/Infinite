#include "SoundAnalyzeNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "audio/dsp/BasicPitch.h"
#include "audio/dsp/EquationDsp.h"
#include "core/AudioDecodeCache.h"
#include "platform/PlatformAudio.h"

namespace
{
   const char* const kOutputLabels[SoundAnalyzeNode::kOutputCount] = {
      "f0", "attack", "decay", "sustain", "release", "bright", "noise",
      "p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8",
      "transient", "transientDecay", "air", "keyLevel", "keyTilt"
   };

   template <typename T>
   bool IsReady(const std::future<T>& f)
   {
      return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
   }
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
   // The transcription worker writes mProgress: stop and join it first. The
   // analysis worker only holds its own copies (mono buffer, notes, options),
   // but a std::async future blocks in its destructor anyway - say so explicitly.
   StopTranscription();
   if (mJobCancel)
      mJobCancel->store(true); // a notes run stops between notes
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

SoundAnalysis::MultiNoteOptions SoundAnalyzeNode::CurrentMultiOptions() const
{
   SoundAnalysis::MultiNoteOptions o;
   o.partials = std::clamp(partials, 1, SoundAnalysis::kMaxPartials);
   o.lowMidi = std::clamp(lowNote, BasicPitch::kLowestMidi, BasicPitch::kLowestMidi + BasicPitch::kKeys - 1);
   o.highMidi = std::clamp(highNote, o.lowMidi, BasicPitch::kLowestMidi + BasicPitch::kKeys - 1);
   return o;
}

SoundAnalyzeNode::Request SoundAnalyzeNode::CurrentRequest() const
{
   Request r;
   r.mode = mode == kModeNotes ? kModeNotes : kModeRegion;
   r.single = CurrentOptions();
   const SoundAnalysis::MultiNoteOptions m = CurrentMultiOptions();
   r.lowNote = m.lowMidi;
   r.highNote = m.highMidi;
   return r;
}

float SoundAnalyzeNode::Value(int index) const
{
   if (!mHaveProfile || index < 0 || index >= kOutputCount)
      return 0.0f;
   const SoundAnalysis::SoundProfile& p = mProfile;
   auto keyOut = [](const SoundAnalysis::KeyFit* f)
   {
      const float db = (f != nullptr && f->valid) ? f->dbPerOct : 0.0f;
      return std::clamp((db + kKeyRangeDbPerOct) / (2.0f * kKeyRangeDbPerOct), 0.0f, 1.0f);
   };
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
   case kTransient:
      return std::clamp(p.transient, 0.0f, 1.0f);
   case kTransientDecay:
      return std::clamp(p.transientDecayMs / kTransientDecayRangeMs, 0.0f, 1.0f);
   case kAir:
      return std::clamp(p.noiseLevel, 0.0f, 1.0f);
   case kKeyLevel:
      return keyOut(mMulti ? &mMulti->keyLevel : nullptr);
   case kKeyTilt:
      return keyOut(mMulti ? &mMulti->keyTilt : nullptr);
   default:
   {
      const size_t k = (size_t)(index - kPartial0);
      if (index >= kPartial0 && index < kPartial0 + kPartialOutputs && k < p.partials.size() && p.partials[k].present)
         return std::clamp(p.partials[k].amp, 0.0f, 1.0f);
      return 0.0f;
   }
   }
}

void SoundAnalyzeNode::SetProfile(SoundAnalysis::SoundProfile profile,
                                  std::shared_ptr<const SoundAnalysis::MultiNoteResult> multi)
{
   mProfile = std::move(profile);
   mMulti = std::move(multi);
   mJson = SoundAnalysis::ToJson(mProfile);
   mJson["file"] = path;
   if (mMulti)
   {
      mJson["mode"] = "notes";
      mJson["multiNote"] = SoundAnalysis::ToJson(*mMulti);
   }
   else
      mJson["mode"] = "region";
   mHaveProfile = mProfile.valid;
}

void SoundAnalyzeNode::StopTranscription()
{
   if (mCancel)
      mCancel->store(true);
   if (mTranscribe.valid())
      mTranscribe.wait(); // a cancelled run stops within one model window
   mTranscribe = std::future<TranscribeResult>();
   mCancel.reset();
}

void SoundAnalyzeNode::LoadFile()
{
   // A run on the previous file must not land on this one: the model run and
   // a notes analysis are cancelled (a region analysis takes a fraction of a
   // second and is simply waited for).
   StopTranscription();
   if (mJobCancel)
      mJobCancel->store(true);
   if (mJob.valid())
      mJob.wait();
   mJob = std::future<JobResult>();
   mJobCancel.reset();
   mLoadedPath = path;
   mMono.reset();
   mOverviewMin.clear();
   mOverviewMax.clear();
   mDurationSeconds = 0.0;
   mSampleRate = 0.0;
   mHaveResultForOptions = false;
   mHaveProfile = false;
   mProfile = SoundAnalysis::SoundProfile();
   mMulti.reset();
   mJson = nlohmann::json();
   mNotes.reset();
   mTranscribeFailed = false;
   mTranscribeError.clear();
   mProgress.store(0.0f);

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

void SoundAnalyzeNode::StartTranscription()
{
   // The model reads the mono mix (what the analysis sees too), resampled to
   // its 22.05 kHz; default settings - the note range is applied afterwards
   // by AnalyzeNotes, so moving lowNote/highNote never re-runs the model.
   mProgress.store(0.0f);
   mCancel = std::make_shared<std::atomic<bool>>(false);
   std::shared_ptr<std::atomic<bool>> cancel = mCancel;
   std::shared_ptr<const std::vector<float>> mono = mMono;
   const double sr = mSampleRate;
   std::atomic<float>* progress = &mProgress;
   mTranscribe = std::async(std::launch::async, [mono, sr, cancel, progress]() -> TranscribeResult
   {
      TranscribeResult r;
      std::vector<float> input;
      if (!BasicPitch::ToModelInput(mono->data(), 1, (int)mono->size(), sr, input, cancel.get()))
      {
         r.error = cancel->load() ? "cancelled" : "could not prepare the audio for the note model";
         return r;
      }
      BasicPitch::Activations act;
      if (!BasicPitch::RunModel(input, act, r.error, progress, cancel.get()))
      {
         if (r.error.empty())
            r.error = "the note model failed";
         return r;
      }
      const std::vector<BasicPitch::Note> found = BasicPitch::NotesFromActivations(act, BasicPitch::Settings());
      auto notes = std::make_shared<std::vector<SoundAnalysis::NoteEvent>>();
      notes->reserve(found.size());
      for (const BasicPitch::Note& n : found)
      {
         SoundAnalysis::NoteEvent e;
         e.start = n.start;
         e.end = n.end;
         e.midi = n.pitch;
         e.amplitude = n.amplitude;
         notes->push_back(e);
      }
      r.notes = std::move(notes);
      return r;
   });
}

void SoundAnalyzeNode::PollTranscription()
{
   if (!IsReady(mTranscribe))
      return;
   TranscribeResult r = mTranscribe.get();
   mCancel.reset();
   if (r.notes)
   {
      mNotes = std::move(r.notes);
      mProgress.store(1.0f);
   }
   else
   {
      // Not retried for this file (Reload() or a new file clears it): on a
      // build without the model it would only fail again.
      mTranscribeFailed = true;
      mTranscribeError = "notes mode: " + (r.error.empty() ? std::string("transcription failed") : r.error);
   }
}

void SoundAnalyzeNode::PollAnalysis()
{
   if (!IsReady(mJob))
      return;
   JobResult r = mJob.get();
   mProfileRequest = mJobRequest;
   mHaveResultForOptions = true;
   if (r.profile.valid)
   {
      SetProfile(std::move(r.profile), std::move(r.multi));
      mState = State::Ready;
      mStatus.clear();
   }
   else
   {
      // Outputs must not keep reporting a region that no longer analyses.
      mHaveProfile = false;
      mMulti.reset();
      mState = State::Failed;
      if (r.multi && !r.multi->error.empty())
         mStatus = r.multi->error;
      else
         mStatus = r.profile.error.empty() ? "analysis failed" : r.profile.error;
   }
}

void SoundAnalyzeNode::StartAnalysisIfStale()
{
   if (!mMono || mJob.valid())
      return;
   const Request want = CurrentRequest();
   if (mHaveResultForOptions && want == mProfileRequest)
      return;

   std::shared_ptr<const std::vector<float>> mono = mMono;
   const double sr = mSampleRate;
   if (want.mode == kModeNotes)
   {
      if (!mNotes)
      {
         if (mTranscribeFailed)
         {
            mProfileRequest = want;
            mHaveResultForOptions = true;
            mHaveProfile = false;
            mMulti.reset();
            mState = State::Failed;
            mStatus = mTranscribeError;
            return;
         }
         if (!mTranscribe.valid())
            StartTranscription();
         mState = State::Analyzing;
         mStatus = "transcribing notes...";
         return;
      }
      // One run at a time; a knob that kept moving is picked up by the next one.
      mJobRequest = want;
      std::shared_ptr<const std::vector<SoundAnalysis::NoteEvent>> notes = mNotes;
      const SoundAnalysis::MultiNoteOptions mopt = CurrentMultiOptions();
      mJobCancel = std::make_shared<std::atomic<bool>>(false);
      std::shared_ptr<std::atomic<bool>> cancel = mJobCancel;
      mJob = std::async(std::launch::async, [mono, sr, notes, mopt, cancel]() -> JobResult
      {
         JobResult r;
         auto multi = std::make_shared<SoundAnalysis::MultiNoteResult>(
            SoundAnalysis::AnalyzeNotes(mono->data(), mono->size(), sr, *notes, mopt, cancel.get()));
         r.profile = multi->profile;
         if (!multi->valid)
         {
            r.profile.valid = false;
            r.profile.error = multi->error;
         }
         r.multi = std::move(multi);
         return r;
      });
   }
   else
   {
      mJobRequest = want;
      const SoundAnalysis::Options opt = want.single;
      mJob = std::async(std::launch::async, [mono, sr, opt]() -> JobResult
      {
         JobResult r;
         r.profile = SoundAnalysis::AnalyzeSound(mono->data(), mono->size(), sr, opt);
         return r;
      });
   }
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
   PollTranscription();
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
   v.Int("mode", mode);
   v.Int("lowNote", lowNote);
   v.Int("highNote", highNote);
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

   // 6. Attack transient: the struck 220 Hz tone with a 1 ms attack at 50 ms,
   // plus a white-noise burst whose RMS at the onset is half the tone's and
   // that decays to -60 dB in 20 ms; and the same tone clean.
   auto struckTone = [&](bool burst)
   {
      std::vector<float> s((size_t)(1.5 * sr), 0.0f);
      const double onset = 0.05;
      const double toneRms = 0.5 * std::sqrt((1.0 + 0.25 + 1.0 / 9.0) / 2.0);
      for (size_t i = 0; i < s.size(); i++)
      {
         const double t = (double)i / sr - onset;
         double v = 0.0;
         if (t >= 0.0)
         {
            for (int h = 0; h < 3; h++)
               v += amps1[h] * std::pow(10.0, -3.0 * t / t60s[h]) * std::sin(2.0 * kPi * 220.0 * (h + 1) * t);
            v *= 0.5 * std::min(1.0, t / 0.001);
            if (burst) // noise() is uniform in +-0.5: RMS 1/sqrt(12)
               v += 0.5 * toneRms * std::pow(10.0, -3.0 * t / 0.02) * noise() * std::sqrt(12.0);
         }
         s[i] = (float)(v + 1e-4 * noise());
      }
      return s;
   };
   SoundAnalysis::SoundProfile pClean;
   {
      const std::vector<float> hit = struckTone(true);
      const SoundAnalysis::SoundProfile p = SoundAnalysis::AnalyzeSound(hit.data(), hit.size(), sr, SoundAnalysis::Options());
      printf("SOUNDANALYSISTEST burst: onset %.4f s transient %.3f decay %.1f ms (high %.3f / %.1f ms) crest %.1f dB\n",
             p.onsetTime, p.transient, p.transientDecayMs, p.transientHigh, p.transientHighDecayMs, p.attackCrestDb);
      check(p.valid && p.transientMeasured, "burst: transient measured");
      check(within(p.onsetTime, 0.05, 0.002), "burst: onset at 50 ms +-2");
      check(p.transient > 0.2f, "burst: transient > 0.2 (0.5 synthesised)");
      check(within(p.transientDecayMs, 20.0, 10.0), "burst: decay 20 ms +-50%");
      const std::vector<float> clean = struckTone(false);
      pClean = SoundAnalysis::AnalyzeSound(clean.data(), clean.size(), sr, SoundAnalysis::Options());
      printf("SOUNDANALYSISTEST clean: transient %.4f decay %.1f ms crest %.1f dB air %.4f @ %.0f Hz\n", pClean.transient,
             pClean.transientDecayMs, pClean.attackCrestDb, pClean.noiseLevel, pClean.noiseToneHz);
      check(pClean.valid && pClean.transientMeasured && pClean.transient < 0.05f, "clean tone: transient < 0.05");
      check(p.attackCrestDb > pClean.attackCrestDb, "burst: crest above the clean tone's");
      check(pClean.airMeasured && pClean.noiseLevel < 0.02f, "clean tone: no air");
      const nlohmann::json j = SoundAnalysis::ToJson(p);
      check(j.contains("transient") && j["transient"].contains("decayMs") && j.contains("air"), "ToJson has transient and air");
   }

   // 7. Air: a held 220 Hz tone (8 harmonics at 1/k) plus white noise through
   // a 4th-order Butterworth highpass at 4 kHz, at 1/10 of the tone's RMS.
   {
      const size_t n = (size_t)(2.0 * sr);
      std::vector<double> hiss(n);
      for (size_t i = 0; i < n; i++)
         hiss[i] = noise();
      // Two RBJ highpass biquads, Q of the 4th-order Butterworth pair.
      const double qs[2] = { 0.54119610, 1.30656296 };
      for (double q : qs)
      {
         const double w0 = 2.0 * kPi * 4000.0 / sr, al = std::sin(w0) / (2.0 * q), c = std::cos(w0), a0 = 1.0 + al;
         const double b0 = (1.0 + c) * 0.5 / a0, b1 = -(1.0 + c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - al) / a0;
         double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
         for (double& v : hiss)
         {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            v = y;
         }
      }
      double ss = 0.0;
      for (size_t i = n / 4; i < n; i++)
         ss += hiss[i] * hiss[i];
      double toneSs = 0.0;
      for (int k = 1; k <= 8; k++)
         toneSs += 0.5 / (double)(k * k);
      const double gain = 0.1 * 0.3 * std::sqrt(toneSs) / std::sqrt(ss / (double)(n - n / 4));
      std::vector<float> tone(n);
      for (size_t i = 0; i < n; i++)
      {
         const double t = (double)i / sr;
         double s = 0.0;
         for (int k = 1; k <= 8; k++)
            s += (1.0 / k) * std::sin(2.0 * kPi * 220.0 * k * t);
         tone[i] = (float)(std::min(1.0, t / 0.05) * (0.3 * s + gain * hiss[i]) + 1e-4 * noise());
      }
      SoundAnalysis::Options o;
      o.partials = 12;
      const SoundAnalysis::SoundProfile p = SoundAnalysis::AnalyzeSound(tone.data(), tone.size(), sr, o);
      printf("SOUNDANALYSISTEST air: noise level %.3f (%.1f dB) corner %.0f Hz, noisiness %.3f\n", p.noiseLevel,
             20.0 * std::log10(std::max(1e-4f, p.noiseLevel)), p.noiseToneHz, p.noisiness);
      check(p.valid && p.pitched && p.airMeasured, "air: measured");
      check(p.noiseLevel > 0.05f && p.noiseLevel < 0.2f, "air: level ~0.1 (-20 dB)");
      check(p.noiseToneHz >= 2000.0f && p.noiseToneHz <= 6000.0f, "air: corner 2..6 kHz");
   }

   // 8. Multi-note: 11 struck notes across two octaves, 1 s apart, whose
   // fundamental falls 3 dB/oct and whose 2nd harmonic rises 6 dB/oct
   // against it (-12 dB at C4); the 3rd stays at -18 dB. The note list is
   // given exactly, plus a transcriber's octave ghost on the C4 (must not cost
   // it its isolation) and a phantom note 50 ms after the D4 (takes the D4 out).
   std::shared_ptr<SoundAnalysis::MultiNoteResult> multi;
   {
      const int midis[11] = { 48, 50, 53, 55, 57, 60, 62, 65, 67, 69, 72 };
      const double t60n[4] = { 1.2, 0.9, 0.7, 0.5 };
      const double spacing = 1.0, first = 0.1;
      std::vector<float> song((size_t)((first + 11 * spacing + 0.3) * sr), 0.0f);
      std::vector<SoundAnalysis::NoteEvent> notes;
      for (int i = 0; i < 11; i++)
      {
         const double oct = (midis[i] - 60) / 12.0;
         const double f0 = 440.0 * std::pow(2.0, (midis[i] - 69) / 12.0);
         const double rel[4] = { 1.0, std::pow(10.0, (-12.0 + 6.0 * oct) / 20.0), std::pow(10.0, -18.0 / 20.0),
                                 std::pow(10.0, -24.0 / 20.0) };
         const double a1 = 0.25 * std::pow(10.0, -3.0 * oct / 20.0);
         const double t0 = first + i * spacing;
         const size_t s0 = (size_t)std::lround(t0 * sr);
         for (size_t j = 0; j < (size_t)(0.95 * sr) && s0 + j < song.size(); j++)
         {
            const double t = (double)j / sr;
            double env = std::min(1.0, t / 0.002);
            if (t > 0.9)
               env *= 0.5 + 0.5 * std::cos(kPi * (t - 0.9) / 0.05); // fade out by 0.95 s
            double v = 0.0;
            for (int k = 0; k < 4; k++)
               v += rel[k] * std::pow(10.0, -3.0 * t / t60n[k]) * std::sin(2.0 * kPi * f0 * (k + 1) * t);
            song[s0 + j] += (float)(a1 * env * v);
         }
         SoundAnalysis::NoteEvent e;
         e.start = t0;
         e.end = t0 + 0.9;
         e.midi = midis[i];
         e.amplitude = 0.8f;
         notes.push_back(e);
         if (midis[i] == 60)
         {
            SoundAnalysis::NoteEvent ghost = e;
            ghost.start += 0.005;
            ghost.midi = 72;
            ghost.amplitude = 0.5f;
            notes.push_back(ghost);
         }
         if (midis[i] == 62)
         {
            SoundAnalysis::NoteEvent phantom = e;
            phantom.start += 0.05;
            phantom.midi = 64;
            notes.push_back(phantom);
         }
      }
      for (float& v : song)
         v += (float)(1e-4 * noise());
      SoundAnalysis::MultiNoteOptions mo;
      mo.partials = 8;
      const auto t0 = std::chrono::steady_clock::now();
      multi = std::make_shared<SoundAnalysis::MultiNoteResult>(SoundAnalysis::AnalyzeNotes(song.data(), song.size(), sr, notes, mo));
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      const SoundAnalysis::MultiNoteResult& r = *multi;
      printf("SOUNDANALYSISTEST notes: %d given, %d isolated, %d used in %.0f ms; level %.2f dB/oct (r2 %.2f, %s), tilt %.2f dB/oct (r2 %.2f), tilt3 %.2f\n",
             r.notesGiven, r.notesIsolated, r.notesUsed, ms, r.keyLevel.dbPerOct, r.keyLevel.r2,
             r.keyLevelFromFundamental ? "fundamental" : "all partials", r.keyTilt.dbPerOct, r.keyTilt.r2, r.keyTilt3.dbPerOct);
      if (!r.valid)
         printf("SOUNDANALYSISTEST notes error: %s\n", r.error.c_str());
      for (const SoundAnalysis::NoteRow& row : r.rows)
         printf("   t=%.2f midi %d f0 %.1f h1 %.1f dB h2-h1 %.1f dB\n", row.start, row.midi, row.f0Hz, row.h1Db, row.h2h1Db);
      check(r.valid, "notes: valid");
      check(r.notesIsolated == 10 && r.notesUsed == 10, "notes: 10 isolated and used (ghost ignored, D4 + phantom dropped)");
      check(r.keyTilt.valid && within(r.keyTilt.dbPerOct, 6.0, 1.5), "notes: key tilt +6 dB/oct +-1.5");
      check(r.keyLevel.valid && r.keyLevelFromFundamental && within(r.keyLevel.dbPerOct, -3.0, 1.0),
            "notes: key level -3 dB/oct +-1");
      check(r.keyTilt3.valid && within(r.keyTilt3.dbPerOct, 0.0, 1.5), "notes: 3rd harmonic flat");
      check(r.profile.valid && r.profile.partials.size() >= 3 && r.profile.partials[1].present &&
               within(r.profile.partials[1].ampDb, -12.0, 1.5),
            "notes: merged partial 2 at C4 = -12 dB (key tilt removed)");
      check(within(r.profile.f0Hz, 261.63, 0.1), "notes: merged profile placed at MIDI 60");
      const nlohmann::json j = SoundAnalysis::ToJson(r);
      check(j.contains("keyTracking") && j["notes"].size() == r.rows.size(), "notes: ToJson has key tracking and rows");
      // Chords are never isolated.
      std::vector<SoundAnalysis::NoteEvent> chord = { { 0.1, 0.7, 60, 0.8f }, { 0.1, 0.7, 64, 0.8f } };
      check(!SoundAnalysis::AnalyzeNotes(song.data(), song.size(), sr, chord, mo).valid, "notes: a chord is not isolated");
   }

   // 9. Outputs: the documented 0..1 mappings, read from a node holding p1.
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
      check(within(node.Value(kTransient), std::clamp(p1.transient, 0.0f, 1.0f), 1e-6), "transient output = level");
      check(within(node.Value(kTransientDecay), std::clamp(p1.transientDecayMs / 200.0f, 0.0f, 1.0f), 1e-6),
            "transientDecay output = ms / 200");
      check(within(node.Value(kAir), std::clamp(p1.noiseLevel, 0.0f, 1.0f), 1e-6), "air output = noise level");
      check(node.Value(kKeyLevel) == 0.5f && node.Value(kKeyTilt) == 0.5f, "region mode: key outputs 0.5 (0 dB/oct)");
      check(node.ProfileJson() != nullptr && (*node.ProfileJson())["file"] == "selftest", "ProfileJson carries the file");
      if (multi && multi->valid)
      {
         node.SetProfile(multi->profile, multi);
         check(within(node.Value(kKeyTilt), (multi->keyTilt.dbPerOct + 12.0) / 24.0, 1e-5), "keyTilt output = (dB/oct + 12) / 24");
         check(within(node.Value(kKeyLevel), (multi->keyLevel.dbPerOct + 12.0) / 24.0, 1e-5), "keyLevel output = (dB/oct + 12) / 24");
         check(node.MultiNote() != nullptr && (*node.ProfileJson())["multiNote"].contains("keyTracking"),
               "mode notes: ProfileJson has multiNote");
      }
      SoundAnalyzeNode empty;
      check(empty.ProfileJson() == nullptr && empty.Value(kF0) == 0.0f && empty.Value(kKeyTilt) == 0.0f,
            "no profile: null json, zero outputs");
   }

   printf("SOUNDANALYSISTEST %s\n", ok ? "OK" : "FAIL");
   return ok;
}
