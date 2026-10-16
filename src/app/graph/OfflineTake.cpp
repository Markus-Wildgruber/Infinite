// One shared shape for every audio-only offline render: render to track,
// render to file, the assistant's offline capture (AudioProbeJobs.cpp), note
// capture (ArrangeMidi.cpp) and the timeline's WAV render (ArrangeRender.cpp).
// Each of those used to borrow the device and transport with its own copy of
// the same steps, and two things were wrong in all of them:
//
//  1. A take did not start from a fresh state. The offline capture skipped the
//     re-prepare entirely (with the device closed RebuildAudioTopology prepares
//     at gHeadlessAudioRate, which was 0, so nothing was touched): a pad that
//     had been sounding live was already at -17 dB in the first block of a
//     "fresh" render from 0, and a Note Sequencer whose last fired step was 0
//     never fired step 0 again (silence). OfflineTakePrepare re-prepares every
//     node at the take's rate (voices, envelopes, delay/reverb tails, step
//     clocks), with gHeadlessAudioRate carrying that rate while the device is
//     closed; transport-locked generators re-derive their phase from the
//     transport on the seek's ResetEpoch bump.
//
//  2. Modulation did not follow the render. Modulators are evaluated and
//     parameters pushed to the audio nodes once per UI frame
//     (ApplyModulationAndPalette + CookIfNeeded), while the pumps render ~60 ms
//     of wall time worth of blocks per frame - seconds of audio. An LFO on a
//     filter moved once or twice across a ten-second render (measured: two
//     distinct centroid values over 10 s). The video render does not have the
//     problem because StageFramePump steps modulation + cook once per video
//     frame, in lockstep with ProcessOffline. OfflineTakeRender does the same
//     on the audio clock: before any block that starts at or past the next
//     step (every 1/kModulationHz s of rendered audio) it sets the transport's
//     offline video time to that block's start, applies modulation and cooks
//     the audio-side nodes. Steps fall on fixed frame counts, so two takes of
//     one patch are identical.
//
// Main thread only (everything here runs from the frame pump, like the video
// render's own lockstep), with the device closed for the whole take.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppArrange.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioEngine.h"
#include "audio/AudioProbe.h"
#include "core/Modulation.h"
#include "core/Transport.h"

namespace app
{
   namespace
   {
      // Modulation/cook cadence of an audio-only take: the same 60 steps per
      // second a 60 fps UI frame (and the video render's default fps) gives a
      // live patch.
      constexpr double kModulationHz = 60.0;

      int* gFrameCounter = nullptr; // DrawFramePump's frameId, bound every frame
      std::vector<uint64_t> gForcedUids; // nodes the running take taps (RenderToTrackForcedNodes)

      int NextFrameId()
      {
         // The main loop's own counter, like the video render's ++frameId: a
         // cook memo keyed on frame ids never sees an id twice. Without a
         // bound counter (a test driving a take before the first frame) a
         // private range far above any real frame id.
         if (gFrameCounter != nullptr)
            return ++*gFrameCounter;
         static int sPrivate = 1 << 28;
         return ++sPrivate;
      }

      bool CooksWithAudio(INode* node)
      {
         return dynamic_cast<IAudioSource*>(node) != nullptr || dynamic_cast<INoteSource*>(node) != nullptr ||
                dynamic_cast<IModulator*>(node) != nullptr || node->AudioNodeForNotePorts() != nullptr;
      }
   }


   void OfflineTakeBindFrameCounter(int* frameId) { gFrameCounter = frameId; }


   void OfflineTakeForceNodes(const std::vector<uint64_t>& uids) { gForcedUids = uids; }
   const std::vector<uint64_t>& OfflineTakeForcedUids() { return gForcedUids; }


   double OfflineTakeRate()
   {
      // The rate the graph is (or will be) prepared at: a running device's
      // rate is the only one its nodes know; with no device, the headless
      // rate, else the configured rate - no device is opened just for a take.
      const double engine = AudioEngine::Instance().SampleRate();
      if (engine > 0.0)
         return engine;
      if (gHeadlessAudioRate > 0.0)
         return gHeadlessAudioRate;
      const double configured = ArrangeRenderActiveSampleRate();
      return configured > 0.0 ? configured : 48000.0;
   }


   int OfflineTakeBlockFrames()
   {
      return std::clamp(OfflineAudioBlockFrames(), 32, std::min(kAudioMaxBlockFrames, AudioProbe::kMaxBlockFrames));
   }


   void OfflineTakeBegin(OfflineTake& take, double rate, double startSeconds, double startBeats)
   {
      take = OfflineTake();
      take.active = true;
      gForcedUids.clear();
      take.rate = rate > 0.0 ? rate : 48000.0;
      take.deviceWasRunning = AudioEngine::Instance().SampleRate() > 0.0;
      take.wasPlaying = Transport::Instance().IsPlaying();
      take.savedSeconds = Transport::Instance().Seconds();
      take.savedHeadlessRate = gHeadlessAudioRate;
      take.vsyncWasOn = gVsync;
      take.modStepFrames = std::max(1, (int)std::llround(take.rate / kModulationHz));

      // Device off first: its callback must not race ProcessOffline. Then the
      // take's rate becomes the one RebuildAudioTopology prepares at.
      if (take.deviceWasRunning)
         AudioEngine::Instance().Stop();
      gHeadlessAudioRate = take.rate;
      SetCanvasSwapInterval(0);
      // The seek bumps ResetEpoch: every transport-locked generator forgets
      // its last step and re-derives its phase from here.
      if (startBeats >= 0.0)
         Transport::Instance().SeekBeats(startBeats);
      else
         Transport::Instance().Seek(std::max(0.0, startSeconds));
      Transport::Instance().SetOfflineMode(true, take.rate); // also suspends the loop
      Transport::Instance().SetPlaying(true);
      // Outside an audio block Seconds() is the offline video time, which the
      // seek just set to the start.
      take.startSeconds = Transport::Instance().Seconds();
   }


   void OfflineTakeModulationStep(OfflineTake& take)
   {
      if (!take.active)
         return;
      // The step is evaluated at the start of the block about to render.
      Transport::Instance().SetOfflineVideoTime(take.startSeconds + (double)take.framesDone / take.rate);
      const int frameId = NextFrameId();
      ApplyModulationAndPalette(frameId);
      // Audio, note and modulator nodes push their (now modulated) parameters
      // to their audio halves in CookIfNeeded. Picture-only nodes are left to
      // the UI frame: an image a modulator reads is pulled through its cable.
      for (GraphNode& gn : gNodes)
         if (gn.node != nullptr && !gn.node->bypassed && CooksWithAudio(gn.node.get()))
            gn.node->CookIfNeeded(frameId);
   }


   void OfflineTakePrepare(OfflineTake& take)
   {
      // Parameters (with modulation at the start time) first, so the
      // re-prepare below starts every smoother on them; then every node from
      // its prepared state; then once more, since the rebuild may have given a
      // node a fresh audio half.
      OfflineTakeModulationStep(take);
      ForceAudioRepare();
      RebuildAudioTopology();
      OfflineTakeModulationStep(take);
      // ...and again before the first block: a take started from a frame
      // stage that has no registered parameters yet (a self-test runs before
      // the node bodies draw) still gets modulation at frame 0 from the pump.
      take.nextModFrame = 0;
   }


   void OfflineTakeRender(OfflineTake& take, float* left, float* right, int frames)
   {
      if (!take.active || frames <= 0)
         return;
      if (take.framesDone >= take.nextModFrame)
      {
         OfflineTakeModulationStep(take);
         while (take.nextModFrame <= take.framesDone)
            take.nextModFrame += take.modStepFrames;
      }
      float* channels[2] = { left, right };
      AudioBuffer buf;
      buf.channels = channels;
      buf.numChannels = 2;
      buf.numFrames = frames;
      AudioEngine::Instance().ProcessOffline(buf);
      take.framesDone += frames;
      // Keeps the drawn playhead and anything reading video time honest
      // between steps (the UI frame cooks too).
      Transport::Instance().SetOfflineVideoTime(take.startSeconds + (double)take.framesDone / take.rate);
   }


   void OfflineTakeRestore(OfflineTake& take, double seekSeconds)
   {
      if (!take.active)
         return;
      take.active = false;
      gForcedUids.clear(); // the rebuilds below drop the forced taps
      Transport::Instance().SetOfflineMode(false);
      Transport::Instance().Seek(seekSeconds >= 0.0 ? seekSeconds : take.savedSeconds);
      gHeadlessAudioRate = take.savedHeadlessRate;
      // Live playback starts clean too, not inside the take's last voices and
      // tails (a take at the device rate would otherwise skip the re-prepare).
      ForceAudioRepare();
      if (take.deviceWasRunning)
         StartAudioEngine(gAudioStartError);
      else
         Transport::Instance().NotifyAudioEngineStopped();
      Transport::Instance().SetPlaying(take.wasPlaying);
      SetCanvasSwapInterval(take.vsyncWasOn ? 1 : 0);
      RebuildAudioTopology();
   }
}
