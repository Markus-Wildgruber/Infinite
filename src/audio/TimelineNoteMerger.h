#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio/AudioBuffer.h"
#include "audio/AudioNode.h"
#include "audio/NoteEvent.h"
#include "audio/NoteEventQueue.h"
#include "core/Transport.h"

// Timeline MIDI playback (Arrange::kLaneMidi). One merger per instrument node
// a MIDI lane plays: RebuildAudioTopology (ArrangeMidi.cpp's
// ArrangeMidiAugmentTopology) places it in the topology right before the
// instrument, takes over the instrument's canvas note wire (the merger reads
// the canvas producer's outbox with the cursor the instrument had) and points
// the instrument's note inbox at the merger's own outbox. Every block the
// merger forwards the canvas events unchanged and merges in the timeline's
// notes for that block, sample-accurately (frameOffset), sorted by frame.
//
// The notes arrive as an immutable TimelineNoteSnapshot (absolute beats),
// swapped in with one atomic pointer store. The main thread owns every
// snapshot and frees a replaced one only after the audio thread has finished
// a topology generation published after the swap (ArrangeMidi.cpp), so the
// audio thread never allocates, frees or locks.
//
// No stuck notes: everything the merger turned on is in mHeld until it sends
// the note-off - at the note's end (clipped to its clip's end when the
// snapshot is built), on transport stop, on a seek or any other jump of the
// block position, inside the block at a transport loop wrap, when a new
// snapshot no longer holds that note (clip/lane muted, deleted, moved or
// edited), and when the merger is re-prepared.
struct TimelineNote
{
   double   onBeat  = 0.0;
   double   offBeat = 0.0; // exclusive; already clipped to the clip's end
   uint64_t key     = 0;   // identity across republishes: (clip id, absolute start tick, pitch)
   int      note    = 60;
   float    velocity = 0.8f;
   bool operator==(const TimelineNote& o) const
   {
      return onBeat == o.onBeat && offBeat == o.offBeat && key == o.key && note == o.note && velocity == o.velocity;
   }
   bool operator!=(const TimelineNote& o) const { return !(*this == o); }
};

struct TimelineNoteSnapshot
{
   // Unique per published snapshot (never 0). The merger tells a new
   // snapshot from the last one by this, not by address: a freed snapshot's
   // address can come back for the next allocation.
   uint64_t serial = 0;
   std::vector<TimelineNote> notes; // sorted by onBeat, then note
};

class TimelineNoteMerger : public AudioNode
{
public:
   static constexpr int kMaxHeld = 128;
   static constexpr int kMaxCanvas = 192;
   static constexpr int kMaxOut = 512;
   static constexpr int kTapCapacity = 1024;

   // Test tap (INFINITE_ARRANGEMIDITEST): every event the merger itself sent
   // (timeline notes and auditions, not forwarded canvas notes), with the
   // block's start beat and beats per sample so the exact sample position is
   // block start + frameOffset.
   struct TapEvent
   {
      double blockStartBeat = 0.0;
      double beatsPerSample = 0.0;
      int frameOffset = 0;
      int note = 0;
      bool on = false;
   };

   void PrepareToPlay(double sampleRate, int /*maxBlockSize*/) override
   {
      mSampleRate = sampleRate;
      // Whatever is held is released on the next block (never just forgotten:
      // the instrument would keep the voice).
      mReleaseRequest.store(true, std::memory_order_relaxed);
   }

   void ProcessBlock(const AudioBuffer* const* /*inputs*/, int /*numInputs*/, AudioBuffer& output) override
   {
      const int numFrames = output.numFrames;
      for (int ch = 0; ch < output.numChannels; ch++)
         if (output.channels[ch] != nullptr)
            std::fill(output.channels[ch], output.channels[ch] + std::max(0, numFrames), 0.0f);
      if (numFrames <= 0)
         return;

      // Canvas notes first (the producer already ran this block).
      int nc = 0;
      if (mInbox != nullptr && mCursor >= 0)
      {
         while (nc < kMaxCanvas)
         {
            const int k = mInbox->Pop(mCursor, mCanvas + nc, kMaxCanvas - nc);
            if (k <= 0)
               break;
            nc += k;
         }
      }

      mNumOut = 0;
      mBlockFrames = numFrames;
      RunAudition();
      RunTimeline(numFrames);

      // Stable insertion sort of this block's own events: by frame, note-offs
      // before note-ons on the same frame (a note ending where the next one
      // starts must not cut the new one).
      for (int i = 1; i < mNumOut; i++)
      {
         const NoteEvent e = mOut[i];
         int j = i - 1;
         while (j >= 0 && Later(mOut[j], e))
         {
            mOut[j + 1] = mOut[j];
            j--;
         }
         mOut[j + 1] = e;
      }

      // Merge with the canvas events, whose own order is kept as is.
      int t = 0;
      for (int i = 0; i < nc; i++)
      {
         while (t < mNumOut && (mOut[t].frameOffset < mCanvas[i].frameOffset ||
                                (mOut[t].frameOffset == mCanvas[i].frameOffset && !mOut[t].isNoteOn)))
            Emit(mOut[t++]);
         mOutbox.Push(mCanvas[i]);
      }
      while (t < mNumOut)
         Emit(mOut[t++]);
      mHeldPublished.store(mHeldCount + (mAuditionVoice != 0 ? 1 : 0), std::memory_order_relaxed);
   }

   NoteEventQueue* NoteOutbox() override { return &mOutbox; }
   void SetNoteInbox(NoteEventQueue* inbox, int cursor) override
   {
      mInbox = inbox;
      mCursor = cursor;
   }

   // ---- main thread ------------------------------------------------------
   // The caller keeps `snap` alive until the audio thread can no longer be
   // reading it (see the class comment). nullptr = no timeline notes.
   void Publish(const TimelineNoteSnapshot* snap) { mSnap.store(snap, std::memory_order_release); }
   const TimelineNoteSnapshot* Published() const { return mSnap.load(std::memory_order_acquire); }
   // Notes this merger has turned on and not yet off, as of its last block.
   int HeldCount() const { return mHeldPublished.load(std::memory_order_relaxed); }
   // Piano-roll audition: one note at a time, independent of the transport.
   void AuditionOn(int note, float velocity)
   {
      mAuditionVelocity.store(std::clamp(velocity, 0.05f, 1.0f), std::memory_order_relaxed);
      mAuditionOnRequest.store(std::clamp(note, 0, 127), std::memory_order_release);
   }
   void AuditionOff() { mAuditionOffRequest.store(true, std::memory_order_release); }

   // Test tap. Enable/clear only while no audio thread runs this merger.
   void SetTapEnabled(bool on)
   {
      mTapCount.store(0, std::memory_order_relaxed);
      mTapOn.store(on, std::memory_order_relaxed);
   }
   int TapCount() const { return mTapCount.load(std::memory_order_acquire); }
   const TapEvent& TapAt(int i) const { return mTap[std::clamp(i, 0, kTapCapacity - 1)]; }

private:
   struct Held
   {
      uint64_t key = 0;
      double onBeat = 0.0;
      double offBeat = 0.0;
      int note = 0;
      int voiceId = 0;
      int onFrame = -1; // frame of the note-on when it was sent this block, else -1
   };

   static bool Later(const NoteEvent& a, const NoteEvent& b)
   {
      if (a.frameOffset != b.frameOffset)
         return a.frameOffset > b.frameOffset;
      return a.isNoteOn && !b.isNoteOn; // offs first
   }

   static int FrameAt(double beat, double segStartBeat, double beatsPerSample, int frameStart)
   {
      const double f = std::ceil((beat - segStartBeat) / beatsPerSample - 1e-6);
      if (f > 1.0e9) return 1000000000;
      if (f < -1.0e9) return -1000000000;
      return frameStart + (int)f;
   }

   bool PushOut(const NoteEvent& e)
   {
      if (mNumOut >= kMaxOut)
         return false;
      NoteEvent c = e;
      c.frameOffset = std::clamp(c.frameOffset, 0, std::max(0, mBlockFrames - 1));
      mOut[mNumOut++] = c;
      return true;
   }

   bool PushOff(int note, int voiceId, int frame)
   {
      NoteEvent off;
      off.note = note;
      off.velocity = 0.0f;
      off.isNoteOn = false;
      off.frameOffset = frame;
      off.source = this;
      off.voiceId = voiceId;
      return PushOut(off);
   }

   void ReleaseAll(int frame)
   {
      int kept = 0;
      for (int h = 0; h < mHeldCount; h++)
         if (!PushOff(mHeld[h].note, mHeld[h].voiceId, frame))
            mHeld[kept++] = mHeld[h]; // no room this block: released on the next one
      mHeldCount = kept;
   }

   void Emit(const NoteEvent& e)
   {
      mOutbox.Push(e);
      if (!mTapOn.load(std::memory_order_relaxed))
         return;
      const int i = mTapCount.load(std::memory_order_relaxed);
      if (i >= kTapCapacity)
         return;
      TapEvent& t = mTap[i];
      t.blockStartBeat = mTapBlockStart;
      t.beatsPerSample = mTapBeatsPerSample;
      t.frameOffset = e.frameOffset;
      t.note = e.note;
      t.on = e.isNoteOn;
      mTapCount.store(i + 1, std::memory_order_release);
   }

   void RunAudition()
   {
      if (mAuditionOffRequest.exchange(false, std::memory_order_acquire) && mAuditionVoice != 0)
      {
         if (PushOff(mAuditionNote, mAuditionVoice, 0))
            mAuditionVoice = 0;
      }
      const int on = mAuditionOnRequest.exchange(-1, std::memory_order_acquire);
      if (on >= 0)
      {
         if (mAuditionVoice != 0 && PushOff(mAuditionNote, mAuditionVoice, 0))
            mAuditionVoice = 0;
         if (mAuditionVoice == 0)
         {
            NoteEvent e;
            e.note = on;
            e.velocity = mAuditionVelocity.load(std::memory_order_relaxed);
            e.isNoteOn = true;
            e.frameOffset = 0;
            e.source = this;
            e.voiceId = NextVoiceId();
            if (PushOut(e))
            {
               mAuditionNote = on;
               mAuditionVoice = e.voiceId;
            }
         }
      }
   }

   // Held notes the new snapshot no longer has (same key) are released now;
   // the rest take the new end, so an edit elsewhere never cuts a sounding
   // note and shortening one ends it at its new end.
   void Reconcile(const TimelineNoteSnapshot* snap)
   {
      for (int h = 0; h < mHeldCount;)
      {
         Held& hd = mHeld[h];
         bool found = false;
         if (snap != nullptr)
         {
            const std::vector<TimelineNote>& v = snap->notes;
            auto it = std::lower_bound(v.begin(), v.end(), hd.onBeat,
                                       [](const TimelineNote& n, double b) { return n.onBeat < b; });
            for (; it != v.end() && it->onBeat <= hd.onBeat; ++it)
               if (it->key == hd.key)
               {
                  hd.offBeat = it->offBeat;
                  found = true;
                  break;
               }
         }
         if (!found && PushOff(hd.note, hd.voiceId, 0))
         {
            mHeld[h] = mHeld[--mHeldCount];
            continue;
         }
         h++;
      }
   }

   // The first note whose on-frame relative to `segStart` is >= 0 - the same
   // rounding Segment uses, so a note is neither skipped nor sent twice.
   void SeekCursor(const TimelineNoteSnapshot* snap, double segStart, double bps)
   {
      const std::vector<TimelineNote>& v = snap->notes;
      auto it = std::lower_bound(v.begin(), v.end(), segStart, [bps](const TimelineNote& n, double s)
      {
         return (n.onBeat - s) / bps - 1e-6 <= -1.0;
      });
      mNextOn = (size_t)(it - v.begin());
      mCursorValid = true;
   }

   // Frames [frameStart, frameEnd) of this block play beats starting at
   // segStart.
   void Segment(const TimelineNoteSnapshot* snap, double segStart, double bps, int frameStart, int frameEnd)
   {
      if (frameEnd <= frameStart)
         return;
      const std::vector<TimelineNote>& v = snap->notes;
      while (mNextOn < v.size())
      {
         const TimelineNote& tn = v[mNextOn];
         int f = FrameAt(tn.onBeat, segStart, bps, frameStart);
         if (f >= frameEnd)
            break;
         // Room for this note-on, its own note-off and every held note's off.
         if (mHeldCount >= kMaxHeld || mNumOut + 2 + mHeldCount >= kMaxOut)
            break; // deferred to the next block
         mNextOn++;
         if (!(tn.offBeat > tn.onBeat))
            continue;
         if (f < frameStart)
            f = frameStart;
         NoteEvent on;
         on.note = std::clamp(tn.note, 0, 127);
         // A zero-velocity note-on reads as a note-off to some consumers.
         on.velocity = std::clamp(tn.velocity, 1.0f / 127.0f, 1.0f);
         on.isNoteOn = true;
         on.frameOffset = f;
         on.source = this;
         on.voiceId = NextVoiceId();
         if (!PushOut(on))
            break;
         Held& hd = mHeld[mHeldCount++];
         hd.key = tn.key;
         hd.onBeat = tn.onBeat;
         hd.offBeat = tn.offBeat;
         hd.note = on.note;
         hd.voiceId = on.voiceId;
         hd.onFrame = f;
      }
      for (int h = 0; h < mHeldCount;)
      {
         Held& hd = mHeld[h];
         int f = FrameAt(hd.offBeat, segStart, bps, frameStart);
         if (f < frameEnd)
         {
            if (f < frameStart)
               f = frameStart;
            if (hd.onFrame >= 0 && f <= hd.onFrame)
            {
               if (hd.onFrame + 1 >= frameEnd)
               {
                  h++; // shorter than a sample: its off goes out first thing next block
                  continue;
               }
               f = hd.onFrame + 1;
            }
            if (PushOff(hd.note, hd.voiceId, f))
            {
               mHeld[h] = mHeld[--mHeldCount];
               continue;
            }
         }
         h++;
      }
   }

   void RunTimeline(int numFrames)
   {
      Transport& tr = Transport::Instance();
      if (mReleaseRequest.exchange(false, std::memory_order_relaxed))
         ReleaseAll(0);
      const TimelineNoteSnapshot* snap = mSnap.load(std::memory_order_acquire);
      const uint64_t serial = snap != nullptr ? snap->serial : 0;
      if (serial != mSeenSerial)
      {
         Reconcile(snap);
         mSeenSerial = serial;
         mCursorValid = false;
      }
      const double sr = mSampleRate > 0.0 ? mSampleRate : tr.AudioSampleRate();
      // AudioSampleRate() > 0: the audio clock runs (a device or an offline
      // take). The device-less note pump has no beat axis to place notes on.
      const bool live = snap != nullptr && sr > 0.0 && tr.AudioSampleRate() > 0.0 && tr.IsPlaying();
      mTapBeatsPerSample = 0.0;
      mTapBlockStart = 0.0;
      if (!live)
      {
         ReleaseAll(0);
         mHaveExpected = false;
         mCursorValid = false;
         return;
      }
      const double bpm = std::max(1.0, (double)tr.Tempo());
      const double bps = bpm / (60.0 * sr);
      const double b0 = tr.BlockStartBeats();
      const double b1 = b0 + (double)numFrames * bps;
      mTapBlockStart = b0;
      mTapBeatsPerSample = bps;
      // A seek, a scrub, a fresh Play or any other jump: release, then pick up
      // from the new position (notes already under way there are not chased).
      if (mHaveExpected && std::fabs(b0 - mExpected) > bps * 0.5)
      {
         ReleaseAll(0);
         mCursorValid = false;
      }
      for (int h = 0; h < mHeldCount; h++)
         mHeld[h].onFrame = -1;

      const double ls = tr.LoopStartBeats();
      const double le = tr.LoopEndBeats();
      const bool loopOn = tr.LoopEnabled() && !tr.LoopSuspended() && le > ls;
      if (loopOn && b0 < le && b1 > le)
      {
         // The block crosses the loop end: the transport's next block starts
         // at ls + (b1 - le), so the frames past the end already belong to the
         // next lap - play them as such, sample-accurately.
         const int split = std::clamp(FrameAt(le, b0, bps, 0), 0, numFrames);
         if (!mCursorValid)
            SeekCursor(snap, b0, bps);
         Segment(snap, b0, bps, 0, split);
         ReleaseAll(std::min(split, numFrames - 1));
         const double lap = ls + (b0 + (double)split * bps - le);
         SeekCursor(snap, lap, bps);
         Segment(snap, lap, bps, split, numFrames);
         mExpected = ls + (b1 - le);
      }
      else
      {
         if (!mCursorValid)
            SeekCursor(snap, b0, bps);
         Segment(snap, b0, bps, 0, numFrames);
         mExpected = b1;
      }
      mHaveExpected = true;
   }

   NoteEventQueue mOutbox;
   NoteEventQueue* mInbox = nullptr;
   int mCursor = -1;
   double mSampleRate = 0.0;

   std::atomic<const TimelineNoteSnapshot*> mSnap { nullptr };
   uint64_t mSeenSerial = 0;
   size_t mNextOn = 0;
   bool mCursorValid = false;
   bool mHaveExpected = false;
   double mExpected = 0.0;
   std::atomic<bool> mReleaseRequest { false };

   Held mHeld[kMaxHeld];
   int mHeldCount = 0;
   std::atomic<int> mHeldPublished { 0 };

   NoteEvent mCanvas[kMaxCanvas];
   NoteEvent mOut[kMaxOut];
   int mNumOut = 0;
   int mBlockFrames = 1;

   std::atomic<int> mAuditionOnRequest { -1 };
   std::atomic<bool> mAuditionOffRequest { false };
   std::atomic<float> mAuditionVelocity { 0.8f };
   int mAuditionNote = 0;
   int mAuditionVoice = 0;

   std::atomic<bool> mTapOn { false };
   std::atomic<int> mTapCount { 0 };
   double mTapBlockStart = 0.0;
   double mTapBeatsPerSample = 0.0;
   TapEvent mTap[kTapCapacity];
};
