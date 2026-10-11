#include "Transport.h"

#include <cmath>

Transport& Transport::Instance()
{
   static Transport instance;
   return instance;
}

double Transport::Seconds() const
{
   const double pending = mPendingSeekSeconds.load(std::memory_order_relaxed);
   if (pending >= 0.0)
      return pending;

   if (mOfflineActive.load(std::memory_order_relaxed))
   {
      if (mOfflineInAudioBlock.load(std::memory_order_relaxed))
      {
         const double sr = mAudioSampleRate.load(std::memory_order_relaxed);
         if (sr > 0.0)
         {
            return mAudioSecondsOffset.load(std::memory_order_relaxed) +
                   (double)mAudioSampleCounter.load(std::memory_order_relaxed) / sr;
         }
      }
      return mOfflineVideoSeconds.load(std::memory_order_relaxed);
   }

   const double sr = mAudioSampleRate.load(std::memory_order_relaxed);
   if (sr > 0.0)
   {
      return mAudioSecondsOffset.load(std::memory_order_relaxed) +
             (double)mAudioSampleCounter.load(std::memory_order_relaxed) / sr;
   }
   return mSeconds;
}

double Transport::Beats() const
{
   const double pending = mPendingSeekSeconds.load(std::memory_order_relaxed);
   if (pending >= 0.0)
      return pending * (mBpm.load(std::memory_order_relaxed) / 60.0);

   const double sr = mAudioSampleRate.load(std::memory_order_relaxed);
   if (sr <= 0.0 && !mOfflineActive.load(std::memory_order_relaxed))
      return mBeats;
   const double secOffset = mAudioSecondsOffset.load(std::memory_order_relaxed);
   return mAudioBeatsOffset.load(std::memory_order_relaxed) +
          (Seconds() - secOffset) * (mBpm.load(std::memory_order_relaxed) / 60.0);
}

void Transport::SetTempo(float bpm)
{
   mPendingBpm.store(bpm < 1.0f ? 1.0f : bpm, std::memory_order_relaxed);
}

void Transport::ApplyPendingTempo()
{
   const float pending = mPendingBpm.exchange(-1.0f, std::memory_order_relaxed);
   if (pending <= 0.0f || pending == mBpm.load(std::memory_order_relaxed))
      return;

   // Rebase first: pin the offsets to where the clock is *now*, under the old
   // bpm, so the new bpm only affects time from this instant forward. Without
   // this the whole elapsed span gets re-measured at the new tempo and the
   // playhead teleports.
   const double beats = Beats();
   const double secs = Seconds();
   mAudioBeatsOffset.store(beats, std::memory_order_relaxed);
   mAudioSecondsOffset.store(secs, std::memory_order_relaxed);
   mAudioSampleCounter.store(0, std::memory_order_relaxed);
   // mBeats/mSeconds are the non-atomic fallback pair, owned by the main
   // thread. Only touch them on the path where no audio clock is live -
   // otherwise this runs on the audio thread and would race Tick().
   if (mAudioSampleRate.load(std::memory_order_relaxed) <= 0.0 &&
       !mOfflineActive.load(std::memory_order_relaxed))
   {
      mBeats = beats;
      mSeconds = secs;
   }
   mBpm.store(pending, std::memory_order_relaxed); // publish last
}

double Transport::CommitTempoForSeek()
{
   // A seek re-seats both offsets from scratch, so there is nothing to rebase
   // - the staged tempo can just land, and the seek is measured with it.
   const float pending = mPendingBpm.exchange(-1.0f, std::memory_order_relaxed);
   if (pending > 0.0f)
      mBpm.store(pending, std::memory_order_relaxed);
   return (double)mBpm.load(std::memory_order_relaxed);
}

void Transport::Tick(float deltaSeconds)
{
   // clamp so a stalled frame (window drag, file dialog) doesn't jump the clock
   if (deltaSeconds > 0.25f)
      deltaSeconds = 0.25f;

   if (mOfflineActive.load(std::memory_order_relaxed))
      return; // offline mode: video frame time is driven explicitly by SetOfflineVideoTime

   if (mAudioSampleRate.load(std::memory_order_relaxed) > 0.0)
      return; // audio-driven: Beats()/Seconds() compute live, nothing to accumulate here

   ApplyPendingTempo(); // applies while paused too - tempo isn't a transport move

   if (!mPlaying.load(std::memory_order_relaxed))
      return;

   mSeconds += deltaSeconds;
   mBeats += deltaSeconds * (mBpm.load(std::memory_order_relaxed) / 60.0);

   WrapLoopIfNeeded();
}

void Transport::SetLoop(bool enabled, double beatStart, double beatEnd)
{
   if (beatStart < 0.0)
      beatStart = 0.0;
   if (beatEnd < beatStart)
      beatEnd = beatStart;
   mLoopStartBeats.store(beatStart, std::memory_order_relaxed);
   mLoopEndBeats.store(beatEnd, std::memory_order_relaxed);
   mLoopEnabled.store(enabled && (beatEnd > beatStart), std::memory_order_relaxed); // publish last
}

void Transport::WrapLoopIfNeeded()
{
   if (!mLoopEnabled.load(std::memory_order_relaxed) ||
       mLoopSuspended.load(std::memory_order_relaxed) ||
       !mPlaying.load(std::memory_order_relaxed))
      return;

   const double loopStart = mLoopStartBeats.load(std::memory_order_relaxed);
   const double loopEnd = mLoopEndBeats.load(std::memory_order_relaxed);
   const double span = loopEnd - loopStart;
   if (!(span > 0.0))
      return;

   const double beats = Beats();
   if (beats < loopEnd)
      return;

   // fmod, not "snap to loopStart": the block that crossed the end carries an
   // overshoot, and dropping it would make every lap a fraction of a block
   // short. fmod also covers the case where the loop was shrunk under a
   // playhead that is now several spans past the end.
   const double wrapped = loopStart + std::fmod(beats - loopStart, span);
   const double bpm = mBpm.load(std::memory_order_relaxed);
   const double seconds = Seconds() - (beats - wrapped) * (60.0 / bpm);

   mAudioBeatsOffset.store(wrapped, std::memory_order_relaxed);
   mAudioSecondsOffset.store(seconds, std::memory_order_relaxed);
   mAudioSampleCounter.store(0, std::memory_order_relaxed);
   if (mAudioSampleRate.load(std::memory_order_relaxed) <= 0.0 &&
       !mOfflineActive.load(std::memory_order_relaxed))
   {
      mBeats = wrapped;
      mSeconds = seconds;
   }
   // Same epoch bump the old panel-side Seek() produced, so anything that
   // retriggers on a transport jump still does on every lap.
   mResetEpoch.fetch_add(1, std::memory_order_relaxed);
}

void Transport::NotifyAudioEngineStarted(double sampleRate)
{
   mAudioSecondsOffset.store(mSeconds, std::memory_order_relaxed);
   mAudioBeatsOffset.store(mBeats, std::memory_order_relaxed);
   mAudioSampleCounter.store(0, std::memory_order_relaxed);
   mAudioSampleRate.store(sampleRate, std::memory_order_relaxed); // publish last
}

void Transport::NotifyAudioEngineStopped()
{
   mSeconds = Seconds(); // read the still-live audio-driven value...
   mBeats = Beats();
   mAudioSampleRate.store(0.0, std::memory_order_relaxed); // ...then switch back to fallback
   mPendingSeekSeconds.store(-1.0, std::memory_order_relaxed);
}

void Transport::AdvanceAudioClock(int numFrames)
{
   const double pending = mPendingSeekSeconds.exchange(-1.0, std::memory_order_relaxed);
   if (pending >= 0.0)
   {
      mAudioSecondsOffset.store(pending, std::memory_order_relaxed);
      mAudioBeatsOffset.store(pending * (mBpm.load(std::memory_order_relaxed) / 60.0), std::memory_order_relaxed);
      mAudioSampleCounter.store(0, std::memory_order_relaxed);
   }
   ApplyPendingTempo(); // after the seek consumed its offsets, before we advance
   mBlockStartBeats.store(Beats(), std::memory_order_relaxed); // before the advance AND before any wrap
   mBlockStartSeconds.store(Seconds(), std::memory_order_relaxed);
   if (mPlaying.load(std::memory_order_relaxed))
      mAudioSampleCounter.fetch_add((uint64_t)numFrames, std::memory_order_relaxed);
   WrapLoopIfNeeded();
}

void Transport::Rewind()
{
   Seek(0.0);
}

void Transport::Seek(double seconds)
{
   if (seconds < 0.0)
      seconds = 0.0;
   const double bpm = CommitTempoForSeek();
   SeekInternal(seconds, seconds * (bpm / 60.0));
}

void Transport::SeekBeats(double beats)
{
   if (beats < 0.0)
      beats = 0.0;
   const double bpm = CommitTempoForSeek();
   SeekInternal(beats * (60.0 / bpm), beats);
}

void Transport::SeekInternal(double seconds, double beats)
{
   mResetEpoch.fetch_add(1, std::memory_order_relaxed);
   mSeconds = seconds;
   mBeats = beats;
   mOfflineVideoSeconds.store(seconds, std::memory_order_relaxed);
   if (mOfflineActive.load(std::memory_order_relaxed))
   {
      mPendingSeekSeconds.store(-1.0, std::memory_order_relaxed);
      mAudioSampleCounter.store(0, std::memory_order_relaxed);
      mAudioSecondsOffset.store(seconds, std::memory_order_relaxed);
      mAudioBeatsOffset.store(mBeats, std::memory_order_relaxed);
   }
   else if (mAudioSampleRate.load(std::memory_order_relaxed) > 0.0)
   {
      mPendingSeekSeconds.store(seconds, std::memory_order_relaxed);
   }
   else
   {
      mPendingSeekSeconds.store(-1.0, std::memory_order_relaxed);
      mAudioSampleCounter.store(0, std::memory_order_relaxed);
      mAudioSecondsOffset.store(seconds, std::memory_order_relaxed);
      mAudioBeatsOffset.store(mBeats, std::memory_order_relaxed);
   }
}

void Transport::SetOfflineMode(bool active, double sampleRate)
{
   if (active)
   {
      mPendingSeekSeconds.store(-1.0, std::memory_order_relaxed);
      mAudioSecondsOffset.store(mSeconds, std::memory_order_relaxed);
      mAudioBeatsOffset.store(mBeats, std::memory_order_relaxed);
      mAudioSampleCounter.store(0, std::memory_order_relaxed);
      mAudioSampleRate.store(sampleRate, std::memory_order_relaxed);
      mOfflineVideoSeconds.store(mSeconds, std::memory_order_relaxed);
      mOfflineInAudioBlock.store(false, std::memory_order_relaxed);
      // An offline render walks the range once; leaving the loop live would
      // wrap it back and render the loop body until the frame budget ran out.
      mLoopSuspended.store(true, std::memory_order_relaxed);
      mOfflineActive.store(true, std::memory_order_relaxed);
   }
   else
   {
      mPendingSeekSeconds.store(-1.0, std::memory_order_relaxed);
      mOfflineActive.store(false, std::memory_order_relaxed);
      mOfflineInAudioBlock.store(false, std::memory_order_relaxed);
      mLoopSuspended.store(false, std::memory_order_relaxed); // the user's loop comes back
   }
}

void Transport::SetOfflineVideoTime(double seconds)
{
   mOfflineVideoSeconds.store(seconds, std::memory_order_relaxed);
}

void Transport::BeginOfflineAudioBlock(int numFrames)
{
   const double pending = mPendingSeekSeconds.exchange(-1.0, std::memory_order_relaxed);
   if (pending >= 0.0)
   {
      mAudioSecondsOffset.store(pending, std::memory_order_relaxed);
      mAudioBeatsOffset.store(pending * (mBpm.load(std::memory_order_relaxed) / 60.0), std::memory_order_relaxed);
      mAudioSampleCounter.store(0, std::memory_order_relaxed);
   }
   ApplyPendingTempo();
   // Enter the block BEFORE reading Beats(): outside an offline audio block
   // Seconds() deliberately reports the renderer's video time, not the audio
   // clock, so capturing the block start any earlier would read the frame's
   // timestamp instead of the sample position. See AdvanceAudioClock for why
   // the capture has to happen before the counter advances.
   mOfflineInAudioBlock.store(true, std::memory_order_relaxed);
   mBlockStartBeats.store(Beats(), std::memory_order_relaxed);
   mBlockStartSeconds.store(Seconds(), std::memory_order_relaxed);
   if (mPlaying.load(std::memory_order_relaxed))
      mAudioSampleCounter.fetch_add((uint64_t)numFrames, std::memory_order_relaxed);
}

void Transport::EndOfflineAudioBlock()
{
   mOfflineInAudioBlock.store(false, std::memory_order_relaxed);
}
