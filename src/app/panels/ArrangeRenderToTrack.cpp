// "Render to track": bounce a canvas node (or a group's audio that leaves it)
// offline into a WAV and drop it on a new timeline track. Per-clip bounce was
// removed from the timeline on purpose (timeline-arrangement-architecture);
// this is the canvas-side replacement and it never touches clips.
//
// Flow (one job at a time, pumped once a frame from ArrangeRenderQueueTick):
//   Start   - refuse while any render/offline capture runs; an OfflineTake
//             (graph/OfflineTake.cpp): stop the device, park the transport at
//             the range start in offline mode (loop suspended), force the
//             tapped node(s) into the topology (OfflineTakeForceNodes ->
//             RenderToTrackForcedNodes), re-prepare the graph at the take's
//             rate, and arm AudioProbe in offline mode on their first output.
//   Render  - OfflineTakeRender block by block (~60 ms per frame): modulation
//             and the audio nodes' cook step on the audio clock, then
//             ProcessOffline; the probe is drained after each block - the
//             AudioProbeJobs.cpp offline-capture pattern, so the result is
//             sample-accurate and repeatable.
//   Restore - OfflineTakeRestore (offline mode off, transport position,
//             device, play state, vsync), then the topology is rebuilt.
//   Commit  - sum the taps, optional peak normalize, write a 16-bit WAV to the
//             recordings folder, then ONE full undo checkpoint covering: a new
//             audio track appended below the others + exactly what dropping
//             that WAV there creates (ArrangeImportMediaFile: hidden Audio File
//             node + Audio Sample clip) at the range start, Sync to Tempo kept
//             off so it stays sample-aligned.
//   Import  - the job stays active until the async decode lands on the clip
//             (ArrangePollMediaImports), so a caller polling RenderToTrackState
//             sees done only once the clip is final.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "arrange/ArrangeModel.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFileWriter.h"
#include "audio/AudioProbe.h"
#include "core/AudioCable.h"
#include "nodes/AudioNodes.h"
#include "nodes/OutputNode.h"
#include "nodes/UtilityNodes.h"

namespace app
{
   namespace
   {
      struct RenderJob
      {
         enum Stage { kIdle, kRendering, kImporting } stage = kIdle;
         std::string title;
         std::vector<uint64_t> tapUids;   // the node(s) whose first audio output is summed
         double startBeats = 0.0, endBeats = 0.0, tailSeconds = 0.0;
         bool normalize = false;
         double rate = 0.0;
         long long framesTotal = 0, framesDone = 0;
         std::vector<std::vector<float>> taps; // interleaved stereo per tap
         bool cancel = false;
         // Borrowed device/transport state, given back by Restore().
         OfflineTake take;
         // Import stage.
         uint64_t clipId = 0;
         double importStarted = 0.0;
      };

      RenderJob gJob;
      RenderToTrackStatus gStatus;
      double gLastRate = 0.0;

      // Popup state (canvas context menu -> "Render to track...").
      int gPopupNode = -1;
      bool gPopupOpenRequest = false;
      Arrange::Tick gPopupStart = 0, gPopupEnd = 0;
      float gPopupTail = 2.0f;
      bool gPopupNormalize = false;
      std::string gPopupError;
   }

   std::string RenderSafeFilePart(const std::string& s)
   {
      std::string out;
      for (char ch : s)
      {
         const unsigned char u = (unsigned char)ch;
         out += (std::isalnum(u) || ch == '-' || ch == '_') ? ch : '_';
         if (out.size() >= 40)
            break;
      }
      return out.empty() ? std::string("node") : out;
   }

   namespace
   {
      GraphNode* GraphNodeOf(INode* n)
      {
         if (n == nullptr)
            return nullptr;
         for (GraphNode& gn : gNodes)
            if (gn.node.get() == n)
               return &gn;
         return nullptr;
      }
   }

   // What a render of `gn` taps. A node with audio: itself. An Audio Out:
   // whatever feeds it. A group: every member whose audio leaves the group
   // (cabled to a non-member) or reaches an Audio Out / Output, summed -
   // what the group contributes to the mix. At most AudioProbe::kSlots.
   bool RenderTapsForNode(GraphNode& gn, std::vector<uint64_t>& out, std::string& err)
   {
      out.clear();
      if (auto* g = dynamic_cast<GroupNode*>(gn.node.get()))
      {
         auto it = gGroupMembers.find(g);
         if (it == gGroupMembers.end() || it->second.empty())
         {
            err = "the group is empty";
            return false;
         }
         const std::set<int>& members = it->second;
         for (GraphNode& dst : gNodes)
         {
            if (dst.node == nullptr)
               continue;
            const bool dstInside = members.count(dst.index) != 0;
            const bool dstIsSink = dynamic_cast<AudioOutputNode*>(dst.node.get()) != nullptr ||
                                   dynamic_cast<OutputNode*>(dst.node.get()) != nullptr;
            if (dstInside && !dstIsSink)
               continue;
            for (int slot = 0; slot < kMaxAudioSlots; slot++)
            {
               AudioCable* cable = dst.node->AudioInputSlot(slot);
               if (cable == nullptr || !cable->IsConnected())
                  continue;
               GraphNode* src = GraphNodeOf(cable->GetSource());
               if (src == nullptr || members.count(src->index) == 0 || !IsNodeAudioCompatible(*src))
                  continue;
               if (std::find(out.begin(), out.end(), src->uid) == out.end())
                  out.push_back(src->uid);
            }
         }
         if (out.empty())
         {
            err = "no audio leaves this group (nothing inside it is cabled to a node outside it or to an Audio Out)";
            return false;
         }
         if ((int)out.size() > AudioProbe::kSlots)
         {
            err = "audio leaves this group from " + std::to_string(out.size()) + " nodes; a render sums at most " +
                  std::to_string(AudioProbe::kSlots) + " - put a Mixer inside the group and render that";
            return false;
         }
         return true;
      }
      if (auto* ao = dynamic_cast<AudioOutputNode*>(gn.node.get()))
      {
         GraphNode* feeder = ao->input.IsConnected() ? GraphNodeOf(ao->input.GetSource()) : nullptr;
         if (feeder == nullptr || !IsNodeAudioCompatible(*feeder))
         {
            err = "this Audio Out has nothing connected";
            return false;
         }
         out.push_back(feeder->uid);
         return true;
      }
      if (!IsNodeAudioCompatible(gn))
      {
         err = NodeTitle(gn) + " has no audio output";
         return false;
      }
      out.push_back(gn.uid);
      return true;
   }

   namespace
   {
      void Restore()
      {
         AudioProbe::Instance().Disarm();
         AudioProbe::Instance().SetOffline(false);
         gJob.tapUids.clear();
         OfflineTakeRestore(gJob.take); // also empties RenderToTrackForcedNodes()
      }

      void Finish(bool ok, const std::string& message)
      {
         gJob.stage = RenderJob::kIdle;
         gJob.taps.clear();
         gJob.taps.shrink_to_fit();
         gStatus.active = false;
         gStatus.done = true;
         gStatus.ok = ok;
         gStatus.progress = ok ? 1.0f : gStatus.progress;
         gStatus.message = message;
         if (!ok)
            fprintf(stderr, "render to track failed: %s\n", message.c_str());
      }

      // Arms the probe on the taps' AudioNodes, resolved after the rebuild.
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

      // Sums the taps, normalizes if asked, writes the WAV.
      bool WriteResult(std::string& err)
      {
         const long long n = gJob.framesTotal;
         std::vector<float> mix((size_t)n * 2, 0.0f);
         bool anyAudio = false;
         for (const std::vector<float>& t : gJob.taps)
         {
            anyAudio = anyAudio || !t.empty();
            const size_t count = std::min(t.size(), mix.size());
            for (size_t i = 0; i < count; i++)
               mix[i] += t[i];
         }
         if (!anyAudio)
         {
            err = "no audio arrived from the node (is it bypassed, or does it only make sound from notes that never came?)";
            return false;
         }
         if (gJob.normalize)
         {
            float peak = 0.0f;
            for (float v : mix)
               peak = std::max(peak, std::fabs(v));
            if (peak > 1e-6f)
            {
               const float gain = 0.96605f / peak; // -0.3 dBFS
               for (float& v : mix)
                  v *= gain;
            }
         }
         AudioFileWriter writer;
         if (!writer.Open(gStatus.path, gJob.rate, 2, AudioFileWriter::Format::Wav))
         {
            err = "could not create " + gStatus.path;
            return false;
         }
         const int kChunk = 65536;
         for (long long f = 0; f < n; f += kChunk)
            writer.Append(mix.data() + (size_t)f * 2, (int)std::min<long long>(kChunk, n - f));
         writer.Close();
         return true;
      }

      // One undo step: a new audio track at the bottom + what dropping the
      // WAV there creates. The checkpoint is taken before anything changes;
      // SpawnNode's own checkpoint and ArrangeEdit's timeline snapshot are
      // suppressed for the duration (the ArrangeMakeClipSourceUnique shape).
      bool CommitToTimeline(std::string& err)
      {
         const bool wasSuppressed = gSuppressUndoCheckpoints;
         PushUndoCheckpoint();
         gSuppressUndoCheckpoints = true;
         const uint64_t laneId = Arrange::AddLane(gArrange, Arrange::kLaneAudio); // appended = below the others
         if (Arrange::Lane* l = Arrange::FindLane(gArrange, laneId))
            l->name = Arrange::UniqueLaneName(gArrange, gJob.title, Arrange::kLaneAudio);
         const size_t pendingBefore = gArrangePendingImports.size();
         ArrangeImportMediaFile(gStatus.path, laneId, Arrange::BeatsToTicks(gJob.startBeats), Arrange::ImportMediaKind::Audio);
         uint64_t clipId = 0;
         if (gArrangePendingImports.size() > pendingBefore)
         {
            ArrangePendingImport& p = gArrangePendingImports.back();
            p.keepNativeSpeed = true;
            clipId = p.clipId;
         }
         if (Arrange::Clip* c = Arrange::FindClip(gArrange, clipId))
         {
            c->name = gJob.title;
            c->syncToTempo = false;
            gArrange.revision++;
         }
         gSuppressUndoCheckpoints = wasSuppressed;
         if (clipId == 0)
         {
            err = "the timeline refused the clip";
            return false;
         }
         ArrangeCommitEdit();
         gArrangePanelOpen = true;
         gArrangeRevealClipId = clipId;
         gArrangeRevealFrames = 3;
         gJob.clipId = clipId;
         gStatus.clipId = clipId;
         gStatus.laneIndex = Arrange::LaneIndex(gArrange, laneId);
         return true;
      }
   }


   const RenderToTrackStatus& RenderToTrackState() { return gStatus; }
   bool RenderToTrackBusy() { return gJob.stage == RenderJob::kRendering; }
   double RenderToTrackSampleRate() { return gLastRate; }


   const std::vector<INode*>& RenderToTrackForcedNodes()
   {
      // Rebuilt from uids on every call, never cached as pointers: if a node
      // vanished mid-take the topology must not walk a dangling INode*.
      // The taps of whichever offline take runs (render to track / file,
      // offline capture - OfflineTakeForceNodes).
      static std::vector<INode*> sNodes;
      sNodes.clear();
      for (uint64_t uid : OfflineTakeForcedUids())
         if (GraphNode* gn = FindNodeByUid(uid))
            if (gn->node != nullptr)
               sNodes.push_back(gn->node.get());
      return sNodes;
   }


   bool StartRenderNodeToTrack(int nodeIndex, double startBeats, double endBeats, double tailSeconds, bool normalize,
                               std::string& err)
   {
      if (gJob.stage != RenderJob::kIdle)
      {
         err = "a render to track is already running";
         return false;
      }
      if (ArrangeRenderBusy() || gOfflineRender.active || Transport::Instance().IsOfflineMode() ||
          AudioProbe::Instance().IsArmed() || RenderToFileBusy())
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
      if (!std::isfinite(startBeats) || !std::isfinite(endBeats) || startBeats < 0.0 || !(endBeats > startBeats))
      {
         err = "the range must have 0 <= start < end";
         return false;
      }
      if (!std::isfinite(tailSeconds) || tailSeconds < 0.0)
         tailSeconds = 0.0;
      tailSeconds = std::min(tailSeconds, 60.0);
      std::vector<uint64_t> taps;
      if (!RenderTapsForNode(*gn, taps, err))
         return false;
      const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
      const double rangeSec = (endBeats - startBeats) * 60.0 / bpm;
      if (rangeSec + tailSeconds > 60.0 * 30.0)
      {
         err = "range plus tail is longer than 30 minutes";
         return false;
      }

      gJob = RenderJob();
      gJob.title = ArrangeSourceTitle(*gn);
      gJob.tapUids = taps;
      gJob.startBeats = startBeats;
      gJob.endBeats = endBeats;
      gJob.tailSeconds = tailSeconds;
      gJob.normalize = normalize;

      // The rate the graph is (or will be) prepared at (OfflineTakeRate): a
      // running device's rate is the only one its nodes know; with no device
      // the take runs at the headless rate (or the configured rate), exactly
      // like a headless render - no device is opened just for this.
      gJob.rate = OfflineTakeRate();
      gJob.framesTotal = (long long)std::llround(rangeSec * gJob.rate) + (long long)std::llround(tailSeconds * gJob.rate);
      gJob.taps.assign(taps.size(), std::vector<float>());
      for (auto& t : gJob.taps)
         t.reserve((size_t)gJob.framesTotal * 2 + 8192);
      gLastRate = gJob.rate;

      gStatus = RenderToTrackStatus();
      gStatus.active = true;
      gStatus.message = "rendering " + gJob.title;
      gStatus.path = AudioRecordings::GenerateFilePath(RenderSafeFilePart(gJob.title), "wav");

      // Device off first (its callback must not race ProcessOffline), then
      // park, force the taps into the topology, re-prepare (every node starts
      // from its prepared state, so two takes match) and arm.
      OfflineTakeBegin(gJob.take, gJob.rate, 0.0, startBeats);
      AudioProbe::Instance().SetOffline(true);
      gJob.stage = RenderJob::kRendering;
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


   void PumpRenderToTrack()
   {
      if (gJob.stage == RenderJob::kImporting)
      {
         ArrangePollMediaImports(); // also polled by the panel; cheap when nothing is pending
         const Arrange::Clip* c = Arrange::FindClip(gArrange, gJob.clipId);
         if (c == nullptr)
            Finish(false, "the rendered clip was removed before its file finished loading");
         else if (!c->importPending)
            Finish(true, "rendered " + gJob.title + " to a new track");
         else if (glfwGetTime() - gJob.importStarted > 30.0)
            Finish(true, "rendered " + gJob.title + " to a new track (the file is still loading)");
         return;
      }
      if (gJob.stage != RenderJob::kRendering)
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
      static float sChunk[8192];
      AudioProbe& probe = AudioProbe::Instance();
      const double budgetStart = glfwGetTime();
      const int blockCap = OfflineTakeBlockFrames();
      while (!gJob.cancel && gJob.framesDone < gJob.framesTotal)
      {
         const int frames = (int)std::min<long long>(blockCap, gJob.framesTotal - gJob.framesDone);
         OfflineTakeRender(gJob.take, sL, sR, frames);
         gJob.framesDone += frames;
         for (size_t s = 0; s < gJob.taps.size(); s++)
         {
            int n;
            while ((n = probe.Drain((int)s, sChunk, 8192)) > 0)
               gJob.taps[s].insert(gJob.taps[s].end(), sChunk, sChunk + n);
         }
         if (glfwGetTime() - budgetStart > 0.06)
            break;
      }
      gStatus.progress = 0.95f * (float)((double)gJob.framesDone / (double)std::max<long long>(1, gJob.framesTotal));
      if (!gJob.cancel && gJob.framesDone < gJob.framesTotal)
         return;

      const bool cancelled = gJob.cancel;
      Restore();
      if (cancelled)
      {
         Finish(false, "cancelled");
         return;
      }
      std::string err;
      if (!WriteResult(err))
      {
         Finish(false, err);
         return;
      }
      if (!CommitToTimeline(err))
      {
         Finish(false, err);
         return;
      }
      gJob.taps.clear();
      gJob.taps.shrink_to_fit();
      gJob.stage = RenderJob::kImporting;
      gJob.importStarted = glfwGetTime();
      gStatus.progress = 0.97f;
      gStatus.message = "loading the rendered file";
   }


   // Same dim + click-catcher as the WAV render's dialog: the take drives the
   // graph synchronously from the main loop, so editing underneath it would be
   // editing what is being rendered.
   void DrawRenderToTrackProgressWindow()
   {
      if (gJob.stage != RenderJob::kRendering)
         return;
      ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
      ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::Begin("##RenderToTrackBlocker", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::InvisibleButton("##RenderToTrackBlockerCatch", io.DisplaySize);
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();

      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.5f));
      PushElevatedPanelStyle(/*isChild=*/false);
      ImGui::Begin(L("Rendering to Track"), nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
      if (gJob.cancel)
         ImGui::Text("%s", T("Cancelling..."));
      else
         ImGui::Text(T("%s: %.1fs of %.1fs at %d Hz"), gJob.title.c_str(), (double)gJob.framesDone / gJob.rate,
                     (double)gJob.framesTotal / gJob.rate, (int)llround(gJob.rate));
      ImGui::ProgressBar((float)((double)gJob.framesDone / (double)std::max<long long>(1, gJob.framesTotal)),
                         ImVec2(280, 0));
      ImGui::BeginDisabled(gJob.cancel);
      if (ImGui::Button(L("Cancel##rendertotrack"), ImVec2(120, 0)))
         gJob.cancel = true;
      ImGui::EndDisabled();
      ImGui::End();
      PopElevatedPanelStyle();
   }


   void OpenRenderToTrackPopup(int nodeIndex)
   {
      gPopupNode = nodeIndex;
      gPopupOpenRequest = true;
      gPopupError.clear();
      // Default range: the loop when it is on, else 4 bars from the playhead.
      const Arrange::LoopRange& loop = gArrange.settings.loop;
      if (loop.enabled && loop.end > loop.start)
      {
         gPopupStart = loop.start;
         gPopupEnd = loop.end;
      }
      else
      {
         gPopupStart = std::max<Arrange::Tick>(0, ArrangePlayTick());
         gPopupEnd = gPopupStart + 4 * Arrange::kTicksPerBar;
      }
   }


   void DrawRenderToTrackPopup()
   {
      const char* kId = "Render to Track##rendertotrackpopup";
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
      ImGui::Text(T("Render %s to a new track"), ArrangeSourceTitle(*gn).c_str());
      ImGui::Separator();
      const float fieldW = 160.0f;
      Arrange::Tick t = 0;
      if (ArrangeTickField("Start##r2tstart", gPopupStart, 0, Arrange::kMaxTick, ArrangeTickUnit::Position, fieldW, &t))
      {
         const Arrange::Tick len = gPopupEnd - gPopupStart;
         gPopupStart = t;
         gPopupEnd = t + std::max<Arrange::Tick>(1, len);
      }
      if (ArrangeTickField("End##r2tend", gPopupEnd, gPopupStart + 1, Arrange::kMaxTick, ArrangeTickUnit::Position, fieldW, &t))
         gPopupEnd = std::max(gPopupStart + 1, t);
      ImGui::SetNextItemWidth(fieldW);
      ImGui::DragFloat(L("Tail##r2ttail"), &gPopupTail, 0.05f, 0.0f, 30.0f, "%.2f s");
      gPopupTail = std::clamp(gPopupTail, 0.0f, 30.0f);
      PushCheckboxStyle();
      ImGui::Checkbox(L("Normalize##r2tnorm"), &gPopupNormalize);
      PopCheckboxStyle();
      ImGui::TextDisabled(T("Length %s + %.2f s tail"), ArrangeFormatLength(gPopupEnd - gPopupStart).c_str(),
                          (double)gPopupTail);
      if (!gPopupError.empty())
      {
         ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
         ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", gPopupError.c_str());
         ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      ImGui::BeginDisabled(gJob.stage != RenderJob::kIdle || ArrangeRenderBusy());
      if (ImGui::Button(L("Render##r2tgo"), ImVec2(120, 0)))
      {
         std::string err;
         if (StartRenderNodeToTrack(gPopupNode, Arrange::TicksToBeats(gPopupStart), Arrange::TicksToBeats(gPopupEnd),
                                    (double)gPopupTail, gPopupNormalize, err))
            ImGui::CloseCurrentPopup();
         else
            gPopupError = err;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(L("Cancel##r2tcancel"), ImVec2(120, 0)))
         ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
   }
}
