// MIDI clip editing for the timeline: the floating piano roll, the mini
// piano roll drawn inside a MIDI clip, "Add MIDI Track" and the MIDI parts of
// the inspector. The audio side (playback, capture, helper API) is
// ArrangeMidi.cpp. See .claude/skills/timeline-arrangement-architecture
// ("MIDI lanes").
//
// Piano roll gestures (every gesture is one undo step: ArrangeGestureBegin at
// the press, ArrangeGestureEnd at the release, pushed only if notes changed):
//   click empty grid      add a note (grid snap, length = one grid step), drag
//                         right to make it longer
//   drag a note           move it (time + pitch); the whole selection moves
//   drag a note's end     resize (the whole selection)
//   Shift/Ctrl click      toggle a note in the selection
//   Shift/Ctrl drag empty marquee select
//   double/right click    delete a note; Delete/Backspace deletes the selection
//   velocity lane drag    set the velocity of the note under the mouse (all
//                         selected notes when it is selected)
//   Alt while dragging    no snap
//   wheel                 scroll pitch; Shift+wheel time; Ctrl+wheel zoom time;
//                         Alt+wheel zoom pitch
//   keys column click     audition; pressing/adding a note auditions it too
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "arrange/ArrangeModel.h"

namespace app
{
   namespace
   {
      const char* const kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

      std::string MidiNoteName(int n)
      {
         n = std::clamp(n, 0, 127);
         return std::string(kNoteNames[n % 12]) + std::to_string(n / 12 - 1); // 60 = C4
      }

      bool IsBlackKey(int n)
      {
         const int k = ((n % 12) + 12) % 12;
         return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
      }

      std::string FreshMidiLaneName(const Arrange::Model& m, const std::string& want)
      {
         if (want.empty())
            return Arrange::UniqueLaneName(m, std::string(), Arrange::kLaneMidi);
         for (const Arrange::Lane& l : m.lanes)
            if (l.name == want)
               return Arrange::UniqueLaneName(m, want, Arrange::kLaneMidi);
         return want;
      }

      // ---- piano roll state ------------------------------------------------
      enum RollMode { kRollNone = 0, kRollMove, kRollResize, kRollCreate, kRollMarquee, kRollVelocity, kRollKeys };

      struct PianoRoll
      {
         bool open = false;
         bool focusRequest = false;
         bool fitRequest = false;
         uint64_t clipId = 0;
         float ppb = 96.0f;      // pixels per beat
         float keyH = 12.0f;     // pixels per semitone
         double scrollBeat = 0.0; // leftmost visible beat, clip-relative
         float topNote = 84.0f;  // pitch of the top row (may be fractional)
         int gridIndex = 1 + MusicTime::kSixteenth; // MusicTime::QuantizeGridList index, 0 = off
         float lastVelocity = 0.8f;
         std::vector<char> sel; // parallel to the clip's notes

         // Gesture in flight.
         RollMode mode = kRollNone;
         std::vector<Arrange::MidiNote> origNotes;
         std::vector<char> origSel;
         int grab = -1;                 // index into origNotes
         Arrange::Tick pressTick = 0;   // clip-relative, unsnapped
         float pressY = 0.0f;
         ImVec2 pressPos { 0, 0 };
         bool dragged = false;
         int auditionNote = -1;
         uint64_t auditionInst = 0;
      };
      PianoRoll gRoll;
      bool gRollOwnsKeys = false;

      Arrange::Tick GridTicks()
      {
         const double beats = MusicTime::QuantizeGridBeats(gRoll.gridIndex);
         return beats > 0.0 ? std::max<Arrange::Tick>(1, Arrange::BeatsToTicks(beats)) : 0;
      }

      void AuditionStart(uint64_t inst, int note, float vel)
      {
         if (inst == 0)
            return;
         if (gRoll.auditionNote == note && gRoll.auditionInst == inst)
            return;
         ArrangeMidiAudition(inst, note, vel);
         gRoll.auditionNote = note;
         gRoll.auditionInst = inst;
      }

      void AuditionStop()
      {
         if (gRoll.auditionNote >= 0)
            ArrangeMidiAuditionOff(gRoll.auditionInst);
         gRoll.auditionNote = -1;
         gRoll.auditionInst = 0;
      }

      // Writes `notes` (with their selection flags) into the clip, sorted the
      // way the model keeps them so the flags stay with their notes.
      void WriteNotes(uint64_t clipId, std::vector<Arrange::MidiNote> notes, std::vector<char> sel)
      {
         Arrange::Clip* c = Arrange::FindClip(gArrange, clipId);
         if (c == nullptr)
            return;
         sel.resize(notes.size(), 0);
         std::vector<size_t> order(notes.size());
         for (size_t i = 0; i < order.size(); i++)
            order[i] = i;
         for (Arrange::MidiNote& n : notes)
         {
            n.start = std::clamp<Arrange::Tick>(n.start, 0, Arrange::kMaxTick);
            n.length = std::clamp<Arrange::Tick>(n.length, 1, Arrange::kMaxTick);
            n.note = std::clamp(n.note, 0, 127);
            n.velocity = std::clamp(n.velocity, 0.0f, 1.0f);
         }
         std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
         {
            return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].note < notes[b].note;
         });
         std::vector<Arrange::MidiNote> sorted;
         std::vector<char> sortedSel;
         sorted.reserve(notes.size());
         sortedSel.reserve(notes.size());
         for (size_t i : order)
         {
            sorted.push_back(notes[i]);
            sortedSel.push_back(sel[i]);
         }
         gRoll.sel = sortedSel;
         if (c->notes == sorted)
            return;
         c->notes = std::move(sorted);
         gArrange.revision++;
      }

      void EndGesture()
      {
         if (gRoll.mode != kRollNone && gRoll.mode != kRollKeys && gRoll.mode != kRollMarquee)
            ArrangeGestureEnd();
         gRoll.mode = kRollNone;
         gRoll.grab = -1;
         AuditionStop();
      }

      void DeleteNotes(uint64_t clipId, const std::vector<char>& which)
      {
         ArrangeEdit([&]()
         {
            Arrange::Clip* c = Arrange::FindClip(gArrange, clipId);
            if (c == nullptr)
               return;
            std::vector<Arrange::MidiNote> keep;
            for (size_t i = 0; i < c->notes.size(); i++)
               if (i >= which.size() || !which[i])
                  keep.push_back(c->notes[i]);
            if (keep.size() == c->notes.size())
               return;
            c->notes = std::move(keep);
            gArrange.revision++;
         });
         gRoll.sel.clear();
      }
   }


   // ---- shared helpers ---------------------------------------------------------

   std::string ArrangeMidiInstrumentLabel(const Arrange::Lane& lane)
   {
      if (lane.instrumentUid == 0)
         return T("(no instrument)");
      GraphNode* gn = FindNodeByUid(lane.instrumentUid);
      return gn != nullptr ? NodeTitleWithInstance(*gn) : std::string(T("(missing)"));
   }


   void ArrangeDrawMidiClipPreview(ImDrawList* dl, const Arrange::Clip& clip, const ImVec2& rectMin, const ImVec2& rectMax,
                                   float clipX0, float clipX1, bool muted)
   {
      if (clip.notes.empty() || clip.length <= 0 || clipX1 <= clipX0)
         return;
      int lo = 127, hi = 0;
      for (const Arrange::MidiNote& n : clip.notes)
         if (n.start < clip.length)
         {
            lo = std::min(lo, n.note);
            hi = std::max(hi, n.note);
         }
      if (lo > hi)
         return;
      // At least an octave tall, so a one-note clip does not draw a block.
      if (hi - lo < 11)
      {
         const int pad = 11 - (hi - lo);
         lo = std::max(0, lo - pad / 2);
         hi = std::min(127, lo + 11);
      }
      const float top = rectMin.y + 4.0f;
      const float bottom = rectMax.y - 4.0f;
      if (bottom - top < 4.0f)
         return;
      const float rowH = (bottom - top) / (float)(hi - lo + 1);
      const float barH = std::max(1.5f, rowH - (rowH > 3.0f ? 1.0f : 0.0f));
      const double pxPerTick = (double)(clipX1 - clipX0) / (double)clip.length;
      const ImU32 col = muted ? IM_COL32(255, 255, 255, 80) : IM_COL32(255, 255, 255, 200);
      dl->PushClipRect(ImVec2(rectMin.x + 1.0f, rectMin.y + 1.0f), ImVec2(rectMax.x - 1.0f, rectMax.y - 1.0f), true);
      for (const Arrange::MidiNote& n : clip.notes)
      {
         if (n.start >= clip.length)
            continue;
         const float x0 = clipX0 + (float)((double)n.start * pxPerTick);
         const float x1 = clipX0 + (float)((double)std::min(n.start + n.length, clip.length) * pxPerTick);
         if (x1 < rectMin.x || x0 > rectMax.x)
            continue;
         const float y = top + (float)(hi - n.note) * rowH;
         dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x1 - 0.5f, x0 + 1.0f), y + barH), col, 0.0f);
      }
      dl->PopClipRect();
   }


   uint64_t ArrangeAddEmptyMidiClip(uint64_t laneId, Arrange::Tick atTick)
   {
      uint64_t made = 0;
      ArrangeEdit([&]()
      {
         const Arrange::Lane* lane = Arrange::FindLane(gArrange, laneId);
         if (lane == nullptr || lane->type != Arrange::kLaneMidi)
            return;
         Arrange::Clip c;
         c.start = std::max<Arrange::Tick>(0, atTick);
         c.length = Arrange::kTicksPerBar;
         Arrange::PlaceOverwrite(gArrange, laneId, c, &made);
      });
      if (made != 0)
      {
         gArrangeSel = { made };
         gArrangeSelAnchor = made;
      }
      return made;
   }


   void ArrangeDrawAddMidiTrackMenu(int insertAfter, uint64_t groupId)
   {
      if (!ImGui::BeginMenu(L("Add MIDI Track")))
         return;
      auto add = [&](uint64_t instUid)
      {
         uint64_t newId = 0;
         ArrangeEdit([&]()
         {
            const int at = (insertAfter < 0 || insertAfter >= (int)gArrange.lanes.size()) ? -1 : insertAfter + 1;
            newId = Arrange::AddLane(gArrange, Arrange::kLaneMidi, at);
            if (Arrange::Lane* l = Arrange::FindLane(gArrange, newId))
            {
               GraphNode* gn = FindNodeByUid(instUid);
               l->instrumentUid = instUid;
               l->name = FreshMidiLaneName(gArrange, gn != nullptr ? ArrangeSourceTitle(*gn) : std::string());
               l->groupId = groupId;
            }
         });
         if (newId != 0)
         {
            gArrangeRowSel.clear();
            gArrangeRowSel.insert(newId);
            gArrangeRowSelAnchor = newId;
         }
      };
      const std::vector<int> insts = ArrangeNoteInstrumentNodes();
      if (insts.empty())
         ImGui::TextDisabled("%s", T("No node with a note input on the canvas"));
      for (int idx : insts)
      {
         GraphNode* gn = FindNodeByIndex(idx);
         if (gn == nullptr)
            continue;
         ImGui::PushID(idx);
         if (ImGui::MenuItem(NodeTitleWithInstance(*gn).c_str()))
            add(gn->uid);
         ImGui::PopID();
      }
      ImGui::Separator();
      if (ImGui::MenuItem(L("No instrument yet")))
         add(0);
      ImGui::EndMenu();
   }


   void ArrangeDrawMidiLaneInspector(uint64_t laneId, float fieldW)
   {
      const Arrange::Lane* lane = Arrange::FindLane(gArrange, laneId);
      if (lane == nullptr || lane->type != Arrange::kLaneMidi)
         return;
      ImGui::TextDisabled("%s", T("Instrument"));
      const uint64_t cur = lane->instrumentUid;
      const std::string label = ArrangeMidiInstrumentLabel(*lane);
      uint64_t pick = cur;
      bool picked = false;
      ImGui::SetNextItemWidth(fieldW);
      PushDropdownStyle();
      if (ImGui::BeginCombo("##midilaneinst", label.c_str()))
      {
         if (ImGui::Selectable(T("(no instrument)"), cur == 0))
         {
            pick = 0;
            picked = true;
         }
         for (int idx : ArrangeNoteInstrumentNodes())
         {
            GraphNode* gn = FindNodeByIndex(idx);
            if (gn == nullptr)
               continue;
            ImGui::PushID(idx);
            if (ImGui::Selectable(NodeTitleWithInstance(*gn).c_str(), gn->uid == cur))
            {
               pick = gn->uid;
               picked = true;
            }
            ImGui::PopID();
         }
         ImGui::EndCombo();
      }
      PopDropdownStyle();
      if (cur != 0 && FindNodeByUid(cur) == nullptr)
         ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s", T("The instrument was deleted - pick another."));
      int notes = 0;
      for (const Arrange::Clip& c : lane->clips)
         notes += (int)c.notes.size();
      ImGui::TextDisabled(T("%d clips, %d notes"), (int)lane->clips.size(), notes);
      if (ImGui::Button(L("Add Clip at Playhead##midilaneaddclip"), ImVec2(-FLT_MIN, 0)))
      {
         const Arrange::Tick grid = ArrangeSnapGridTicks();
         const Arrange::Tick at = grid > 0 ? Arrange::GridFloor(ArrangePlayTick(), grid) : ArrangePlayTick();
         ArrangeAddEmptyMidiClip(laneId, at);
      }
      if (picked && pick != cur)
      {
         ArrangeEdit([&]()
         {
            if (Arrange::Lane* l = Arrange::FindLane(gArrange, laneId))
            {
               l->instrumentUid = pick;
               gArrange.revision++;
            }
         });
      }
   }


   void ArrangeDrawMidiClipInspector(uint64_t clipId, float fieldW)
   {
      (void)fieldW;
      const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
      if (!loc.Valid())
         return;
      const Arrange::Lane& lane = gArrange.lanes[(size_t)loc.lane];
      const Arrange::Clip& clip = lane.clips[(size_t)loc.index];
      ImGui::TextDisabled("%s", T("Notes"));
      int lo = 127, hi = -1;
      for (const Arrange::MidiNote& n : clip.notes)
      {
         lo = std::min(lo, n.note);
         hi = std::max(hi, n.note);
      }
      if (hi >= 0)
         ImGui::Text(T("%d notes, %s to %s"), (int)clip.notes.size(), MidiNoteName(lo).c_str(), MidiNoteName(hi).c_str());
      else
         ImGui::TextDisabled("%s", T("No notes yet - double-click the clip to draw some."));
      ImGui::TextDisabled(T("Plays: %s"), ArrangeMidiInstrumentLabel(lane).c_str());
      if (ImGui::Button(L("Open Piano Roll##midiclipopen"), ImVec2(-FLT_MIN, 0)))
         OpenArrangePianoRoll(clipId);
      // Transpose the whole clip, one undo step per press.
      ImGui::TextDisabled("%s", T("Transpose"));
      const int steps[4] = { -12, -1, 1, 12 };
      const char* labels[4] = { "-12##tr0", "-1##tr1", "+1##tr2", "+12##tr3" };
      const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
      for (int i = 0; i < 4; i++)
      {
         if (i > 0)
            ImGui::SameLine();
         if (ImGui::Button(labels[i], ImVec2(std::max(24.0f, bw), 0)))
         {
            const int d = steps[i];
            ArrangeEdit([&]()
            {
               Arrange::Clip* c = Arrange::FindClip(gArrange, clipId);
               if (c == nullptr || c->notes.empty())
                  return;
               int mn = 127, mx = 0;
               for (const Arrange::MidiNote& n : c->notes)
               {
                  mn = std::min(mn, n.note);
                  mx = std::max(mx, n.note);
               }
               if (mn + d < 0 || mx + d > 127)
                  return; // never squash notes against the range ends
               for (Arrange::MidiNote& n : c->notes)
                  n.note += d;
               gArrange.revision++;
            });
         }
      }
   }


   // ---- piano roll window ------------------------------------------------------

   void OpenArrangePianoRoll(uint64_t clipId)
   {
      const Arrange::Loc loc = Arrange::Find(gArrange, clipId);
      if (!loc.Valid() || gArrange.lanes[(size_t)loc.lane].type != Arrange::kLaneMidi)
         return;
      if (gRoll.mode != kRollNone)
         EndGesture();
      if (gRoll.clipId != clipId || !gRoll.open)
      {
         gRoll.sel.clear();
         const Arrange::Clip& c = gArrange.lanes[(size_t)loc.lane].clips[(size_t)loc.index];
         int hi = -1;
         int lo = 128;
         for (const Arrange::MidiNote& n : c.notes)
         {
            hi = std::max(hi, n.note);
            lo = std::min(lo, n.note);
         }
         gRoll.topNote = hi >= 0 ? (float)std::min(127, std::max(hi + 6, lo + 24)) : 84.0f;
         gRoll.scrollBeat = 0.0;
         gRoll.fitRequest = true;
      }
      gRoll.clipId = clipId;
      gRoll.open = true;
      gRoll.focusRequest = true;
   }


   bool ArrangePianoRollOwnsKeys() { return gRollOwnsKeys; }


   bool ArrangePianoRollGestureOpen()
   {
      return gRoll.open && (gRoll.mode == kRollMove || gRoll.mode == kRollResize || gRoll.mode == kRollCreate ||
                            gRoll.mode == kRollVelocity);
   }


   void DrawArrangePianoRollWindow()
   {
      if (!gRoll.open)
      {
         gRollOwnsKeys = false;
         return;
      }
      const Arrange::Loc loc = Arrange::Find(gArrange, gRoll.clipId);
      if (!loc.Valid() || gArrange.lanes[(size_t)loc.lane].type != Arrange::kLaneMidi)
      {
         // The clip went away (deleted, undone): the editor goes with it.
         if (gRoll.mode != kRollNone)
            EndGesture();
         gRoll.open = false;
         gRollOwnsKeys = false;
         return;
      }
      const uint64_t clipId = gRoll.clipId;
      const Arrange::Lane& laneRef = gArrange.lanes[(size_t)loc.lane];
      const uint64_t instUid = laneRef.instrumentUid;
      const std::string instLabel = ArrangeMidiInstrumentLabel(laneRef);
      const Arrange::Clip clip = laneRef.clips[(size_t)loc.index]; // a copy: edits below go through ids
      const Arrange::Tick clipStart = clip.start;
      const double clipBeats = Arrange::TicksToBeats(clip.length);
      if (gRoll.sel.size() != clip.notes.size())
         gRoll.sel.assign(clip.notes.size(), 0); // changed underneath (undo, the API): selection drops

      const bool light = IsThemeLight();
      ImGui::SetNextWindowSize(ImVec2(900.0f, 480.0f), ImGuiCond_FirstUseEver);
      if (gRoll.focusRequest)
      {
         ImGui::SetNextWindowFocus();
         gRoll.focusRequest = false;
      }
      char title[200];
      snprintf(title, sizeof(title), "%s - %s###ArrangePianoRoll", T("Piano Roll"),
               clip.name.empty() ? laneRef.name.c_str() : clip.name.c_str());
      bool open = true;
      PushElevatedPanelStyle(/*isChild=*/false);
      const bool visible = ImGui::Begin(title, &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                         ImGuiWindowFlags_NoCollapse);
      if (!visible)
      {
         ImGui::End();
         PopElevatedPanelStyle();
         gRollOwnsKeys = false;
         if (!open)
         {
            if (gRoll.mode != kRollNone)
               EndGesture();
            gRoll.open = false;
         }
         return;
      }

      const bool readOnly = ArrangeRenderBusy();
      ImGuiIO& io = ImGui::GetIO();

      // ---- toolbar ----
      ImGui::TextDisabled(T("Plays: %s"), instLabel.c_str());
      ImGui::SameLine(0.0f, 16.0f);
      ImGui::TextUnformatted(T("Grid"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(84.0f);
      PushDropdownStyle();
      const std::vector<std::string>& grids = MusicTime::QuantizeGridList();
      if (ImGui::BeginCombo("##prgrid", grids[(size_t)std::clamp(gRoll.gridIndex, 0, (int)grids.size() - 1)].c_str()))
      {
         for (int i = 0; i < (int)grids.size(); i++)
            if (ImGui::Selectable(grids[(size_t)i].c_str(), gRoll.gridIndex == i))
               gRoll.gridIndex = i;
         ImGui::EndCombo();
      }
      PopDropdownStyle();
      const Arrange::Tick grid = GridTicks();
      auto snapRel = [&](Arrange::Tick rel) -> Arrange::Tick
      {
         // Grid lines are the timeline's (absolute) ones, not the clip's.
         if (grid <= 0 || io.KeyAlt)
            return rel;
         return Arrange::SnapToGrid(clipStart + rel, grid) - clipStart;
      };
      ImGui::SameLine(0.0f, 12.0f);
      ImGui::BeginDisabled(readOnly || clip.notes.empty() || grid <= 0);
      if (ImGui::Button(L("Quantize##prq")))
      {
         // Selected notes, or every note when none is selected: starts to the
         // nearest grid line, lengths kept.
         bool anySel = false;
         for (char s : gRoll.sel)
            anySel = anySel || s != 0;
         std::vector<Arrange::MidiNote> v = clip.notes;
         for (size_t i = 0; i < v.size(); i++)
            if (!anySel || gRoll.sel[i])
               v[i].start = std::max<Arrange::Tick>(0, Arrange::SnapToGrid(clipStart + v[i].start, grid) - clipStart);
         ArrangeEdit([&]() { WriteNotes(clipId, v, gRoll.sel); });
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("%s", T("Snap the selected notes (all when none is selected) to the grid"));
      ImGui::SameLine(0.0f, 12.0f);
      if (ImGui::Button("-##przoomout")) // i18n-ok
         gRoll.ppb = std::max(8.0f, gRoll.ppb / 1.25f);
      ImGui::SameLine();
      if (ImGui::Button("+##przoomin")) // i18n-ok
         gRoll.ppb = std::min(800.0f, gRoll.ppb * 1.25f);
      ImGui::SameLine();
      if (ImGui::Button(L("Fit##przoomfit")))
         gRoll.fitRequest = true;
      ImGui::SameLine(0.0f, 12.0f);
      {
         int nsel = 0;
         for (char s : gRoll.sel)
            nsel += s != 0;
         ImGui::TextDisabled(T("%d notes, %d selected"), (int)clip.notes.size(), nsel);
      }

      // ---- layout ----
      const float kKeyW = 52.0f, kRulerH = 18.0f, kVelH = 56.0f, kGap = 4.0f;
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      if (avail.x < kKeyW + 60.0f || avail.y < kRulerH + kVelH + 60.0f)
      {
         ImGui::End();
         PopElevatedPanelStyle();
         gRollOwnsKeys = false;
         return;
      }
      const ImVec2 gridMin(p0.x + kKeyW, p0.y + kRulerH);
      const ImVec2 gridMax(p0.x + avail.x, p0.y + avail.y - kVelH - kGap);
      const ImVec2 velMin(gridMin.x, gridMax.y + kGap);
      const ImVec2 velMax(gridMax.x, p0.y + avail.y);
      const float gridW = gridMax.x - gridMin.x;
      const float gridH = gridMax.y - gridMin.y;
      if (gRoll.fitRequest)
      {
         gRoll.ppb = std::clamp((gridW - 24.0f) / (float)std::max(0.25, clipBeats), 8.0f, 800.0f);
         gRoll.scrollBeat = 0.0;
         gRoll.fitRequest = false;
      }
      gRoll.topNote = std::clamp(gRoll.topNote, gridH / gRoll.keyH - 1.0f, 127.0f);

      auto beatToX = [&](double b) { return gridMin.x + (float)((b - gRoll.scrollBeat) * gRoll.ppb); };
      auto xToTick = [&](float x) { return Arrange::BeatsToTicks(gRoll.scrollBeat + (double)(x - gridMin.x) / gRoll.ppb); };
      auto tickToX = [&](Arrange::Tick t) { return beatToX(Arrange::TicksToBeats(t)); };
      auto noteToY = [&](int n) { return gridMin.y + (gRoll.topNote - (float)n) * gRoll.keyH; };
      auto yToNote = [&](float y) { return std::clamp((int)std::ceil(gRoll.topNote - (y - gridMin.y) / gRoll.keyH), 0, 127); };

      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 rowWhite = light ? IM_COL32(246, 247, 250, 255) : IM_COL32(30, 31, 36, 255);
      const ImU32 rowBlack = light ? IM_COL32(232, 234, 240, 255) : IM_COL32(24, 25, 29, 255);
      const ImU32 lineFaint = light ? IM_COL32(0, 0, 0, 18) : IM_COL32(255, 255, 255, 14);
      const ImU32 lineBeat = light ? IM_COL32(0, 0, 0, 40) : IM_COL32(255, 255, 255, 32);
      const ImU32 lineBar = light ? IM_COL32(0, 0, 0, 85) : IM_COL32(255, 255, 255, 70);
      const ImU32 textCol = light ? IM_COL32(40, 44, 56, 255) : IM_COL32(205, 208, 220, 255);
      const ImU32 noteFill = IM_COL32(59, 130, 246, 255);   // the MIDI lane accent
      const ImU32 noteSelFill = IM_COL32(250, 204, 21, 255); // the timeline's selection gold

      // ---- input: wheel ----
      const ImVec2 mouse = io.MousePos;
      const bool overCanvas = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && mouse.x >= p0.x &&
                              mouse.x < p0.x + avail.x && mouse.y >= p0.y && mouse.y < p0.y + avail.y;
      if (overCanvas && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
      {
         if (io.KeyCtrl || io.KeySuper)
         {
            const double atBeat = gRoll.scrollBeat + (double)(mouse.x - gridMin.x) / gRoll.ppb;
            gRoll.ppb = std::clamp(gRoll.ppb * (io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f), 8.0f, 800.0f);
            gRoll.scrollBeat = atBeat - (double)(mouse.x - gridMin.x) / gRoll.ppb;
         }
         else if (io.KeyAlt)
         {
            gRoll.keyH = std::clamp(gRoll.keyH + (io.MouseWheel > 0.0f ? 1.0f : -1.0f), 6.0f, 28.0f);
         }
         else if (io.KeyShift || io.MouseWheelH != 0.0f)
         {
            const float w = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
            gRoll.scrollBeat -= (double)w * 40.0 / gRoll.ppb;
         }
         else
         {
            gRoll.topNote += io.MouseWheel * 3.0f;
         }
         gRoll.scrollBeat = std::clamp(gRoll.scrollBeat, -1.0, std::max(0.0, clipBeats + 64.0));
         gRoll.topNote = std::clamp(gRoll.topNote, gridH / gRoll.keyH - 1.0f, 127.0f);
      }

      // ---- background: pitch rows, grid lines, past-the-end shade ----
      dl->PushClipRect(gridMin, gridMax, true);
      const int topRow = std::min(127, (int)std::ceil(gRoll.topNote));
      const int bottomRow = std::max(0, (int)std::floor(gRoll.topNote - gridH / gRoll.keyH) - 1);
      for (int n = topRow; n >= bottomRow; n--)
      {
         const float y = noteToY(n);
         dl->AddRectFilled(ImVec2(gridMin.x, y), ImVec2(gridMax.x, y + gRoll.keyH), IsBlackKey(n) ? rowBlack : rowWhite);
         if (n % 12 == 0)
            dl->AddLine(ImVec2(gridMin.x, y + gRoll.keyH), ImVec2(gridMax.x, y + gRoll.keyH), lineBeat, 1.0f);
      }
      {
         const double viewB0 = gRoll.scrollBeat;
         const double viewB1 = gRoll.scrollBeat + (double)gridW / gRoll.ppb;
         const double absB0 = Arrange::TicksToBeats(clipStart) + viewB0;
         const double absB1 = Arrange::TicksToBeats(clipStart) + viewB1;
         const double gridBeats = grid > 0 ? Arrange::TicksToBeats(grid) : 0.25;
         const double step = (gridBeats * gRoll.ppb < 6.0) ? 1.0 : gridBeats; // too dense: beats only
         const double barBeats = Arrange::TicksToBeats(Arrange::kTicksPerBar);
         for (double b = std::floor(absB0 / step) * step; b <= absB1 + 1e-9; b += step)
         {
            const float x = beatToX(b - Arrange::TicksToBeats(clipStart));
            const bool isBar = std::fabs(std::remainder(b, barBeats)) < 1e-6;
            const bool isBeat = std::fabs(std::remainder(b, 1.0)) < 1e-6;
            dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), isBar ? lineBar : (isBeat ? lineBeat : lineFaint), 1.0f);
         }
         const float endX = beatToX(clipBeats);
         if (endX < gridMax.x)
            dl->AddRectFilled(ImVec2(std::max(endX, gridMin.x), gridMin.y), gridMax, light ? IM_COL32(0, 0, 0, 30) : IM_COL32(0, 0, 0, 90));
         dl->AddLine(ImVec2(endX, gridMin.y), ImVec2(endX, gridMax.y), noteFill, 1.5f);
      }

      // ---- notes ----
      auto noteRect = [&](const Arrange::MidiNote& n, ImVec2& a, ImVec2& b)
      {
         a = ImVec2(tickToX(n.start), noteToY(n.note));
         b = ImVec2(std::max(tickToX(n.start + n.length), a.x + 3.0f), a.y + gRoll.keyH);
      };
      for (size_t i = 0; i < clip.notes.size(); i++)
      {
         const Arrange::MidiNote& n = clip.notes[i];
         ImVec2 a, b;
         noteRect(n, a, b);
         if (b.x < gridMin.x || a.x > gridMax.x || b.y < gridMin.y || a.y > gridMax.y)
            continue;
         const bool selected = i < gRoll.sel.size() && gRoll.sel[i] != 0;
         const bool pastEnd = n.start >= clip.length;
         ImVec4 fc = ImGui::ColorConvertU32ToFloat4(selected ? noteSelFill : noteFill);
         fc.w = pastEnd ? 0.25f : (0.45f + 0.55f * n.velocity); // velocity reads as strength
         dl->AddRectFilled(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), ImGui::ColorConvertFloat4ToU32(fc), 2.0f);
         dl->AddRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f),
                     selected ? IM_COL32(255, 255, 255, 230) : IM_COL32(0, 0, 0, light ? 90 : 160), 2.0f, 0, 1.0f);
         if (b.x - a.x > 28.0f && gRoll.keyH >= 11.0f)
            dl->AddText(ImVec2(a.x + 3.0f, a.y + (gRoll.keyH - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 230),
                        MidiNoteName(n.note).c_str());
      }

      // ---- playhead ----
      {
         const double play = Transport::Instance().Beats() - Arrange::TicksToBeats(clipStart);
         const float x = beatToX(play);
         if (x >= gridMin.x && x <= gridMax.x)
            dl->AddLine(ImVec2(x, gridMin.y), ImVec2(x, gridMax.y), IM_COL32(239, 68, 68, 230), 1.5f);
      }

      // ---- grid interaction ----
      auto hitNote = [&](ImVec2 m, bool& onEnd) -> int
      {
         onEnd = false;
         for (int i = (int)clip.notes.size() - 1; i >= 0; i--)
         {
            ImVec2 a, b;
            noteRect(clip.notes[(size_t)i], a, b);
            if (m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y)
            {
               onEnd = m.x >= b.x - std::min(6.0f, (b.x - a.x) * 0.35f);
               return i;
            }
         }
         return -1;
      };
      ImGui::SetCursorScreenPos(gridMin);
      ImGui::InvisibleButton("##prgridbtn", ImVec2(gridW, gridH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      const bool gridHovered = ImGui::IsItemHovered();
      const bool gridActivated = ImGui::IsItemActivated();
      const bool gridActive = ImGui::IsItemActive();
      const bool gridReleased = ImGui::IsItemDeactivated();
      bool hoverEnd = false;
      const int hoverNote = gridHovered ? hitNote(mouse, hoverEnd) : -1;
      if (gridHovered && gRoll.mode == kRollNone && hoverNote >= 0)
         ImGui::SetMouseCursor(hoverEnd ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Hand);
      if (gRoll.mode == kRollResize || gRoll.mode == kRollCreate)
         ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

      if (!readOnly && gridActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
      {
         if (hoverNote >= 0)
         {
            std::vector<char> which(clip.notes.size(), 0);
            which[(size_t)hoverNote] = 1;
            DeleteNotes(clipId, which);
         }
      }
      else if (!readOnly && gridActivated && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      {
         const bool additive = io.KeyShift || io.KeyCtrl || io.KeySuper;
         gRoll.pressPos = mouse;
         gRoll.pressY = mouse.y;
         gRoll.pressTick = xToTick(mouse.x);
         gRoll.dragged = false;
         if (hoverNote >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !additive)
         {
            std::vector<char> which(clip.notes.size(), 0);
            which[(size_t)hoverNote] = 1;
            DeleteNotes(clipId, which);
         }
         else if (hoverNote >= 0 && additive)
         {
            gRoll.sel[(size_t)hoverNote] = gRoll.sel[(size_t)hoverNote] ? 0 : 1;
         }
         else if (hoverNote >= 0)
         {
            if (!gRoll.sel[(size_t)hoverNote])
            {
               gRoll.sel.assign(clip.notes.size(), 0);
               gRoll.sel[(size_t)hoverNote] = 1;
            }
            ArrangeGestureBegin();
            gRoll.origNotes = clip.notes;
            gRoll.origSel = gRoll.sel;
            gRoll.grab = hoverNote;
            gRoll.mode = hoverEnd ? kRollResize : kRollMove;
            gRoll.lastVelocity = clip.notes[(size_t)hoverNote].velocity;
            AuditionStart(instUid, clip.notes[(size_t)hoverNote].note, clip.notes[(size_t)hoverNote].velocity);
         }
         else if (additive)
         {
            gRoll.mode = kRollMarquee;
            gRoll.origSel = gRoll.sel;
         }
         else
         {
            // A new note: on the grid line at/left of the mouse, one grid
            // step long (a 1/16 when the grid is off), the last velocity used.
            const Arrange::Tick rel = std::max<Arrange::Tick>(0, xToTick(mouse.x));
            const Arrange::Tick g = grid > 0 ? grid : Arrange::kPPQ / 4;
            Arrange::Tick start = rel;
            if (grid > 0 && !io.KeyAlt)
               start = std::max<Arrange::Tick>(0, Arrange::GridFloor(clipStart + rel, grid) - clipStart);
            Arrange::MidiNote nn;
            nn.start = start;
            nn.length = g;
            nn.note = yToNote(mouse.y);
            nn.velocity = gRoll.lastVelocity;
            ArrangeGestureBegin();
            std::vector<Arrange::MidiNote> v = clip.notes;
            std::vector<char> s(clip.notes.size(), 0);
            v.push_back(nn);
            s.push_back(1);
            WriteNotes(clipId, v, s);
            // Find the new note again (sorted in) as the grabbed one.
            gRoll.origNotes.clear();
            gRoll.origSel = gRoll.sel;
            if (const Arrange::Clip* c = Arrange::FindClip(gArrange, clipId))
            {
               gRoll.origNotes = c->notes;
               for (size_t i = 0; i < gRoll.sel.size(); i++)
                  if (gRoll.sel[i])
                     gRoll.grab = (int)i;
            }
            gRoll.pressTick = gRoll.origNotes.empty() || gRoll.grab < 0
                                 ? rel
                                 : gRoll.origNotes[(size_t)gRoll.grab].start + gRoll.origNotes[(size_t)gRoll.grab].length;
            gRoll.mode = kRollCreate;
            AuditionStart(instUid, nn.note, nn.velocity);
         }
      }

      if (gridActive && gRoll.mode != kRollNone && gRoll.mode != kRollVelocity && gRoll.mode != kRollKeys)
      {
         const float ddx = mouse.x - gRoll.pressPos.x, ddy = mouse.y - gRoll.pressPos.y;
         if (ddx * ddx + ddy * ddy > 9.0f)
            gRoll.dragged = true;
         const Arrange::Tick raw = xToTick(mouse.x) - gRoll.pressTick;
         if ((gRoll.mode == kRollMove || gRoll.mode == kRollResize || gRoll.mode == kRollCreate) && gRoll.grab >= 0 &&
             gRoll.grab < (int)gRoll.origNotes.size())
         {
            const Arrange::MidiNote& g = gRoll.origNotes[(size_t)gRoll.grab];
            std::vector<Arrange::MidiNote> v = gRoll.origNotes;
            if (gRoll.mode == kRollMove)
            {
               Arrange::Tick dt = gRoll.dragged ? snapRel(g.start + raw) - g.start : 0;
               int dn = gRoll.dragged ? (int)std::lround((gRoll.pressY - mouse.y) / gRoll.keyH) : 0;
               Arrange::Tick minStart = Arrange::kMaxTick;
               int minNote = 127, maxNote = 0;
               for (size_t i = 0; i < v.size(); i++)
                  if (gRoll.origSel[i])
                  {
                     minStart = std::min(minStart, v[i].start);
                     minNote = std::min(minNote, v[i].note);
                     maxNote = std::max(maxNote, v[i].note);
                  }
               dt = std::max(dt, -minStart);
               dn = std::clamp(dn, -minNote, 127 - maxNote);
               for (size_t i = 0; i < v.size(); i++)
                  if (gRoll.origSel[i])
                  {
                     v[i].start += dt;
                     v[i].note += dn;
                  }
               AuditionStart(instUid, g.note + dn, g.velocity);
            }
            else
            {
               // Resize / create: the grabbed note's end goes to the grid
               // line nearest the mouse; every selected note gets the same
               // change, never below one tick.
               const Arrange::Tick end = g.start + g.length;
               const Arrange::Tick dt = gRoll.mode == kRollCreate
                                           ? snapRel(std::max<Arrange::Tick>(g.start + 1, end + raw)) - end
                                           : snapRel(end + raw) - end;
               for (size_t i = 0; i < v.size(); i++)
                  if (gRoll.origSel[i])
                  {
                     Arrange::Tick len = v[i].length + dt;
                     if (len < 1)
                        len = grid > 0 ? std::min(grid, v[i].length) : 1;
                     v[i].length = len;
                  }
            }
            WriteNotes(clipId, v, gRoll.origSel);
         }
         else if (gRoll.mode == kRollMarquee)
         {
            const ImVec2 a(std::min(gRoll.pressPos.x, mouse.x), std::min(gRoll.pressPos.y, mouse.y));
            const ImVec2 b(std::max(gRoll.pressPos.x, mouse.x), std::max(gRoll.pressPos.y, mouse.y));
            gRoll.sel = gRoll.origSel;
            gRoll.sel.resize(clip.notes.size(), 0);
            for (size_t i = 0; i < clip.notes.size(); i++)
            {
               ImVec2 na, nb;
               noteRect(clip.notes[i], na, nb);
               if (nb.x >= a.x && na.x <= b.x && nb.y >= a.y && na.y <= b.y)
                  gRoll.sel[i] = 1;
            }
            dl->AddRectFilled(a, b, IM_COL32(250, 204, 21, 30));
            dl->AddRect(a, b, IM_COL32(250, 204, 21, 200), 0.0f, 0, 1.0f);
         }
      }
      if (gridReleased && gRoll.mode != kRollNone && gRoll.mode != kRollVelocity && gRoll.mode != kRollKeys)
      {
         // A plain click on a note of a bigger selection narrows it to that note.
         if (gRoll.mode == kRollMove && !gRoll.dragged && gRoll.grab >= 0 && gRoll.grab < (int)gRoll.sel.size())
         {
            const uint64_t grabbedKeyStart = (uint64_t)gRoll.origNotes[(size_t)gRoll.grab].start;
            const int grabbedNote = gRoll.origNotes[(size_t)gRoll.grab].note;
            if (const Arrange::Clip* c = Arrange::FindClip(gArrange, clipId))
            {
               gRoll.sel.assign(c->notes.size(), 0);
               for (size_t i = 0; i < c->notes.size(); i++)
                  if ((uint64_t)c->notes[i].start == grabbedKeyStart && c->notes[i].note == grabbedNote)
                  {
                     gRoll.sel[i] = 1;
                     break;
                  }
            }
         }
         EndGesture();
      }
      dl->PopClipRect();

      // ---- ruler (absolute bars of the timeline) ----
      {
         const ImVec2 ra(gridMin.x, p0.y), rb(gridMax.x, gridMin.y);
         dl->AddRectFilled(ra, rb, light ? IM_COL32(226, 228, 234, 255) : IM_COL32(20, 21, 25, 255));
         dl->PushClipRect(ra, rb, true);
         const double barBeats = Arrange::TicksToBeats(Arrange::kTicksPerBar);
         const double absB0 = Arrange::TicksToBeats(clipStart) + gRoll.scrollBeat;
         const double absB1 = absB0 + (double)gridW / gRoll.ppb;
         const double step = barBeats * gRoll.ppb < 36.0 ? barBeats * 4.0 : barBeats;
         for (double b = std::floor(absB0 / step) * step; b <= absB1 + 1e-9; b += step)
         {
            const float x = beatToX(b - Arrange::TicksToBeats(clipStart));
            dl->AddLine(ImVec2(x, ra.y + 6.0f), ImVec2(x, rb.y), lineBar, 1.0f);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", (int)std::lround(b / barBeats) + 1);
            dl->AddText(ImVec2(x + 3.0f, ra.y + 2.0f), textCol, buf);
         }
         dl->PopClipRect();
      }

      // ---- keys column (click to audition) ----
      {
         const ImVec2 ka(p0.x, gridMin.y), kb(gridMin.x - 1.0f, gridMax.y);
         dl->PushClipRect(ka, kb, true);
         for (int n = topRow; n >= bottomRow; n--)
         {
            const float y = noteToY(n);
            const bool black = IsBlackKey(n);
            const ImU32 kc = black ? (light ? IM_COL32(40, 42, 48, 255) : IM_COL32(14, 14, 17, 255))
                                   : (light ? IM_COL32(252, 252, 253, 255) : IM_COL32(200, 202, 210, 255));
            const float w = black ? (kb.x - ka.x) * 0.62f : (kb.x - ka.x);
            dl->AddRectFilled(ImVec2(ka.x, y), ImVec2(ka.x + w, y + gRoll.keyH), kc);
            dl->AddLine(ImVec2(ka.x, y + gRoll.keyH), ImVec2(kb.x, y + gRoll.keyH), IM_COL32(0, 0, 0, 60), 1.0f);
            if (gRoll.auditionNote == n)
               dl->AddRectFilled(ImVec2(ka.x, y), ImVec2(ka.x + w, y + gRoll.keyH), IM_COL32(59, 130, 246, 160));
            if (n % 12 == 0 && gRoll.keyH >= 9.0f)
               dl->AddText(ImVec2(kb.x - ImGui::CalcTextSize("C-1").x - 2.0f, y + (gRoll.keyH - ImGui::GetFontSize()) * 0.5f),
                           IM_COL32(60, 64, 76, 255), MidiNoteName(n).c_str());
         }
         dl->PopClipRect();
         ImGui::SetCursorScreenPos(ka);
         ImGui::InvisibleButton("##prkeysbtn", ImVec2(kb.x - ka.x, kb.y - ka.y));
         if (ImGui::IsItemActive())
         {
            if (gRoll.mode == kRollNone)
               gRoll.mode = kRollKeys;
            if (gRoll.mode == kRollKeys)
               AuditionStart(instUid, yToNote(mouse.y), gRoll.lastVelocity);
         }
         if (ImGui::IsItemDeactivated() && gRoll.mode == kRollKeys)
         {
            gRoll.mode = kRollNone;
            AuditionStop();
         }
         if (ImGui::IsItemHovered() && instUid == 0)
            ImGui::SetTooltip("%s", T("Pick the track's instrument to hear notes"));
      }

      // ---- velocity lane ----
      {
         dl->AddRectFilled(velMin, velMax, light ? IM_COL32(236, 238, 243, 255) : IM_COL32(22, 23, 27, 255));
         dl->PushClipRect(velMin, velMax, true);
         const float h = velMax.y - velMin.y - 4.0f;
         for (size_t i = 0; i < clip.notes.size(); i++)
         {
            const Arrange::MidiNote& n = clip.notes[i];
            const float x = tickToX(n.start);
            if (x < velMin.x - 4.0f || x > velMax.x + 4.0f)
               continue;
            const bool selected = i < gRoll.sel.size() && gRoll.sel[i] != 0;
            const float y = velMax.y - 2.0f - h * n.velocity;
            const ImU32 c = selected ? noteSelFill : noteFill;
            dl->AddLine(ImVec2(x + 1.0f, velMax.y - 2.0f), ImVec2(x + 1.0f, y), c, 2.0f);
            dl->AddCircleFilled(ImVec2(x + 1.0f, y), 3.0f, c);
         }
         dl->AddText(ImVec2(p0.x + 4.0f, velMin.y + 2.0f), textCol, T("vel"));
         dl->PopClipRect();
         ImGui::SetCursorScreenPos(velMin);
         ImGui::InvisibleButton("##prvelbtn", ImVec2(velMax.x - velMin.x, velMax.y - velMin.y));
         const float velAt = std::clamp((velMax.y - 2.0f - mouse.y) / std::max(1.0f, h), 0.0f, 1.0f);
         if (!readOnly && ImGui::IsItemActivated())
         {
            // The note whose stem is nearest the mouse (within 8 px).
            int best = -1;
            float bestD = 8.0f;
            for (size_t i = 0; i < clip.notes.size(); i++)
            {
               const float d = std::fabs(tickToX(clip.notes[i].start) + 1.0f - mouse.x);
               if (d <= bestD)
               {
                  bestD = d;
                  best = (int)i;
               }
            }
            if (best >= 0)
            {
               ArrangeGestureBegin();
               gRoll.mode = kRollVelocity;
               gRoll.grab = best;
               gRoll.origNotes = clip.notes;
               gRoll.origSel = gRoll.sel;
            }
         }
         if (ImGui::IsItemActive() && gRoll.mode == kRollVelocity && gRoll.grab >= 0 && gRoll.grab < (int)gRoll.origNotes.size())
         {
            std::vector<Arrange::MidiNote> v = gRoll.origNotes;
            const bool grabSel = gRoll.grab < (int)gRoll.origSel.size() && gRoll.origSel[(size_t)gRoll.grab];
            for (size_t i = 0; i < v.size(); i++)
               if ((int)i == gRoll.grab || (grabSel && gRoll.origSel[i]))
                  v[i].velocity = velAt;
            gRoll.lastVelocity = velAt;
            WriteNotes(clipId, v, gRoll.origSel);
            ImGui::SetTooltip(T("velocity %d"), (int)std::lround(velAt * 127.0f));
         }
         if (ImGui::IsItemDeactivated() && gRoll.mode == kRollVelocity)
            EndGesture();
      }

      // ---- keys ----
      gRollOwnsKeys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
      if (gRollOwnsKeys && !readOnly && !io.WantTextInput && gRoll.mode == kRollNone)
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
         {
            bool any = false;
            for (char s : gRoll.sel)
               any = any || s != 0;
            if (any)
               DeleteNotes(clipId, gRoll.sel);
         }
         if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_A, false))
            gRoll.sel.assign(clip.notes.size(), 1);
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            gRoll.sel.assign(clip.notes.size(), 0);
      }

      ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + avail.y));
      ImGui::Dummy(ImVec2(0.0f, 0.0f));
      ImGui::End();
      PopElevatedPanelStyle();
      if (!open)
      {
         if (gRoll.mode != kRollNone)
            EndGesture();
         gRoll.open = false;
         gRollOwnsKeys = false;
      }
   }
}
