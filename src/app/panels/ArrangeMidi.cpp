// Timeline MIDI: MIDI lanes (Arrange::kLaneMidi) play their clips' notes into
// one instrument node each. This file owns the audio side (the per-instrument
// TimelineNoteMerger placed into the topology by RebuildAudioTopology, the
// immutable note snapshots it plays and their deferred retire), the MIDI
// helper API for the assistant, "Capture notes to timeline" (offline note
// render of a canvas note producer onto a new MIDI lane) and
// INFINITE_ARRANGEMIDITEST. The piano roll lives in ArrangePianoRoll.cpp.
// See .claude/skills/timeline-arrangement-architecture ("MIDI lanes").
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "app/ui/design/components/FormParts.h"
#include "app/ui/design/components/MenuParts.h"
#include "app/ui/design/components/ScreenBlocker.h"
#include "arrange/ArrangeModel.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioEngine.h"
#include "audio/AudioNode.h"
#include "audio/AudioProbe.h"
#include "audio/DspMath.h"
#include "audio/MidiFile.h"
#include "audio/NoteEventQueue.h"
#include "audio/TimelineNoteMerger.h"
#include "nodes/AudioToMidiNode.h"
#include "nodes/NoteNodes.h"

namespace app
{
   // AudioTopology.cpp, not in a header (their types live in AudioEngine.h).
   void CollectAudioChain(INode* node, std::set<INode*>& visited, std::vector<AudioTopologyEntry>& outOrder,
                          std::unordered_map<AudioNode*, int>& bufferIndexOf, int& nextBufferIndex);
   CompensationDelay& ArrangeTerminalCompensation(uint64_t laneId, uint64_t srcUid, int srcOutput);

   namespace
   {
      // ---- node roles ------------------------------------------------------

      int FirstNoteSlot(INode* node)
      {
         if (node == nullptr)
            return -1;
         for (int slot = 0; slot < kMaxNoteSlots; slot++)
            if (node->NoteInputSlot(slot) != nullptr)
               return slot;
         return -1;
      }

      // ---- per-instrument mergers -----------------------------------------
      //
      // One merger per instrument uid. The merger object and the snapshot it
      // plays are referenced by the audio thread for as long as a topology
      // generation that contains it can run, so both are freed only once
      // CompletedGeneration() has passed the last generation that could see
      // them (the gArrangeTerminalComp / gRetiredNodes rule), or at once
      // when no audio thread can race (no device: ProcessOffline and the
      // device-less pump both run on this thread).
      struct MergerSlot
      {
         std::unique_ptr<TimelineNoteMerger> node;
         std::unique_ptr<TimelineNoteSnapshot> snap; // what node plays, null = none
         uint64_t lastUsedGeneration = 0;
         bool used = false;
      };
      std::unordered_map<uint64_t, MergerSlot> gMergers;

      struct RetiredSnap
      {
         std::unique_ptr<TimelineNoteSnapshot> snap;
         uint64_t afterGeneration = 0; // free once CompletedGeneration() > this
      };
      std::vector<RetiredSnap> gRetiredSnaps;
      uint64_t gNextSnapSerial = 1;

      bool AudioRaceable()
      {
         return AudioEngine::Instance().SampleRate() > 0.0 && AudioEngine::Instance().IsAlive();
      }

      uint64_t NoteKey(uint64_t clipId, Arrange::Tick absStart, int note)
      {
         uint64_t k = clipId * 0x9E3779B97F4A7C15ull;
         k ^= (uint64_t)absStart + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2);
         k ^= (uint64_t)(uint32_t)note + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2);
         return k == 0 ? 1 : k;
      }

      // ---- capture job (declared early: the topology hook reads it) -------
      class NoteCaptureTap : public AudioNode
      {
      public:
         struct Ev
         {
            long long frame = 0;
            int note = 0;
            float velocity = 0.0f;
            bool on = false;
            int voiceId = 0;
            const void* source = nullptr;
         };
         static constexpr size_t kMaxEvents = 1 << 18;

         void Begin()
         {
            mInbox = nullptr;
            mCursor = -1;
            mFrames = 0;
            mEvents.clear();
            mEvents.reserve(1 << 14);
         }
         void SetNoteInbox(NoteEventQueue* inbox, int cursor) override
         {
            mInbox = inbox;
            mCursor = cursor;
         }
         // Offline only: ProcessOffline runs on the main thread with the device
         // stopped, so recording into a vector here is not on a real-time thread.
         void ProcessBlock(const AudioBuffer* const* /*inputs*/, int /*numInputs*/, AudioBuffer& output) override
         {
            const int numFrames = output.numFrames;
            for (int ch = 0; ch < output.numChannels; ch++)
               if (output.channels[ch] != nullptr)
                  std::fill(output.channels[ch], output.channels[ch] + std::max(0, numFrames), 0.0f);
            if (mInbox != nullptr && mCursor >= 0 && numFrames > 0)
            {
               NoteEvent ev[64];
               int n = 0;
               while ((n = mInbox->Pop(mCursor, ev, 64)) > 0)
                  for (int i = 0; i < n; i++)
                  {
                     if (ev[i].bendUpdate || mEvents.size() >= kMaxEvents)
                        continue;
                     Ev e;
                     e.frame = mFrames + std::clamp(ev[i].frameOffset, 0, numFrames - 1);
                     e.note = ev[i].note;
                     e.velocity = ev[i].velocity;
                     e.on = ev[i].isNoteOn && ev[i].velocity > 0.0f;
                     e.voiceId = ev[i].voiceId;
                     e.source = ev[i].source;
                     mEvents.push_back(e);
                  }
            }
            mFrames += std::max(0, numFrames);
         }
         bool Wired() const { return mInbox != nullptr && mCursor >= 0; }
         const std::vector<Ev>& Events() const { return mEvents; }

      private:
         NoteEventQueue* mInbox = nullptr;
         int mCursor = -1;
         long long mFrames = 0;
         std::vector<Ev> mEvents;
      };

      struct CaptureJob
      {
         enum Stage { kIdle, kRendering } stage = kIdle;
         uint64_t producerUid = 0;
         uint64_t instrumentUid = 0;
         std::string title;
         double startBeats = 0.0, endBeats = 0.0;
         double rate = 0.0, bpm = 120.0;
         long long framesTotal = 0, framesDone = 0;
         bool pushUndo = true;
         bool cancel = false;
         // Borrowed device/transport state (graph/OfflineTake.cpp), given
         // back by CaptureRestore().
         OfflineTake take;
      };
      CaptureJob gCapture;
      CaptureNotesStatus gCaptureStatus;
      // Never freed: a topology the job built may still be retired with a
      // pointer to it, and one tap is all a single job needs.
      NoteCaptureTap gCaptureTap;
      bool gCaptureTapWired = false;
   }


   // ---- node roles -----------------------------------------------------------

   bool ArrangeIsNoteInstrument(const GraphNode& gn)
   {
      if (gn.node == nullptr || ArrangeIsGroupNode(gn))
         return false;
      return FirstNoteSlot(gn.node.get()) >= 0;
   }


   bool ArrangeIsNoteProducer(const GraphNode& gn)
   {
      if (gn.node == nullptr || ArrangeIsGroupNode(gn))
         return false;
      if (dynamic_cast<MidiFileNode*>(gn.node.get()) != nullptr || dynamic_cast<AudioToMidiNode*>(gn.node.get()) != nullptr)
         return true;
      AudioNode* an = AudioNodeOfAny(gn.node.get());
      return an != nullptr && an->NoteOutbox(0) != nullptr;
   }


   std::vector<int> ArrangeNoteInstrumentNodes()
   {
      std::vector<int> out;
      for (const GraphNode& gn : gNodes)
         if (!gn.hiddenFromCanvas && ArrangeIsNoteInstrument(gn))
            out.push_back(gn.index);
      return out;
   }


   // The first instrument (in gNodes order) whose note input is cabled from
   // `producer`, or 0.
   uint64_t ArrangeFirstInstrumentOf(const GraphNode& producer)
   {
      if (producer.node == nullptr)
         return 0;
      for (const GraphNode& gn : gNodes)
      {
         if (gn.node == nullptr || &gn == &producer)
            continue;
         for (int slot = 0; slot < kMaxNoteSlots; slot++)
         {
            NoteCable* c = gn.node->NoteInputSlot(slot);
            if (c == nullptr || !c->IsConnected())
               continue;
            if (c->GetSource() == producer.node.get() || ResolvedAudioSource(c->GetSource()) == producer.node.get())
               return gn.uid;
         }
      }
      return 0;
   }


   // ---- audio topology hook ----------------------------------------------------
   //
   // Called once per RebuildAudioTopology, after every seed has its `order`
   // entry and the canvas note wiring has been described, before PrepareToPlay.
   void ArrangeMidiAugmentTopology(std::vector<AudioTopologyEntry>& order, std::set<INode*>& visited,
                                   std::unordered_map<AudioNode*, int>& bufferIndexOf, int& nextBufferIndex,
                                   std::vector<AudioTerminal>& terminals, std::vector<ClipWindow>& clipWindows,
                                   std::vector<NoteOutboxPlan>& noteOutboxes, std::vector<NoteWire>& noteWires,
                                   bool timelineRouting, int (*gateSlotOf)(const INode*))
   {
      const uint64_t current = AudioEngine::Instance().CurrentGeneration();
      const uint64_t completed = AudioEngine::Instance().CompletedGeneration();
      const bool raceable = AudioRaceable();
      gRetiredSnaps.erase(std::remove_if(gRetiredSnaps.begin(), gRetiredSnaps.end(),
                                         [&](const RetiredSnap& r) { return !raceable || completed > r.afterGeneration; }),
                          gRetiredSnaps.end());

      // 1. What plays: per instrument, every note of every enabled clip on a
      //    playing MIDI lane - the audio lanes' rules (scope of a lane render,
      //    ancestor-aware enable, mute, solo across audio + MIDI lanes).
      bool anySolo = false;
      for (const Arrange::Lane& lane : gArrange.lanes)
         anySolo = anySolo || (Arrange::LaneHasMixer(lane.type) && lane.solo);
      std::vector<uint64_t> wanted; // instruments, in lane order
      std::unordered_map<uint64_t, std::vector<TimelineNote>> notesByInst;
      struct LaneOut { uint64_t laneId; uint64_t inst; float gain; float pan; };
      std::vector<LaneOut> laneOuts;
      for (const Arrange::Lane& lane : gArrange.lanes)
      {
         if (lane.type != Arrange::kLaneMidi || lane.instrumentUid == 0)
            continue;
         const uint64_t inst = lane.instrumentUid;
         if (std::find(wanted.begin(), wanted.end(), inst) == wanted.end())
            wanted.push_back(inst);
         if (!timelineRouting)
            continue; // like audio clips: the timeline plays only with timeline audio on
         if (!gArrangeRenderActiveLaneScope.empty() && !gArrangeRenderActiveLaneScope.count(lane.id))
            continue;
         if (!Arrange::LaneEffectivelyEnabled(gArrange, lane))
            continue;
         const bool silenced = lane.mute || (anySolo && !lane.solo);
         laneOuts.push_back({ lane.id, inst, silenced ? 0.0f : std::pow(10.0f, lane.gainDb / 20.0f), lane.pan });
         if (silenced)
            continue;
         std::vector<TimelineNote>& out = notesByInst[inst];
         for (const Arrange::Clip& c : lane.clips)
         {
            if (!c.enabled || c.length <= 0)
               continue;
            for (const Arrange::MidiNote& n : c.notes)
            {
               if (n.start >= c.length)
                  continue; // past the clip's end (kept, not played)
               const Arrange::Tick on = c.start + n.start;
               const Arrange::Tick off = std::min(on + n.length, c.End());
               if (off <= on)
                  continue;
               TimelineNote tn;
               tn.onBeat = Arrange::TicksToBeats(on);
               tn.offBeat = Arrange::TicksToBeats(off);
               tn.key = NoteKey(c.id, on, n.note);
               tn.note = std::clamp(n.note, 0, 127);
               tn.velocity = std::clamp(n.velocity, 0.0f, 1.0f);
               out.push_back(tn);
            }
         }
      }
      for (auto& kv : notesByInst)
         std::stable_sort(kv.second.begin(), kv.second.end(), [](const TimelineNote& a, const TimelineNote& b)
         {
            return a.onBeat != b.onBeat ? a.onBeat < b.onBeat : a.note < b.note;
         });

      // 2. One merger in front of each instrument. A merger no lane wants
      //    any more stays (with no notes) while it still holds a note, so the
      //    instrument gets its note-off.
      for (auto& kv : gMergers)
         kv.second.used = false;
      std::vector<uint64_t> place = wanted;
      for (auto& kv : gMergers)
         if (std::find(wanted.begin(), wanted.end(), kv.first) == wanted.end() && kv.second.node != nullptr &&
             kv.second.node->HeldCount() > 0)
            place.push_back(kv.first);
      static const std::vector<TimelineNote> kNoNotes;
      for (uint64_t inst : place)
      {
         GraphNode* gn = FindNodeByUid(inst);
         if (gn == nullptr || gn->node == nullptr || gn->node->bypassed)
            continue;
         const int slot = FirstNoteSlot(gn->node.get());
         AudioNode* consumer = AudioNodeOfAny(gn->node.get());
         if (slot < 0 || consumer == nullptr)
            continue;
         CollectAudioChain(gn->node.get(), visited, order, bufferIndexOf, nextBufferIndex);
         int at = -1;
         for (int k = 0; k < (int)order.size(); k++)
            if (order[(size_t)k].node == consumer) { at = k; break; }
         int wireAt = -1;
         for (int w = 0; w < (int)noteWires.size(); w++)
            if (noteWires[(size_t)w].consumer == consumer && noteWires[(size_t)w].slot == slot) { wireAt = w; break; }
         if (at < 0 || wireAt < 0)
            continue;

         MergerSlot& ms = gMergers[inst];
         if (ms.node == nullptr)
            ms.node = std::make_unique<TimelineNoteMerger>();
         ms.used = true;
         ms.lastUsedGeneration = current + 1; // the generation about to be published

         // Publish only on a real change, so an unrelated edit never resets
         // the merger's cursor (and a sounding note keeps sounding).
         auto it = notesByInst.find(inst);
         const std::vector<TimelineNote>& want = it != notesByInst.end() ? it->second : kNoNotes;
         const bool same = ms.snap != nullptr ? ms.snap->notes == want : want.empty();
         if (!same)
         {
            std::unique_ptr<TimelineNoteSnapshot> fresh;
            if (!want.empty())
            {
               fresh = std::make_unique<TimelineNoteSnapshot>();
               fresh->serial = gNextSnapSerial++;
               fresh->notes = want;
            }
            ms.node->Publish(fresh.get());
            if (ms.snap != nullptr)
            {
               if (raceable)
                  gRetiredSnaps.push_back(RetiredSnap{ std::move(ms.snap), current });
               ms.snap.reset();
            }
            ms.snap = std::move(fresh);
         }

         AudioTopologyEntry e;
         e.node = ms.node.get();
         e.numInputs = 0;
         e.numOutputs = 1;
         e.outputBufferIndex = nextBufferIndex;
         e.outputBufferIndices[0] = nextBufferIndex;
         nextBufferIndex++;
         e.noteOnly = true; // forwards canvas notes for the device-less pump too
         e.stageId = kAudioStageOther;
         order.insert(order.begin() + at, e);

         // The merger reads what the instrument used to read (same cursor,
         // same group gate); the instrument reads the merger.
         const NoteWire canvasWire = noteWires[(size_t)wireAt];
         noteWires[(size_t)wireAt].inbox = ms.node->NoteOutbox();
         noteWires[(size_t)wireAt].cursor = 0;
         noteWires[(size_t)wireAt].noteGate = nullptr;
         NoteWire mw;
         mw.consumer = ms.node.get();
         mw.slot = 0;
         mw.inbox = canvasWire.inbox;
         mw.cursor = canvasWire.cursor;
         mw.noteGate = canvasWire.noteGate;
         noteWires.push_back(mw);
         NoteOutboxPlan plan;
         plan.queue = ms.node->NoteOutbox();
         plan.consumerCount = 1;
         noteOutboxes.push_back(plan);
      }

      // 3. Timeline routing bypasses every canvas Audio Out, so a MIDI lane is
      //    also its instrument's way to the device: one terminal per
      //    instrument, the lane's gain/pan/mute/solo, a single window spanning
      //    the whole timeline (silent while paused, like every timeline
      //    terminal). sourceNode stays null: no clip position lock or pitch
      //    override is pushed into an instrument. An instrument that is also
      //    the source of an audio-lane clip is routed by that lane instead.
      if (timelineRouting)
      {
         std::set<uint64_t> audioLaneSources;
         for (const Arrange::Lane& lane : gArrange.lanes)
            if (lane.type == Arrange::kLaneAudio)
               for (const Arrange::Clip& c : lane.clips)
                  if (c.srcUid != 0)
                     audioLaneSources.insert(c.srcUid);
         std::set<uint64_t> routed;
         for (size_t i = 0; i < laneOuts.size(); i++)
         {
            const uint64_t inst = laneOuts[i].inst;
            if (routed.count(inst) || audioLaneSources.count(inst))
               continue;
            routed.insert(inst);
            // Several lanes on one instrument share its one output: the first
            // lane that is not silenced sets level and pan.
            LaneOut lo = laneOuts[i];
            for (size_t k = i; k < laneOuts.size(); k++)
               if (laneOuts[k].inst == inst && laneOuts[k].gain > 0.0f) { lo = laneOuts[k]; break; }
            GraphNode* gn = FindNodeByUid(inst);
            if (gn == nullptr || gn->node == nullptr)
               continue;
            INode* resolved = ResolvedAudioSource(gn->node.get());
            if (resolved == nullptr)
               continue;
            CollectAudioChain(resolved, visited, order, bufferIndexOf, nextBufferIndex);
            const int idx = AudioBufferIndexOf(resolved, 0, bufferIndexOf);
            if (idx < 0)
               continue;
            ClipWindow w;
            w.clipId = 0; // no live waveform for a MIDI lane
            w.startBeat = 0.0;
            w.endBeat = Arrange::TicksToBeats(Arrange::kMaxTick);
            w.abutsPrev = true; // no declick ramp at beat 0
            w.abutsNext = true;
            w.retrigger = false;
            w.gain = 1.0f;
            DspMath::EqualPowerPan(std::clamp(lo.pan, -1.0f, 1.0f), w.panL, w.panR);
            w.panL *= 1.41421356f;
            w.panR *= 1.41421356f;
            AudioTerminal term;
            term.bufferIndex = idx;
            term.gain = 1.0f;
            term.sourceNode = nullptr;
            term.laneGain = lo.gain;
            term.lanePanL = w.panL;
            term.lanePanR = w.panR;
            term.windowOffset = (int)clipWindows.size();
            term.numWindows = 1;
            term.externalCompensation = &ArrangeTerminalCompensation(lo.laneId, inst, 0);
            term.gateSlot = gateSlotOf != nullptr ? gateSlotOf(resolved) : -1;
            clipWindows.push_back(w);
            terminals.push_back(term);
         }
      }

      // 4. Capture: a tap reading the producer's note outbox with its own
      //    cursor, after everything else in the block.
      gCaptureTapWired = false;
      if (gCapture.stage == CaptureJob::kRendering)
      {
         GraphNode* prod = FindNodeByUid(gCapture.producerUid);
         AudioNode* pa = prod != nullptr && prod->node != nullptr && !prod->node->bypassed ? AudioNodeOfAny(prod->node.get()) : nullptr;
         NoteEventQueue* q = pa != nullptr ? pa->NoteOutbox(0) : nullptr;
         if (q != nullptr)
         {
            CollectAudioChain(prod->node.get(), visited, order, bufferIndexOf, nextBufferIndex);
            int cursor = -1;
            bool havePlan = false;
            for (NoteOutboxPlan& plan : noteOutboxes)
               if (plan.queue == q)
               {
                  havePlan = true;
                  if (plan.consumerCount < NoteEventQueue::kMaxConsumers)
                     cursor = plan.consumerCount++;
                  break;
               }
            if (!havePlan)
            {
               NoteOutboxPlan plan;
               plan.queue = q;
               plan.consumerCount = 1;
               noteOutboxes.push_back(plan);
               cursor = 0;
            }
            if (cursor >= 0)
            {
               AudioTopologyEntry e;
               e.node = &gCaptureTap;
               e.numInputs = 0;
               e.numOutputs = 1;
               e.outputBufferIndex = nextBufferIndex;
               e.outputBufferIndices[0] = nextBufferIndex;
               nextBufferIndex++;
               e.noteOnly = true;
               e.stageId = kAudioStageOther;
               order.push_back(e);
               NoteWire tw;
               tw.consumer = &gCaptureTap;
               tw.slot = 0;
               tw.inbox = q;
               tw.cursor = cursor;
               noteWires.push_back(tw);
               gCaptureTapWired = true;
            }
         }
      }

      // 5. Mergers nothing placed this time: freed once no generation that
      //    could still run them is left.
      for (auto it = gMergers.begin(); it != gMergers.end();)
      {
         if (!it->second.used && (!raceable || completed > it->second.lastUsedGeneration))
            it = gMergers.erase(it);
         else
            ++it;
      }
   }


   TimelineNoteMerger* ArrangeMidiMergerFor(uint64_t instrumentUid)
   {
      auto it = gMergers.find(instrumentUid);
      return it != gMergers.end() && it->second.used ? it->second.node.get() : nullptr;
   }


   void ArrangeMidiAudition(uint64_t instrumentUid, int note, float velocity)
   {
      if (TimelineNoteMerger* m = ArrangeMidiMergerFor(instrumentUid))
         m->AuditionOn(note, velocity);
   }


   void ArrangeMidiAuditionOff(uint64_t instrumentUid)
   {
      if (TimelineNoteMerger* m = ArrangeMidiMergerFor(instrumentUid))
         m->AuditionOff();
   }


   // ---- helper API (assistant RPC) ---------------------------------------------
   //
   // Same contract as ArrangeTools.cpp's helpers: edit a copy, validate, swap
   // in (one revision change), never push undo - the caller pushes the
   // checkpoint first.

   namespace
   {
      void CommitModel(Arrange::Model& edited)
      {
         Arrange::Normalize(edited);
         if (edited.revision <= gArrange.revision)
            edited.revision = gArrange.revision + 1;
         gArrange = std::move(edited);
         ArrangeCommitEdit();
      }

      std::string FreshLaneName(const Arrange::Model& m, const std::string& want)
      {
         if (want.empty())
            return Arrange::UniqueLaneName(m, std::string(), Arrange::kLaneMidi);
         for (const Arrange::Lane& l : m.lanes)
            if (l.name == want)
               return Arrange::UniqueLaneName(m, want, Arrange::kLaneMidi);
         return want;
      }

      bool ParseNotes(const nlohmann::json& notes, std::vector<Arrange::MidiNote>& out, std::string& err)
      {
         out.clear();
         if (!notes.is_array())
         {
            err = "notes must be an array of {t, dur, midi, vel}";
            return false;
         }
         for (size_t i = 0; i < notes.size(); i++)
         {
            const nlohmann::json& n = notes[i];
            const std::string at = "note " + std::to_string(i) + ": ";
            if (!n.is_object())
            {
               err = at + "must be an object {t, dur, midi, vel}";
               return false;
            }
            if (!n.contains("t") || !n["t"].is_number() || !(n["t"].get<double>() >= 0.0) || !std::isfinite(n["t"].get<double>()))
            {
               err = at + "t (beats from the clip start) must be a number >= 0";
               return false;
            }
            if (!n.contains("dur") || !n["dur"].is_number() || !(n["dur"].get<double>() > 0.0) || !std::isfinite(n["dur"].get<double>()))
            {
               err = at + "dur (beats) must be a number > 0";
               return false;
            }
            if (!n.contains("midi") || !n["midi"].is_number())
            {
               err = at + "midi (0..127) is required";
               return false;
            }
            const double midi = n["midi"].get<double>();
            if (!(midi >= 0.0 && midi <= 127.0))
            {
               err = at + "midi must be 0..127";
               return false;
            }
            float vel = 0.8f;
            if (n.contains("vel"))
            {
               if (!n["vel"].is_number() || !(n["vel"].get<double>() >= 0.0 && n["vel"].get<double>() <= 1.0))
               {
                  err = at + "vel must be a number 0..1";
                  return false;
               }
               vel = n["vel"].get<float>();
            }
            const double t = n["t"].get<double>();
            const double dur = n["dur"].get<double>();
            if (t + dur > Arrange::TicksToBeats(Arrange::kMaxTick))
            {
               err = at + "ends past the end of the timeline";
               return false;
            }
            Arrange::MidiNote mn;
            mn.start = Arrange::BeatsToTicks(t);
            mn.length = std::max<Arrange::Tick>(1, Arrange::BeatsToTicks(t + dur) - mn.start);
            mn.note = (int)std::lround(midi);
            mn.velocity = vel;
            out.push_back(mn);
         }
         Arrange::SanitizeMidiNotes(out);
         return true;
      }
   }


   bool ArrangeAddMidiLane(uint64_t instrumentUid, const std::string& name, int& outLaneIndex, std::string& err)
   {
      outLaneIndex = -1;
      GraphNode* gn = nullptr;
      if (instrumentUid != 0)
      {
         gn = FindNodeByUid(instrumentUid);
         if (gn == nullptr || gn->node == nullptr)
         {
            err = "no node with that uid";
            return false;
         }
         if (!ArrangeIsNoteInstrument(*gn))
         {
            err = NodeTitle(*gn) + " has no note input, so it cannot play a MIDI lane";
            return false;
         }
      }
      Arrange::Model m = gArrange;
      const uint64_t laneId = Arrange::AddLane(m, Arrange::kLaneMidi);
      Arrange::Lane* lane = Arrange::FindLane(m, laneId);
      if (lane == nullptr)
      {
         err = "the timeline refused the lane";
         return false;
      }
      lane->instrumentUid = instrumentUid;
      lane->name = FreshLaneName(m, !name.empty() ? name : (gn != nullptr ? ArrangeSourceTitle(*gn) : std::string()));
      std::string why;
      if (!Arrange::Validate(m, &why))
      {
         err = "the edit would break the timeline: " + why;
         return false;
      }
      CommitModel(m);
      outLaneIndex = Arrange::LaneIndex(gArrange, laneId);
      return true;
   }


   bool ArrangeSetMidiLaneInstrument(uint64_t laneId, uint64_t instrumentUid, std::string& err)
   {
      const Arrange::Lane* lane = Arrange::FindLane(gArrange, laneId);
      if (lane == nullptr || lane->type != Arrange::kLaneMidi)
      {
         err = "not a MIDI lane";
         return false;
      }
      if (instrumentUid != 0)
      {
         GraphNode* gn = FindNodeByUid(instrumentUid);
         if (gn == nullptr || !ArrangeIsNoteInstrument(*gn))
         {
            err = "that node has no note input";
            return false;
         }
      }
      if (lane->instrumentUid == instrumentUid)
         return true;
      Arrange::Model m = gArrange;
      Arrange::FindLane(m, laneId)->instrumentUid = instrumentUid;
      m.revision++;
      CommitModel(m);
      return true;
   }


   bool ArrangeSetMidiNotes(uint64_t clipId, const nlohmann::json& notes, std::string& err)
   {
      const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
      if (!loc.Valid())
      {
         err = "no clip " + std::to_string(clipId) + " (list_clips lists them)";
         return false;
      }
      if (gArrange.lanes[(size_t)loc.lane].type != Arrange::kLaneMidi)
      {
         err = "clip " + std::to_string(clipId) + " is not on a MIDI lane";
         return false;
      }
      std::vector<Arrange::MidiNote> parsed;
      if (!ParseNotes(notes, parsed, err))
         return false;
      Arrange::Model m = gArrange;
      Arrange::FindClip(m, clipId)->notes = std::move(parsed);
      m.revision++;
      std::string why;
      if (!Arrange::Validate(m, &why))
      {
         err = "the edit would break the timeline: " + why;
         return false;
      }
      CommitModel(m);
      return true;
   }


   nlohmann::json ArrangeMidiNotesJson(uint64_t clipId)
   {
      using json = nlohmann::json;
      const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
      if (!loc.Valid())
         return json{ { "error", "no clip " + std::to_string(clipId) } };
      const Arrange::Lane& lane = gArrange.lanes[(size_t)loc.lane];
      if (lane.type != Arrange::kLaneMidi)
         return json{ { "error", "clip " + std::to_string(clipId) + " is not on a MIDI lane" } };
      const Arrange::Clip& c = lane.clips[(size_t)loc.index];
      GraphNode* inst = FindNodeByUid(lane.instrumentUid);
      json notes = json::array();
      for (const Arrange::MidiNote& n : c.notes)
         notes.push_back({ { "t", Arrange::TicksToBeats(n.start) },
                           { "dur", Arrange::TicksToBeats(n.length) },
                           { "midi", n.note },
                           { "vel", n.velocity } });
      return json{ { "clipId", c.id },
                   { "lane", loc.lane },
                   { "laneId", lane.id },
                   { "name", c.name },
                   { "startBeats", Arrange::TicksToBeats(c.start) },
                   { "lengthBeats", Arrange::TicksToBeats(c.length) },
                   { "enabled", c.enabled },
                   { "instrumentUid", lane.instrumentUid },
                   { "instrumentIndex", inst != nullptr ? inst->index : -1 },
                   { "noteCount", (int)c.notes.size() },
                   { "notes", notes } };
   }


   // ---- capture notes to timeline ---------------------------------------------
   //
   //   Start   - MIDI File / Audio to MIDI: their notes are read directly (exact,
   //             no render) and committed at once. Anything else: refuse while
   //             any render/offline capture runs; stop the device, park the
   //             transport at the range start in offline mode (loop suspended),
   //             re-prepare the graph, and let RebuildAudioTopology wire the
   //             capture tap to the producer's outbox (ArrangeMidiAugmentTopology).
   //   Render  - ProcessOffline in small blocks (kCaptureBlock frames, so a
   //             block-granular producer is placed within that many samples),
   //             ~60 ms of work per UI frame.
   //   Restore - everything borrowed goes back (CaptureRestore), topology rebuilt.
   //   Commit  - pair note-on/off (a note still held at the range end ends
   //             there), then a new MIDI lane on the instrument with one clip at
   //             the range start. One undo step (the panel's path pushes it; the
   //             assistant's caller pushed its own before starting).

   namespace
   {
      constexpr int kCaptureBlock = 64;

      std::string CaptureTitle(const GraphNode& gn) { return ArrangeSourceTitle(gn); }

      bool CommitCapture(const std::vector<Arrange::MidiNote>& notes, std::string& err)
      {
         const bool wasSuppressed = gSuppressUndoCheckpoints;
         if (gCapture.pushUndo)
            PushUndoCheckpoint();
         gSuppressUndoCheckpoints = true;
         const uint64_t laneId = Arrange::AddLane(gArrange, Arrange::kLaneMidi); // appended = below the others
         uint64_t clipId = 0;
         if (Arrange::Lane* l = Arrange::FindLane(gArrange, laneId))
         {
            l->instrumentUid = gCapture.instrumentUid;
            l->name = FreshLaneName(gArrange, gCapture.title);
            Arrange::Clip c;
            c.start = Arrange::BeatsToTicks(gCapture.startBeats);
            c.length = std::max<Arrange::Tick>(1, Arrange::BeatsToTicks(gCapture.endBeats) - c.start);
            c.name = gCapture.title;
            c.notes = notes;
            Arrange::SanitizeMidiNotes(c.notes);
            Arrange::PlaceOverwrite(gArrange, laneId, c, &clipId);
         }
         gArrange.revision++;
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
         gArrangeFlashClipId = clipId;
         gArrangeFlashStart = ImGui::GetTime();
         gCaptureStatus.clipId = clipId;
         gCaptureStatus.laneIndex = Arrange::LaneIndex(gArrange, laneId);
         gCaptureStatus.noteCount = (int)notes.size();
         return true;
      }

      void FinishCapture(bool ok, const std::string& message)
      {
         gCapture.stage = CaptureJob::kIdle;
         gCaptureStatus.active = false;
         gCaptureStatus.done = true;
         gCaptureStatus.ok = ok;
         gCaptureStatus.progress = ok ? 1.0f : gCaptureStatus.progress;
         gCaptureStatus.message = message;
         if (!ok)
            fprintf(stderr, "capture notes failed: %s\n", message.c_str());
      }

      void CaptureRestore()
      {
         gCapture.stage = CaptureJob::kIdle; // the next rebuild drops the tap
         OfflineTakeRestore(gCapture.take);
      }

      // Pairs the tap's events into notes relative to the range start.
      std::vector<Arrange::MidiNote> PairCapturedNotes()
      {
         std::vector<Arrange::MidiNote> out;
         const double beatsPerFrame = gCapture.bpm / (60.0 * std::max(1.0, gCapture.rate));
         struct Open { int note; int voiceId; const void* src; long long frame; float vel; };
         std::vector<Open> open;
         auto emit = [&](const Open& o, long long endFrame)
         {
            const Arrange::Tick s = std::max<Arrange::Tick>(0, Arrange::BeatsToTicks((double)o.frame * beatsPerFrame));
            const Arrange::Tick e = Arrange::BeatsToTicks((double)endFrame * beatsPerFrame);
            Arrange::MidiNote n;
            n.start = s;
            n.length = std::max<Arrange::Tick>(1, e - s);
            n.note = std::clamp(o.note, 0, 127);
            n.velocity = std::clamp(o.vel, 0.0f, 1.0f);
            out.push_back(n);
         };
         for (const NoteCaptureTap::Ev& ev : gCaptureTap.Events())
         {
            if (ev.frame >= gCapture.framesTotal)
               break;
            if (ev.on)
            {
               open.push_back(Open{ ev.note, ev.voiceId, ev.source, ev.frame, ev.velocity });
               continue;
            }
            // Oldest matching open note: by voice id when both carry one,
            // else by pitch and producer.
            for (size_t i = 0; i < open.size(); i++)
            {
               const Open& o = open[i];
               const bool match = (ev.voiceId != 0 && o.voiceId != 0) ? o.voiceId == ev.voiceId
                                                                       : (o.note == ev.note && o.src == ev.source);
               if (!match)
                  continue;
               emit(o, ev.frame);
               open.erase(open.begin() + (long)i);
               break;
            }
         }
         for (const Open& o : open)
            emit(o, gCapture.framesTotal); // still held at the range end: ends there
         return out;
      }

      // MIDI File: its song on the beat grid, from `position`, at `speed`,
      // looped at LoopBeats() - what AudioSongPlayer plays from a transport
      // start at the range start.
      bool DirectNotesFromMidiFile(MidiFileNode& mf, double rangeBeats, std::vector<Arrange::MidiNote>& out, std::string& err)
      {
         const MidiFile::Song* song = mf.GetSong();
         if (song == nullptr || song->events.empty())
         {
            err = "the MIDI File node has no file loaded";
            return false;
         }
         struct SongNote { double on, off; int note; int channel; float vel; };
         std::vector<SongNote> notes;
         std::vector<SongNote> open;
         std::vector<int> openTrack;
         for (const MidiFile::Event& e : song->events)
         {
            if (e.on)
            {
               open.push_back(SongNote{ e.beat, e.beat, (int)e.note, (int)e.channel, (float)e.velocity / 127.0f });
               openTrack.push_back((int)e.track);
               continue;
            }
            for (size_t i = 0; i < open.size(); i++)
               if (open[i].note == (int)e.note && openTrack[i] == (int)e.track)
               {
                  SongNote n = open[i];
                  n.off = e.beat;
                  notes.push_back(n);
                  open.erase(open.begin() + (long)i);
                  openTrack.erase(openTrack.begin() + (long)i);
                  break;
               }
         }
         for (SongNote n : open)
         {
            n.off = std::max(n.on, song->lengthBeats);
            notes.push_back(n);
         }
         const double len = std::max(1.0, mf.LoopBeats());
         const bool loop = mf.loop;
         const double speed = std::clamp((double)mf.speed, 0.05, 8.0);
         const double p0 = std::clamp((double)mf.position, 0.0, 1.0) * len;
         const float velScale = mf.velocity;
         for (long long k = 0; k < 100000; k++)
         {
            const double base = loop ? (double)k * len : 0.0;
            if ((base - p0) / speed >= rangeBeats || (!loop && k > 0))
               break;
            for (const SongNote& n : notes)
            {
               if (loop && n.on >= len)
                  continue;
               const double fb = base + n.on;
               if (fb < p0)
                  continue;
               const double t = (fb - p0) / speed;
               if (t >= rangeBeats)
                  continue;
               double offFb = base + n.off;
               if (loop)
                  offFb = std::min(offFb, base + len);
               const double dur = std::max(0.0, offFb - fb) / speed;
               Arrange::MidiNote mn;
               mn.start = Arrange::BeatsToTicks(t);
               mn.length = std::max<Arrange::Tick>(1, Arrange::BeatsToTicks(t + dur) - mn.start);
               mn.note = std::clamp(n.note + (n.channel == 9 ? 0 : mf.transpose), 0, 127);
               mn.velocity = std::clamp(n.vel * velScale, 0.0f, 1.0f);
               out.push_back(mn);
            }
         }
         return true;
      }

      // Audio to MIDI: transcribed notes in file seconds, locked to the
      // transport (song second = transport second - offset), looping over the
      // whole file when loop is on - what its player plays.
      bool DirectNotesFromAudioToMidi(AudioToMidiNode& a2m, double startBeats, double endBeats,
                                      std::vector<Arrange::MidiNote>& out, std::string& err)
      {
         const std::vector<BasicPitch::Note>& notes = a2m.Notes();
         if (notes.empty())
         {
            err = "the Audio to MIDI node has no notes yet (load or record audio first)";
            return false;
         }
         const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
         const double bps = bpm / 60.0;
         const double offsetBeats = (double)std::clamp(a2m.offsetMs, -2000.0f, 2000.0f) / 1000.0 * bps;
         const double lenBeats = std::max(1.0, a2m.DurationSeconds() * bps);
         const bool loop = a2m.loop;
         for (long long k = 0; k < 100000; k++)
         {
            const double base = (loop ? (double)k * lenBeats : 0.0) + offsetBeats;
            if (base >= endBeats || (!loop && k > 0))
               break;
            for (const BasicPitch::Note& n : notes)
            {
               double on = (double)n.start * bps;
               double off = std::max(on, (double)n.end * bps);
               if (loop)
               {
                  if (on >= lenBeats)
                     continue;
                  off = std::min(off, lenBeats);
               }
               on += base;
               off += base;
               if (on < startBeats || on >= endBeats || on < 0.0)
                  continue;
               Arrange::MidiNote mn;
               mn.start = Arrange::BeatsToTicks(on - startBeats);
               mn.length = std::max<Arrange::Tick>(1, Arrange::BeatsToTicks(off - startBeats) - mn.start);
               mn.note = std::clamp(n.pitch, 0, 127);
               mn.velocity = (float)std::clamp((int)std::lround(127.0f * n.amplitude), 1, 127) / 127.0f;
               out.push_back(mn);
            }
         }
         return true;
      }

      bool StartCaptureInternal(int nodeIndex, uint64_t instrumentUid, double startBeats, double endBeats, bool pushUndo,
                                std::string& err)
      {
         if (gCapture.stage != CaptureJob::kIdle)
         {
            err = "a note capture is already running";
            return false;
         }
         GraphNode* gn = FindNodeByIndex(nodeIndex);
         if (gn == nullptr || gn->node == nullptr)
         {
            err = "unknown node index " + std::to_string(nodeIndex);
            return false;
         }
         if (!ArrangeIsNoteProducer(*gn))
         {
            err = NodeTitle(*gn) + " has no note output";
            return false;
         }
         if (gn->node->bypassed)
         {
            err = NodeTitle(*gn) + " is bypassed";
            return false;
         }
         if (!std::isfinite(startBeats) || !std::isfinite(endBeats) || startBeats < 0.0 || !(endBeats > startBeats))
         {
            err = "the range must have 0 <= start < end";
            return false;
         }
         const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
         if ((endBeats - startBeats) * 60.0 / bpm > 60.0 * 30.0)
         {
            err = "the range is longer than 30 minutes";
            return false;
         }
         if (instrumentUid == 0)
            instrumentUid = ArrangeFirstInstrumentOf(*gn);
         else
         {
            GraphNode* inst = FindNodeByUid(instrumentUid);
            if (inst == nullptr || !ArrangeIsNoteInstrument(*inst))
            {
               err = "the target instrument has no note input";
               return false;
            }
         }

         gCapture = CaptureJob();
         gCapture.producerUid = gn->uid;
         gCapture.instrumentUid = instrumentUid;
         gCapture.title = CaptureTitle(*gn);
         gCapture.startBeats = startBeats;
         gCapture.endBeats = endBeats;
         gCapture.bpm = bpm;
         gCapture.pushUndo = pushUndo;
         gCaptureStatus = CaptureNotesStatus();
         gCaptureStatus.active = true;
         gCaptureStatus.message = "capturing " + gCapture.title;

         // Exact sources: read the notes, no render.
         std::vector<Arrange::MidiNote> direct;
         bool isDirect = false;
         bool directOk = false;
         if (auto* mf = dynamic_cast<MidiFileNode*>(gn->node.get()))
         {
            isDirect = true;
            directOk = DirectNotesFromMidiFile(*mf, endBeats - startBeats, direct, err);
         }
         else if (auto* a2m = dynamic_cast<AudioToMidiNode*>(gn->node.get()))
         {
            isDirect = true;
            directOk = DirectNotesFromAudioToMidi(*a2m, startBeats, endBeats, direct, err);
         }
         if (isDirect)
         {
            if (!directOk)
            {
               FinishCapture(false, err);
               return false;
            }
            if (direct.empty())
            {
               err = "no notes fall inside that range";
               FinishCapture(false, err);
               return false;
            }
            if (!CommitCapture(direct, err))
            {
               FinishCapture(false, err);
               return false;
            }
            FinishCapture(true, "captured " + std::to_string(direct.size()) + " notes from " + gCapture.title);
            return true;
         }

         if (ArrangeRenderBusy() || gOfflineRender.active || Transport::Instance().IsOfflineMode() ||
             AudioProbe::Instance().IsArmed() || RenderToFileBusy())
         {
            err = "another render or audio capture is running; wait for it to finish";
            gCaptureStatus = CaptureNotesStatus();
            return false;
         }

         // An OfflineTake (graph/OfflineTake.cpp), like render to track: the
         // producer starts from its prepared state at startBeats (its step
         // clock fires the step under the start) and modulation is stepped on
         // the audio clock, so a modulated generator captures what it plays.
         gCapture.rate = OfflineTakeRate();
         gCapture.framesTotal = std::max<long long>(1, (long long)std::llround((endBeats - startBeats) * 60.0 / bpm * gCapture.rate));
         OfflineTakeBegin(gCapture.take, gCapture.rate, 0.0, startBeats);
         gCaptureTap.Begin();
         gCapture.stage = CaptureJob::kRendering; // before the rebuild: it wires the tap
         OfflineTakePrepare(gCapture.take);
         if (!gCaptureTapWired)
         {
            CaptureRestore();
            err = "could not read " + gCapture.title + "'s notes (its note output has too many listeners)";
            FinishCapture(false, err);
            return false;
         }
         return true;
      }
   }


   bool StartCaptureNotesToTimeline(int nodeIndex, uint64_t instrumentUid, double startBeats, double endBeats,
                                    std::string& err)
   {
      return StartCaptureInternal(nodeIndex, instrumentUid, startBeats, endBeats, /*pushUndo=*/false, err);
   }


   const CaptureNotesStatus& CaptureNotesState() { return gCaptureStatus; }
   bool CaptureNotesBusy() { return gCapture.stage == CaptureJob::kRendering; }


   void PumpCaptureNotes()
   {
      if (gCapture.stage != CaptureJob::kRendering)
         return;
      if (FindNodeByUid(gCapture.producerUid) == nullptr)
      {
         CaptureRestore();
         FinishCapture(false, "the node being captured was deleted");
         return;
      }
      static float sL[kAudioMaxBlockFrames];
      static float sR[kAudioMaxBlockFrames];
      const double budgetStart = glfwGetTime();
      while (!gCapture.cancel && gCapture.framesDone < gCapture.framesTotal)
      {
         const int frames = (int)std::min<long long>(kCaptureBlock, gCapture.framesTotal - gCapture.framesDone);
         OfflineTakeRender(gCapture.take, sL, sR, frames);
         gCapture.framesDone += frames;
         if (glfwGetTime() - budgetStart > 0.06)
            break;
      }
      gCaptureStatus.progress = 0.95f * (float)((double)gCapture.framesDone / (double)std::max<long long>(1, gCapture.framesTotal));
      if (!gCapture.cancel && gCapture.framesDone < gCapture.framesTotal)
         return;

      const bool cancelled = gCapture.cancel;
      CaptureRestore();
      if (cancelled)
      {
         FinishCapture(false, "cancelled");
         return;
      }
      const std::vector<Arrange::MidiNote> notes = PairCapturedNotes();
      if (notes.empty())
      {
         FinishCapture(false, gCapture.title + " sent no notes in that range");
         return;
      }
      std::string err;
      if (!CommitCapture(notes, err))
      {
         FinishCapture(false, err);
         return;
      }
      FinishCapture(true, "captured " + std::to_string(notes.size()) + " notes from " + gCapture.title);
   }


   void DrawCaptureNotesProgressWindow()
   {
      if (gCapture.stage != CaptureJob::kRendering)
         return;
      ImGuiIO& io = ImGui::GetIO();
      ScreenBlocker::Draw("##CaptureNotesBlocker");

      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.5f));
      PushElevatedPanelStyle(/*isChild=*/false);
      ImGui::Begin(L("Capturing Notes"), nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
      if (gCapture.cancel)
         ImGui::Text("%s", T("Cancelling..."));
      else
         ImGui::Text(T("%s: %.1fs of %.1fs"), gCapture.title.c_str(), (double)gCapture.framesDone / gCapture.rate,
                     (double)gCapture.framesTotal / gCapture.rate);
      ImGui::ProgressBar((float)((double)gCapture.framesDone / (double)std::max<long long>(1, gCapture.framesTotal)),
                         ImVec2(280, 0));
      ImGui::BeginDisabled(gCapture.cancel);
      if (ActionButton::Draw(L("Cancel##capturenotes"), ImVec2(120, 0)))
         gCapture.cancel = true;
      ImGui::EndDisabled();
      ImGui::End();
      PopElevatedPanelStyle();
   }


   namespace
   {
      int gCapPopupNode = -1;
      bool gCapPopupOpenRequest = false;
      Arrange::Tick gCapPopupStart = 0, gCapPopupEnd = 0;
      uint64_t gCapPopupInstrument = 0;
      std::string gCapPopupError;
   }


   void OpenCaptureNotesPopup(int nodeIndex)
   {
      gCapPopupNode = nodeIndex;
      gCapPopupOpenRequest = true;
      gCapPopupError.clear();
      // Default range: the loop when it is on, else 4 bars from the playhead.
      const Arrange::LoopRange& loop = gArrange.settings.loop;
      if (loop.enabled && loop.end > loop.start)
      {
         gCapPopupStart = loop.start;
         gCapPopupEnd = loop.end;
      }
      else
      {
         gCapPopupStart = std::max<Arrange::Tick>(0, ArrangePlayTick());
         gCapPopupEnd = gCapPopupStart + 4 * Arrange::kTicksPerBar;
      }
      GraphNode* gn = FindNodeByIndex(nodeIndex);
      gCapPopupInstrument = gn != nullptr ? ArrangeFirstInstrumentOf(*gn) : 0;
   }


   void DrawCaptureNotesPopup()
   {
      const char* kId = "Capture Notes to Timeline##capturenotespopup";
      if (gCapPopupOpenRequest)
      {
         ImGui::OpenPopup(kId);
         gCapPopupOpenRequest = false;
      }
      ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                              ImVec2(0.5f, 0.5f));
      if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
         return;
      GraphNode* gn = FindNodeByIndex(gCapPopupNode);
      if (gn == nullptr)
      {
         ImGui::CloseCurrentPopup();
         ImGui::EndPopup();
         return;
      }
      ImGui::Text(T("Capture %s's notes to a new MIDI track"), ArrangeSourceTitle(*gn).c_str());
      MenuParts::Separator();
      const float fieldW = 180.0f;
      Arrange::Tick t = 0;
      if (ArrangeTickField("Start##capstart", gCapPopupStart, 0, Arrange::kMaxTick, ArrangeTickUnit::Position, fieldW, &t))
      {
         const Arrange::Tick len = gCapPopupEnd - gCapPopupStart;
         gCapPopupStart = t;
         gCapPopupEnd = t + std::max<Arrange::Tick>(1, len);
      }
      if (ArrangeTickField("End##capend", gCapPopupEnd, gCapPopupStart + 1, Arrange::kMaxTick, ArrangeTickUnit::Position, fieldW, &t))
         gCapPopupEnd = std::max(gCapPopupStart + 1, t);

      // Target instrument: any node with a note input; default the first one
      // this node is cabled into.
      GraphNode* cur = FindNodeByUid(gCapPopupInstrument);
      const std::string curLabel = cur != nullptr ? NodeTitleWithInstance(*cur) : std::string(T("(none - pick later)"));
      ImGui::SetNextItemWidth(fieldW);
      PushDropdownStyle();
      if (ImGui::BeginCombo(L("Instrument##capinst"), curLabel.c_str()))
      {
         if (ImGui::Selectable(T("(none - pick later)"), gCapPopupInstrument == 0))
            gCapPopupInstrument = 0;
         for (int idx : ArrangeNoteInstrumentNodes())
         {
            GraphNode* in = FindNodeByIndex(idx);
            if (in == nullptr || in == gn)
               continue;
            ImGui::PushID(idx);
            if (ImGui::Selectable(NodeTitleWithInstance(*in).c_str(), in->uid == gCapPopupInstrument))
               gCapPopupInstrument = in->uid;
            ImGui::PopID();
         }
         ImGui::EndCombo();
      }
      PopDropdownStyle();
      ImGui::TextDisabled(T("Length %s"), ArrangeFormatLength(gCapPopupEnd - gCapPopupStart).c_str());
      if (!gCapPopupError.empty())
      {
         ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
         ImGui::TextColored(tok::V4(tok::palf::v_950_450_400_1000), "%s", gCapPopupError.c_str());
         ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      ImGui::BeginDisabled(gCapture.stage != CaptureJob::kIdle || ArrangeRenderBusy());
      if (ActionButton::Draw(L("Capture##capgo"), ImVec2(120, 0)))
      {
         std::string err;
         if (StartCaptureInternal(gCapPopupNode, gCapPopupInstrument, Arrange::TicksToBeats(gCapPopupStart),
                                  Arrange::TicksToBeats(gCapPopupEnd), /*pushUndo=*/true, err))
            ImGui::CloseCurrentPopup();
         else
            gCapPopupError = err;
      }
      ImGui::EndDisabled();
      FormParts::Inline();
      if (ActionButton::Draw(L("Cancel##capcancel"), ImVec2(120, 0)))
         ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
   }


   // ---- INFINITE_ARRANGEMIDITEST ---------------------------------------------
   //
   // A MIDI lane on an Equation Synth with a 4-note clip at beats 4..8, rendered
   // offline through the real topology: every note-on and note-off the
   // instrument's inbox received must sit on the exact first sample at/after
   // its beat (the last note is cut at the clip end), and nothing may stay
   // held after stop, seek or an edit mid-note. Then save/load, split, the
   // helper API, ClipsJson, and a capture of a Note Sequencer over one bar.
   void FrameTest_ARRANGEMIDITEST(int frameId, GLFWwindow* window)
   {
      if (getenv("INFINITE_ARRANGEMIDITEST") == nullptr)
         return;
      static bool sAllOk = true;
      static int sStage = 0; // 0 not started, 3 capture pending, 1 capture running, 2 done
      static int sCaptureFrame = 0;
      static uint64_t sEqUid = 0;
      static uint64_t sSeqUid = 0;
      static size_t sLanesBefore = 0;
      auto check = [](bool ok, const char* what)
      {
         printf("arrange midi test: %s %s\n", what, ok ? "OK" : "FAIL");
         sAllOk = sAllOk && ok;
      };
      auto finish = [&]()
      {
         sStage = 2;
         printf("arrange midi test: all %s\n", sAllOk ? "OK" : "FAIL");
         glfwSetWindowShouldClose(window, GLFW_TRUE);
      };

      if (frameId == 4 && sStage == 0)
      {
         NewPatch();
         Transport::Instance().SetTempo(120.0f);
         Transport::Instance().SetLoop(false, 0.0, 0.0);
         Transport::Instance().SetPlaying(false);
         Arrange::gSampleLiveTempoBpm = 120.0;
         {
            const uint64_t rev = gArrange.revision;
            gArrange = Arrange::Model();
            gArrange.revision = rev + 1;
         }
         gAudioMode = AudioMode::Timeline;

         GraphNode* eq = SpawnNode("Equation Synth", "Synths", 0.0f, 0.0f);
         const uint64_t eqUid = eq != nullptr ? eq->uid : 0;
         GraphNode* seq = SpawnNode("Note Sequencer", "Notes", -400.0f, 0.0f);
         const int seqIndex = seq != nullptr ? seq->index : -1;
         const uint64_t seqUid = seq != nullptr ? seq->uid : 0;
         sEqUid = eqUid;
         check(eqUid != 0 && seqIndex >= 0, "spawn");
         if (eqUid == 0 || seqIndex < 0)
         {
            finish();
            return;
         }
         if (GraphNode* g = FindNodeByUid(eqUid))
            check(ArrangeIsNoteInstrument(*g) && !ArrangeIsNoteProducer(*g), "Equation Synth is an instrument");
         if (GraphNode* g = FindNodeByIndex(seqIndex))
            check(ArrangeIsNoteProducer(*g), "Note Sequencer is a note producer");

         // A. Lane, clip and notes through the helper API.
         std::string err;
         int laneIdx = -1;
         const bool laneOk = ArrangeAddMidiLane(eqUid, "Keys", laneIdx, err);
         check(laneOk && laneIdx == 0 && gArrange.lanes[0].type == Arrange::kLaneMidi &&
                  gArrange.lanes[0].instrumentUid == eqUid && gArrange.lanes[0].name == "Keys",
               "add MIDI lane");
         uint64_t clipId = 0;
         const bool clipOk = ArrangeAddClip(0, 0, laneIdx, 4.0, 4.0, clipId, err);
         const Arrange::Clip* c0 = Arrange::FindClip(gArrange, clipId);
         check(clipOk && c0 != nullptr && c0->srcUid == 0 && c0->notes.empty() && c0->start == Arrange::BeatsToTicks(4.0),
               "add empty MIDI clip");
         const nlohmann::json notesIn = nlohmann::json::array({
            { { "t", 0.0 }, { "dur", 0.5 }, { "midi", 60 }, { "vel", 0.9 } },
            { { "t", 1.0 }, { "dur", 0.5 }, { "midi", 64 }, { "vel", 0.7 } },
            { { "t", 2.25 }, { "dur", 0.5 }, { "midi", 67 }, { "vel", 0.5 } },
            { { "t", 3.5 }, { "dur", 1.0 }, { "midi", 72 }, { "vel", 1.0 } } });
         std::string badErr;
         const bool badRefused = !ArrangeSetMidiNotes(clipId, nlohmann::json::array({ { { "t", 0.0 }, { "midi", 60 } } }), badErr);
         check(badRefused && !badErr.empty(), "malformed notes refused");
         check(ArrangeSetMidiNotes(clipId, notesIn, err), "set notes");
         {
            const nlohmann::json j = ArrangeMidiNotesJson(clipId);
            bool same = j.contains("notes") && j["notes"].size() == 4 && j.value("noteCount", 0) == 4 &&
                        j.value("instrumentUid", (uint64_t)0) == eqUid;
            for (size_t i = 0; same && i < 4; i++)
               same = std::fabs(j["notes"][i]["t"].get<double>() - notesIn[i]["t"].get<double>()) < 1e-9 &&
                      std::fabs(j["notes"][i]["dur"].get<double>() - notesIn[i]["dur"].get<double>()) < 1e-9 &&
                      j["notes"][i]["midi"].get<int>() == notesIn[i]["midi"].get<int>() &&
                      std::fabs(j["notes"][i]["vel"].get<double>() - notesIn[i]["vel"].get<double>()) < 1e-6;
            check(same, "notes json round trip");
            const nlohmann::json cj = ArrangeClipsJson();
            bool kinds = false;
            for (const auto& l : cj["lanes"])
               kinds = l.value("kind", std::string()) == "midi" && l.value("instrumentUid", (uint64_t)0) == eqUid;
            bool clipKind = false;
            for (const auto& cc : cj["clips"])
               clipKind = clipKind || (cc.value("kind", std::string()) == "midi" && cc.value("noteCount", 0) == 4);
            check(kinds && clipKind, "clips json reports midi lane and clip");
         }

         // B. Offline render, beats 3..9, 256-frame blocks at 48 kHz.
         const bool deviceWas = AudioEngine::Instance().SampleRate() > 0.0;
         if (deviceWas)
            AudioEngine::Instance().Stop();
         const double savedHeadless = gHeadlessAudioRate;
         const double rate = 48000.0;
         gHeadlessAudioRate = rate;
         Transport::Instance().SeekBeats(3.0);
         Transport::Instance().SetOfflineMode(true, rate);
         Transport::Instance().SetPlaying(true);
         ForceAudioRepare();
         RebuildAudioTopology();
         TimelineNoteMerger* mg = ArrangeMidiMergerFor(eqUid);
         check(mg != nullptr, "merger placed in front of the instrument");
         static float sL[kAudioMaxBlockFrames];
         static float sR[kAudioMaxBlockFrames];
         static float* sCh[2] = { sL, sR };
         auto renderBlocks = [&](int blocks)
         {
            for (int b = 0; b < blocks; b++)
            {
               AudioBuffer buf;
               buf.channels = sCh;
               buf.numChannels = 2;
               buf.numFrames = 256;
               AudioEngine::Instance().ProcessOffline(buf);
            }
         };
         auto lastTap = [&](TimelineNoteMerger* m) -> const TimelineNoteMerger::TapEvent*
         {
            return m != nullptr && m->TapCount() > 0 ? &m->TapAt(m->TapCount() - 1) : nullptr;
         };
         if (mg != nullptr)
         {
            mg->SetTapEnabled(true);
            const double bps = 120.0 / (60.0 * rate);
            renderBlocks((int)std::ceil(6.0 / bps / 256.0)); // beats 3..9
            struct Want { double beat; int note; bool on; };
            const Want want[] = { { 4.0, 60, true },   { 4.5, 60, false },  { 5.0, 64, true },  { 5.5, 64, false },
                                  { 6.25, 67, true },  { 6.75, 67, false }, { 7.5, 72, true },  { 8.0, 72, false } };
            const int got = mg->TapCount();
            bool exact = got == 8;
            for (int i = 0; exact && i < 8; i++)
            {
               const TimelineNoteMerger::TapEvent& e = mg->TapAt(i);
               const double at = e.blockStartBeat + (double)e.frameOffset * e.beatsPerSample;
               const bool ok = e.note == want[i].note && e.on == want[i].on && at >= want[i].beat - 1e-9 &&
                               at - want[i].beat < e.beatsPerSample;
               if (!ok)
                  printf("arrange midi test: event %d note %d %s at beat %.6f (block %.6f + %d), want %s %d at %.6f\n", i,
                         e.note, e.on ? "on" : "off", at, e.blockStartBeat, e.frameOffset, want[i].on ? "on" : "off",
                         want[i].note, want[i].beat);
               exact = exact && ok;
            }
            printf("arrange midi test: %d events\n", got);
            check(exact, "note-ons/offs on the exact sample, last one cut at the clip end");
            check(mg->HeldCount() == 0, "nothing held after the clip");

            // C. Stop mid-note: the note-off goes out on the next block.
            mg->SetTapEnabled(true);
            Transport::Instance().SeekBeats(4.0);
            renderBlocks(8); // note 60 sounding
            const bool heldBefore = mg->HeldCount() == 1;
            Transport::Instance().SetPlaying(false);
            renderBlocks(1);
            const TimelineNoteMerger::TapEvent* e = lastTap(mg);
            check(heldBefore && mg->HeldCount() == 0 && e != nullptr && !e->on && e->note == 60 && e->frameOffset == 0,
                  "stop releases the sounding note");

            // D. Seek mid-note.
            Transport::Instance().SetPlaying(true);
            Transport::Instance().SeekBeats(5.0);
            renderBlocks(8); // note 64 sounding
            const bool heldSeek = mg->HeldCount() == 1;
            Transport::Instance().SeekBeats(1.0);
            renderBlocks(1);
            e = lastTap(mg);
            check(heldSeek && mg->HeldCount() == 0 && e != nullptr && !e->on && e->note == 64, "seek releases the sounding note");

            // E. Edit mid-note: delete the sounding note.
            Transport::Instance().SeekBeats(5.0);
            renderBlocks(8);
            const bool heldEdit = mg->HeldCount() == 1;
            nlohmann::json without = notesIn;
            without.erase(1);
            ArrangeSetMidiNotes(clipId, without, err);
            RebuildAudioTopology();
            renderBlocks(1);
            e = lastTap(mg);
            check(heldEdit && mg->HeldCount() == 0 && e != nullptr && !e->on && e->note == 64,
                  "editing the clip releases the removed note");
            // An unrelated edit while a note sounds keeps it.
            ArrangeSetMidiNotes(clipId, notesIn, err);
            RebuildAudioTopology();
            Transport::Instance().SeekBeats(5.0);
            renderBlocks(8);
            gArrange.lanes[0].name = "Keys!";
            gArrange.revision++;
            RebuildAudioTopology();
            renderBlocks(1);
            check(mg->HeldCount() == 1, "an unrelated edit keeps the sounding note");
            // Muting the lane releases it.
            gArrange.lanes[0].mute = true;
            gArrange.revision++;
            RebuildAudioTopology();
            renderBlocks(1);
            check(mg->HeldCount() == 0, "muting the lane releases the sounding note");
            gArrange.lanes[0].mute = false;
            gArrange.lanes[0].name = "Keys";
            gArrange.revision++;
            mg->SetTapEnabled(false);
         }
         Transport::Instance().SetOfflineMode(false);
         Transport::Instance().Seek(0.0);
         gHeadlessAudioRate = savedHeadless;
         if (deviceWas)
            StartAudioEngine(gAudioStartError);
         else
            Transport::Instance().NotifyAudioEngineStopped();
         Transport::Instance().SetPlaying(false);
         RebuildAudioTopology();

         // F. Save/load keeps lane, instrument and notes.
         const std::vector<Arrange::MidiNote> before = Arrange::FindClip(gArrange, clipId)->notes;
         {
            const std::string path = TmpPath("arrange_miditest.inf");
            SavePatchTo(path);
            LoadPatchFrom(path);
            std::remove(path.c_str());
            const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
            const Arrange::Clip* rc = Arrange::FindClip(gArrange, clipId);
            bool same = loc.Valid() && rc != nullptr && rc->notes.size() == before.size() &&
                        gArrange.lanes[(size_t)loc.lane].type == Arrange::kLaneMidi &&
                        gArrange.lanes[(size_t)loc.lane].instrumentUid == eqUid && FindNodeByUid(eqUid) != nullptr;
            for (size_t i = 0; same && i < before.size(); i++)
               same = rc->notes[i].start == before[i].start && rc->notes[i].length == before[i].length &&
                      rc->notes[i].note == before[i].note && std::fabs(rc->notes[i].velocity - before[i].velocity) < 1e-4f;
            check(same, "save/load keeps the MIDI lane and its notes");
         }

         // G. Split at beat 5.25 cuts note 64 (5.0..5.5) in two.
         {
            uint64_t rightId = 0;
            ArrangeEdit([&]() { Arrange::Split(gArrange, clipId, Arrange::BeatsToTicks(5.25), &rightId); });
            const Arrange::Clip* l = Arrange::FindClip(gArrange, clipId);
            const Arrange::Clip* r = Arrange::FindClip(gArrange, rightId);
            const Arrange::Tick q = Arrange::kPPQ / 4;
            const bool leftOk = l != nullptr && l->notes.size() == 2 && l->notes[0].note == 60 && l->notes[1].note == 64 &&
                                l->notes[1].start == Arrange::kPPQ && l->notes[1].length == q;
            const bool rightOk = r != nullptr && r->notes.size() == 3 && r->notes[0].note == 64 && r->notes[0].start == 0 &&
                                 r->notes[0].length == q && r->notes[1].note == 67 && r->notes[1].start == Arrange::kPPQ &&
                                 r->notes[2].note == 72 && r->notes[2].start == Arrange::BeatsToTicks(2.25);
            check(leftOk && rightOk, "split crops the crossing note and keeps the rest");
            // Duplicate keeps notes.
            std::vector<uint64_t> made;
            ArrangeEdit([&]() { Arrange::DuplicateBlock(gArrange, { rightId }, &made); });
            const Arrange::Clip* d = made.size() == 1 ? Arrange::FindClip(gArrange, made[0]) : nullptr;
            check(d != nullptr && r != nullptr && Arrange::FindClip(gArrange, rightId) != nullptr &&
                     d->notes == Arrange::FindClip(gArrange, rightId)->notes,
                  "duplicate keeps the notes");
         }

         // H. (a few frames later, so the reloaded sequencer has been cooked
         //    and pushed its steps to its audio node) - see below.
         sSeqUid = seqUid;
         sStage = 3;
         sCaptureFrame = frameId;
      }
      else if (sStage == 3 && frameId >= sCaptureFrame + 3)
      {
         // H. Capture a Note Sequencer (8 steps of 1/16, note 60, gate 70%)
         //    over one bar onto a new lane on the Equation Synth.
         sLanesBefore = gArrange.lanes.size();
         std::string cErr;
         GraphNode* seqNow = FindNodeByUid(sSeqUid); // indices may move across the reload above
         if (seqNow != nullptr && StartCaptureNotesToTimeline(seqNow->index, sEqUid, 0.0, 4.0, cErr))
         {
            sStage = 1; // pumped by ArrangeRenderQueueTick from the next frame on
            sCaptureFrame = frameId;
         }
         else
         {
            check(false, ("capture could not start: " + cErr).c_str());
            finish();
         }
      }
      else if (sStage == 1)
      {
         const CaptureNotesStatus& st = CaptureNotesState();
         if (st.done || frameId - sCaptureFrame > 600)
         {
            check(st.ok, ("capture finished: " + st.message).c_str());
            const Arrange::Clip* c = Arrange::FindClip(gArrange, st.clipId);
            const Arrange::Loc loc = Arrange::Find(gArrange, st.clipId);
            const bool laneOk = loc.Valid() && gArrange.lanes.size() == sLanesBefore + 1 &&
                                gArrange.lanes[(size_t)loc.lane].type == Arrange::kLaneMidi &&
                                gArrange.lanes[(size_t)loc.lane].instrumentUid == sEqUid && c != nullptr && c->start == 0 &&
                                c->length == Arrange::BeatsToTicks(4.0);
            check(laneOk, "capture made a MIDI lane on the instrument with a one-bar clip");
            // A block-granular producer lands within one capture block (64
            // frames at the take rate) of its step; allow that plus a tick.
            const Arrange::Tick tol = 4;
            bool notesOk = c != nullptr && c->notes.size() == 16;
            for (size_t i = 0; notesOk && i < c->notes.size(); i++)
            {
               const Arrange::MidiNote& n = c->notes[i];
               const Arrange::Tick wantStart = (Arrange::Tick)i * (Arrange::kPPQ / 4);
               const Arrange::Tick wantLen = Arrange::BeatsToTicks(0.25 * 0.7);
               notesOk = n.note == 60 && std::llabs(n.start - wantStart) <= tol && std::llabs(n.length - wantLen) <= tol &&
                         std::fabs(n.velocity - 0.8f) < 1e-3f;
               if (!notesOk)
                  printf("arrange midi test: captured note %d: start %lld len %lld note %d vel %.3f\n", (int)i,
                         (long long)n.start, (long long)n.length, n.note, (double)n.velocity);
            }
            if (c != nullptr && c->notes.size() != 16)
               printf("arrange midi test: captured %d notes, want 16\n", (int)c->notes.size());
            check(notesOk, "captured notes match the sequencer");
            check(!Transport::Instance().IsOfflineMode() && !CaptureNotesBusy(), "transport and device restored");
            finish();
         }
      }
   }
}
