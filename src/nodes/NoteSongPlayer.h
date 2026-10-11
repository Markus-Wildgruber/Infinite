#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "audio/AudioBuffer.h"
#include "audio/AudioNode.h"
#include "audio/MidiFile.h"
#include "audio/NoteEvent.h"
#include "audio/NoteEventQueue.h"
#include "core/Transport.h"

// Audio half shared by the note nodes that play a MidiFile::Song on the
// transport: MIDI File (a loaded .mid) and Audio to MIDI (notes transcribed
// from audio). Plays the song's beat grid at the project bpm times `speed`,
// keeps its own playhead (restarting at `position` whenever the transport
// starts, seeks or loops back, or the position moves), loops at `loopBeats`,
// and releases every held note on stop, jump and wrap so nothing rings on.
//
// Locked (Audio to MIDI): no playhead of its own - the song position is the
// transport's time (minus an offset), so after a seek it plays the notes that
// belong at that time, in step with an Audio File following the transport.
//
// The song pointer is swapped atomically; the owner must keep a replaced song
// alive until the audio thread can no longer be reading it.
class AudioSongPlayer : public AudioNode
{
public:
   void PrepareToPlay(double sampleRate, int /*maxBlockSize*/) override
   {
      mSampleRate = sampleRate;
      mHeldCount = 0;
      mLastTransport = -1.0;
   }

   void ProcessBlock(const AudioBuffer* const* /*inputs*/, int /*numInputs*/, AudioBuffer& output) override
   {
      const int numFrames = output.numFrames;
      if (numFrames <= 0)
         return; // the per-event offset below divides by this block's length
      const MidiFile::Song* song = mSong.load(std::memory_order_acquire);
      if (!song || !Transport::Instance().IsPlaying())
      {
         ReleaseAll(0);
         mLastTransport = -1.0;
         mPlayhead.store(-1.0, std::memory_order_relaxed);
         return;
      }

      const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
      const double speed = std::clamp((double)mSpeed.load(std::memory_order_relaxed), 0.05, 8.0);
      // Speed 1 is the project bpm; the file's own tempo never matters, it is just notes on a beat grid.
      const double dTransport = (double)numFrames * bpm / (60.0 * mSampleRate); // transport beats this block
      const double dBeats = dTransport * speed;                                  // file beats this block
      const bool loop = mLoop.load(std::memory_order_relaxed);
      const double len = std::max(1.0, (double)mLoopBeats.load(std::memory_order_relaxed));
      const int transpose = mTranspose.load(std::memory_order_relaxed);
      const float velScale = mVelocity.load(std::memory_order_relaxed);
      auto beatOf = [](const MidiFile::Event& e) { return e.beat; };

      const float position = std::clamp(mPosition.load(std::memory_order_relaxed), 0.0f, 1.0f);
      double t0 = 0.0, t1 = 0.0, dBeatsBlock = dBeats;
      if (mLock.load(std::memory_order_relaxed))
      {
         // Locked: song beat = (transport seconds - offset) * bpm / 60, real
         // time (speed does not apply). Contiguous blocks carry on from the
         // last block's end so no event is dropped or doubled; a transport
         // jump (start, seek, loop lap - ResetEpoch) or a drift past 10 ms
         // re-seats it. Before the song's start nothing plays.
         Transport& tr = Transport::Instance();
         const double clock = tr.AudioSampleRate() > 0.0 ? tr.BlockStartSeconds() : tr.Seconds();
         const double target = (clock - (double)mOffsetSeconds.load(std::memory_order_relaxed)) * bpm / 60.0 +
                               (double)position * len;
         const unsigned long long epoch = tr.ResetEpoch();
         const bool jumped = mLastTransport < 0.0 || epoch != mLastEpoch || std::fabs(target - mPlay) > 0.010 * bpm / 60.0;
         mLastTransport = 1.0;
         mLastEpoch = epoch;
         if (jumped)
         {
            ReleaseAll(0);
            mPlay = target;
         }
         dBeatsBlock = dTransport;
         t0 = mPlay;
         t1 = t0 + dBeatsBlock;
         mPlay = t1; // unwrapped: the loop is applied per event below
         mPlayhead.store(loop && t0 >= 0.0 ? std::fmod(t0, len) : t0, std::memory_order_relaxed);
      }
      else
      {
         // The node keeps its own playhead. It starts at `position` (0 = file start, 1 = file end) whenever the
         // transport starts, seeks or loops back, and whenever the position knob moves; otherwise it runs on.
         const double tStart = Transport::Instance().BlockStartBeats(); // the clock was advanced before the graph ran
         const bool jumped = mLastTransport < 0.0 || tStart < mLastTransport - 1e-6 ||
                             tStart > mLastTransport + dTransport * 4.0 + 0.05 || std::fabs(position - mLastPosition) > 1e-6f;
         mLastTransport = tStart + dTransport;
         if (jumped)
         {
            ReleaseAll(0);
            mPlay = (double)position * len;
         }
         t0 = mPlay;
         t1 = t0 + dBeats;
         mPlayhead.store(t0, std::memory_order_relaxed);
         mPlay = loop ? std::fmod(t1, len) : t1;
      }
      mLastPosition = position;
      if (t1 <= 0.0)
         return; // locked, before the song starts

      const auto& ev = song->events;
      const long long k0 = loop ? std::max(0LL, (long long)std::floor(t0 / len)) : 0;
      const long long k1 = loop ? (long long)std::floor(t1 / len) : 0;
      for (long long k = k0; k <= k1; k++)
      {
         const double base = loop ? (double)k * len : 0.0;
         // The file wrapped inside this block: whatever is still held must not ring into the restart.
         if (k > k0)
            ReleaseAll(std::clamp((int)((base - t0) / dBeatsBlock * (double)numFrames), 0, numFrames - 1));
         const double lo = std::max(0.0, t0 - base);
         const double hi = loop ? std::min(len, t1 - base) : t1;
         auto it = std::lower_bound(ev.begin(), ev.end(), lo, [&](const MidiFile::Event& e, double b) { return beatOf(e) < b; });
         for (; it != ev.end() && beatOf(*it) < hi; ++it)
         {
            const double abs = base + beatOf(*it);
            const int offset = std::clamp((int)((abs - t0) / dBeatsBlock * (double)numFrames), 0, numFrames - 1);
            if (it->on)
            {
               NoteEvent on;
               // General MIDI percussion (channel 10) is a drum map, not pitch: transposing it would change the instrument.
               on.note = std::clamp((int)it->note + (it->channel == 9 ? 0 : transpose), 0, 127);
               on.velocity = std::clamp((float)it->velocity / 127.0f * velScale, 0.0f, 1.0f);
               on.isNoteOn = true;
               on.frameOffset = offset;
               on.source = this;
               on.voiceId = NextVoiceId();
               if (mHeldCount < kMaxHeld)
               {
                  mHeld[mHeldCount++] = { it->note, it->track, (uint8_t)on.note, on.voiceId };
                  mOutbox.Push(on);
               }
            }
            else
            {
               for (int i = 0; i < mHeldCount; i++)
                  if (mHeld[i].note == it->note && mHeld[i].track == it->track)
                  {
                     PushOff(mHeld[i], offset);
                     mHeld[i] = mHeld[--mHeldCount];
                     break;
                  }
            }
         }
      }
   }

   NoteEventQueue* NoteOutbox() override { return &mOutbox; }

   // Main thread only.
   void PushSong(const MidiFile::Song* song, double loopBeats, int transpose, float position, bool loop, float speed,
                 float velocity, bool lockToTransport = false, float offsetSeconds = 0.0f)
   {
      mLock.store(lockToTransport, std::memory_order_relaxed);
      mOffsetSeconds.store(offsetSeconds, std::memory_order_relaxed);
      mTranspose.store(transpose, std::memory_order_relaxed);
      mPosition.store(position, std::memory_order_relaxed);
      mLoop.store(loop, std::memory_order_relaxed);
      mSpeed.store(speed, std::memory_order_relaxed);
      mVelocity.store(velocity, std::memory_order_relaxed);
      mLoopBeats.store((float)loopBeats, std::memory_order_relaxed);
      mSong.store(song, std::memory_order_release);
   }
   double Playhead() const { return mPlayhead.load(std::memory_order_relaxed); }

private:
   static constexpr int kMaxHeld = 64;
   struct Held { uint8_t note, track, outNote; int voiceId; };

   void PushOff(const Held& h, int offset)
   {
      NoteEvent off;
      off.note = h.outNote;
      off.velocity = 0.0f;
      off.isNoteOn = false;
      off.frameOffset = offset;
      off.source = this;
      off.voiceId = h.voiceId;
      mOutbox.Push(off);
   }
   void ReleaseAll(int offset)
   {
      for (int i = 0; i < mHeldCount; i++)
         PushOff(mHeld[i], offset);
      mHeldCount = 0;
   }

   NoteEventQueue mOutbox;
   double mSampleRate = 48000.0;
   double mLastTransport = -1.0;
   unsigned long long mLastEpoch = 0;
   float mLastPosition = 0.0f;
   double mPlay = 0.0; // file position in beats
   Held mHeld[kMaxHeld] = {};
   int mHeldCount = 0;
   std::atomic<const MidiFile::Song*> mSong { nullptr };
   std::atomic<int> mTranspose { 0 };
   std::atomic<float> mPosition { 0.0f };
   std::atomic<bool> mLoop { true };
   std::atomic<float> mSpeed { 1.0f };
   std::atomic<float> mVelocity { 1.0f };
   std::atomic<float> mLoopBeats { 4.0f };
   std::atomic<double> mPlayhead { -1.0 };
   std::atomic<bool> mLock { false };
   std::atomic<float> mOffsetSeconds { 0.0f };
};
