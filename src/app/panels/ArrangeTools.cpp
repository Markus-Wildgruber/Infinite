// Timeline tools: groups as clip sources (GroupTimelineWindows), the clip/loop
// helper API the assistant RPC calls instead of poking gArrange directly, and
// INFINITE_ARRANGEGROUPTEST. See .claude/skills/timeline-arrangement-architecture
// ("Group clips", "Helper API").
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "arrange/ArrangeModel.h"
#include "nodes/AnalyzeNodes.h"
#include "nodes/UtilityNodes.h"

namespace app
{
   // ---- group sources ------------------------------------------------------

   GroupNode* ArrangeGroupOfUid(uint64_t uid)
   {
      if (uid == 0)
         return nullptr;
      GraphNode* gn = FindNodeByUid(uid);
      return gn != nullptr ? dynamic_cast<GroupNode*>(gn->node.get()) : nullptr;
   }


   bool ArrangeIsGroupSource(uint64_t uid)
   {
      return ArrangeGroupOfUid(uid) != nullptr;
   }


   bool ArrangeIsGroupNode(const GraphNode& gn)
   {
      return dynamic_cast<GroupNode*>(gn.node.get()) != nullptr;
   }


   // A group gates whatever leaves it, so it fits any lane: the lane type only
   // decides where the clip is drawn. Its natural lane is audio when a member
   // makes sound, else video when a member makes pictures, else audio.
   int ArrangeNaturalLaneType(const GraphNode& gn)
   {
      if (auto* g = dynamic_cast<GroupNode*>(gn.node.get()))
      {
         bool anyVideo = false;
         auto it = gGroupMembers.find(g);
         if (it != gGroupMembers.end())
            for (int idx : it->second)
               if (GraphNode* m = FindNodeByIndex(idx))
               {
                  if (IsNodeAudioCompatible(*m))
                     return Arrange::kLaneAudio;
                  anyVideo = anyVideo || IsNodeVideoCompatible(*m);
               }
         return anyVideo ? Arrange::kLaneVideo : Arrange::kLaneAudio;
      }
      return ArrangeLaneTypeForNode(gn);
   }


   bool ArrangeClipSourceFits(const GraphNode& gn, int laneType)
   {
      if (gn.node == nullptr)
         return false;
      // A MIDI lane holds note clips only: its instrument is a lane setting
      // (Lane::instrumentUid), never a clip source.
      if (laneType == Arrange::kLaneMidi)
         return false;
      if (ArrangeIsGroupNode(gn))
         return true;
      return laneType == Arrange::kLaneAudio ? IsNodeAudioCompatible(gn) : IsNodeVideoCompatible(gn);
   }


   std::string ArrangeSourceTitle(const GraphNode& gn)
   {
      if (auto* g = dynamic_cast<GroupNode*>(gn.node.get()))
         return g->label.empty() ? std::string("Group") : g->label;
      return NodeTitle(gn);
   }


   bool ArrangeGroupSourceColor(uint64_t uid, ImU32& out)
   {
      GroupNode* g = ArrangeGroupOfUid(uid);
      if (g == nullptr)
         return false;
      out = IM_COL32((int)std::lround(std::clamp(g->color[0], 0.0f, 1.0f) * 255.0f),
                     (int)std::lround(std::clamp(g->color[1], 0.0f, 1.0f) * 255.0f),
                     (int)std::lround(std::clamp(g->color[2], 0.0f, 1.0f) * 255.0f), 225);
      return true;
   }


   // Per srcUid: every uid that has ANY clip gets an entry (possibly empty),
   // so "has a clip" and "where it plays" come out of one pass. The cache is
   // keyed on gArrange.revision - every model edit bumps it - plus the active
   // render lane scope, which changes without a revision bump. It is built
   // from the model alone (no node lookup), so callers ask by uid and a
   // non-group uid simply answers for its own clips.
   namespace
   {
      struct GroupWindowCache
      {
         bool built = false;
         uint64_t revision = 0;
         uint64_t scopeSig = 0;
         std::unordered_map<uint64_t, std::vector<std::pair<double, double>>> windows;
      };
      GroupWindowCache gGroupWindowCache;

      uint64_t LaneScopeSignature()
      {
         uint64_t sig = (uint64_t)gArrangeRenderActiveLaneScope.size() * 0x9E3779B97F4A7C15ull;
         for (uint64_t id : gArrangeRenderActiveLaneScope)
            sig ^= id * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull; // order-free
         return sig;
      }

      void RebuildGroupWindowCache()
      {
         GroupWindowCache& c = gGroupWindowCache;
         c.windows.clear();
         bool anySolo = false;
         for (const Arrange::Lane& lane : gArrange.lanes)
            anySolo = anySolo || (Arrange::LaneHasMixer(lane.type) && lane.solo);
         for (const Arrange::Lane& lane : gArrange.lanes)
         {
            // The same lane rules a node clip lives by (RebuildAudioTopology's
            // scheduled loop): ancestor-aware enable, a lane-scoped render's
            // scope, and on an audio lane the mixer's mute/solo rule (a soloed
            // MIDI lane counts as a solo too).
            const bool inScope = gArrangeRenderActiveLaneScope.empty() || gArrangeRenderActiveLaneScope.count(lane.id) != 0;
            const bool silenced = lane.type == Arrange::kLaneAudio && (lane.mute || (anySolo && !lane.solo));
            const bool lanePlays = inScope && !silenced && Arrange::LaneEffectivelyEnabled(gArrange, lane);
            for (const Arrange::Clip& clip : lane.clips)
            {
               if (clip.srcUid == 0)
                  continue;
               std::vector<std::pair<double, double>>& w = c.windows[clip.srcUid];
               if (!lanePlays || !clip.enabled || clip.length <= 0)
                  continue;
               w.emplace_back(Arrange::TicksToBeats(clip.start), Arrange::TicksToBeats(clip.End()));
            }
         }
         // Sorted, and overlapping/abutting windows (the same group on two
         // lanes, or two clips edge to edge) merged into one run, so a caller
         // walking them never opens and closes the gate on a shared edge.
         const double kAbut = 0.5 / (double)Arrange::kPPQ;
         for (auto& kv : c.windows)
         {
            auto& v = kv.second;
            std::sort(v.begin(), v.end());
            std::vector<std::pair<double, double>> merged;
            for (const auto& p : v)
            {
               if (!merged.empty() && p.first <= merged.back().second + kAbut)
                  merged.back().second = std::max(merged.back().second, p.second);
               else
                  merged.push_back(p);
            }
            v.swap(merged);
         }
         c.revision = gArrange.revision;
         c.scopeSig = LaneScopeSignature();
         c.built = true;
      }
   }


   // Loop and offline-render semantics come for free: windows are absolute
   // beats, the same axis ClipWindow uses, so the transport loop simply wraps
   // the playhead through them and an offline take (which suspends the loop)
   // walks them once - exactly as for a node clip.
   bool GroupTimelineWindows(uint64_t groupUid, std::vector<std::pair<double, double>>& outBeatWindows)
   {
      outBeatWindows.clear();
      if (groupUid == 0)
         return false;
      GroupWindowCache& c = gGroupWindowCache;
      if (!c.built || c.revision != gArrange.revision || c.scopeSig != LaneScopeSignature())
         RebuildGroupWindowCache();
      auto it = c.windows.find(groupUid);
      if (it == c.windows.end())
         return false;
      outBeatWindows = it->second;
      return true;
   }


   // ---- helper API (assistant RPC) ----------------------------------------
   //
   // Each mutating helper edits a COPY of gArrange through the Arrange:: ops,
   // checks the result, and only then swaps it in - so a refused call leaves
   // the timeline untouched, and an accepted one lands as a single revision
   // change (the copy's revision already moved past the live one). None of
   // them pushes undo: the caller pushes a checkpoint first.

   namespace
   {
      std::string LaneLabel(const Arrange::Model& m, int laneIndex)
      {
         const Arrange::Lane& l = m.lanes[(size_t)laneIndex];
         return "lane " + std::to_string(laneIndex) + (l.name.empty() ? std::string() : " (" + l.name + ")");
      }

      // First clip on `lane` overlapping [a, b), ignoring `exceptId`.
      const Arrange::Clip* Overlapping(const Arrange::Lane& lane, Arrange::Tick a, Arrange::Tick b, uint64_t exceptId)
      {
         for (const Arrange::Clip& c : lane.clips)
            if (c.id != exceptId && c.start < b && c.End() > a)
               return &c;
         return nullptr;
      }

      std::string OverlapError(const Arrange::Model& m, int laneIndex, const Arrange::Clip& hit)
      {
         char buf[256];
         snprintf(buf, sizeof(buf), "%s already has clip %llu at beats %.3f-%.3f there (clips on a lane never overlap); "
                  "pick a free range, another lane, or lane -1 for a new one",
                  LaneLabel(m, laneIndex).c_str(), (unsigned long long)hit.id, Arrange::TicksToBeats(hit.start),
                  Arrange::TicksToBeats(hit.End()));
         return buf;
      }

      bool ToTicks(double beats, bool allowZero, Arrange::Tick& out, const char* what, std::string& err)
      {
         if (!std::isfinite(beats) || beats < 0.0 || (!allowZero && beats <= 0.0))
         {
            err = std::string(what) + (allowZero ? " must be >= 0" : " must be > 0");
            return false;
         }
         out = Arrange::BeatsToTicks(beats);
         if (!allowZero && out <= 0)
            out = 1;
         if (out > Arrange::kMaxTick)
         {
            err = std::string(what) + " is past the end of the timeline";
            return false;
         }
         return true;
      }

      void Commit(Arrange::Model& edited)
      {
         Arrange::Normalize(edited);
         if (edited.revision <= gArrange.revision)
            edited.revision = gArrange.revision + 1;
         gArrange = std::move(edited);
         ArrangeCommitEdit();
      }

      const char* ClipKind(const Arrange::Lane& lane, const Arrange::Clip& c)
      {
         if (lane.type == Arrange::kLaneMidi)
            return "midi";
         if (ArrangeIsGroupSource(c.srcUid))
            return "group";
         if (lane.type == Arrange::kLaneVideo)
            return "video";
         if (c.sampleDropped)
            return "sample";
         return "node";
      }
   }


   nlohmann::json ArrangeClipsJson()
   {
      using json = nlohmann::json;
      json lanes = json::array();
      json clips = json::array();
      for (int li = 0; li < (int)gArrange.lanes.size(); li++)
      {
         const Arrange::Lane& l = gArrange.lanes[(size_t)li];
         const bool midi = l.type == Arrange::kLaneMidi;
         json lj = { { "index", li },
                     { "id", l.id },
                     { "name", l.name },
                     { "kind", l.type == Arrange::kLaneAudio ? "audio" : (midi ? "midi" : "video") },
                     { "mute", l.mute },
                     { "solo", l.solo },
                     { "enabled", l.enabled },
                     { "effectivelyEnabled", Arrange::LaneEffectivelyEnabled(gArrange, l) },
                     { "gainDb", l.gainDb } };
         if (midi)
         {
            // A MIDI lane plays one instrument node; its clips have no source.
            GraphNode* inst = FindNodeByUid(l.instrumentUid);
            lj["instrumentUid"] = l.instrumentUid;
            lj["instrumentIndex"] = inst != nullptr ? inst->index : -1;
            lj["instrumentType"] = inst != nullptr ? inst->typeName : std::string();
         }
         lanes.push_back(lj);
         for (const Arrange::Clip& c : l.clips)
         {
            GraphNode* gn = FindNodeByUid(c.srcUid);
            std::string name = c.name;
            if (name.empty())
               name = gn != nullptr ? ArrangeSourceTitle(*gn) : std::string("Unassigned");
            json cj = { { "id", c.id },
                              { "lane", li },
                              { "startBeats", Arrange::TicksToBeats(c.start) },
                              { "lengthBeats", Arrange::TicksToBeats(c.length) },
                              { "srcUid", c.srcUid },
                              { "srcIndex", gn != nullptr ? gn->index : -1 },
                              { "srcType", gn != nullptr ? gn->typeName : std::string() },
                              { "srcOutput", c.srcOutput },
                              { "name", name },
                              { "enabled", c.enabled },
                              { "gainDb", c.gainDb },
                              { "fadeInBeats", Arrange::TicksToBeats(c.fadeIn) },
                              { "fadeOutBeats", Arrange::TicksToBeats(c.fadeOut) },
                              { "kind", ClipKind(l, c) } };
            if (midi)
            {
               cj["noteCount"] = (int)c.notes.size();
               if (c.name.empty())
                  cj["name"] = "MIDI Clip";
            }
            clips.push_back(cj);
         }
      }
      const Arrange::LoopRange& loop = gArrange.settings.loop;
      json result = json::object();
      result["lanes"] = lanes;
      result["clips"] = clips;
      result["loop"] = { { "enabled", loop.enabled },
                         { "startBeats", Arrange::TicksToBeats(loop.start) },
                         { "endBeats", Arrange::TicksToBeats(loop.end) } };
      return result;
   }


   bool ArrangeAddClip(uint64_t srcUid, int srcOutput, int laneIndex, double startBeats, double lengthBeats,
                       uint64_t& outClipId, std::string& err)
   {
      outClipId = 0;
      Arrange::Tick start = 0, length = 0;
      if (!ToTicks(startBeats, true, start, "startBeats", err) || !ToTicks(lengthBeats, false, length, "lengthBeats", err))
         return false;
      if (start + length > Arrange::kMaxTick)
      {
         err = "the clip would end past the end of the timeline";
         return false;
      }
      // A MIDI lane: an empty MIDI clip (its lane's instrument plays it;
      // srcUid is ignored). Notes go in with ArrangeSetMidiNotes.
      if (laneIndex >= 0 && laneIndex < (int)gArrange.lanes.size() &&
          gArrange.lanes[(size_t)laneIndex].type == Arrange::kLaneMidi)
      {
         if (const Arrange::Clip* hit = Overlapping(gArrange.lanes[(size_t)laneIndex], start, start + length, 0))
         {
            err = OverlapError(gArrange, laneIndex, *hit);
            return false;
         }
         Arrange::Model m = gArrange;
         Arrange::Clip c;
         c.start = start;
         c.length = length;
         uint64_t made = 0;
         if (!Arrange::PlaceOverwrite(m, m.lanes[(size_t)laneIndex].id, c, &made) || made == 0)
         {
            err = "the timeline refused the clip";
            return false;
         }
         Commit(m);
         outClipId = made;
         return true;
      }
      GraphNode* gn = FindNodeByUid(srcUid);
      if (gn == nullptr || gn->node == nullptr)
      {
         err = "no node with that uid";
         return false;
      }
      const bool isGroup = ArrangeIsGroupNode(*gn);

      // Lane type: the given lane's, or for a new lane the node's natural one
      // (an explicitly asked-for audio output of a picture+sound node, e.g.
      // Video's output 1, goes on an audio lane).
      int laneType = 0;
      if (laneIndex >= 0)
      {
         if (laneIndex >= (int)gArrange.lanes.size())
         {
            err = "lane " + std::to_string(laneIndex) + " does not exist (" + std::to_string(gArrange.lanes.size()) +
                  " lanes; -1 adds a new one)";
            return false;
         }
         laneType = gArrange.lanes[(size_t)laneIndex].type;
      }
      else
      {
         laneType = ArrangeNaturalLaneType(*gn);
         auto* audio = dynamic_cast<IAudioSource*>(gn->node.get());
         if (!isGroup && srcOutput > 0 && audio != nullptr && IsNodeAudioCompatible(*gn) && audio->IsAudioOutputIndex(srcOutput))
            laneType = Arrange::kLaneAudio;
      }
      if (!ArrangeClipSourceFits(*gn, laneType))
      {
         err = NodeTitle(*gn) + (laneType == Arrange::kLaneAudio ? " makes no audio, so it cannot go on an audio lane"
                                                                  : " makes no picture, so it cannot go on a video lane");
         return false;
      }
      int output = 0;
      if (!isGroup)
      {
         const std::vector<int> outs = ArrangeOutputsOfType(*gn, laneType);
         if (std::find(outs.begin(), outs.end(), srcOutput) != outs.end())
            output = srcOutput;
         else if (srcOutput == 0 && !outs.empty())
            output = outs.front(); // the default "output 0" means "its usual output for this lane"
         else
         {
            err = "output " + std::to_string(srcOutput) + " of " + NodeTitle(*gn) + " is not a " +
                  (laneType == Arrange::kLaneAudio ? "sound" : "picture") + " output";
            return false;
         }
      }

      Arrange::Model m = gArrange;
      int li = laneIndex;
      if (li < 0)
         li = Arrange::LaneIndex(m, Arrange::AddLane(m, laneType));
      else if (const Arrange::Clip* hit = Overlapping(m.lanes[(size_t)li], start, start + length, 0))
      {
         err = OverlapError(m, li, *hit);
         return false;
      }
      Arrange::Clip c;
      c.start = start;
      c.length = length;
      c.srcUid = srcUid;
      c.srcOutput = output;
      uint64_t made = 0;
      if (li < 0 || !Arrange::PlaceOverwrite(m, m.lanes[(size_t)li].id, c, &made) || made == 0)
      {
         err = "the timeline refused the clip";
         return false;
      }
      std::string why;
      if (!Arrange::Validate(m, &why))
      {
         err = "the edit would break the timeline: " + why;
         return false;
      }
      Commit(m);
      outClipId = made;
      return true;
   }


   bool ArrangeMoveClip(uint64_t clipId, int laneIndex, double startBeats, double lengthBeats, std::string& err)
   {
      const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
      if (!loc.Valid())
      {
         err = "no clip " + std::to_string(clipId) + " (list_clips lists them)";
         return false;
      }
      Arrange::Clip c = gArrange.lanes[(size_t)loc.lane].clips[(size_t)loc.index];
      const int fromLane = loc.lane;
      const int toLane = laneIndex < 0 ? fromLane : laneIndex;
      if (toLane >= (int)gArrange.lanes.size())
      {
         err = "lane " + std::to_string(toLane) + " does not exist";
         return false;
      }
      const bool isGroup = ArrangeIsGroupSource(c.srcUid);
      if (gArrange.lanes[(size_t)toLane].type != gArrange.lanes[(size_t)fromLane].type && !isGroup)
      {
         const int fromType = gArrange.lanes[(size_t)fromLane].type;
         err = std::string("clip ") + std::to_string(clipId) + " is on " +
               (fromType == Arrange::kLaneAudio ? "an audio" : (fromType == Arrange::kLaneMidi ? "a MIDI" : "a video")) + " lane; " +
               LaneLabel(gArrange, toLane) + " is not (only group clips change lane type)";
         return false;
      }
      if ((gArrange.lanes[(size_t)toLane].type == Arrange::kLaneMidi) != (gArrange.lanes[(size_t)fromLane].type == Arrange::kLaneMidi))
      {
         err = LaneLabel(gArrange, toLane) + (gArrange.lanes[(size_t)toLane].type == Arrange::kLaneMidi
                                                 ? " is a MIDI lane; only MIDI clips go there"
                                                 : " is not a MIDI lane; a MIDI clip stays on MIDI lanes");
         return false;
      }
      Arrange::Tick start = c.start, length = c.length;
      if (startBeats >= 0.0 && !ToTicks(startBeats, true, start, "startBeats", err))
         return false;
      if (lengthBeats > 0.0 && !ToTicks(lengthBeats, false, length, "lengthBeats", err))
         return false;
      if (start + length > Arrange::kMaxTick)
      {
         err = "the clip would end past the end of the timeline";
         return false;
      }
      if (const Arrange::Clip* hit = Overlapping(gArrange.lanes[(size_t)toLane], start, start + length, clipId))
      {
         err = OverlapError(gArrange, toLane, *hit);
         return false;
      }
      if (toLane == fromLane && start == c.start && length == c.length)
         return true; // nothing to do

      // Lifted out and put back: no trim semantics (a Sample keeps its source
      // offset), the id and clip-group membership survive. A group clip moved
      // to a lane of the other type leaves its clip group - clip groups are
      // moved together and never span lane types.
      Arrange::Model m = gArrange;
      std::vector<Arrange::Clip>& from = m.lanes[(size_t)fromLane].clips;
      from.erase(from.begin() + loc.index);
      if (m.lanes[(size_t)toLane].type != m.lanes[(size_t)fromLane].type)
         c.groupId = 0;
      c.start = start;
      c.length = length;
      c.fadeIn = std::min(c.fadeIn, length);
      c.fadeOut = std::min(c.fadeOut, length);
      if (!Arrange::PlaceOverwrite(m, m.lanes[(size_t)toLane].id, c))
      {
         err = "the timeline refused the move";
         return false;
      }
      std::string why;
      if (!Arrange::Validate(m, &why))
      {
         err = "the edit would break the timeline: " + why;
         return false;
      }
      Commit(m);
      return true;
   }


   bool ArrangeDeleteClip(uint64_t clipId, std::string& err)
   {
      if (!Arrange::Find(gArrange, clipId).Valid())
      {
         err = "no clip " + std::to_string(clipId) + " (list_clips lists them)";
         return false;
      }
      Arrange::Model m = gArrange;
      if (!Arrange::Delete(m, { clipId }))
      {
         err = "the timeline refused the delete";
         return false;
      }
      Commit(m);
      gArrangeSel.erase(clipId);
      return true;
   }


   bool ArrangeSetClipProps(uint64_t clipId, const nlohmann::json& props, std::string& err)
   {
      if (!Arrange::Find(gArrange, clipId).Valid())
      {
         err = "no clip " + std::to_string(clipId) + " (list_clips lists them)";
         return false;
      }
      if (!props.is_object())
      {
         err = "props must be an object";
         return false;
      }
      Arrange::Model m = gArrange;
      Arrange::Clip* c = Arrange::FindClip(m, clipId);
      bool any = false;
      if (props.contains("enabled"))
      {
         if (!props["enabled"].is_boolean()) { err = "enabled must be true or false"; return false; }
         c->enabled = props["enabled"].get<bool>();
         any = true;
      }
      if (props.contains("gainDb"))
      {
         if (!props["gainDb"].is_number()) { err = "gainDb must be a number"; return false; }
         // The inspector's range.
         c->gainDb = std::clamp(props["gainDb"].get<float>(), -60.0f, 12.0f);
         any = true;
      }
      for (const char* key : { "fadeInBeats", "fadeOutBeats" })
      {
         if (!props.contains(key))
            continue;
         if (!props[key].is_number() || !(props[key].get<double>() >= 0.0))
         {
            err = std::string(key) + " must be a number >= 0";
            return false;
         }
         const Arrange::Tick t = std::clamp<Arrange::Tick>(Arrange::BeatsToTicks(props[key].get<double>()), 0, c->length);
         (key[4] == 'I' ? c->fadeIn : c->fadeOut) = t;
         any = true;
      }
      if (props.contains("name"))
      {
         if (!props["name"].is_string()) { err = "name must be a string"; return false; }
         c->name = props["name"].get<std::string>();
         any = true;
      }
      if (!any)
      {
         err = "nothing to set: give enabled, gainDb, fadeInBeats, fadeOutBeats or name";
         return false;
      }
      m.revision++;
      Commit(m);
      return true;
   }


   bool ArrangeSetLoop(bool enabled, double startBeats, double endBeats, std::string& err)
   {
      if (!enabled)
      {
         // Off keeps the range, the way the panel's loop toggle does.
         ArrangeSetLoop(false, gArrange.settings.loop.start, gArrange.settings.loop.end);
         return true;
      }
      Arrange::Tick a = 0, b = 0;
      if (!ToTicks(startBeats, true, a, "startBeats", err) || !ToTicks(endBeats, false, b, "endBeats", err))
         return false;
      if (b <= a)
      {
         err = "endBeats must be after startBeats";
         return false;
      }
      ArrangeSetLoop(true, a, b); // model + revision + dirty + Transport::SetLoop (PublishArrangeLoop)
      return true;
   }


   // ---- INFINITE_ARRANGEGROUPTEST -------------------------------------------
   //
   // Group clips (GroupTimelineWindows), the helper API, a group clip's save/
   // load round trip, and render-to-track end to end: an Oscillator rendered
   // over one bar plus a tail must come back as a WAV of exactly
   // round(range seconds * rate) + round(tail * rate) frames on a new lane,
   // as an Audio Sample clip at the range start.
   void FrameTest_ARRANGEGROUPTEST(int frameId, GLFWwindow* window)
   {
      if (getenv("INFINITE_ARRANGEGROUPTEST") == nullptr)
         return;
      static bool sAllOk = true;
      static int sRenderStage = 0; // 0 not started, 1 running, 2 reported
      static int sRenderStartFrame = 0;
      static long long sExpectFrames = 0;
      static Arrange::Tick sExpectStart = 0;
      static size_t sLanesBefore = 0;
      auto check = [](bool ok, const char* what)
      {
         printf("arrange group test: %s %s\n", what, ok ? "OK" : "FAIL");
         sAllOk = sAllOk && ok;
      };

      if (frameId == 4)
      {
         NewPatch();
         Transport::Instance().SetTempo(120.0f);
         Transport::Instance().SetLoop(false, 0.0, 0.0);
         Arrange::gSampleLiveTempoBpm = 120.0;
         {
            const uint64_t rev = gArrange.revision;
            gArrange = Arrange::Model();
            gArrange.revision = rev + 1;
         }

         // Uids read right after each spawn (SpawnNode can reallocate gNodes).
         GraphNode* osc = SpawnNode("Oscillator", "Synthesizers", 0.0f, 0.0f);
         const uint64_t oscUid = osc != nullptr ? osc->uid : 0;
         const int oscIndex = osc != nullptr ? osc->index : -1;
         GraphNode* out = SpawnNode("Audio Out", "Utility", 400.0f, 0.0f);
         const int outIndex = out != nullptr ? out->index : -1;
         GraphNode* grpGn = SpawnNode("Group", "Compositing", -40.0f, -40.0f);
         const uint64_t groupUid = grpGn != nullptr ? grpGn->uid : 0;
         GroupNode* group = grpGn != nullptr ? dynamic_cast<GroupNode*>(grpGn->node.get()) : nullptr;
         check(oscUid != 0 && outIndex >= 0 && group != nullptr, "spawn");
         if (group == nullptr || oscUid == 0)
         {
            printf("arrange group test: all FAIL\n");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            return;
         }
         gGroupMembers[group] = { oscIndex };
         std::string err;
         ConnectNodes(oscIndex, 0, outIndex, 0, err);

         // A. No clip: ungated.
         std::vector<std::pair<double, double>> w;
         check(!GroupTimelineWindows(groupUid, w) && w.empty(), "no clips -> false");
         check(ArrangeIsGroupSource(groupUid) && !ArrangeIsGroupSource(oscUid), "group source detection");
         if (GraphNode* g = FindNodeByUid(groupUid))
            check(ArrangeNaturalLaneType(*g) == Arrange::kLaneAudio, "group with a synth -> audio lane");

         // B. Helper API: add (new lane), add on that lane, overlap refused.
         uint64_t c1 = 0, c2 = 0, c3 = 0;
         const bool add1 = ArrangeAddClip(groupUid, 0, -1, 0.0, 4.0, c1, err);
         const bool add2 = ArrangeAddClip(groupUid, 0, 0, 8.0, 4.0, c2, err);
         std::string overlapErr;
         const bool add3 = ArrangeAddClip(groupUid, 0, 0, 2.0, 4.0, c3, overlapErr);
         check(add1 && add2 && !add3 && c3 == 0 && overlapErr.find("overlap") != std::string::npos &&
                  gArrange.lanes.size() == 1 && gArrange.lanes[0].clips.size() == 2,
               "add clips, overlap refused");

         check(GroupTimelineWindows(groupUid, w) && w.size() == 2 && w[0].first == 0.0 && w[0].second == 4.0 &&
                  w[1].first == 8.0 && w[1].second == 12.0,
               "windows of enabled clips");

         // C. A disabled clip and a muted lane drop out; the group stays gated.
         std::string e2;
         ArrangeSetClipProps(c2, nlohmann::json{ { "enabled", false }, { "clipId", 99 } }, e2);
         check(GroupTimelineWindows(groupUid, w) && w.size() == 1 && w[0].second == 4.0, "disabled clip excluded");
         gArrange.lanes[0].mute = true;
         gArrange.revision++;
         check(GroupTimelineWindows(groupUid, w) && w.empty(), "muted lane -> gated, silent");
         gArrange.lanes[0].mute = false;
         gArrange.revision++;
         ArrangeSetClipProps(c2, nlohmann::json{ { "enabled", true } }, e2);

         // D. Move: refuse an overlap, accept a free spot; delete.
         std::string moveErr;
         const bool moveBad = ArrangeMoveClip(c2, -1, 2.0, -1.0, moveErr);
         const bool moveOk = ArrangeMoveClip(c2, -1, 16.0, 2.0, err);
         const Arrange::Clip* moved = Arrange::FindClip(gArrange, c2);
         check(!moveBad && moveOk && moved != nullptr && moved->start == Arrange::BeatsToTicks(16.0) &&
                  moved->length == Arrange::BeatsToTicks(2.0),
               "move refuses overlap, moves into free space");
         // A rebuild with group clips on an audio lane must skip them
         // (ArrangeIsGroupSource in RebuildAudioTopology) without tripping.
         RebuildAudioTopology();
         check(GroupTimelineWindows(groupUid, w) && w.size() == 2 && w[1].first == 16.0, "windows follow the move");

         // E. Loop helper publishes to Transport.
         const bool loopOk = ArrangeSetLoop(true, 4.0, 8.0, err);
         std::string loopErr;
         const bool loopBad = ArrangeSetLoop(true, 8.0, 4.0, loopErr);
         check(loopOk && !loopBad && Transport::Instance().LoopEnabled() && Transport::Instance().LoopStartBeats() == 4.0 &&
                  Transport::Instance().LoopEndBeats() == 8.0 && gArrange.settings.loop.end == Arrange::BeatsToTicks(8.0),
               "set loop");
         ArrangeSetLoop(false, 0.0, 0.0, err);
         check(!Transport::Instance().LoopEnabled(), "loop off");

         // F. ClipsJson describes a group clip as such.
         const nlohmann::json js = ArrangeClipsJson();
         bool sawGroup = false;
         for (const auto& cj : js["clips"])
            sawGroup = sawGroup || (cj.value("kind", std::string()) == "group" && cj.value("srcIndex", -1) == grpGn->index);
         check(sawGroup, "clips json kind group");

         // G. Save/load round trip keeps the group clip pointing at the group.
         {
            const std::string path = TmpPath("arrange_grouptest.inf");
            SavePatchTo(path);
            LoadPatchFrom(path);
            std::remove(path.c_str());
            const Arrange::Clip* rc = Arrange::FindClip(gArrange, c1);
            check(rc != nullptr && rc->srcUid == groupUid && ArrangeIsGroupSource(rc->srcUid) &&
                     GroupTimelineWindows(groupUid, w) && w.size() == 2,
                  "group clip survives save/load");
         }

         // H. Delete.
         const bool del = ArrangeDeleteClip(c1, err);
         std::string delErr;
         check(del && !ArrangeDeleteClip(c1, delErr) && Arrange::FindClip(gArrange, c1) == nullptr, "delete clip");

         // H2. Deleting the group leaves its clip offline, like a deleted node.
         if (GraphNode* g = FindNodeByUid(groupUid))
            RemoveNodeByIndex(g->index);
         const Arrange::Clip* orphan = Arrange::FindClip(gArrange, c2);
         check(FindNodeByUid(groupUid) == nullptr && orphan != nullptr && orphan->srcUid == 0 &&
                  !GroupTimelineWindows(groupUid, w),
               "deleting the group leaves its clip offline");

         // I. Render to track: Oscillator, bar 2 (beats 4..8 at 120 bpm = 2 s), 0.5 s tail.
         GraphNode* oscAfter = FindNodeByUid(oscUid);
         sLanesBefore = gArrange.lanes.size();
         sExpectStart = Arrange::BeatsToTicks(4.0);
         std::string rErr;
         if (oscAfter != nullptr && StartRenderNodeToTrack(oscAfter->index, 4.0, 8.0, 0.5, false, rErr))
         {
            sRenderStage = 1;
            sRenderStartFrame = frameId;
         }
         else
         {
            printf("arrange group test: render to track could not start (%s) SKIP\n", rErr.c_str());
            sRenderStage = 2;
         }
         check(!StartRenderNodeToTrack(oscAfter != nullptr ? oscAfter->index : -1, 0.0, 4.0, 0.0, false, rErr),
               "second render refused while one runs");
      }
      else if (sRenderStage == 1)
      {
         const RenderToTrackStatus& st = RenderToTrackState();
         if (st.done || frameId - sRenderStartFrame > 600)
         {
            sRenderStage = 3;
            const double rate = st.ok ? RenderToTrackSampleRate() : 0.0;
            sExpectFrames = (long long)std::llround(2.0 * rate) + (long long)std::llround(0.5 * rate);
            std::error_code ec;
            const long long bytes = st.ok ? (long long)std::filesystem::file_size(AppPaths::FsPath(st.path), ec) : 0;
            const long long frames = bytes > 44 ? (bytes - 44) / 4 : 0; // 16-bit stereo
            check(st.ok, ("render finished: " + st.message).c_str());
            printf("arrange group test: wav %lld frames, expected %lld at %.0f Hz\n", frames, sExpectFrames, rate);
            check(st.ok && frames == sExpectFrames, "wav length = range + tail at the engine rate");
            const Arrange::Loc loc = Arrange::Find(gArrange, st.clipId);
            const Arrange::Clip* c = Arrange::FindClip(gArrange, st.clipId);
            check(loc.Valid() && gArrange.lanes.size() == sLanesBefore + 1 && loc.lane == (int)gArrange.lanes.size() - 1 &&
                     loc.lane == st.laneIndex && gArrange.lanes[(size_t)loc.lane].type == Arrange::kLaneAudio &&
                     c != nullptr && c->start == sExpectStart && c->sampleDropped && !c->syncToTempo && !c->importPending,
                  "new lane with an unsynced sample clip at the range start");
            GraphNode* src = c != nullptr ? FindNodeByUid(c->srcUid) : nullptr;
            auto* af = src != nullptr ? dynamic_cast<AudioFileNode*>(src->node.get()) : nullptr;
            check(af != nullptr && af->Buffer() != nullptr && (long long)af->Buffer()->numFrames == sExpectFrames,
                  "the clip's Audio File holds the rendered file");
            check(!Transport::Instance().IsOfflineMode() && !RenderToTrackBusy(), "transport and device restored");
            if (!st.path.empty())
               std::filesystem::remove(AppPaths::FsPath(st.path), ec);
            printf("arrange group test: all %s\n", sAllOk ? "OK" : "FAIL");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
         }
      }
      else if (sRenderStage == 2 && frameId > 4)
      {
         sRenderStage = 3;
         printf("arrange group test: all %s\n", sAllOk ? "OK" : "FAIL");
         glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
   }
}
