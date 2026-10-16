// Node-editor groups: explicit membership, mute / solo / lock, and the gates
// that make mute mean "silence what leaves the group".
//
// Membership is explicit and saved: a node joins only through Ctrl+G, the
// context menu's "Add to group" or the API below, never by being dragged over
// a box. The live list is gGroupMembers (keyed by node index); GroupNode::
// membersText carries it through every serialization as member uids.
//
// Mute does not stop the members - they keep cooking, so unmuting is instant
// and in time with the transport. What it closes is every cable whose source
// is a member and whose consumer is not:
//   audio      a per-pin / per-terminal gain in the audio topology, ramped over
//              kGroupGateRampSeconds on the audio thread (AudioEngine::
//              AdvanceGroupGates); member-to-member cables are untouched
//   notes      the consumer's NoteEventQueue cursor reads through a gate:
//              held notes get their note-off at the moment it closes, then
//              everything is dropped until it opens
//   images     ImageCable::SetGroupGate - the consumer sees a shared 1x1
//              transparent-black texture; the member still cooks
//   modulators a binding whose modulator is a member holds the destination at
//              its last value (the apply loop skips the write)
//   geometry   Render 3D (outside the group) does not draw geometry that comes
//              from the muted group, directly or through a non-member chain
// Solo is groups-only: while any group is soloed, every other group behaves
// as muted. The timeline (GroupTimelineWindows, src/arrange) gates a group
// that has clips to the inside of those clips; a stopped transport is closed.
//
// Lock refuses hand edits: see NodeEditLocked and its call sites.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "audio/AudioEngine.h"
#include "core/ImageCable.h"
#include "nodes/Geometry3DNodes.h"
#include "nodes/UtilityNodes.h"

namespace app
{
   namespace
   {
      // Gate slot per live group. A slot is what pins/terminals/cables name,
      // so it has to outlive any single frame or topology.
      std::map<GroupNode*, int> sSlotOf;
      GroupNode* sSlotOwner[kMaxGroupGates] = {};

      // Main thread, per slot: true while the group's outputs are closed
      // (muted, soloed out, or outside its timeline windows). ImageCables
      // point straight at these, so the array never moves.
      bool sOutClosed[kMaxGroupGates] = {};
      bool sAnyClosed = false;
      // Whether any ImageCable may still hold a gate pointer from an earlier
      // frame, so a patch whose last group just went away gets cleaned once.
      bool sCablesGated = false;

      // This frame's member maps (rebuilt by UpdateGroupGates).
      std::unordered_map<const INode*, int> sFrameSlotOf;
      std::unordered_map<int, int> sFrameSlotOfIndex;
      // Geometry: which closed-group slots each node's geometry derives from.
      std::unordered_map<const INode*, uint64_t> sGeoTaint;

      // Membership hash the last RebuildAudioTopology was built against, and
      // the slots its topology named (a slot newly named snaps to its target
      // instead of fading from a stale gain).
      uint64_t sTopologyHash = 0;
      uint64_t sPrevTopoSlots = 0;

      // Locked members pinned in place: uid -> canvas position.
      std::map<uint64_t, ImVec2> sLockedPos;

      uint64_t Bit(int slot) { return (slot >= 0 && slot < 64) ? (1ull << slot) : 0ull; }

      GraphNode* GraphNodeOfGroup(const GroupNode* g)
      {
         for (GraphNode& gn : gNodes)
            if (gn.node.get() == g)
               return &gn;
         return nullptr;
      }

      // Live groups, in gNodes order.
      std::vector<std::pair<GraphNode*, GroupNode*>> LiveGroups()
      {
         std::vector<std::pair<GraphNode*, GroupNode*>> out;
         for (GraphNode& gn : gNodes)
            if (auto* g = dynamic_cast<GroupNode*>(gn.node.get()))
               out.emplace_back(&gn, g);
         return out;
      }

      void ReleaseDeadSlots(const std::vector<std::pair<GraphNode*, GroupNode*>>& live)
      {
         for (auto it = sSlotOf.begin(); it != sSlotOf.end();)
         {
            bool alive = false;
            for (const auto& entry : live)
               alive = alive || entry.second == it->first;
            if (alive)
            {
               ++it;
               continue;
            }
            const int slot = it->second;
            sSlotOwner[slot] = nullptr;
            sOutClosed[slot] = false;
            GroupGate& gate = AudioEngine::Instance().GroupGateAt(slot);
            gate.target.store(1.0f, std::memory_order_relaxed);
            gate.notesOpen.store(true, std::memory_order_relaxed);
            it = sSlotOf.erase(it);
         }
      }

      // Group membership as (member node -> slot) and (member index -> slot),
      // plus a hash of it - what the audio topology's gate assignment depends on.
      uint64_t ComputeSlotMaps(std::unordered_map<const INode*, int>& byNode,
                               std::unordered_map<int, int>* byIndex)
      {
         byNode.clear();
         if (byIndex != nullptr)
            byIndex->clear();
         if (gGroupMembers.empty())
            return 0;
         std::unordered_map<int, GraphNode*> nodeByIndex;
         nodeByIndex.reserve(gNodes.size());
         for (GraphNode& gn : gNodes)
            nodeByIndex[gn.index] = &gn;
         uint64_t hash = 0;
         for (const auto& entry : gGroupMembers)
         {
            auto slotIt = sSlotOf.find(entry.first);
            if (slotIt == sSlotOf.end())
               continue;
            const int slot = slotIt->second;
            for (int index : entry.second)
            {
               auto nodeIt = nodeByIndex.find(index);
               if (nodeIt == nodeByIndex.end() || nodeIt->second->node == nullptr)
                  continue;
               const INode* n = nodeIt->second->node.get();
               byNode[n] = slot;
               if (byIndex != nullptr)
                  (*byIndex)[index] = slot;
               uint64_t h = (uint64_t)(uintptr_t)n * 0x9E3779B97F4A7C15ull;
               h ^= (uint64_t)(slot + 1) * 0xC2B2AE3D27D4EB4Full;
               hash += h ^ (h >> 29); // order-independent
            }
         }
         return hash;
      }

      int FrameSlotOf(const INode* n)
      {
         auto it = sFrameSlotOf.find(n);
         return it != sFrameSlotOf.end() ? it->second : -1;
      }

      void EnsureMutedTexture()
      {
         if (gImageCableMutedTexture != 0 || glfwGetCurrentContext() == nullptr)
            return;
         GLuint tex = 0;
         glGenTextures(1, &tex);
         glBindTexture(GL_TEXTURE_2D, tex);
         const unsigned char px[4] = { 0, 0, 0, 0 };
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
         glBindTexture(GL_TEXTURE_2D, 0);
         gImageCableMutedTexture = tex;
      }

      // Every image cable whose source (or a bypassed node it reads through)
      // is a member of a group its consumer is not in gets that group's gate.
      void GateImageCables()
      {
         bool any = false;
         for (GraphNode& gn : gNodes)
         {
            if (gn.node == nullptr)
               continue;
            const int mySlot = FrameSlotOf(gn.node.get());
            const int inputs = InputCountFor(gn);
            for (int slot = 0; slot < inputs; slot++)
            {
               ImageCable* cable = CableFor(gn, slot);
               if (cable == nullptr)
                  continue;
               const bool* gate = nullptr;
               INode* n = cable->GetSource();
               for (int hops = 0; n != nullptr && hops < 64; hops++)
               {
                  const int s = FrameSlotOf(n);
                  if (s >= 0 && s != mySlot)
                  {
                     gate = &sOutClosed[s];
                     break;
                  }
                  if (!n->bypassed)
                     break;
                  n = n->BypassSource();
               }
               cable->SetGroupGate(gate);
               any = any || gate != nullptr;
            }
         }
         sCablesGated = any;
      }

      void ClearImageCableGates()
      {
         for (GraphNode& gn : gNodes)
         {
            const int inputs = InputCountFor(gn);
            for (int slot = 0; slot < inputs; slot++)
               if (ImageCable* cable = CableFor(gn, slot))
                  cable->SetGroupGate(nullptr);
         }
         sCablesGated = false;
      }

      uint64_t GeometryTaintOf(INode* n, int depth, std::set<const INode*>& visiting)
      {
         if (n == nullptr || depth > 32)
            return 0;
         auto memo = sGeoTaint.find(n);
         if (memo != sGeoTaint.end())
            return memo->second;
         if (!visiting.insert(n).second)
            return 0; // a cycle - geometry cannot really loop, but never spin
         uint64_t taint = 0;
         const int self = FrameSlotOf(n);
         if (self >= 0 && sOutClosed[self])
            taint |= Bit(self);
         for (int slot = 0; slot < 16; slot++)
         {
            IGeometrySource** field = n->GeometryInputSlot(slot);
            if (field == nullptr || *field == nullptr)
               continue;
            taint |= GeometryTaintOf(dynamic_cast<INode*>(*field), depth + 1, visiting);
         }
         if (auto* asGeo = dynamic_cast<IGeometrySource*>(n))
            for (int c = 0; c < asGeo->GroupChildCount() && c < 64; c++)
               taint |= GeometryTaintOf(dynamic_cast<INode*>(asGeo->GroupChild(c)), depth + 1, visiting);
         visiting.erase(n);
         sGeoTaint[n] = taint;
         return taint;
      }

      bool GeometryGate(const INode* consumer, IGeometrySource* source)
      {
         if (!sAnyClosed || source == nullptr)
            return false;
         auto* srcNode = dynamic_cast<INode*>(source);
         if (srcNode == nullptr)
            return false;
         auto it = sGeoTaint.find(srcNode);
         uint64_t taint = 0;
         if (it != sGeoTaint.end())
            taint = it->second;
         else
         {
            std::set<const INode*> visiting;
            taint = GeometryTaintOf(srcNode, 0, visiting);
         }
         const int consumerSlot = FrameSlotOf(consumer);
         return (taint & ~Bit(consumerSlot)) != 0;
      }

      bool InAnyWindow(const std::vector<std::pair<double, double>>& windows, double beat)
      {
         for (const auto& w : windows)
            if (beat >= w.first && beat < w.second)
               return true;
         return false;
      }

      void SetGroupFlag(GroupNode* g, bool GroupNode::*field, bool value)
      {
         if (g == nullptr || g->*field == value)
            return;
         g->*field = value;
         gPatchDirty = true;
      }
   }


   // ---- gate slots ----------------------------------------------------------

   int GroupGateSlot(GroupNode* g)
   {
      if (g == nullptr)
         return -1;
      auto it = sSlotOf.find(g);
      if (it != sSlotOf.end())
         return it->second;
      for (int s = 0; s < kMaxGroupGates; s++)
      {
         if (sSlotOwner[s] != nullptr)
            continue;
         sSlotOwner[s] = g;
         sSlotOf[g] = s;
         GroupGate& gate = AudioEngine::Instance().GroupGateAt(s);
         gate.target.store(g->mute ? 0.0f : 1.0f, std::memory_order_relaxed);
         gate.notesOpen.store(!g->mute, std::memory_order_relaxed);
         gate.snap.store(true, std::memory_order_relaxed);
         return s;
      }
      return -1; // more than kMaxGroupGates groups: this one cannot mute
   }


   void BuildGroupGateSlotMap(std::unordered_map<const INode*, int>& out)
   {
      // Slots first, so a group created since the last frame already has one
      // (and a group an undo/load just destroyed gives its slot back).
      const auto live = LiveGroups();
      ReleaseDeadSlots(live);
      for (const auto& entry : live)
         GroupGateSlot(entry.second);
      sTopologyHash = ComputeSlotMaps(out, nullptr);
   }


   void DescribeGroupGates(const std::set<int>& slots, std::vector<GroupGateTopo>& outGates,
                           std::vector<GroupBeatWindow>& outWindows)
   {
      outGates.clear();
      outWindows.clear();
      uint64_t mask = 0;
      for (int slot : slots)
      {
         if (slot < 0 || slot >= kMaxGroupGates)
            continue;
         GroupGateTopo t;
         t.slot = slot;
         t.windowOffset = (int)outWindows.size();
         if (GroupNode* g = sSlotOwner[slot])
         {
            if (GraphNode* gn = GraphNodeOfGroup(g))
            {
               std::vector<std::pair<double, double>> windows;
               if (GroupTimelineWindows(gn->uid, windows))
               {
                  t.hasClips = true;
                  for (const auto& w : windows)
                     if (w.second > w.first)
                        outWindows.push_back({ w.first, w.second });
               }
            }
         }
         t.numWindows = (int)outWindows.size() - t.windowOffset;
         outGates.push_back(t);
         mask |= Bit(slot);
         if ((sPrevTopoSlots & Bit(slot)) == 0)
            AudioEngine::Instance().GroupGateAt(slot).snap.store(true, std::memory_order_relaxed);
      }
      sPrevTopoSlots = mask;
   }


   // ---- once a frame ----------------------------------------------------------

   static bool GroupStateFor(GraphNode& gn, GroupNode* g, bool anySolo, bool& outMuted)
   {
      outMuted = g->mute || (anySolo && !g->solo);
      bool timelineOpen = true;
      std::vector<std::pair<double, double>> windows;
      if (GroupTimelineWindows(gn.uid, windows))
         timelineOpen = Transport::Instance().IsPlaying() && InAnyWindow(windows, Transport::Instance().Beats());
      return outMuted || !timelineOpen; // closed
   }


   void UpdateGroupGates()
   {
      const auto live = LiveGroups();
      ReleaseDeadSlots(live);
      if (live.empty() && sSlotOf.empty())
      {
         if (sCablesGated)
            ClearImageCableGates();
         for (bool& c : sOutClosed)
            c = false;
         sAnyClosed = false;
         sFrameSlotOf.clear();
         sFrameSlotOfIndex.clear();
         sGeoTaint.clear();
         gGeometryGroupGate = nullptr;
         // A patch that lost its last group still needs one rebuild to drop
         // the gates its topology named.
         if (sTopologyHash != 0 && !gOfflineRender.active && !gDeferAudioRebuild)
            RebuildAudioTopology();
         return;
      }

      bool anySolo = false;
      for (const auto& entry : live)
         anySolo = anySolo || entry.second->solo;

      for (bool& c : sOutClosed)
         c = false;
      sAnyClosed = false;
      for (const auto& entry : live)
      {
         const int slot = GroupGateSlot(entry.second);
         if (slot < 0)
            continue;
         bool muted = false;
         const bool closed = GroupStateFor(*entry.first, entry.second, anySolo, muted);
         GroupGate& gate = AudioEngine::Instance().GroupGateAt(slot);
         gate.target.store(muted ? 0.0f : 1.0f, std::memory_order_relaxed);
         gate.notesOpen.store(!closed, std::memory_order_relaxed);
         sOutClosed[slot] = closed;
         sAnyClosed = sAnyClosed || closed;
      }

      // Membership moved since the topology was built (any path: Ctrl+G, a
      // menu, paste, undo, a fixture writing gGroupMembers directly): the
      // pins' gate assignment is stale, so rebuild once.
      const uint64_t hash = ComputeSlotMaps(sFrameSlotOf, &sFrameSlotOfIndex);
      if (hash != sTopologyHash && !gOfflineRender.active && !gDeferAudioRebuild)
         RebuildAudioTopology();

      if (!sFrameSlotOf.empty() || sCablesGated)
         GateImageCables();
      if (sAnyClosed)
         EnsureMutedTexture();

      sGeoTaint.clear();
      if (sAnyClosed)
      {
         std::set<const INode*> visiting;
         for (GraphNode& gn : gNodes)
            if (gn.node != nullptr && dynamic_cast<IGeometrySource*>(gn.node.get()) != nullptr)
               GeometryTaintOf(gn.node.get(), 0, visiting);
         gGeometryGroupGate = &GeometryGate;
      }
      else
      {
         gGeometryGroupGate = nullptr;
      }
   }


   void ApplyGroupState()
   {
      UpdateGroupGates();
   }


   bool GroupIsClosed(const GroupNode* g)
   {
      auto it = sSlotOf.find(const_cast<GroupNode*>(g));
      return it != sSlotOf.end() && sOutClosed[it->second];
   }


   bool NodeGroupSilenced(int nodeIndex)
   {
      if (!sAnyClosed)
         return false;
      auto it = sFrameSlotOfIndex.find(nodeIndex);
      return it != sFrameSlotOfIndex.end() && sOutClosed[it->second];
   }


   bool GroupHoldsModulation(int modNodeIndex, int dstNodeIndex)
   {
      if (!sAnyClosed)
         return false;
      auto src = sFrameSlotOfIndex.find(modNodeIndex);
      if (src == sFrameSlotOfIndex.end() || !sOutClosed[src->second])
         return false;
      auto dst = sFrameSlotOfIndex.find(dstNodeIndex);
      return dst == sFrameSlotOfIndex.end() || dst->second != src->second;
   }


   // ---- membership API --------------------------------------------------------

   GroupNode* GroupOfNode(int nodeIndex)
   {
      return GroupOwning(nodeIndex);
   }


   std::vector<int> GroupMemberIndices(const GroupNode* group)
   {
      std::vector<int> out;
      auto it = gGroupMembers.find(const_cast<GroupNode*>(group));
      if (it != gGroupMembers.end())
         out.assign(it->second.begin(), it->second.end());
      return out;
   }


   bool NodeEditLocked(int nodeIndex, std::string* reason)
   {
      // Called for every node every frame (the body draw), so the common
      // case - no locked group at all - must not scan gNodes. Every group
      // that exists is a key here (AutoFitGroupToMembers / the API create it).
      bool anyLocked = false;
      for (const auto& entry : gGroupMembers)
         if (entry.first->locked)
         {
            anyLocked = true;
            break;
         }
      if (!anyLocked)
         return false;
      GraphNode* gn = FindNodeByIndex(nodeIndex);
      if (gn == nullptr || gn->node == nullptr)
         return false;
      GroupNode* g = dynamic_cast<GroupNode*>(gn->node.get());
      if (g == nullptr)
         g = GroupOwning(nodeIndex);
      if (g == nullptr || !g->locked)
         return false;
      if (reason != nullptr)
         *reason = "Group '" + g->label + "' is locked";
      return true;
   }


   bool LinkEditLocked(int linkId, std::string* reason)
   {
      const LinkInfo* link = FindLink(linkId);
      if (link == nullptr)
         return false;
      return NodeEditLocked(GraphNode::NodeIndexFromPin(link->srcPin), reason) ||
             NodeEditLocked(GraphNode::NodeIndexFromPin(link->dstPin), reason);
   }


   namespace
   {
      // Shared checks for a node about to join `target` (nullptr = a new group).
      bool CheckJoinable(int index, GroupNode* target, std::string& err)
      {
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr || gn->node == nullptr)
         {
            err = "node " + std::to_string(index) + " does not exist";
            return false;
         }
         if (dynamic_cast<GroupNode*>(gn->node.get()) != nullptr)
         {
            err = "node " + std::to_string(index) + " is a group (groups cannot be nested)";
            return false;
         }
         GroupNode* owner = GroupOwning(index);
         if (owner != nullptr && owner != target)
         {
            err = owner->locked ? "node " + std::to_string(index) + " is in locked group '" + owner->label + "'"
                                : "node " + std::to_string(index) + " is already in group '" + owner->label + "'";
            return false;
         }
         return true;
      }
   }


   GroupNode* CreateGroupForNodes(const std::vector<int>& nodeIndices, const std::string& label, std::string& err)
   {
      err.clear();
      if (nodeIndices.empty())
      {
         err = "no nodes to group";
         return nullptr;
      }
      std::set<int> picked;
      bool any = false;
      float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
      for (int index : nodeIndices)
      {
         if (!CheckJoinable(index, nullptr, err))
            return nullptr;
         picked.insert(index);
         GraphNode* gn = FindNodeByIndex(index);
         // Cached live position (valid outside the editor context too); the
         // box fits itself to its members from the next frame on, so this
         // only seeds where it first appears.
         const float x = gn->liveX, y = gn->liveY;
         const float w = 220.0f, h = 160.0f;
         if (!any)
         {
            minX = x; minY = y; maxX = x + w; maxY = y + h;
            any = true;
         }
         else
         {
            minX = std::min(minX, x); minY = std::min(minY, y);
            maxX = std::max(maxX, x + w); maxY = std::max(maxY, y + h);
         }
      }

      const float kPad = 32.0f;
      const float kHeader = 24.0f;
      const bool suppressBefore = gSuppressUndoCheckpoints;
      gSuppressUndoCheckpoints = true;
      GraphNode* ggn = SpawnNode("Group", "Compositing", minX - kPad, minY - kPad - kHeader);
      gSuppressUndoCheckpoints = suppressBefore;
      GroupNode* grp = ggn != nullptr ? dynamic_cast<GroupNode*>(ggn->node.get()) : nullptr;
      if (grp == nullptr)
      {
         err = "could not create a Group node";
         return nullptr;
      }
      grp->width = (maxX - minX) + kPad * 2.0f;
      grp->height = (maxY - minY) + kPad * 2.0f + kHeader;
      if (!label.empty())
         grp->label = label;
      grp->membersText.clear(); // explicit from birth - never legacy-adopted
      gGroupMembers[grp] = picked;
      gPatchDirty = true;
      ApplyGroupState();
      return grp;
   }


   bool AddNodesToGroup(GroupNode* group, const std::vector<int>& nodeIndices, std::string& err)
   {
      err.clear();
      if (group == nullptr || GraphNodeOfGroup(group) == nullptr)
      {
         err = "no such group";
         return false;
      }
      if (group->locked)
      {
         err = "Group '" + group->label + "' is locked";
         return false;
      }
      for (int index : nodeIndices)
         if (!CheckJoinable(index, group, err))
            return false;
      std::set<int>& members = gGroupMembers[group];
      for (int index : nodeIndices)
         members.insert(index);
      gPatchDirty = true;
      ApplyGroupState();
      return true;
   }


   bool RemoveNodesFromGroup(GroupNode* group, const std::vector<int>& nodeIndices, std::string& err)
   {
      err.clear();
      if (group == nullptr || GraphNodeOfGroup(group) == nullptr)
      {
         err = "no such group";
         return false;
      }
      if (group->locked)
      {
         err = "Group '" + group->label + "' is locked";
         return false;
      }
      std::set<int>& members = gGroupMembers[group];
      for (int index : nodeIndices)
      {
         if (members.count(index) == 0)
         {
            err = "node " + std::to_string(index) + " is not in group '" + group->label + "'";
            return false;
         }
      }
      for (int index : nodeIndices)
         members.erase(index);
      gPatchDirty = true;
      ApplyGroupState();
      return true;
   }


   // ---- save / load -----------------------------------------------------------

   void SyncGroupMembersText()
   {
      for (const auto& entry : LiveGroups())
      {
         GroupNode* g = entry.second;
         // A legacy group still waiting for its one geometric adoption (it
         // has not been laid out yet - or never will be, in a headless job)
         // stays legacy, rather than being saved as an explicit empty group.
         if (g->legacyAdoptFrames >= 0)
         {
            g->membersText = GroupNode::kMembersUnset;
            continue;
         }
         std::vector<uint64_t> uids;
         auto it = gGroupMembers.find(g);
         if (it != gGroupMembers.end())
            for (int index : it->second)
               if (GraphNode* member = FindNodeByIndex(index))
                  uids.push_back(member->uid);
         std::sort(uids.begin(), uids.end());
         std::string text;
         for (uint64_t uid : uids)
         {
            if (!text.empty())
               text += ' ';
            text += std::to_string((unsigned long long)uid);
         }
         // Never empty on disk: "-" is an explicit empty group, so it cannot
         // be confused with a patch that predates the key.
         g->membersText = text.empty() ? std::string("-") : text;
      }
   }


   void RestoreGroupMembership()
   {
      PruneDeadGroups();
      sLockedPos.clear();
      std::set<int> taken;
      for (const auto& entry : LiveGroups())
      {
         GroupNode* g = entry.second;
         std::set<int>& members = gGroupMembers[g];
         members.clear();
         if (g->membersText == GroupNode::kMembersUnset)
         {
            // Saved before membership was explicit: adopt by geometry once,
            // after the members have been laid out (AutoFitGroupToMembers).
            g->legacyAdoptFrames = 3;
            continue;
         }
         g->legacyAdoptFrames = -1;
         std::istringstream in(g->membersText);
         std::string token;
         while (in >> token)
         {
            char* end = nullptr;
            const unsigned long long uid = std::strtoull(token.c_str(), &end, 10);
            if (uid == 0 || end == token.c_str())
               continue;
            GraphNode* member = FindNodeByUid((uint64_t)uid);
            if (member == nullptr || dynamic_cast<GroupNode*>(member->node.get()) != nullptr)
               continue;
            if (!taken.insert(member->index).second)
               continue; // exclusive: the first group to name a node keeps it
            members.insert(member->index);
         }
      }
   }


   // The one-off adoption for a legacy group (see RestoreGroupMembership):
   // exactly the old every-frame rule, run once.
   void LegacyAdoptByGeometry(GraphNode& gn, GroupNode* n)
   {
      const ImVec2 nodePos = ed::GetNodePosition(gn.NodeId());
      const ImVec2 boxMin(nodePos.x, nodePos.y + n->headerH);
      const ImVec2 boxMax(boxMin.x + n->width, boxMin.y + n->height);
      std::set<int>& members = gGroupMembers[n];
      for (GraphNode& other : gNodes)
      {
         if (other.index == gn.index || dynamic_cast<GroupNode*>(other.node.get()) != nullptr)
            continue;
         GroupNode* owner = GroupOwning(other.index);
         if (owner != nullptr && owner != n)
            continue;
         const ImVec2 p = ed::GetNodePosition(other.NodeId());
         const ImVec2 s = ed::GetNodeSize(other.NodeId());
         if (p.x >= boxMin.x && p.y >= boxMin.y && p.x + s.x <= boxMax.x && p.y + s.y <= boxMax.y)
            members.insert(other.index);
      }
   }


   // ---- lock ------------------------------------------------------------------

   // Inside the editor context, before any node draws: a locked group's
   // members go back to where they were pinned, whatever moved them this
   // frame (a node drag, a drag of the group header, which the editor applies
   // to everything inside the box, arrow keys). The box itself fits its
   // members, so it follows.
   void EnforceGroupLocks()
   {
      std::set<uint64_t> seen;
      for (const auto& entry : LiveGroups())
      {
         GroupNode* g = entry.second;
         if (!g->locked)
            continue;
         auto it = gGroupMembers.find(g);
         if (it == gGroupMembers.end())
            continue;
         for (int index : it->second)
         {
            GraphNode* member = FindNodeByIndex(index);
            if (member == nullptr)
               continue;
            // Not laid out yet (just loaded): its editor position is not real.
            if (member->needsPosition)
            {
               sLockedPos.erase(member->uid);
               continue;
            }
            seen.insert(member->uid);
            const ImVec2 p = ed::GetNodePosition(member->NodeId());
            auto pin = sLockedPos.find(member->uid);
            if (pin == sLockedPos.end())
               sLockedPos[member->uid] = p;
            else if (std::fabs(p.x - pin->second.x) > 0.01f || std::fabs(p.y - pin->second.y) > 0.01f)
               ed::SetNodePosition(member->NodeId(), pin->second);
         }
      }
      for (auto it = sLockedPos.begin(); it != sLockedPos.end();)
         it = seen.count(it->first) ? std::next(it) : sLockedPos.erase(it);
   }


   // ---- UI --------------------------------------------------------------------

   namespace
   {
      void ToggleGroupFlag(GroupNode* g, bool GroupNode::*field)
      {
         PushUndoCheckpoint();
         SetGroupFlag(g, field, !(g->*field));
         ApplyGroupState();
      }

      // One square header button: hover highlight, icon drawn by `drawIcon`.
      template <typename DrawFn>
      bool HeaderIconButton(const char* id, const char* tooltip, bool active, ImU32 activeCol, DrawFn drawIcon)
      {
         const float sz = ImGui::GetFrameHeight() * 0.85f;
         const bool pressed = ImGui::InvisibleButton(id, ImVec2(sz, sz));
         const bool hovered = ImGui::IsItemHovered();
         const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
         ImDrawList* dl = ImGui::GetWindowDrawList();
         if (active)
            dl->AddRectFilled(mn, mx, activeCol, 3.0f);
         else if (hovered)
            dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.6f), 3.0f);
         const ImU32 iconCol = active ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text);
         drawIcon(dl, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), sz * 0.9f, iconCol);
         if (hovered)
         {
            // HelpTip suspends the canvas transform only while this flag says
            // a node is being drawn; a group header is drawn outside that loop.
            const bool wasInside = gInsideNodeCanvas;
            gInsideNodeCanvas = true;
            HelpTip("%s", tooltip);
            gInsideNodeCanvas = wasInside;
         }
         return pressed;
      }
   }


   void DrawGroupHeaderButtons(GraphNode& gn, GroupNode* g)
   {
      (void)gn;
      ImGui::SameLine(0.0f, 10.0f);
      if (HeaderIconButton("##grpmute", g->mute ? T("Unmute group (M)") : T("Mute group (M)"), g->mute,
                           IM_COL32(200, 120, 40, 230),
                           [&](ImDrawList* dl, ImVec2 c, float s, ImU32 col) { Tabler::DrawVolume(dl, c, s, col, g->mute); }))
         ToggleGroupFlag(g, &GroupNode::mute);
      ImGui::SameLine(0.0f, 4.0f);
      if (HeaderIconButton("##grpsolo", g->solo ? T("Unsolo group") : T("Solo group (other groups go quiet)"), g->solo,
                           IM_COL32(200, 170, 40, 230),
                           [&](ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
                              const ImVec2 ts = ImGui::CalcTextSize("S");
                              dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, "S");
                              (void)s;
                           }))
         ToggleGroupFlag(g, &GroupNode::solo);
      ImGui::SameLine(0.0f, 4.0f);
      if (HeaderIconButton("##grplock", g->locked ? T("Unlock group") : T("Lock group (no edits by hand)"), g->locked,
                           IM_COL32(120, 120, 140, 230),
                           [&](ImDrawList* dl, ImVec2 c, float s, ImU32 col) { Tabler::DrawLock(dl, c, s, col, !g->locked); }))
         ToggleGroupFlag(g, &GroupNode::locked);
   }


   void DrawGroupContextMenuItems(GraphNode& gn, GroupNode* g)
   {
      (void)gn;
      if (ImGui::MenuItem(L("Mute"), "M", g->mute))
         ToggleGroupFlag(g, &GroupNode::mute);
      if (ImGui::MenuItem(L("Solo"), nullptr, g->solo))
         ToggleGroupFlag(g, &GroupNode::solo);
      if (ImGui::MenuItem(L("Lock"), nullptr, g->locked))
         ToggleGroupFlag(g, &GroupNode::locked);
   }


   void DetachNodeFromGroupUi(int nodeIndex)
   {
      GroupNode* owner = GroupOwning(nodeIndex);
      GraphNode* gn = FindNodeByIndex(nodeIndex);
      if (owner == nullptr || gn == nullptr)
         return;
      std::string err;
      if (owner->locked)
      {
         gPatchStatus = "Group '" + owner->label + "' is locked";
         return;
      }
      PushUndoCheckpoint();
      // Move it just below the box first, so it visibly leaves rather than
      // sitting inside a box it no longer belongs to.
      if (GraphNode* ownerGn = GraphNodeOfGroup(owner))
      {
         const ImVec2 gp = ed::GetNodePosition(ownerGn->NodeId());
         const ImVec2 gs = ed::GetNodeSize(ownerGn->NodeId());
         const ImVec2 mp = ed::GetNodePosition(gn->NodeId());
         ed::SetNodePosition(gn->NodeId(), ImVec2(mp.x, gp.y + gs.y + 40.0f));
      }
      RemoveNodesFromGroup(owner, { nodeIndex }, err);
   }


   void DrawNodeGroupMembershipMenu(GraphNode& gn)
   {
      if (GroupNode* owner = GroupOwning(gn.index))
      {
         // T, not L: L appends a "###" id that would hide the group name.
         std::string label = std::string(T("Remove from group")) + " '" + owner->label + "'##rmgroup";
         if (ImGui::MenuItem(label.c_str(), nullptr, false, !owner->locked))
            DetachNodeFromGroupUi(gn.index);
         return;
      }
      const auto live = LiveGroups();
      if (live.empty())
         return;
      if (ImGui::BeginMenu(L("Add to group")))
      {
         for (const auto& entry : live)
         {
            GroupNode* g = entry.second;
            ImGui::PushID(g);
            const std::string item = g->label + (g->locked ? "  (locked)" : "");
            if (ImGui::MenuItem(item.c_str(), nullptr, false, !g->locked))
            {
               std::string err;
               PushUndoCheckpoint();
               if (!AddNodesToGroup(g, { gn.index }, err))
                  gPatchStatus = err;
            }
            ImGui::PopID();
         }
         ImGui::EndMenu();
      }
   }


   // M: mute/unmute the selected groups - a selected group header, or the
   // group a selected member belongs to. All muted -> all unmute, otherwise
   // all mute, so a mixed selection lands in one state. One undo step.
   void ToggleMuteOnSelectedGroups()
   {
      const int count = ed::GetSelectedObjectCount();
      if (count <= 0)
         return;
      std::vector<ed::NodeId> sel(count);
      const int nodeCount = ed::GetSelectedNodes(sel.data(), count);
      std::vector<GroupNode*> groups;
      for (int i = 0; i < nodeCount; i++)
      {
         GraphNode* gn = FindNodeByIndex((int)sel[i].Get() / GraphNode::kStride);
         if (gn == nullptr)
            continue;
         GroupNode* g = dynamic_cast<GroupNode*>(gn->node.get());
         if (g == nullptr)
            g = GroupOwning(gn->index);
         if (g != nullptr && std::find(groups.begin(), groups.end(), g) == groups.end())
            groups.push_back(g);
      }
      if (groups.empty())
         return;
      bool allMuted = true;
      for (GroupNode* g : groups)
         allMuted = allMuted && g->mute;
      PushUndoCheckpoint();
      for (GroupNode* g : groups)
         SetGroupFlag(g, &GroupNode::mute, !allMuted);
      ApplyGroupState();
   }
}
