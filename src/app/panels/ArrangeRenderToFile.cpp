// "Render to file" / "Export audio": bounce a node's audio (an Audio Out: what
// feeds it; a Mixer or any other audio node: its output; a group: the audio
// that leaves it) offline into a WAV file, without touching the timeline.
//
// Flow (one job at a time, pumped once a frame from ArrangeRenderQueueTick):
//   Start   - validate, resolve the path, open the WAV (so a bad path fails
//             before the device is touched), then an OfflineTake
//             (graph/OfflineTake.cpp): device stopped, transport parked at
//             startSeconds in offline mode, the taps forced into the topology,
//             every node re-prepared at the take's rate (fresh voices, tails,
//             step clocks), AudioProbe armed in offline mode on the taps.
//   Render  - OfflineTakeRender block by block (~60 ms of work per frame):
//             modulation and the audio nodes' cook step on the audio clock,
//             then ProcessOffline. After each block the taps are drained,
//             summed, converted to the asked rate when it differs from the
//             take's (StreamResampler, windowed sinc) and appended to the file
//             - nothing is held beyond one block, so the WAV format's 4 GiB is
//             the only length limit. Peak and RMS of what is written are
//             accumulated on the way.
//   Finish  - restore everything the take borrowed (OfflineTakeRestore), fix
//             up the WAV header, report peak/RMS. Cancel or a failure removes
//             the partial file.
//
// The file holds exactly llround(duration * rate) + llround(tail * rate)
// frames at the asked rate, stereo, 16/24-bit PCM or 32-bit float, with only
// fmt (+ fact for float) and data chunks (audio/WavFormat.h).
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "arrange/ArrangeModel.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioEngine.h"
#include "audio/AudioProbe.h"
#include "audio/WavFormat.h"
#include "audio/dsp/StreamResampler.h"
#include "core/Modulation.h"
#include "core/Transport.h"
#include "nodes/AudioNodes.h"
#include "nodes/EquationNode.h"
#include "nodes/ModulatorNodes.h"
#include "nodes/NoteNodes.h"
#include "nodes/OscillatorNode.h"
#include "platform/PlatformAudio.h"

#include <fstream>
#include <iterator>

namespace app
{
   namespace
   {
      struct FileJob
      {
         enum Stage { kIdle, kRendering } stage = kIdle;
         std::string title;
         std::vector<uint64_t> tapUids;
         double startSeconds = 0.0, durationSeconds = 0.0, tailSeconds = 0.0;
         int bitDepth = 24;
         double outRate = 48000.0;  // the file's rate
         double rate = 0.0;         // the take's (graph's) rate
         long long outFramesTotal = 0;
         long long inFramesTotal = 0;
         long long framesDone = 0;  // rendered (take rate)
         long long framesWritten = 0;
         bool resample = false;
         StreamResampler resampler;
         std::vector<std::vector<float>> pending; // per tap, drained but not yet mixed
         std::vector<float> mix, converted;
         double sumSquares = 0.0;
         double peak = 0.0;
         bool cancel = false;
         bool fromUi = false;
         OfflineTake take;
      };

      FileJob gJob;
      // Outside FileJob: the job is reset by assignment, and a writer must
      // never be copied (it owns its FILE*).
      AudioFileWriter gWriter;
      RenderToFileStatus gStatus;

      // Popup state (canvas context menu -> "Render to file..." / "Export audio...").
      int gPopupNode = -1;
      bool gPopupOpenRequest = false;
      bool gPopupExport = false; // Mixer / Audio Out wording
      float gPopupStart = 0.0f, gPopupLength = 60.0f, gPopupTail = 2.0f;
      int gPopupFormat = 1; // 0 = 16-bit, 1 = 24-bit, 2 = 32-bit float
      int gPopupRate = 1;   // index into kRates
      std::string gPopupPath;
      std::string gPopupError;
      bool gResultOpen = false;

      const int kRates[] = { 44100, 48000, 88200, 96000, 192000 };
      const char* const kFormatNames[] = { "16-bit PCM", "24-bit PCM", "32-bit float" };

      double ToDb(double amplitude) { return amplitude > 1e-6 ? 20.0 * std::log10(amplitude) : -120.0; }

      bool EndsWithWav(const std::string& p)
      {
         if (p.size() < 4)
            return false;
         std::string ext = p.substr(p.size() - 4);
         for (char& c : ext)
            c = (char)std::tolower((unsigned char)c);
         return ext == ".wav";
      }

      // Absolute stays; relative goes under the recordings folder; empty = the
      // recordings folder + the node's title (never overwriting a file there).
      // ".wav" is appended when missing.
      std::string ResolvePath(const std::string& asked, const std::string& title)
      {
         const std::string dir = AudioRecordings::GetRecordingsDirectory();
         std::string p = asked;
         if (p.empty())
         {
            p = dir + "/" + RenderSafeFilePart(title) + ".wav";
            std::error_code ec;
            if (std::filesystem::exists(AppPaths::FsPath(p), ec))
            {
               const std::string stem = p.substr(0, p.size() - 4);
               for (int n = 2; n < 10000; n++)
               {
                  const std::string candidate = stem + " (" + std::to_string(n) + ").wav";
                  if (!std::filesystem::exists(AppPaths::FsPath(candidate), ec))
                  {
                     p = candidate;
                     break;
                  }
               }
            }
            return p;
         }
         if (!AppPaths::FsPath(p).is_absolute())
            p = dir + "/" + p;
         if (!EndsWithWav(p))
            p += ".wav";
         return p;
      }

      void RemovePartialFile(const std::string& path)
      {
         std::error_code ec;
         if (!path.empty())
            std::filesystem::remove(AppPaths::FsPath(path), ec);
      }

      bool ArmTaps(std::string& err)
      {
         const AudioNode* targets[AudioProbe::kSlots] = {};
         for (size_t i = 0; i < gJob.tapUids.size(); i++)
         {
            GraphNode* gn = FindNodeByUid(gJob.tapUids[i]);
            auto* src = gn != nullptr ? dynamic_cast<IAudioSource*>(gn->node.get()) : nullptr;
            targets[i] = src != nullptr ? src->GetAudioNode() : nullptr;
            if (targets[i] == nullptr)
            {
               err = "a node to render has no audio engine node";
               return false;
            }
         }
         AudioProbe::Instance().Arm(targets, (int)gJob.tapUids.size());
         return true;
      }

      void Restore()
      {
         AudioProbe::Instance().Disarm();
         AudioProbe::Instance().SetOffline(false);
         OfflineTakeRestore(gJob.take); // also empties RenderToTrackForcedNodes()
      }

      void Finish(bool ok, const std::string& message)
      {
         gJob.stage = FileJob::kIdle;
         gJob.pending.clear();
         gJob.mix.clear();
         gJob.mix.shrink_to_fit();
         gJob.converted.clear();
         gJob.converted.shrink_to_fit();
         gStatus.active = false;
         gStatus.done = true;
         gStatus.ok = ok;
         gStatus.progress = ok ? 1.0f : gStatus.progress;
         gStatus.message = message;
         if (!ok)
         {
            if (gWriter.IsOpen())
               gWriter.Close();
            RemovePartialFile(gStatus.path);
            fprintf(stderr, "render to file failed: %s\n", message.c_str());
         }
         if (gJob.fromUi)
            gResultOpen = true;
      }

      // Appends interleaved frames to the file and to the level meters.
      void WriteFrames(const float* interleaved, long long frames)
      {
         if (frames <= 0)
            return;
         const bool pcm = gJob.bitDepth != 32;
         const size_t n = (size_t)frames * 2;
         for (size_t i = 0; i < n; i++)
         {
            double v = std::isfinite(interleaved[i]) ? (double)interleaved[i] : 0.0;
            if (pcm)
               v = std::clamp(v, -1.0, 1.0); // what the PCM encoder keeps
            gJob.peak = std::max(gJob.peak, std::fabs(v));
            gJob.sumSquares += v * v;
         }
         gWriter.Append(interleaved, (int)frames);
         gJob.framesWritten += frames;
      }

      // Drains the probe after a block of `frames`, sums the taps over exactly
      // that block (a tap that delivered nothing counts as silence) and writes
      // it, converted to the file's rate when needed.
      void MixAndWriteBlock(int frames)
      {
         AudioProbe& probe = AudioProbe::Instance();
         static float sChunk[8192];
         gJob.mix.assign((size_t)frames * 2, 0.0f);
         for (size_t s = 0; s < gJob.pending.size(); s++)
         {
            std::vector<float>& pend = gJob.pending[s];
            int n;
            while ((n = probe.Drain((int)s, sChunk, 8192)) > 0)
               pend.insert(pend.end(), sChunk, sChunk + n);
            const size_t take = std::min(pend.size(), gJob.mix.size());
            for (size_t i = 0; i < take; i++)
               gJob.mix[i] += pend[i];
            pend.erase(pend.begin(), pend.begin() + (std::ptrdiff_t)take);
         }
         const long long room = gJob.outFramesTotal - gJob.framesWritten;
         if (!gJob.resample)
         {
            WriteFrames(gJob.mix.data(), std::min<long long>(frames, room));
            return;
         }
         gJob.converted.clear();
         gJob.resampler.Push(gJob.mix.data(), frames, gJob.converted, gJob.outFramesTotal);
         WriteFrames(gJob.converted.data(), std::min<long long>((long long)gJob.converted.size() / 2, room));
      }

      void Complete()
      {
         // The converter's last outputs read the zero padding past the take.
         if (gJob.resample && gJob.framesWritten < gJob.outFramesTotal)
         {
            gJob.converted.clear();
            gJob.resampler.Finish(gJob.converted, gJob.outFramesTotal);
            WriteFrames(gJob.converted.data(),
                        std::min<long long>((long long)gJob.converted.size() / 2, gJob.outFramesTotal - gJob.framesWritten));
         }
         // Never short: pad with silence (only reachable if a converter
         // rounding left a frame out).
         if (gJob.framesWritten < gJob.outFramesTotal)
         {
            std::vector<float> zeros((size_t)std::min<long long>(gJob.outFramesTotal - gJob.framesWritten, 65536) * 2, 0.0f);
            while (gJob.framesWritten < gJob.outFramesTotal)
               WriteFrames(zeros.data(), std::min<long long>((long long)zeros.size() / 2, gJob.outFramesTotal - gJob.framesWritten));
         }
         gWriter.Close();
         if (!gWriter.WriteOk())
         {
            Finish(false, "could not write " + gStatus.path + " (disk full or removed?)");
            return;
         }
         const double samples = (double)gJob.framesWritten * 2.0;
         gStatus.peakDb = (float)ToDb(gJob.peak);
         gStatus.rmsDb = samples > 0.0 && gJob.sumSquares > 0.0
                            ? (float)std::max(-120.0, 10.0 * std::log10(gJob.sumSquares / samples))
                            : -120.0f;
         char buf[160];
         snprintf(buf, sizeof(buf), "rendered %s: %.2f s at %d Hz, peak %.1f dBFS, RMS %.1f dBFS", gJob.title.c_str(),
                  gStatus.seconds, (int)std::llround(gJob.outRate), (double)gStatus.peakDb, (double)gStatus.rmsDb);
         Finish(true, buf);
      }
   }


   const RenderToFileStatus& RenderToFileState() { return gStatus; }
   bool RenderToFileBusy() { return gJob.stage == FileJob::kRendering; }
   void CancelRenderToFile()
   {
      if (gJob.stage == FileJob::kRendering)
         gJob.cancel = true;
   }


   bool StartRenderToFile(int nodeIndex, double startSeconds, double durationSeconds, double tailSeconds,
                          const std::string& path, int bitDepth, double sampleRate, std::string& err)
   {
      if (gJob.stage != FileJob::kIdle)
      {
         err = "a render to file is already running";
         return false;
      }
      if (ArrangeRenderBusy() || gOfflineRender.active || Transport::Instance().IsOfflineMode() ||
          AudioProbe::Instance().IsArmed())
      {
         err = "another render or audio capture is running; wait for it to finish";
         return false;
      }
      GraphNode* gn = FindNodeByIndex(nodeIndex);
      if (gn == nullptr || gn->node == nullptr)
      {
         err = "unknown node index " + std::to_string(nodeIndex);
         return false;
      }
      if (!std::isfinite(startSeconds) || startSeconds < 0.0)
      {
         err = "start must be >= 0 seconds";
         return false;
      }
      if (!std::isfinite(durationSeconds) || !(durationSeconds > 0.0))
      {
         err = "the length must be > 0 seconds";
         return false;
      }
      if (!std::isfinite(tailSeconds) || tailSeconds < 0.0)
         tailSeconds = 0.0;
      tailSeconds = std::min(tailSeconds, 60.0);
      if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32)
      {
         err = "bitDepth must be 16, 24 or 32 (float)";
         return false;
      }
      if (!(sampleRate > 0.0))
         sampleRate = 48000.0;
      if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0)
      {
         err = "sampleRate must be 8000..384000 Hz";
         return false;
      }
      std::vector<uint64_t> taps;
      if (!RenderTapsForNode(*gn, taps, err))
         return false;

      const WavFormat::Encoding enc = bitDepth == 16 ? WavFormat::kPcm16 : (bitDepth == 24 ? WavFormat::kPcm24 : WavFormat::kFloat32);
      const long long outFrames = (long long)std::llround(durationSeconds * sampleRate) + (long long)std::llround(tailSeconds * sampleRate);
      if ((uint64_t)outFrames * 2ull * (uint64_t)WavFormat::BytesPerSample(enc) > WavFormat::MaxDataBytes(enc))
      {
         err = "that is more than a WAV file can hold (4 GiB of audio data); render a shorter span or at 16-bit";
         return false;
      }

      gJob = FileJob();
      gJob.title = ArrangeSourceTitle(*gn);
      gJob.tapUids = taps;
      gJob.startSeconds = startSeconds;
      gJob.durationSeconds = durationSeconds;
      gJob.tailSeconds = tailSeconds;
      gJob.bitDepth = bitDepth;
      gJob.outRate = sampleRate;
      gJob.outFramesTotal = outFrames;
      gJob.rate = OfflineTakeRate();
      gJob.resample = std::fabs(gJob.rate - gJob.outRate) > 0.5;
      if (gJob.resample)
      {
         gJob.resampler.Prepare(gJob.rate, gJob.outRate, 2);
         gJob.inFramesTotal = (long long)std::ceil((double)outFrames * gJob.rate / gJob.outRate) + gJob.resampler.Lookahead();
      }
      else
         gJob.inFramesTotal = outFrames;
      gJob.pending.assign(taps.size(), std::vector<float>());

      gStatus = RenderToFileStatus();
      gStatus.path = ResolvePath(path, gJob.title);
      gStatus.seconds = (double)outFrames / sampleRate;
      gStatus.sampleRate = sampleRate;
      gStatus.channels = 2;
      {
         std::error_code ec;
         const std::filesystem::path parent = AppPaths::FsPath(gStatus.path).parent_path();
         if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
      }
      const AudioFileWriter::WavEncoding wavEnc = bitDepth == 16 ? AudioFileWriter::WavEncoding::Pcm16
                                                  : (bitDepth == 24 ? AudioFileWriter::WavEncoding::Pcm24
                                                                    : AudioFileWriter::WavEncoding::Float32);
      if (!gWriter.Open(gStatus.path, sampleRate, 2, AudioFileWriter::Format::Wav, wavEnc))
      {
         err = "could not create " + gStatus.path;
         gStatus = RenderToFileStatus();
         gJob = FileJob();
         return false;
      }
      gStatus.active = true;
      gStatus.message = "rendering " + gJob.title;

      OfflineTakeBegin(gJob.take, gJob.rate, startSeconds);
      AudioProbe::Instance().SetOffline(true);
      gJob.stage = FileJob::kRendering;
      OfflineTakeForceNodes(taps); // before the rebuild: it reads RenderToTrackForcedNodes()
      OfflineTakePrepare(gJob.take);
      if (!ArmTaps(err))
      {
         Restore();
         Finish(false, err);
         return false;
      }
      return true;
   }


   void PumpRenderToFile()
   {
      if (gJob.stage != FileJob::kRendering)
         return;
      for (uint64_t uid : gJob.tapUids)
         if (FindNodeByUid(uid) == nullptr)
         {
            Restore();
            Finish(false, "a node being rendered was deleted");
            return;
         }

      static float sL[kAudioMaxBlockFrames];
      static float sR[kAudioMaxBlockFrames];
      const double budgetStart = glfwGetTime();
      const int blockCap = OfflineTakeBlockFrames();
      while (!gJob.cancel && gJob.framesDone < gJob.inFramesTotal)
      {
         const int frames = (int)std::min<long long>(blockCap, gJob.inFramesTotal - gJob.framesDone);
         OfflineTakeRender(gJob.take, sL, sR, frames);
         gJob.framesDone += frames;
         MixAndWriteBlock(frames);
         if (!gWriter.WriteOk())
            break;
         if (glfwGetTime() - budgetStart > 0.06)
            break;
      }
      gStatus.progress = (float)((double)gJob.framesWritten / (double)std::max<long long>(1, gJob.outFramesTotal));
      if (!gWriter.WriteOk())
      {
         Restore();
         Finish(false, "could not write " + gStatus.path + " (disk full or removed?)");
         return;
      }
      if (!gJob.cancel && gJob.framesDone < gJob.inFramesTotal)
         return;

      const bool cancelled = gJob.cancel;
      Restore();
      if (cancelled)
      {
         Finish(false, "cancelled");
         return;
      }
      Complete();
   }


   // Same dim + click-catcher as render to track: the take drives the graph
   // synchronously from the main loop, so editing underneath it would be
   // editing what is being rendered.
   void DrawRenderToFileProgressWindow()
   {
      ImGuiIO& io = ImGui::GetIO();
      if (gJob.stage != FileJob::kRendering)
      {
         if (!gResultOpen)
            return;
         // The outcome of a render the user started from the popup.
         ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                 ImVec2(0.5f, 0.5f));
         PushElevatedPanelStyle(/*isChild=*/false);
         ImGui::Begin(L("Render to File##rendertofileresult"), nullptr,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
         ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
         if (gStatus.ok)
         {
            ImGui::TextUnformatted(gStatus.message.c_str());
            ImGui::TextDisabled("%s", gStatus.path.c_str());
         }
         else
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", gStatus.message.c_str());
         ImGui::PopTextWrapPos();
         if (ImGui::Button(L("OK##rendertofileok"), ImVec2(120, 0)))
            gResultOpen = false;
         ImGui::End();
         PopElevatedPanelStyle();
         return;
      }
      ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
      ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::Begin("##RenderToFileBlocker", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::InvisibleButton("##RenderToFileBlockerCatch", io.DisplaySize);
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();

      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.5f));
      PushElevatedPanelStyle(/*isChild=*/false);
      ImGui::Begin(L("Rendering to File"), nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
      if (gJob.cancel)
         ImGui::Text("%s", T("Cancelling..."));
      else
         ImGui::Text(T("%s: %.1fs of %.1fs (%s, %d Hz)"), gJob.title.c_str(), (double)gJob.framesWritten / gJob.outRate,
                     (double)gJob.outFramesTotal / gJob.outRate, kFormatNames[gJob.bitDepth == 16 ? 0 : (gJob.bitDepth == 24 ? 1 : 2)],
                     (int)std::llround(gJob.outRate));
      ImGui::ProgressBar(gStatus.progress, ImVec2(280, 0));
      ImGui::BeginDisabled(gJob.cancel);
      if (ImGui::Button(L("Cancel##rendertofile"), ImVec2(120, 0)))
         gJob.cancel = true;
      ImGui::EndDisabled();
      ImGui::End();
      PopElevatedPanelStyle();
   }


   void OpenRenderToFilePopup(int nodeIndex)
   {
      gPopupNode = nodeIndex;
      gPopupOpenRequest = true;
      gPopupError.clear();
      gPopupPath.clear();
      GraphNode* gn = FindNodeByIndex(nodeIndex);
      gPopupExport = gn != nullptr && (dynamic_cast<AudioOutputNode*>(gn->node.get()) != nullptr ||
                                       dynamic_cast<MixerNode*>(gn->node.get()) != nullptr);
      // Default span: the timeline loop when it is on, else 60 s from 0.
      const Arrange::LoopRange& loop = gArrange.settings.loop;
      const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
      if (loop.enabled && loop.end > loop.start)
      {
         gPopupStart = (float)(Arrange::TicksToBeats(loop.start) * 60.0 / bpm);
         gPopupLength = (float)(Arrange::TicksToBeats(loop.end - loop.start) * 60.0 / bpm);
      }
      else
      {
         gPopupStart = 0.0f;
         gPopupLength = 60.0f;
      }
   }


   void DrawRenderToFilePopup()
   {
      const char* kId = "Render to File##rendertofilepopup";
      if (gPopupOpenRequest)
      {
         ImGui::OpenPopup(kId);
         gPopupOpenRequest = false;
      }
      ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                              ImVec2(0.5f, 0.5f));
      if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
         return;
      GraphNode* gn = FindNodeByIndex(gPopupNode);
      if (gn == nullptr)
      {
         ImGui::CloseCurrentPopup();
         ImGui::EndPopup();
         return;
      }
      const std::string title = ArrangeSourceTitle(*gn);
      ImGui::Text(gPopupExport ? T("Export the audio of %s") : T("Render %s to a WAV file"), title.c_str());
      ImGui::Separator();
      const float fieldW = 160.0f;
      ImGui::SetNextItemWidth(fieldW);
      ImGui::DragFloat(L("Start##r2fstart"), &gPopupStart, 0.05f, 0.0f, 36000.0f, "%.2f s");
      gPopupStart = std::max(0.0f, gPopupStart);
      ImGui::SetNextItemWidth(fieldW);
      ImGui::DragFloat(L("Length##r2flen"), &gPopupLength, 0.1f, 0.01f, 36000.0f, "%.2f s");
      gPopupLength = std::max(0.01f, gPopupLength);
      ImGui::SetNextItemWidth(fieldW);
      ImGui::DragFloat(L("Tail##r2ftail"), &gPopupTail, 0.05f, 0.0f, 60.0f, "%.2f s");
      gPopupTail = std::clamp(gPopupTail, 0.0f, 60.0f);
      ImGui::SetNextItemWidth(fieldW);
      ImGui::Combo(L("Format##r2ffmt"), &gPopupFormat, kFormatNames, 3);
      ImGui::SetNextItemWidth(fieldW);
      const std::string rateLabel = std::to_string(kRates[gPopupRate]) + " Hz";
      if (ImGui::BeginCombo(L("Sample rate##r2frate"), rateLabel.c_str()))
      {
         for (int i = 0; i < (int)(sizeof(kRates) / sizeof(kRates[0])); i++)
            if (ImGui::Selectable((std::to_string(kRates[i]) + " Hz").c_str(), i == gPopupRate))
               gPopupRate = i;
         ImGui::EndCombo();
      }
      ImGui::SetNextItemWidth(fieldW * 1.6f);
      ImGui::InputTextWithHint(L("##r2fpath"), T("recordings folder"), &gPopupPath);
      ImGui::SameLine();
      if (ImGui::Button(L("Browse...##r2fbrowse")))
      {
         const std::string chosen = Platform::SaveWavDialog(RenderSafeFilePart(title) + ".wav");
         if (!chosen.empty())
            gPopupPath = chosen;
      }
      const double engineRate = OfflineTakeRate();
      if (std::fabs(engineRate - (double)kRates[gPopupRate]) > 0.5)
         ImGui::TextDisabled(T("Rendered at %d Hz, converted to %d Hz"), (int)std::llround(engineRate), kRates[gPopupRate]);
      ImGui::TextDisabled(T("%.2f s + %.2f s tail"), (double)gPopupLength, (double)gPopupTail);
      if (!gPopupError.empty())
      {
         ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
         ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", gPopupError.c_str());
         ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      ImGui::BeginDisabled(gJob.stage != FileJob::kIdle || ArrangeRenderBusy());
      if (ImGui::Button(gPopupExport ? L("Export##r2fgo") : L("Render##r2fgo"), ImVec2(120, 0)))
      {
         std::string err;
         const int bits = gPopupFormat == 0 ? 16 : (gPopupFormat == 1 ? 24 : 32);
         if (StartRenderToFile(gPopupNode, (double)gPopupStart, (double)gPopupLength, (double)gPopupTail, gPopupPath, bits,
                               (double)kRates[gPopupRate], err))
         {
            gJob.fromUi = true;
            ImGui::CloseCurrentPopup();
         }
         else
            gPopupError = err;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(L("Cancel##r2fcancel"), ImVec2(120, 0)))
         ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
   }


   // ---- INFINITE_RENDERFILETEST ------------------------------------------------
   //
   // A frame test (not a Startup hook): the renders run through the real pump
   // (ArrangeRenderQueueTick, from DrawFramePump), where the node bodies have
   // registered their parameters, so modulation is stepped exactly as for a
   // user's render. Any audio device is closed for the test and the take rate
   // pinned to 48 kHz (gHeadlessAudioRate), so the numbers are the same on
   // every machine. Checks:
   //   A. an Oscillator, 2.0 s + 0.5 s tail at 48 kHz: exactly 120000 frames,
   //      exact 16/24/float headers, chunks fmt+data (float: fmt+fact+data);
   //   B. two renders with the same parameters are byte-identical;
   //   C. the render equals an offline analyze capture of the same node/span
   //      (correlation > 0.99, in practice bit-identical);
   //   D. a render at 44.1 kHz (converted) has llround(2 * 44100) frames and
   //      the same level as the native one;
   //   E. BUG 1: an Equation Synth with a 1 s attack, played by a Note
   //      Sequencer, rendered (and captured) from 0 after "live" playback left
   //      it sounding: the first 50 ms stay >= 20 dB under the level at 1 s;
   //   F. BUG 2: Note Sequencer (10 beats, 4 steps): 96 bpm, play, stop, 60 bpm,
   //      rewind, play -> a note at beat 0 (driven by hand with ProcessOffline);
   //   G. BUG 3: LFO (10 beats) -> Audio Filter freq on a saw over 10 beats: the
   //      spectral centroid follows the LFO (max/min > 2, most 0.25 s frames
   //      differ from the one before, peak and trough where the LFO has them).
   namespace
   {
      std::vector<uint8_t> ReadFileBytes(const std::string& path)
      {
         std::ifstream f(AppPaths::FsPath(path), std::ios::binary);
         return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      }

      bool DecodeWav(const std::vector<uint8_t>& bytes, WavFormat::Info& info, std::vector<float>& interleaved)
      {
         info = WavFormat::Parse(bytes);
         interleaved.clear();
         if (!info.ok)
            return false;
         const uint64_t samples = info.frames * info.channels;
         interleaved.resize((size_t)samples);
         for (uint64_t i = 0; i < samples; i++)
            interleaved[(size_t)i] = WavFormat::SampleAt(bytes, info, i);
         return true;
      }

      // RMS in dBFS of stereo frames [f0, f1).
      double RangeRmsDb(const std::vector<float>& inter, size_t f0, size_t f1)
      {
         f1 = std::min(f1, inter.size() / 2);
         if (f1 <= f0)
            return -120.0;
         double sum = 0.0;
         for (size_t i = f0 * 2; i < f1 * 2; i++)
            sum += (double)inter[i] * (double)inter[i];
         const double mean = sum / (double)((f1 - f0) * 2);
         return mean > 1e-12 ? 10.0 * std::log10(mean) : -120.0;
      }

      struct RenderFileTest
      {
         int stage = 0;
         double stageStart = 0.0;
         bool allOk = true;
         int osc = -1, seqA = -1, eqA = -1, outA = -1, osc2 = -1, flt = -1, lfo = -1, seqB = -1, eqB = -1;
         bool deviceWasRunning = false;
         double savedHeadless = 0.0;
         double rate = 48000.0;
         std::string dir;
         std::vector<std::string> files;
         std::string path24;
         int captureId = -1;
         double nativeRmsDb = 0.0;
         std::vector<float> native; // render C's samples
      };
      RenderFileTest gT;

      void Check(bool ok, const std::string& what)
      {
         printf("RENDERFILETEST %s %s\n", what.c_str(), ok ? "OK" : "FAIL");
         gT.allOk = gT.allOk && ok;
      }

      std::string TestPath(const std::string& name)
      {
         const std::string p = gT.dir + "/" + name;
         gT.files.push_back(p);
         return p;
      }

      bool StartTestRender(int node, double start, double dur, double tail, const std::string& name, int bits,
                           double sr)
      {
         std::string err;
         const bool ok = StartRenderToFile(node, start, dur, tail, TestPath(name), bits, sr, err);
         if (!ok)
            Check(false, "render " + name + " started (" + err + ")");
         gT.stageStart = glfwGetTime();
         return ok;
      }

      // True once the running render finished (false while it runs). A
      // render that failed or ran past 60 s of wall time is a FAIL.
      bool RenderDone(bool& ok)
      {
         const RenderToFileStatus& st = RenderToFileState();
         if (st.active && glfwGetTime() - gT.stageStart < 60.0)
            return false;
         ok = st.done && st.ok;
         if (!ok)
         {
            if (st.active)
               CancelRenderToFile();
            Check(false, "render finished (" + st.message + ")");
         }
         return true;
      }

      void CheckHeader(const std::string& path, int bits, long long wantFrames, double wantRate)
      {
         const std::vector<uint8_t> bytes = ReadFileBytes(path);
         const WavFormat::Info info = WavFormat::Parse(bytes);
         const bool isFloat = bits == 32;
         const int bps = bits / 8;
         const std::vector<std::string> wantChunks = isFloat ? std::vector<std::string>{ "fmt ", "fact", "data" }
                                                             : std::vector<std::string>{ "fmt ", "data" };
         const uint32_t sr = (uint32_t)std::llround(wantRate);
         const size_t headerBytes = isFloat ? 58 : 44;
         const uint64_t dataBytes = (uint64_t)wantFrames * 2ull * (uint64_t)bps;
         bool ok = info.ok && info.frames == (uint64_t)wantFrames && info.channels == 2 && info.sampleRate == sr &&
                   info.formatTag == (isFloat ? 3 : 1) && info.bitsPerSample == bits && info.blockAlign == 2 * bps &&
                   info.byteRate == sr * 2u * (uint32_t)bps && info.fmtSize == (isFloat ? 18u : 16u) &&
                   info.chunks == wantChunks && info.dataOffset == headerBytes && info.dataBytes == dataBytes &&
                   (uint64_t)bytes.size() == headerBytes + dataBytes + (dataBytes & 1u) &&
                   (uint64_t)info.riffSize == (uint64_t)bytes.size() - 8ull &&
                   (!isFloat || (info.hasFact && info.factFrames == (uint32_t)wantFrames));
         std::string chunks;
         for (const std::string& c : info.chunks)
            chunks += "'" + c + "' ";
         printf("RENDERFILETEST %d-bit: %llu frames (want %lld), %u Hz, tag %u, bits %u, align %u, rate %u, chunks %s, %zu bytes%s\n",
                bits, (unsigned long long)info.frames, wantFrames, info.sampleRate, info.formatTag, info.bitsPerSample,
                info.blockAlign, info.byteRate, chunks.c_str(), bytes.size(), info.ok ? "" : (" - " + info.error).c_str());
         Check(ok, std::to_string(bits) + "-bit header, frame count and chunk list exact");
      }

      // E: the first 50 ms against the level around 1 s (the 1 s attack's top).
      void CheckFreshStart(const std::vector<float>& inter, double rate, const std::string& what)
      {
         const double first = RangeRmsDb(inter, 0, (size_t)(0.05 * rate));
         const double top = RangeRmsDb(inter, (size_t)(0.9 * rate), (size_t)(1.0 * rate));
         printf("RENDERFILETEST %s: first 50 ms %.1f dB, 0.9-1.0 s %.1f dB\n", what.c_str(), first, top);
         Check(top > -50.0 && first < top - 20.0, what + " starts from silence (fresh voices)");
      }

      // "Live" playback without a device: the graph runs on from where it
      // is, NOT re-prepared - what the device callback does between two
      // renders - for `seconds`, leaving the 1 s-attack pad sounding.
      void LivePlaybackStandIn(double seconds)
      {
         const double rate = OfflineTakeRate();
         Transport::Instance().Seek(0.0);
         Transport::Instance().SetOfflineMode(true, rate);
         Transport::Instance().SetPlaying(true);
         RebuildAudioTopology(); // nothing re-prepared beyond nodes that never were
         GraphNode* eq = FindNodeByIndex(gT.eqA);
         auto* src = eq != nullptr ? dynamic_cast<IAudioSource*>(eq->node.get()) : nullptr;
         const AudioNode* targets[AudioProbe::kSlots] = { src != nullptr ? src->GetAudioNode() : nullptr };
         AudioProbe::Instance().SetOffline(true);
         static float sL[kAudioMaxBlockFrames], sR[kAudioMaxBlockFrames];
         float* channels[2] = { sL, sR };
         const long long total = (long long)std::llround(seconds * rate);
         const long long armAt = total - (long long)std::llround(0.2 * rate);
         std::vector<float> tail;
         float chunk[8192];
         for (long long done = 0; done < total;)
         {
            if (done >= armAt && !AudioProbe::Instance().IsArmed())
               AudioProbe::Instance().Arm(targets, 1);
            const int n = (int)std::min<long long>(512, total - done);
            AudioBuffer buf;
            buf.channels = channels;
            buf.numChannels = 2;
            buf.numFrames = n;
            AudioEngine::Instance().ProcessOffline(buf);
            done += n;
            int got;
            while ((got = AudioProbe::Instance().Drain(0, chunk, 8192)) > 0)
               tail.insert(tail.end(), chunk, chunk + got);
         }
         AudioProbe::Instance().Disarm();
         AudioProbe::Instance().SetOffline(false);
         Transport::Instance().SetOfflineMode(false);
         Transport::Instance().NotifyAudioEngineStopped();
         Transport::Instance().Seek(0.0);
         const double hot = RangeRmsDb(tail, 0, tail.size() / 2);
         printf("RENDERFILETEST live stand-in: pad at %.1f dB after %.1f s\n", hot, seconds);
         Check(hot > -40.0, "live playback left the pad sounding (precondition)");
      }

      // F: driven by hand on an OfflineTake, no pump.
      void RunTempoRewindCheck()
      {
         GraphNode* eq = FindNodeByIndex(gT.eqB);
         auto* src = eq != nullptr ? dynamic_cast<IAudioSource*>(eq->node.get()) : nullptr;
         if (src == nullptr)
         {
            Check(false, "tempo/rewind sequencer fixture built");
            return;
         }
         Transport& tr = Transport::Instance();
         tr.SetTempo(96.0f);
         OfflineTake take;
         const double rate = OfflineTakeRate();
         OfflineTakeBegin(take, rate, 0.0); // the seek commits 96 bpm
         AudioProbe::Instance().SetOffline(true);
         OfflineTakeForceNodes({ eq->uid });
         OfflineTakePrepare(take);
         const AudioNode* targets[AudioProbe::kSlots] = { src->GetAudioNode() };
         AudioProbe::Instance().Arm(targets, 1);
         static float sL[kAudioMaxBlockFrames], sR[kAudioMaxBlockFrames];
         float chunk[8192];
         auto render = [&](double seconds)
         {
            std::vector<float> out;
            const long long total = (long long)std::llround(seconds * rate);
            for (long long done = 0; done < total;)
            {
               const int n = (int)std::min<long long>(512, total - done);
               OfflineTakeRender(take, sL, sR, n);
               done += n;
               int got;
               while ((got = AudioProbe::Instance().Drain(0, chunk, 8192)) > 0)
                  out.insert(out.end(), chunk, chunk + got);
            }
            return out;
         };
         const std::vector<float> firstRun = render(1.0);   // note at 0, off after 1 beat (0.625 s)
         tr.SetPlaying(false);                              // stop inside step 0
         const std::vector<float> stopped = render(0.5);
         tr.SetTempo(60.0f);
         tr.Seek(0.0);                                      // rewind (commits 60 bpm)
         take.startSeconds = 0.0;                           // the take's own clock follows the rewind
         take.framesDone = 0;
         take.nextModFrame = 0;
         tr.SetPlaying(true);
         const std::vector<float> secondRun = render(0.5);
         AudioProbe::Instance().Disarm();
         AudioProbe::Instance().SetOffline(false);
         OfflineTakeRestore(take);
         tr.SetTempo(120.0f);
         tr.Seek(0.0);
         const double a = RangeRmsDb(firstRun, 0, (size_t)(0.25 * rate));
         const double quiet = RangeRmsDb(stopped, (size_t)(0.25 * rate), (size_t)(0.5 * rate));
         const double b = RangeRmsDb(secondRun, 0, (size_t)(0.1 * rate));
         printf("RENDERFILETEST tempo/rewind: first run %.1f dB, stopped %.1f dB, after rewind at 60 bpm %.1f dB\n", a,
                quiet, b);
         Check(a > -40.0 && quiet < -60.0 && b > -40.0, "Note Sequencer fires step 0 at beat 0 after stop, tempo change and rewind");
      }

      void FinishTest(GLFWwindow* window)
      {
         AudioProbeKeepLastCapture(false);
         gHeadlessAudioRate = gT.savedHeadless;
         if (gT.deviceWasRunning)
            StartAudioEngine(gAudioStartError);
         std::error_code ec;
         for (const std::string& f : gT.files)
            std::filesystem::remove(AppPaths::FsPath(f), ec);
         printf("RENDERFILETEST all %s\n", gT.allOk ? "OK" : "FAIL");
         gT.stage = 99;
         glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
   }


   void FrameTest_RENDERFILETEST(int frameId, GLFWwindow* window)
   {
      if (getenv("INFINITE_RENDERFILETEST") == nullptr || gT.stage == 99)
         return;
      bool ok = false;
      if (gT.stage == 0)
      {
         if (frameId != 4)
            return;
         NewPatch();
         gT.deviceWasRunning = AudioEngine::Instance().SampleRate() > 0.0;
         if (gT.deviceWasRunning)
            AudioEngine::Instance().Stop();
         gT.savedHeadless = gHeadlessAudioRate;
         gHeadlessAudioRate = 48000.0; // OfflineTakeRate() = 48 kHz on every machine
         gT.rate = OfflineTakeRate();
         gT.dir = AppPaths::TempDir() + "/infinite_renderfiletest";
         AppPaths::EnsureDir(gT.dir);
         Transport::Instance().SetTempo(120.0f);
         Transport::Instance().Seek(0.0);
         Transport::Instance().SetPlaying(true);
         // Indices only: SpawnNode can reallocate gNodes under an older GraphNode*.
         auto spawn = [](const char* type, const char* cat, float x, float y) {
            GraphNode* gn = SpawnNode(type, cat, x, y);
            return gn != nullptr ? gn->index : -1;
         };
         gT.osc = spawn("Oscillator", "Synths", 0.0f, 0.0f);
         gT.seqA = spawn("Note Sequencer", "Notes", 0.0f, 400.0f);
         gT.eqA = spawn("Equation Synth", "Synths", 400.0f, 400.0f);
         gT.outA = spawn("Audio Out", "Utility", 900.0f, 400.0f);
         gT.osc2 = spawn("Oscillator", "Synths", 0.0f, 900.0f);
         gT.flt = spawn("Audio Filter", "AudioEffects", 400.0f, 900.0f);
         gT.lfo = spawn("LFO", "Modulators", 900.0f, 900.0f);
         gT.seqB = spawn("Note Sequencer", "Notes", 0.0f, 1400.0f);
         gT.eqB = spawn("Equation Synth", "Synths", 400.0f, 1400.0f);
         std::string err;
         bool wired = ConnectNodes(gT.seqA, 0, gT.eqA, 0, err) && ConnectNodes(gT.eqA, 0, gT.outA, 0, err) &&
                      ConnectNodes(gT.osc2, 0, gT.flt, 0, err) && ConnectNodes(gT.seqB, 0, gT.eqB, 0, err);
         auto nodeOf = [](int idx) -> INode* {
            GraphNode* gn = FindNodeByIndex(idx);
            return gn != nullptr ? gn->node.get() : nullptr;
         };
         auto* seqA = dynamic_cast<NoteSequencerNode*>(nodeOf(gT.seqA));
         auto* eqA = dynamic_cast<EquationNode*>(nodeOf(gT.eqA));
         auto* seqB = dynamic_cast<NoteSequencerNode*>(nodeOf(gT.seqB));
         auto* eqB = dynamic_cast<EquationNode*>(nodeOf(gT.eqB));
         auto* lfo = dynamic_cast<LFONode*>(nodeOf(gT.lfo));
         wired = wired && seqA != nullptr && eqA != nullptr && seqB != nullptr && eqB != nullptr && lfo != nullptr &&
                 dynamic_cast<OscillatorNode*>(nodeOf(gT.osc)) != nullptr;
         if (wired)
         {
            seqA->steps = 1;          // one note every 4 beats (2 s at 120 bpm), held 90%
            seqA->rateMode = 0;
            seqA->rateBeats = 4.0f;
            seqA->gatePercent = 90.0f;
            eqA->ampAttack = 1000.0f; // the slow pad
            eqA->ampSustain = 1.0f;
            eqA->ampRelease = 50.0f;
            seqB->steps = 4;          // the breathing patch's sequencer: 10 beats a step
            seqB->rateMode = 0;
            seqB->rateBeats = 10.0f;
            seqB->gatePercent = 10.0f;
            eqB->ampAttack = 1.0f;
            eqB->ampSustain = 1.0f;
            eqB->ampRelease = 10.0f;
            lfo->shape = 0;           // sine, one cycle over 10 beats
            lfo->rateBeats = 10.0f;
            lfo->low = 0.0f;
            lfo->high = 1.0f;
            if (GraphNode* f = FindNodeByIndex(gT.flt))
               f->showParams = true;  // the freq knob draws, so its ParamRef registers
         }
         Check(wired, "fixture built");
         if (!wired)
         {
            FinishTest(window);
            return;
         }
         AudioProbeKeepLastCapture(true);
         gT.stage = 1;
         return;
      }
      if (gT.stage == 1)
      {
         if (frameId < 8)
            return;
         // The filter has drawn: bind LFO -> freq over 200..8000 Hz.
         int freqParam = -1;
         for (const auto& kv : Modulation::Instance().AllKnownParams())
            if (kv.first.first == gT.flt && kv.second.name.rfind("freq", 0) == 0)
            {
               freqParam = kv.first.second;
               break;
            }
         Check(freqParam >= 0, "Audio Filter freq is a modulatable parameter");
         if (freqParam >= 0)
         {
            Modulation::Instance().Bind(gT.flt, freqParam, gT.lfo, 0);
            Modulation::Instance().SetRange(gT.flt, freqParam, 200.0f, 8000.0f);
         }
         if (StartTestRender(gT.osc, 0.0, 2.0, 0.5, "osc16.wav", 16, 48000.0))
            gT.stage = 2;
         else
            FinishTest(window);
         return;
      }
      if (gT.stage >= 2 && gT.stage <= 5)
      {
         if (!RenderDone(ok))
            return;
         const long long want = 96000 + 24000; // 2.0 s + 0.5 s tail at 48 kHz
         if (gT.stage == 2)
         {
            if (ok)
               CheckHeader(gT.files.back(), 16, want, 48000.0);
            gT.stage = StartTestRender(gT.osc, 0.0, 2.0, 0.5, "osc24.wav", 24, 48000.0) ? 3 : 98;
         }
         else if (gT.stage == 3)
         {
            if (ok)
               CheckHeader(gT.files.back(), 24, want, 48000.0);
            gT.path24 = gT.files.back();
            gT.stage = StartTestRender(gT.osc, 0.0, 2.0, 0.5, "osc32.wav", 32, 48000.0) ? 4 : 98;
         }
         else if (gT.stage == 4)
         {
            if (ok)
               CheckHeader(gT.files.back(), 32, want, 48000.0);
            gT.stage = StartTestRender(gT.osc, 0.0, 2.0, 0.5, "osc24b.wav", 24, 48000.0) ? 5 : 98;
         }
         else if (gT.stage == 5)
         {
            const std::vector<uint8_t> a = ReadFileBytes(gT.path24);
            const std::vector<uint8_t> b = ReadFileBytes(gT.files.back());
            Check(ok && !a.empty() && a == b, "two renders with the same parameters are byte-identical");
            // 7 = wait for it, then the offline capture to compare with
            gT.stage = StartTestRender(gT.osc, 0.0, 2.0, 0.0, "oscNative.wav", 32, gT.rate) ? 7 : 98;
         }
         if (gT.stage == 98)
            FinishTest(window);
         return;
      }
      if (gT.stage == 7)
      {
         if (!RenderDone(ok))
            return;
         WavFormat::Info info;
         const bool decoded = ok && DecodeWav(ReadFileBytes(gT.files.back()), info, gT.native);
         gT.nativeRmsDb = RenderToFileState().rmsDb;
         Check(decoded, "native-rate render decodes");
         nlohmann::json res;
         std::string err;
         const nlohmann::json params = { { "node", gT.osc }, { "seconds", 2.0 }, { "offline", true }, { "start", 0.0 } };
         if (HandleAudioProbeRpc("capture_audio", params, res, err) && res.contains("id"))
         {
            gT.captureId = res["id"].get<int>();
            gT.stageStart = glfwGetTime();
            gT.stage = 8;
         }
         else
         {
            Check(false, "offline capture started (" + err + ")");
            FinishTest(window);
         }
         return;
      }
      // Polls the running capture; true once it is done (ok set).
      auto captureDone = [&](bool& capOk) {
         nlohmann::json res;
         std::string err;
         HandleAudioProbeRpc("analysis_result", { { "id", gT.captureId } }, res, err);
         const std::string status = res.value("status", std::string("error"));
         if ((status == "capturing" || status == "analyzing") && glfwGetTime() - gT.stageStart < 60.0)
            return false;
         capOk = status == "done";
         if (!capOk)
            Check(false, "offline capture finished (" + res.dump() + ")");
         return true;
      };
      if (gT.stage == 8)
      {
         bool capOk = false;
         if (!captureDone(capOk))
            return;
         std::vector<float> cap;
         double capRate = 0.0;
         if (capOk && AudioProbeLastCapture(cap, capRate))
         {
            const size_t n = std::min(cap.size(), gT.native.size());
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, maxDiff = 0;
            for (size_t i = 0; i < n; i++)
            {
               const double a = gT.native[i], b = cap[i];
               sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
               maxDiff = std::max(maxDiff, std::fabs(a - b));
            }
            const double dn = (double)std::max<size_t>(1, n);
            const double cov = sab / dn - (sa / dn) * (sb / dn);
            const double va = saa / dn - (sa / dn) * (sa / dn), vb = sbb / dn - (sb / dn) * (sb / dn);
            const double corr = va > 0.0 && vb > 0.0 ? cov / std::sqrt(va * vb) : 0.0;
            printf("RENDERFILETEST render vs offline capture: %zu / %zu samples, correlation %.6f, max diff %.3g\n", n,
                   gT.native.size(), corr, maxDiff);
            Check(n == gT.native.size() && corr > 0.99, "render to file matches an offline capture of the same span");
         }
         else
            Check(false, "offline capture kept its samples");
         const double other = std::fabs(gT.rate - 44100.0) < 0.5 ? 48000.0 : 44100.0;
         gT.stage = StartTestRender(gT.osc, 0.0, 2.0, 0.0, "oscConverted.wav", 32, other) ? 9 : 98;
         if (gT.stage == 98)
            FinishTest(window);
         return;
      }
      if (gT.stage == 9)
      {
         if (!RenderDone(ok))
            return;
         const double other = RenderToFileState().sampleRate;
         const std::vector<uint8_t> bytes = ReadFileBytes(gT.files.back());
         const WavFormat::Info info = WavFormat::Parse(bytes);
         const double diff = (double)RenderToFileState().rmsDb - gT.nativeRmsDb;
         printf("RENDERFILETEST converted to %.0f Hz: %llu frames, RMS %+.3f dB against the native render\n", other,
                (unsigned long long)info.frames, diff);
         Check(ok && info.ok && info.frames == (uint64_t)std::llround(2.0 * other) &&
                  info.sampleRate == (uint32_t)std::llround(other) && std::fabs(diff) < 0.5,
               "sample-rate conversion keeps length and level");
         gT.stage = StartTestRender(gT.eqA, 0.0, 1.5, 0.0, "padFresh1.wav", 32, gT.rate) ? 10 : 98;
         if (gT.stage == 98)
            FinishTest(window);
         return;
      }
      if (gT.stage == 10 || gT.stage == 11)
      {
         if (!RenderDone(ok))
            return;
         std::vector<float> inter;
         WavFormat::Info info;
         if (ok && DecodeWav(ReadFileBytes(gT.files.back()), info, inter))
            CheckFreshStart(inter, gT.rate, gT.stage == 10 ? "pad render #1" : "pad render #2 (after live playback)");
         LivePlaybackStandIn(3.0);
         if (gT.stage == 10)
         {
            gT.stage = StartTestRender(gT.eqA, 0.0, 1.5, 0.0, "padFresh2.wav", 32, gT.rate) ? 11 : 98;
         }
         else
         {
            // The path the user hit: analyze_audio offline:true start:0.
            nlohmann::json res;
            std::string err;
            const nlohmann::json params = { { "node", gT.eqA }, { "seconds", 1.5 }, { "offline", true }, { "start", 0.0 } };
            if (HandleAudioProbeRpc("capture_audio", params, res, err) && res.contains("id"))
            {
               gT.captureId = res["id"].get<int>();
               gT.stageStart = glfwGetTime();
               gT.stage = 12;
            }
            else
            {
               Check(false, "pad capture started (" + err + ")");
               gT.stage = 98;
            }
         }
         if (gT.stage == 98)
            FinishTest(window);
         return;
      }
      if (gT.stage == 12)
      {
         bool capOk = false;
         if (!captureDone(capOk))
            return;
         std::vector<float> cap;
         double capRate = 0.0;
         if (capOk && AudioProbeLastCapture(cap, capRate))
            CheckFreshStart(cap, capRate, "pad offline capture (after live playback)");
         else
            Check(false, "pad capture kept its samples");
         RunTempoRewindCheck();
         // G: 10 beats at 120 bpm.
         gT.stage = StartTestRender(gT.flt, 0.0, 5.0, 0.0, "lfoFilter.wav", 32, gT.rate) ? 13 : 98;
         if (gT.stage == 98)
            FinishTest(window);
         return;
      }
      if (gT.stage == 13)
      {
         if (!RenderDone(ok))
            return;
         std::vector<float> inter;
         WavFormat::Info info;
         if (ok && DecodeWav(ReadFileBytes(gT.files.back()), info, inter))
         {
            AudioAnalysis::Options opt;
            opt.frameSeconds = 0.25;
            const AudioAnalysis::Result r = AudioAnalysis::Analyze(AudioAnalysis::MixToMono(inter), gT.rate, opt);
            float lo = 1e9f, hi = 0.0f;
            int changed = 0, pairs = 0;
            float atPeak = 0.0f, atTrough = 0.0f;
            std::string curve;
            for (size_t i = 0; i < r.frames.size(); i++)
            {
               const AudioAnalysis::Frame& f = r.frames[i];
               if (f.rmsDb < -60.0f || !(f.centroidHz > 0.0f))
                  continue;
               lo = std::min(lo, f.centroidHz);
               hi = std::max(hi, f.centroidHz);
               if (i > 0 && r.frames[i - 1].centroidHz > 0.0f)
               {
                  pairs++;
                  if (std::fabs(f.centroidHz - r.frames[i - 1].centroidHz) > 0.005f * f.centroidHz)
                     changed++;
               }
               // The LFO's peak is at beat 2.5 (1.25 s), its trough at beat 7.5 (3.75 s).
               if (std::fabs(f.time - 1.25f) < 0.13f)
                  atPeak = f.centroidHz;
               if (std::fabs(f.time - 3.75f) < 0.13f)
                  atTrough = f.centroidHz;
               curve += std::to_string((int)std::lround(f.centroidHz)) + " ";
            }
            printf("RENDERFILETEST LFO -> filter centroid (Hz, 0.25 s frames): %s\n", curve.c_str());
            printf("RENDERFILETEST centroid min %.0f max %.0f, %d of %d frames moved, at the LFO peak %.0f, trough %.0f\n",
                   (double)lo, (double)hi, changed, pairs, (double)atPeak, (double)atTrough);
            Check(hi > 2.0f * lo && pairs > 0 && changed * 4 >= pairs * 3 && atPeak > 2.0f * std::max(1.0f, atTrough),
                  "modulation follows the offline render (lockstep)");
         }
         else
            Check(false, "LFO filter render decodes");
         FinishTest(window);
         return;
      }
   }
}
