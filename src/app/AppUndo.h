#pragma once
// The undo/redo stacks. Split out of AppShared.h because an entry holds an
// Arrange::Model by value, which would otherwise put ArrangeModel.h in every
// app/ TU (docs/plans/compile-times/include-split.md, Phase 3).
#include "app/AppShared.h"
#include "arrange/ArrangeModel.h"

namespace app
{
   // deque, not vector: erase(begin()) at the depth cap below shifts every
   // remaining element, and each element is a full Patch::Data (two
   // heap-allocated strings per param per node) - expensive to shift at a
   // 200-deep cap on a large patch. pop_front() is O(1) on a deque.
   // One point in history. `patch` is the graph; `gestures` is the Shift-drag
   // recordings looping at that moment (see GestureRecorder). Recordings are
   // session state rather than patch content - they are not in Patch::Data
   // and never reach a saved file - but undo still has to make one appear and
   // disappear at the right point in history, exactly the way a typed
   // expression (which IS in Patch::Data) already does. Snapshotting them
   // alongside the graph is what gives that: the checkpoint pushed when the
   // user grabbed the knob predates the recording, so undoing to it removes
   // the recording, and redo brings it back. Carried across ApplyPatchData's
   // respawn through the same old-index -> new-index remap as RemapGestures.
   struct UndoEntry
   {
      Patch::Data patch;
      GestureRecorder::PlaybackMap gestures;
      // Set only for timeline-only gestures (move a clip, trim, split, add a
      // marker). Undoing one of these swaps the arrangement back and touches
      // nothing else - it must NOT go through ApplyPatchData, which tears down
      // and respawns the whole graph. That respawn is why dragging a clip used
      // to reset every node's internal state, drop audio, and rebuild every
      // FBO. Clips reference node uids, which the graph side never changes
      // here, so the two entry kinds coexist with no remapping.
      bool arrangeOnly = false;
      Arrange::Model arrange;
      // What the edit this entry undoes was called ("Move node"). Empty until named: a call site that passed no label
      // gets one derived from the before/after patches the first time something shows it. Never written to a patch.
      std::string label;
   };

extern std::deque<UndoEntry> gUndoStack;

extern std::deque<UndoEntry> gRedoStack;


   inline const size_t kMaxUndoDepth = 200;

}
