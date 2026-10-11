#include "BasicPitch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>

#include "platform/PlatformPlugins.h"

#if defined(INFINITE_HAS_ORT)
#include <onnxruntime_cxx_api.h>
#endif

// Constants and the window/unwrap/note-creation steps follow Basic Pitch's
// reference Python (basic_pitch/constants.py, inference.py, note_creation.py,
// Copyright 2022 Spotify AB, Apache-2.0), ported to C++ here.
namespace BasicPitch
{
   namespace
   {
      constexpr int kFftHop = 256;
      constexpr int kWindowSamples = kSampleRate * 2 - kFftHop; // AUDIO_N_SAMPLES = 43844
      constexpr int kWindowFrames = (kSampleRate / kFftHop) * 2; // ANNOT_N_FRAMES = 172
      constexpr int kOverlapFrames = 30;                          // DEFAULT_OVERLAPPING_FRAMES
      constexpr int kOverlapSamples = kOverlapFrames * kFftHop;  // 7680
      constexpr int kHopSamples = kWindowSamples - kOverlapSamples;
      constexpr int kEnergyTolerance = 11;
      constexpr double kMagicAlignmentOffset = 0.0018;

      // Frame index -> seconds (note_creation.model_frames_to_time).
      double FrameTime(int frame)
      {
         const double original = (double)frame * kFftHop / kSampleRate;
         const double windowOffset = ((double)kFftHop / kSampleRate) * (kWindowFrames - (double)kWindowSamples / kFftHop) +
                                     kMagicAlignmentOffset;
         return original - windowOffset * std::floor((double)frame / kWindowFrames);
      }
   }

   std::string ModelPath()
   {
      const std::filesystem::path exe = std::filesystem::u8path(Platform::ExecutablePath());
      return (exe.parent_path() / "assets" / "models" / "basic-pitch.onnx").u8string();
   }

   bool ToModelInput(const float* planar, int channels, int frames, double sampleRate, std::vector<float>& out,
                     const std::atomic<bool>* cancel)
   {
      out.clear();
      if (planar == nullptr || channels <= 0 || frames <= 0 || sampleRate <= 0.0)
         return false;

      std::vector<float> mono((size_t)frames, 0.0f);
      const float g = 1.0f / (float)channels;
      for (int c = 0; c < channels; c++)
      {
         const float* src = planar + (size_t)c * frames;
         for (int i = 0; i < frames; i++)
            mono[(size_t)i] += src[i] * g;
      }
      if (std::fabs(sampleRate - kSampleRate) < 0.5)
      {
         out = std::move(mono);
         return true;
      }

      // Windowed-sinc resampler (Blackman window, 16 zero crossings each
      // side at the cutoff). Cutoff sits just under the lower Nyquist, so
      // downsampling never folds anything back into the model's band.
      const double ratio = kSampleRate / sampleRate; // output samples per input sample
      const double cutoff = std::min(1.0, ratio) * 0.95;
      const int zeroCrossings = 16;
      const double halfWidth = zeroCrossings / cutoff; // in input samples
      const size_t outLen = (size_t)std::floor((double)frames * ratio);
      out.resize(outLen);
      const double kPi = 3.14159265358979323846;

      // The kernel h(x) = sinc(cutoff x) * Blackman(x), tabulated once at
      // kOversample points per input sample and read with linear
      // interpolation: a few minutes of audio is ~10^8 taps, far too many
      // for a sin and two cos each.
      const int kOversample = 512;
      const int tableHalf = (int)std::ceil(halfWidth * kOversample);
      std::vector<float> table((size_t)(2 * tableHalf + 2), 0.0f);
      for (int i = 0; i <= 2 * tableHalf; i++)
      {
         const double x = (double)(i - tableHalf) / kOversample;
         if (std::fabs(x) > halfWidth)
            continue;
         const double a = kPi * cutoff * x;
         const double sinc = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
         const double u = (x / halfWidth + 1.0) * 0.5; // 0..1 across the window
         const double win = 0.42 - 0.5 * std::cos(2.0 * kPi * u) + 0.08 * std::cos(4.0 * kPi * u);
         table[(size_t)i] = (float)(sinc * win);
      }
      auto kernel = [&](double x)
      {
         const double pos = x * kOversample + tableHalf; // >= 0, as |x| <= halfWidth
         const int i = (int)pos;
         if (i < 0 || i >= 2 * tableHalf + 1)
            return 0.0;
         const double f = pos - i;
         return (double)table[(size_t)i] * (1.0 - f) + (double)table[(size_t)i + 1] * f;
      };

      for (size_t n = 0; n < outLen; n++)
      {
         if ((n & 8191) == 0 && cancel != nullptr && cancel->load(std::memory_order_relaxed))
            return false;
         const double t = (double)n / ratio;
         const long long first = (long long)std::ceil(t - halfWidth);
         const long long last = (long long)std::floor(t + halfWidth);
         double acc = 0.0, wsum = 0.0;
         for (long long k = std::max(0LL, first); k <= std::min((long long)frames - 1, last); k++)
         {
            const double h = kernel(t - (double)k);
            acc += h * mono[(size_t)k];
            wsum += h;
         }
         // Normalising by the tap sum keeps DC gain at 1 at the file edges too.
         out[n] = wsum > 1e-9 ? (float)(acc / wsum) : 0.0f;
      }
      return true;
   }

#if defined(INFINITE_HAS_ORT)
   namespace
   {
      struct ModelSession
      {
         std::unique_ptr<Ort::Env> env;
         std::unique_ptr<Ort::Session> session;
         std::string inputName;
         std::string noteName, onsetName;
         std::string error;
      };

      // Built once, on first use. Ort::Session::Run is thread-safe, so a
      // second node converting at the same time shares it.
      ModelSession& Session()
      {
         static ModelSession s;
         static std::once_flag once;
         std::call_once(once, [] {
            const std::string path = ModelPath();
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(path), ec))
            {
               s.error = "model file missing: " + path;
               return;
            }
            try
            {
               s.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "InfiniteBasicPitch");
               Ort::SessionOptions options;
               options.SetIntraOpNumThreads(4);
               options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
               s.session = std::make_unique<Ort::Session>(*s.env, std::filesystem::u8path(path).native().c_str(), options);

               Ort::AllocatorWithDefaultOptions allocator;
               s.inputName = s.session->GetInputNameAllocated(0, allocator).get();
               // The exported graph names its three heads by TF serving
               // signature: :0 contour (264 bins), :1 note, :2 onset (88 each).
               for (size_t i = 0; i < s.session->GetOutputCount(); i++)
               {
                  const std::string name = s.session->GetOutputNameAllocated(i, allocator).get();
                  if (name.size() >= 2 && name.compare(name.size() - 2, 2, ":1") == 0)
                     s.noteName = name;
                  else if (name.size() >= 2 && name.compare(name.size() - 2, 2, ":2") == 0)
                     s.onsetName = name;
               }
               if (s.noteName.empty() || s.onsetName.empty())
               {
                  s.error = "unexpected model outputs";
                  s.session.reset();
               }
            }
            catch (const Ort::Exception& e)
            {
               s.session.reset();
               s.error = std::string("could not load the transcription model: ") + e.what();
            }
         });
         return s;
      }
   }

   bool RunModel(const std::vector<float>& audio22k, Activations& out, std::string& error,
                 std::atomic<float>* progress, const std::atomic<bool>* cancel)
   {
      out = Activations();
      ModelSession& s = Session();
      if (!s.session)
      {
         error = s.error.empty() ? "transcription model unavailable" : s.error;
         return false;
      }
      if (audio22k.empty())
      {
         error = "no audio";
         return false;
      }

      // Half an overlap of silence up front, so the first window's trimmed
      // edge frames are padding rather than music (inference.get_audio_input).
      std::vector<float> padded((size_t)kOverlapSamples / 2, 0.0f);
      padded.insert(padded.end(), audio22k.begin(), audio22k.end());
      const size_t windows = (padded.size() + kHopSamples - 1) / kHopSamples;
      const int keepPerWindow = kWindowFrames - kOverlapFrames; // 142
      const int trim = kOverlapFrames / 2;

      std::vector<float> note, onset;
      note.reserve(windows * keepPerWindow * kKeys);
      onset.reserve(windows * keepPerWindow * kKeys);
      std::vector<float> input((size_t)kWindowSamples);
      const int64_t shape[3] = { 1, kWindowSamples, 1 };
      const char* inNames[1] = { s.inputName.c_str() };
      const char* outNames[2] = { s.noteName.c_str(), s.onsetName.c_str() };
      try
      {
         Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
         for (size_t w = 0; w < windows; w++)
         {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
            {
               error = "cancelled";
               return false;
            }
            const size_t start = w * (size_t)kHopSamples;
            std::fill(input.begin(), input.end(), 0.0f);
            const size_t n = std::min((size_t)kWindowSamples, padded.size() - start);
            std::copy(padded.begin() + (long long)start, padded.begin() + (long long)(start + n), input.begin());

            Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape, 3);
            std::vector<Ort::Value> outputs = s.session->Run(Ort::RunOptions{ nullptr }, inNames, &tensor, 1, outNames, 2);
            for (int head = 0; head < 2; head++)
            {
               const Ort::TensorTypeAndShapeInfo info = outputs[(size_t)head].GetTensorTypeAndShapeInfo();
               const std::vector<int64_t> dims = info.GetShape();
               if (dims.size() != 3 || dims[1] != kWindowFrames || dims[2] != kKeys)
               {
                  error = "unexpected model output shape";
                  return false;
               }
               const float* data = outputs[(size_t)head].GetTensorData<float>();
               std::vector<float>& dst = head == 0 ? note : onset;
               dst.insert(dst.end(), data + (size_t)trim * kKeys, data + (size_t)(trim + keepPerWindow) * kKeys);
            }
            if (progress != nullptr)
               progress->store((float)(w + 1) / (float)windows, std::memory_order_relaxed);
         }
      }
      catch (const Ort::Exception& e)
      {
         error = std::string("transcription failed: ") + e.what();
         return false;
      }

      // Frames belonging to the original length only (inference.unwrap_output).
      const double expectedWindows = (double)audio22k.size() / kHopSamples;
      const int frames = std::min((int)(note.size() / kKeys), (int)(expectedWindows * keepPerWindow));
      note.resize((size_t)frames * kKeys);
      onset.resize((size_t)frames * kKeys);
      out.frames = frames;
      out.note = std::move(note);
      out.onset = std::move(onset);
      return true;
   }
#else
   bool RunModel(const std::vector<float>&, Activations& out, std::string& error, std::atomic<float>*, const std::atomic<bool>*)
   {
      out = Activations();
      error = "audio-to-MIDI is not available on this platform";
      return false;
   }
#endif

   std::vector<Note> NotesFromActivations(const Activations& a, const Settings& set)
   {
      std::vector<Note> notes;
      const int T = a.frames;
      if (T < 3 || a.note.size() != (size_t)T * kKeys || a.onset.size() != (size_t)T * kKeys)
         return notes;
      const float onsetThresh = set.onsetThreshold;
      const float frameThresh = std::max(1e-4f, set.frameThreshold);
      const int minLen = (int)std::lround(set.minNoteMs / 1000.0 * ((double)kSampleRate / kFftHop));
      const int fLo = std::clamp(set.lowMidi - kLowestMidi, 0, kKeys);
      const int fHi = std::clamp(set.highMidi - kLowestMidi + 1, fLo, kKeys); // exclusive
      auto at = [](int t, int f) { return (size_t)t * kKeys + (size_t)f; };

      // Frequency limits (constrain_frequency).
      std::vector<float> frames = a.note;
      std::vector<float> onsets = a.onset;
      for (int t = 0; t < T; t++)
         for (int f = 0; f < kKeys; f++)
            if (f < fLo || f >= fHi)
               frames[at(t, f)] = onsets[at(t, f)] = 0.0f;

      // Extra onsets where the note activation jumps (get_infered_onsets, n_diff = 2).
      {
         std::vector<float> diff((size_t)T * kKeys, 0.0f);
         float maxDiff = 0.0f, maxOnset = 0.0f;
         for (int t = 0; t < T; t++)
            for (int f = 0; f < kKeys; f++)
            {
               const float x = frames[at(t, f)];
               const float d1 = x - (t >= 1 ? frames[at(t - 1, f)] : 0.0f);
               const float d2 = x - (t >= 2 ? frames[at(t - 2, f)] : 0.0f);
               const float d = t < 2 ? 0.0f : std::max(0.0f, std::min(d1, d2));
               diff[at(t, f)] = d;
               maxDiff = std::max(maxDiff, d);
               maxOnset = std::max(maxOnset, onsets[at(t, f)]);
            }
         if (maxDiff > 0.0f)
         {
            const float scale = maxOnset / maxDiff;
            for (size_t i = 0; i < onsets.size(); i++)
               onsets[i] = std::max(onsets[i], diff[i] * scale);
         }
      }

      // Onset peaks over time, strongest-later first (argrelmax + reversed np.where order).
      struct Cell { int t, f; };
      std::vector<Cell> peaks;
      for (int t = 1; t < T - 1; t++)
         for (int f = 0; f < kKeys; f++)
         {
            const float x = onsets[at(t, f)];
            if (x >= onsetThresh && x > onsets[at(t - 1, f)] && x > onsets[at(t + 1, f)])
               peaks.push_back({ t, f });
         }
      std::reverse(peaks.begin(), peaks.end());

      std::vector<float> remaining = frames;
      auto meanFrames = [&](int t0, int t1, int f)
      {
         double sum = 0.0;
         for (int t = t0; t < t1; t++)
            sum += frames[at(t, f)];
         return t1 > t0 ? (float)(sum / (t1 - t0)) : 0.0f;
      };
      auto clearAround = [&](int t, int f)
      {
         remaining[at(t, f)] = 0.0f;
         if (f < kKeys - 1)
            remaining[at(t, f + 1)] = 0.0f;
         if (f > 0)
            remaining[at(t, f - 1)] = 0.0f;
      };
      auto addNote = [&](int t0, int t1, int f)
      {
         notes.push_back({ (float)FrameTime(t0), (float)FrameTime(t1), f + kLowestMidi, meanFrames(t0, t1, f) });
      };

      for (const Cell& p : peaks)
      {
         if (p.t >= T - 1)
            continue;
         // Hold while the note stays above the frame threshold, allowing
         // short dips of up to kEnergyTolerance frames.
         int i = p.t + 1, k = 0;
         while (i < T - 1 && k < kEnergyTolerance)
         {
            k = remaining[at(i, p.f)] < frameThresh ? k + 1 : 0;
            i++;
         }
         i -= k;
         if (i - p.t <= minLen)
            continue;
         for (int t = p.t; t < i; t++)
            clearAround(t, p.f);
         addNote(p.t, i, p.f);
      }

      // "Melodia trick": notes with no detected onset, grown outward from the
      // strongest leftover activation. The reference repeatedly takes the
      // global argmax; since cells are only ever zeroed, visiting the
      // candidates once in descending order is the same walk.
      std::vector<std::pair<float, int>> candidates;
      for (size_t i = 0; i < remaining.size(); i++)
         if (remaining[i] > frameThresh)
            candidates.push_back({ remaining[i], (int)i });
      std::stable_sort(candidates.begin(), candidates.end(),
                       [](const std::pair<float, int>& x, const std::pair<float, int>& y) { return x.first > y.first; });
      for (const auto& c : candidates)
      {
         if (remaining[(size_t)c.second] <= frameThresh)
            continue; // consumed by an earlier note
         const int mid = c.second / kKeys, f = c.second % kKeys;
         remaining[(size_t)c.second] = 0.0f;

         int i = mid + 1, k = 0;
         while (i < T - 1 && k < kEnergyTolerance)
         {
            k = remaining[at(i, f)] < frameThresh ? k + 1 : 0;
            clearAround(i, f);
            i++;
         }
         const int end = i - 1 - k;

         i = mid - 1;
         k = 0;
         while (i > 0 && k < kEnergyTolerance)
         {
            k = remaining[at(i, f)] < frameThresh ? k + 1 : 0;
            clearAround(i, f);
            i--;
         }
         const int start = i + 1 + k;
         if (end - start <= minLen)
            continue;
         addNote(start, end, f);
      }

      std::sort(notes.begin(), notes.end(), [](const Note& x, const Note& y)
                { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; });

      // Polyphony limit: walk the notes by start; when one starts with the
      // limit already sounding, the quietest of them all gives way - a held
      // note is cut where the new one starts, a new note that is quietest
      // is dropped. Notes cut to under half the minimum length are dropped.
      if (set.maxPolyphony > 0)
      {
         const float minSeconds = set.minNoteMs / 1000.0f;
         std::vector<Note> kept;
         std::vector<size_t> active; // indices into kept
         for (const Note& n : notes)
         {
            active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return kept[i].end <= n.start; }),
                         active.end());
            if ((int)active.size() < set.maxPolyphony)
            {
               active.push_back(kept.size());
               kept.push_back(n);
               continue;
            }
            auto quietest = std::min_element(active.begin(), active.end(),
                                             [&](size_t x, size_t y) { return kept[x].amplitude < kept[y].amplitude; });
            if (kept[*quietest].amplitude >= n.amplitude)
               continue; // the new note is the quietest: dropped
            kept[*quietest].end = n.start;
            *quietest = kept.size();
            kept.push_back(n);
         }
         notes.clear();
         for (const Note& n : kept)
            if (n.end - n.start >= minSeconds * 0.5f)
               notes.push_back(n);
      }
      return notes;
   }
}
