// Sound as numbers for the assistant (and any RemoteControl client): live
// captures of up to four nodes' audio through AudioProbe, and whole-file
// analysis of a decoded file, both measured by audio/AudioAnalysis.
//
// RemoteControl handlers run synchronously on the main thread, so nothing
// here waits: a start call returns a job id at once, PumpAudioProbeJobs()
// (every frame) drains the probe and hands finished buffers to a worker, and
// analysis_result polls. The MCP bridge (core/McpBridge.cpp) runs in its own
// process and does the waiting, so the model sees one blocking tool call.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "audio/AudioAnalysis.h"
#include "audio/AudioEngine.h"
#include "audio/AudioProbe.h"
#include "audio/SoundAnalysis.h"
#include "audio/dsp/BasicPitch.h"
#include "core/AudioDecodeCache.h"
#include "nodes/AudioNodes.h"

#include <chrono>
#include <future>
#include <map>

namespace app
{
   namespace
   {
      using json = nlohmann::json;

      double Now()
      {
         return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      struct Job
      {
         enum Kind { Capture, File } kind = Capture;
         enum State { Collecting, Analyzing, Done, Failed } state = Collecting;
         double started = 0.0;
         double seconds = 0.0;      // capture length asked for
         double frameSeconds = 0.1;
         std::vector<int> nodeIndices;
         std::vector<std::string> nodeTypes;
         std::vector<std::vector<float>> buffers; // interleaved stereo per slot
         uint64_t overflows = 0;
         // Offline capture: the job renders the graph itself, device stopped
         // (see BeginOffline / PumpOffline).
         bool offline = false;
         double offlineRate = 0.0;
         long long framesTotal = 0;
         long long framesDone = 0;
         std::string note;          // e.g. which node was tapped instead of an Audio Out
         std::future<json> analysis;
         json result;
         std::string error;
      };

      std::map<int, Job>& Jobs()
      {
         static std::map<int, Job> jobs;
         return jobs;
      }
      int gNextJobId = 1;
      int gCaptureJob = -1; // the job that owns AudioProbe right now

      // What an offline capture borrowed, given back by EndOffline.
      struct OfflineRestore
      {
         bool active = false;
         bool deviceWasRunning = false;
         bool wasPlaying = false;
         double seconds = 0.0;
      };
      OfflineRestore gOffline;

      // Offline capture: the same measurement as a live one, but the graph is
      // rendered here, block by block, from `start` seconds on the transport -
      // the way an offline render does - instead of listening to the device.
      // Every run renders the same samples (the transport is exact, players
      // that follow it land on the same sample, nothing depends on UI timing),
      // so a synth rendered from Audio to MIDI's notes can be compared with
      // the Audio File it was transcribed from, repeatably.
      bool BeginOffline(Job& job, double startSeconds, std::string& err)
      {
         if (ArrangeRenderBusy() || Transport::Instance().IsOfflineMode())
         {
            err = "an offline render is running; wait for it to finish";
            return false;
         }
         const bool deviceWasRunning = AudioEngine::Instance().SampleRate() > 0.0;
         // The graph's AudioNodes are prepared when a device opens, so open it
         // once if it never was; the rate is the one the graph was prepared at.
         if (!deviceWasRunning && !StartAudioEngine(gAudioStartError))
         {
            err = "the audio device could not start (" + gAudioStartError + "); offline capture needs it once to prepare the graph";
            return false;
         }
         const double rate = AudioEngine::Instance().SampleRate();
         if (!(rate > 0.0))
         {
            err = "no audio sample rate";
            return false;
         }
         gOffline.active = true;
         gOffline.deviceWasRunning = deviceWasRunning;
         gOffline.wasPlaying = Transport::Instance().IsPlaying();
         gOffline.seconds = Transport::Instance().Seconds();
         AudioEngine::Instance().Stop();
         Transport::Instance().Seek(std::max(0.0, startSeconds));
         Transport::Instance().SetOfflineMode(true, rate);
         Transport::Instance().SetPlaying(true);
         AudioProbe::Instance().SetOffline(true);
         RebuildAudioTopology();
         job.offline = true;
         job.offlineRate = rate;
         job.framesTotal = (long long)std::llround(job.seconds * rate);
         job.framesDone = 0;
         return true;
      }

      void EndOffline()
      {
         if (!gOffline.active)
            return;
         gOffline.active = false;
         AudioProbe::Instance().SetOffline(false);
         Transport::Instance().SetOfflineMode(false);
         Transport::Instance().Seek(gOffline.seconds);
         if (gOffline.deviceWasRunning)
            StartAudioEngine(gAudioStartError);
         else
            Transport::Instance().NotifyAudioEngineStopped();
         Transport::Instance().SetPlaying(gOffline.wasPlaying);
         RebuildAudioTopology();
      }

      // One main-loop slice of an offline capture: renders blocks for up to
      // ~60 ms, draining the probe after each so its rings never fill.
      void PumpOffline(Job& job)
      {
         static float sL[4096];
         static float sR[4096];
         static float* sChannels[2] = { sL, sR };
         AudioProbe& probe = AudioProbe::Instance();
         float chunk[8192];
         const auto start = std::chrono::steady_clock::now();
         const int blockCap = std::clamp(OfflineAudioBlockFrames(), 32, 4096);
         while (job.framesDone < job.framesTotal)
         {
            const int frames = (int)std::min<long long>(blockCap, job.framesTotal - job.framesDone);
            AudioBuffer buf;
            buf.channels = sChannels;
            buf.numChannels = 2;
            buf.numFrames = frames;
            AudioEngine::Instance().ProcessOffline(buf);
            job.framesDone += frames;
            for (size_t s = 0; s < job.buffers.size(); s++)
            {
               int n;
               while ((n = probe.Drain((int)s, chunk, 8192)) > 0)
                  job.buffers[s].insert(job.buffers[s].end(), chunk, chunk + n);
               job.overflows += probe.TakeOverflows((int)s);
            }
            Transport::Instance().SetOfflineVideoTime(Transport::Instance().Seconds());
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(60))
               break;
         }
      }

      // Old finished jobs are dropped so a long session does not pile up results.
      void TrimJobs()
      {
         auto& jobs = Jobs();
         while (jobs.size() > 16)
         {
            auto it = jobs.begin();
            if (it->second.state == Job::Collecting || it->second.state == Job::Analyzing)
               break;
            jobs.erase(it);
         }
      }

      // The AudioNode whose output a capture of `gn` should tap. Audio Out has
      // no output of its own, so it resolves to whatever feeds it.
      AudioNode* TapTarget(GraphNode& gn, std::string& note, std::string& err)
      {
         if (auto* src = dynamic_cast<IAudioSource*>(gn.node.get()))
            return src->GetAudioNode();
         if (auto* out = dynamic_cast<AudioOutputNode*>(gn.node.get()))
         {
            if (out->input.IsConnected())
               if (auto* feeder = dynamic_cast<IAudioSource*>(out->input.GetSource()))
               {
                  for (GraphNode& other : gNodes)
                     if (other.node.get() == out->input.GetSource())
                        note += "node " + std::to_string(gn.index) + " is an Audio Out: measured node " +
                                std::to_string(other.index) + " (" + other.typeName + ") that feeds it. ";
                  return feeder->GetAudioNode();
               }
            err = "node " + std::to_string(gn.index) + " is an Audio Out with nothing connected";
            return nullptr;
         }
         err = "node " + std::to_string(gn.index) + " (" + gn.typeName +
               ") has no audio output - measure an audio source, an effect, or the node feeding it";
         return nullptr;
      }

      json AnalyzeCapture(std::vector<std::vector<float>> buffers, std::vector<int> indices, std::vector<std::string> types,
                          double sampleRate, double frameSeconds)
      {
         AudioAnalysis::Options opt;
         opt.frameSeconds = frameSeconds;
         size_t frames = SIZE_MAX;
         for (const auto& b : buffers)
            frames = std::min(frames, b.size() / 2);
         std::vector<std::vector<float>> monos;
         std::vector<AudioAnalysis::Result> results;
         json nodes = json::array();
         for (size_t i = 0; i < buffers.size(); i++)
         {
            buffers[i].resize(frames * 2); // same span for every node
            monos.push_back(AudioAnalysis::MixToMono(buffers[i]));
            results.push_back(AudioAnalysis::Analyze(monos.back(), sampleRate, opt));
            json n = AudioAnalysis::ToJson(results.back(), 50);
            n["index"] = indices[i];
            n["type"] = types[i];
            if (results.back().peakDb < -90.0f)
               n["warning"] = "silent: is the transport playing, and does this node make sound right now?";
            nodes.push_back(n);
         }
         json out = { { "nodes", nodes } };
         if (results.size() >= 2)
         {
            json cmp = json::array();
            for (size_t i = 1; i < results.size(); i++)
            {
               json c = AudioAnalysis::Compare(results[0], results[i], monos[0], monos[i], sampleRate);
               c["a"] = indices[0];
               c["b"] = indices[i];
               cmp.push_back(c);
            }
            out["compare"] = cmp;
         }
         return out;
      }

      struct SoundRequest
      {
         bool enabled = false;
         bool overNotes = false; // average over the transcribed, isolated notes (Sound Analyze's notes mode)
         SoundAnalysis::Options options;
         int lowMidi = 21, highMidi = 108;
      };

      json AnalyzeFile(std::shared_ptr<Platform::SampleBuffer> buf, std::string path, bool withNotes, SoundRequest sound)
      {
         std::vector<float> mono((size_t)buf->numFrames, 0.0f);
         const float g = 1.0f / (float)std::max(1, buf->channels);
         for (int c = 0; c < buf->channels; c++)
            for (int i = 0; i < buf->numFrames; i++)
               mono[(size_t)i] += buf->channelData[(size_t)c * buf->numFrames + i] * g;

         AudioAnalysis::Options opt;
         opt.rhythmAndKey = true;
         const double seconds = (double)buf->numFrames / buf->sampleRate;
         opt.frameSeconds = std::max(0.1, seconds / 600.0); // keeps long files quick
         const AudioAnalysis::Result r = AudioAnalysis::Analyze(mono, buf->sampleRate, opt);
         json out = AudioAnalysis::ToJson(r, 60);
         out["file"] = path;
         out["channels"] = buf->channels;

         if (sound.enabled && !sound.overNotes)
         {
            // What a synth needs to recreate the sound: partials (level, decay),
            // envelope, spectral envelope - the Sound Analyze node's analysis.
            const SoundAnalysis::SoundProfile profile = SoundAnalysis::AnalyzeSound(mono.data(), mono.size(), buf->sampleRate, sound.options);
            out["sound"] = SoundAnalysis::ToJson(profile);
         }
         if (sound.enabled && sound.overNotes)
         {
            // Over many notes: transcribe, keep the isolated strikes, merge them
            // and fit key tracking (level and harmonic balance per octave).
            std::vector<float> input;
            BasicPitch::Activations act;
            std::string err;
            if (BasicPitch::ToModelInput(mono.data(), 1, (int)mono.size(), buf->sampleRate, input, nullptr) &&
                BasicPitch::RunModel(input, act, err, nullptr, nullptr))
            {
               std::vector<SoundAnalysis::NoteEvent> events;
               for (const BasicPitch::Note& n : BasicPitch::NotesFromActivations(act, BasicPitch::Settings()))
               {
                  SoundAnalysis::NoteEvent e;
                  e.start = n.start;
                  e.end = n.end;
                  e.midi = n.pitch;
                  e.amplitude = n.amplitude;
                  events.push_back(e);
               }
               SoundAnalysis::MultiNoteOptions mo;
               mo.partials = sound.options.partials;
               mo.lowMidi = sound.lowMidi;
               mo.highMidi = sound.highMidi;
               const SoundAnalysis::MultiNoteResult mr = SoundAnalysis::AnalyzeNotes(mono.data(), mono.size(), buf->sampleRate, events, mo);
               json s = mr.valid ? SoundAnalysis::ToJson(mr.profile) : json::object();
               s["mode"] = "notes";
               s["multiNote"] = SoundAnalysis::ToJson(mr);
               if (!mr.valid)
                  s["error"] = mr.error.empty() ? std::string("no isolated notes to analyse") : mr.error;
               out["sound"] = s;
            }
            else
               out["sound"] = { { "error", err.empty() ? "transcription unavailable" : err } };
         }

         if (withNotes)
         {
            // Polyphonic transcription summary from the Audio to MIDI model.
            std::vector<float> input;
            BasicPitch::Activations act;
            std::string err;
            if (BasicPitch::ToModelInput(buf->channelData.data(), buf->channels, buf->numFrames, buf->sampleRate, input, nullptr) &&
                BasicPitch::RunModel(input, act, err, nullptr, nullptr))
            {
               const std::vector<BasicPitch::Note> notes = BasicPitch::NotesFromActivations(act, BasicPitch::Settings());
               int lo = 127, hi = 0;
               double noteSeconds = 0.0;
               int histogram[12] = {};
               for (const BasicPitch::Note& n : notes)
               {
                  lo = std::min(lo, n.pitch);
                  hi = std::max(hi, n.pitch);
                  noteSeconds += n.end - n.start;
                  histogram[n.pitch % 12]++;
               }
               json pcs = json::array();
               for (int h : histogram)
                  pcs.push_back(h);
               json first = json::array();
               for (size_t i = 0; i < notes.size() && i < 48; i++)
                  first.push_back({ { "t", std::round(notes[i].start * 1000.0) / 1000.0 },
                                    { "note", AudioAnalysis::NoteName((float)(440.0 * std::pow(2.0, (notes[i].pitch - 69) / 12.0))) },
                                    { "dur", std::round((notes[i].end - notes[i].start) * 1000.0) / 1000.0 } });
               out["notes"] = { { "count", (int)notes.size() },
                                { "perSecond", seconds > 0.0 ? std::round(notes.size() / seconds * 10.0) / 10.0 : 0.0 },
                                { "averagePolyphony", seconds > 0.0 ? std::round(noteSeconds / seconds * 100.0) / 100.0 : 0.0 },
                                { "range", notes.empty() ? json::array() : json::array({ AudioAnalysis::NoteName((float)(440.0 * std::pow(2.0, (lo - 69) / 12.0))),
                                                                                        AudioAnalysis::NoteName((float)(440.0 * std::pow(2.0, (hi - 69) / 12.0))) }) },
                                { "pitchClassCounts", pcs },
                                { "first", first } };
            }
            else
               out["notes"] = { { "error", err.empty() ? "transcription unavailable" : err } };
         }
         return out;
      }

      bool StartCapture(const json& params, json& outResult, std::string& outError)
      {
         if (gCaptureJob >= 0)
         {
            outError = "a capture is already running (job " + std::to_string(gCaptureJob) + "); wait for it";
            return false;
         }
         const bool offline = params.value("offline", false);
         if (!offline && AudioEngine::Instance().SampleRate() <= 0.0)
         {
            outError = "the audio engine is off - start it first (transport with action \"play\", or the Start Audio button), "
                       "or pass offline:true";
            return false;
         }
         std::vector<int> indices;
         if (params.contains("nodes") && params["nodes"].is_array())
         {
            for (const json& v : params["nodes"])
               if (v.is_number_integer())
                  indices.push_back(v.get<int>());
         }
         else if (params.contains("node") && params["node"].is_number_integer())
            indices.push_back(params["node"].get<int>());
         if (indices.empty() || (int)indices.size() > AudioProbe::kSlots)
         {
            outError = "give 'node' (an index) or 'nodes' (1 to " + std::to_string(AudioProbe::kSlots) + " indices)";
            return false;
         }

         Job job;
         job.kind = Job::Capture;
         job.started = Now();
         job.seconds = std::clamp(params.value("seconds", 3.0), 0.2, 30.0);
         job.frameSeconds = std::clamp(params.value("frameSeconds", 0.1), 0.02, 2.0);
         const AudioNode* targets[AudioProbe::kSlots] = {};
         for (size_t i = 0; i < indices.size(); i++)
         {
            GraphNode* gn = FindNodeByIndex(indices[i]);
            if (gn == nullptr)
            {
               outError = "unknown node index " + std::to_string(indices[i]);
               return false;
            }
            std::string err;
            const AudioNode* target = TapTarget(*gn, job.note, err);
            if (target == nullptr)
            {
               outError = err;
               return false;
            }
            targets[i] = target;
            job.nodeIndices.push_back(gn->index);
            job.nodeTypes.push_back(gn->typeName);
         }
         job.buffers.resize(indices.size());
         if (offline)
         {
            const double start = params.contains("start") && params["start"].is_number() ? params["start"].get<double>()
                                                                                         : Transport::Instance().Seconds();
            if (!BeginOffline(job, start, outError))
               return false;
            // The topology was just rebuilt; resolve the taps again in case a node's audio half was recreated.
            for (size_t i = 0; i < indices.size(); i++)
            {
               std::string note, err;
               if (GraphNode* gn = FindNodeByIndex(indices[i]))
                  targets[i] = TapTarget(*gn, note, err);
            }
         }
         AudioProbe::Instance().Arm(targets, (int)indices.size());

         const int id = gNextJobId++;
         gCaptureJob = id;
         Jobs()[id] = std::move(job);
         TrimJobs();
         outResult = { { "id", id }, { "seconds", Jobs()[id].seconds } };
         if (offline)
            outResult["offline"] = true;
         return true;
      }

      bool StartFile(const json& params, json& outResult, std::string& outError)
      {
         std::string path = params.value("path", std::string());
         if (path.empty() && params.contains("node"))
         {
            GraphNode* gn = params["node"].is_number_integer() ? FindNodeByIndex(params["node"].get<int>()) : nullptr;
            if (gn == nullptr)
            {
               outError = "unknown node index";
               return false;
            }
            std::vector<std::pair<std::string, std::string>> raw;
            Patch::SaveParams(gn->node.get(), raw);
            for (const auto& kv : raw)
               if (kv.first == "s path")
               {
                  // Saved text is patch-escaped; only backslashes matter in a path.
                  for (size_t i = 0; i < kv.second.size(); i++)
                  {
                     if (kv.second[i] == '\\' && i + 1 < kv.second.size() && kv.second[i + 1] == '\\')
                        i++;
                     path += kv.second[i];
                  }
               }
            if (path.empty())
            {
               outError = "node " + std::to_string(gn->index) + " (" + gn->typeName + ") has no file path";
               return false;
            }
         }
         if (path.empty())
         {
            outError = "give 'path' (an audio file) or 'node' (a node with a file, e.g. Audio File)";
            return false;
         }
         auto buf = std::make_shared<Platform::SampleBuffer>();
         std::string err;
         if (!AudioDecodeCache::DecodeCached(path, *buf, err) || buf->numFrames <= 0 || buf->sampleRate <= 0.0)
         {
            outError = "could not read " + path + (err.empty() ? std::string() : ": " + err);
            return false;
         }
         Job job;
         job.kind = Job::File;
         job.state = Job::Analyzing;
         job.started = Now();
         job.seconds = (double)buf->numFrames / buf->sampleRate;
         const bool withNotes = params.value("notes", false);
         SoundRequest sound;
         sound.enabled = params.value("sound", false);
         sound.overNotes = params.value("soundMode", std::string("region")) == "notes";
         sound.options.startSeconds = std::max(0.0, params.value("start", 0.0));
         sound.options.lengthSeconds = std::max(0.0, params.value("length", 0.0));
         sound.options.f0HintHz = std::max(0.0, params.value("f0", 0.0));
         sound.options.partials = std::clamp(params.value("partials", 16), 1, 32);
         sound.lowMidi = std::clamp(params.value("low", 21), 0, 127);
         sound.highMidi = std::clamp(params.value("high", 108), sound.lowMidi, 127);
         job.analysis = std::async(std::launch::async, AnalyzeFile, buf, path, withNotes, sound);
         const int id = gNextJobId++;
         Jobs()[id] = std::move(job);
         TrimJobs();
         outResult = { { "id", id }, { "seconds", std::round(Jobs()[id].seconds * 1000.0) / 1000.0 } };
         return true;
      }
   }

   bool IsAudioProbeRpc(const std::string& method)
   {
      return method == "capture_audio" || method == "analyze_file" || method == "analysis_result";
   }

   bool HandleAudioProbeRpc(const std::string& method, const nlohmann::json& params, nlohmann::json& outResult,
                            std::string& outError)
   {
      if (method == "capture_audio")
         return StartCapture(params, outResult, outError);
      if (method == "analyze_file")
         return StartFile(params, outResult, outError);

      // analysis_result
      if (!params.contains("id") || !params["id"].is_number_integer())
      {
         outError = "analysis_result needs the integer 'id' that capture_audio / analyze_file returned";
         return false;
      }
      const int id = params["id"].get<int>();
      auto it = Jobs().find(id);
      if (it == Jobs().end())
      {
         outError = "unknown analysis id " + std::to_string(id);
         return false;
      }
      Job& job = it->second;
      if (job.state == Job::Done)
      {
         outResult = { { "status", "done" }, { "result", job.result } };
         if (!job.note.empty())
            outResult["note"] = job.note;
         return true;
      }
      if (job.state == Job::Failed)
      {
         outResult = { { "status", "error" }, { "error", job.error } };
         return true;
      }
      double progress = 0.0;
      if (job.state == Job::Collecting && job.offline && job.framesTotal > 0)
         progress = (double)job.framesDone / (double)job.framesTotal;
      else if (job.state == Job::Collecting && job.seconds > 0.0 && !job.buffers.empty())
      {
         const double sr = AudioProbe::Instance().SampleRate();
         progress = sr > 0.0 ? std::min(1.0, (double)(job.buffers[0].size() / 2) / (job.seconds * sr)) : 0.0;
      }
      outResult = { { "status", job.state == Job::Collecting ? "capturing" : "analyzing" }, { "progress", std::round(progress * 100.0) / 100.0 } };
      return true;
   }

   void PumpAudioProbeJobs()
   {
      auto& jobs = Jobs();
      if (jobs.empty())
         return;

      if (gCaptureJob >= 0)
      {
         auto it = jobs.find(gCaptureJob);
         if (it == jobs.end())
         {
            AudioProbe::Instance().Disarm();
            EndOffline();
            gCaptureJob = -1;
         }
         else
         {
            Job& job = it->second;
            AudioProbe& probe = AudioProbe::Instance();
            float chunk[8192];
            if (job.offline)
               PumpOffline(job);
            for (size_t s = 0; s < job.buffers.size(); s++)
            {
               int n;
               while ((n = probe.Drain((int)s, chunk, 8192)) > 0)
                  job.buffers[s].insert(job.buffers[s].end(), chunk, chunk + n);
               job.overflows += probe.TakeOverflows((int)s);
            }
            const double sr = job.offline ? job.offlineRate : probe.SampleRate();
            size_t have = SIZE_MAX;
            for (const auto& b : job.buffers)
               have = std::min(have, b.size() / 2);
            const bool full = job.offline ? job.framesDone >= job.framesTotal : (sr > 0.0 && (double)have >= job.seconds * sr);
            const double elapsed = Now() - job.started;
            // Offline renders as fast as the graph allows; a heavy graph may be slower than real time.
            const bool overdue = elapsed > (job.offline ? job.seconds * 20.0 + 30.0 : job.seconds * 1.5 + 3.0);
            if (full || overdue)
            {
               probe.Disarm();
               if (job.offline)
               {
                  EndOffline();
                  job.note += "rendered offline from the transport position given (sample-accurate, repeatable). ";
               }
               gCaptureJob = -1;
               if (have == 0 || sr <= 0.0)
               {
                  job.state = Job::Failed;
                  job.error = "no audio arrived: the node is not being processed. It must lead to an Audio Out "
                              "(or be a source the engine runs anyway), and the audio engine must be on.";
               }
               else
               {
                  if (!full)
                     job.note += "only " + std::to_string((double)have / sr).substr(0, 4) + " s arrived (the node may have been removed or the graph rebuilt). ";
                  if (job.overflows > 0)
                     job.note += "the UI stalled and some samples were lost. ";
                  const size_t want = (size_t)(job.seconds * sr);
                  for (auto& b : job.buffers)
                     if (b.size() / 2 > want)
                        b.resize(want * 2);
                  job.state = Job::Analyzing;
                  job.analysis = std::async(std::launch::async, AnalyzeCapture, std::move(job.buffers), job.nodeIndices,
                                            job.nodeTypes, sr, job.frameSeconds);
                  job.buffers.clear();
               }
            }
         }
      }

      for (auto& kv : jobs)
      {
         Job& job = kv.second;
         if (job.state != Job::Analyzing || !job.analysis.valid() ||
             job.analysis.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;
         try
         {
            job.result = job.analysis.get();
            job.state = Job::Done;
         }
         catch (const std::exception& e)
         {
            job.error = std::string("analysis failed: ") + e.what();
            job.state = Job::Failed;
         }
      }
   }

   // INFINITE_AUDIOANALYSISTEST: AudioAnalysis on signals with known answers
   // (level, pitch, band, tempo, key, onsets, A/B lag) and AudioProbe's
   // arm/write/drain path driven by hand. Headless, no device.
   bool RunAudioAnalysisTest()
   {
      bool ok = true;
      auto check = [&ok](bool cond, const char* what, double got)
      {
         printf("AUDIOANALYSISTEST %s: %s (%.3f)\n", what, cond ? "OK" : "FAIL", got);
         ok = ok && cond;
      };
      const double sr = 48000.0;
      const double kPi = 3.14159265358979323846;
      AudioAnalysis::Options opt;

      // A 0.5-amplitude 440 Hz sine: -9 dB RMS, -6 dB peak, A4, energy in the 440 Hz band.
      std::vector<float> sine((size_t)(sr * 2.0));
      for (size_t i = 0; i < sine.size(); i++)
         sine[i] = (float)(0.5 * std::sin(2.0 * kPi * 440.0 * (double)i / sr));
      const AudioAnalysis::Result s = AudioAnalysis::Analyze(sine, sr, opt);
      check(std::fabs(s.rmsDb - (-9.03f)) < 0.3f, "sine rms dB", s.rmsDb);
      check(std::fabs(s.peakDb - (-6.02f)) < 0.3f, "sine peak dB", s.peakDb);
      check(std::fabs(1200.0 * std::log2(s.medianPitchHz / 440.0)) < 15.0, "sine pitch Hz", s.medianPitchHz);
      check(s.pitchedRatio > 0.9f, "sine pitched ratio", s.pitchedRatio);
      int loudest = 0;
      for (int b = 1; b < AudioAnalysis::kBands; b++)
         if (s.bandsDb[b] > s.bandsDb[loudest])
            loudest = b;
      check(440.0 >= 20.0 * std::pow(1000.0, loudest / 8.0) && 440.0 < 20.0 * std::pow(1000.0, (loudest + 1) / 8.0),
            "sine band", loudest);
      check(std::fabs(s.bandsDb[loudest] - s.rmsDb) < 1.0f, "sine band level matches rms", s.bandsDb[loudest]);

      // Clicks at 120 bpm for 8 s: tempo ~120, ~16 onsets.
      std::vector<float> clicks((size_t)(sr * 8.0), 0.0f);
      for (int beat = 0; beat < 16; beat++)
         for (int i = 0; i < 400; i++)
            clicks[(size_t)(beat * sr * 0.5) + i] = (float)(0.8 * std::exp(-i / 60.0) * std::sin(2.0 * kPi * 1500.0 * i / sr));
      AudioAnalysis::Options full;
      full.rhythmAndKey = true;
      const AudioAnalysis::Result c = AudioAnalysis::Analyze(clicks, sr, full);
      check(std::fabs(c.tempoBpm - 120.0f) < 3.0f, "click tempo bpm", c.tempoBpm);
      check(c.onsets.size() >= 15 && c.onsets.size() <= 17, "click onsets", (double)c.onsets.size());

      // C major triads cycling C-F-G-C: key C major.
      std::vector<float> chords((size_t)(sr * 8.0), 0.0f);
      const int triads[4][3] = { { 60, 64, 67 }, { 65, 69, 72 }, { 67, 71, 74 }, { 60, 64, 67 } };
      for (size_t i = 0; i < chords.size(); i++)
      {
         const int which = (int)((double)i / sr / 2.0) % 4;
         double v = 0.0;
         for (int n : triads[which])
            v += 0.15 * std::sin(2.0 * kPi * 440.0 * std::pow(2.0, (n - 69) / 12.0) * (double)i / sr);
         chords[i] = (float)v;
      }
      const AudioAnalysis::Result k = AudioAnalysis::Analyze(chords, sr, full);
      check(k.keyRoot == 0 && !k.keyMinor, "chords key is C major", k.keyRoot);

      // B is A delayed by 10 ms: correlation at the best lag ~1, lag +10 ms.
      std::vector<float> noiseA((size_t)sr), noiseB((size_t)sr, 0.0f);
      unsigned seed = 12345;
      for (float& v : noiseA)
      {
         seed = seed * 1664525u + 1013904223u;
         v = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.5f;
      }
      const size_t delay = (size_t)(0.010 * sr);
      for (size_t i = delay; i < noiseB.size(); i++)
         noiseB[i] = noiseA[i - delay];
      const json cmp = AudioAnalysis::Compare(AudioAnalysis::Analyze(noiseA, sr, opt), AudioAnalysis::Analyze(noiseB, sr, opt),
                                              noiseA, noiseB, sr);
      check(std::fabs(cmp.value("bestLagMs", 0.0) - 10.0) < 0.2, "delayed copy lag ms", cmp.value("bestLagMs", 0.0));
      check(cmp.value("correlationAtBestLag", 0.0) > 0.95, "delayed copy correlation", cmp.value("correlationAtBestLag", 0.0));
      const json same = AudioAnalysis::Compare(s, s, sine, sine, sr);
      check(same.value("correlation", 0.0) > 0.999 && std::fabs(same.value("levelDiffDb", 1.0)) < 0.05,
            "identical signals", same.value("correlation", 0.0));

      // AudioProbe: two "nodes" (any distinct addresses do), written by hand as the audio thread would.
      {
         AudioProbe& probe = AudioProbe::Instance();
         int tokenA = 0, tokenB = 0;
         const AudioNode* nodes[2] = { reinterpret_cast<const AudioNode*>(&tokenA), reinterpret_cast<const AudioNode*>(&tokenB) };
         probe.Arm(nodes, 2);
         std::vector<float> left(256), right(256);
         for (int i = 0; i < 256; i++)
         {
            left[(size_t)i] = (float)i;
            right[(size_t)i] = (float)-i;
         }
         float* chans[2] = { left.data(), right.data() };
         AudioBuffer buf;
         buf.channels = chans;
         buf.numChannels = 2;
         buf.numFrames = 256;
         probe.BeginBlock(sr);
         probe.OnNodeProcessed(nodes[0], &buf, 256);
         probe.OnNodeProcessed(nodes[1], nullptr, 256);
         std::vector<float> got(1024);
         const int na = probe.Drain(0, got.data(), 1024);
         check(na == 512 && got[2] == 1.0f && got[3] == -1.0f, "probe slot A interleaved", na);
         const int nb = probe.Drain(1, got.data(), 1024);
         check(nb == 512 && got[100] == 0.0f, "probe slot B silence when no output", nb);
         probe.Disarm();
         probe.BeginBlock(sr);
         probe.OnNodeProcessed(nodes[0], &buf, 256);
         check(probe.Drain(0, got.data(), 1024) == 0, "probe disarmed writes nothing", 0.0);
      }

      printf("AUDIOANALYSISTEST %s\n", ok ? "OK" : "FAIL");
      return ok;
   }
}
