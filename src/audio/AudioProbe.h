#pragma once

#include <atomic>
#include <cstdint>

#include "AudioBuffer.h"
#include "AudioCaptureRing.h"

class AudioNode;

// Live tap on up to kSlots nodes' audio output, for measuring sound as numbers
// (the assistant's analyze_audio, app/graph/AudioProbeJobs.cpp). The audio
// thread copies each armed node's first output, as interleaved stereo, into
// that slot's AudioCaptureRing right after the node processes; the main
// thread drains the rings every frame.
//
// All slots are armed together and the audio thread latches "armed" once per
// block (BeginBlock), so every slot starts on the same block - two nodes
// captured together line up sample for sample, which is what makes comparing
// them meaningful.
//
// Cost when nothing is armed: one relaxed load per block plus one bool test
// per processed node.
class AudioProbe
{
public:
   static constexpr int kSlots = 4;
   static constexpr int kMaxBlockFrames = 4096; // == kAudioMaxBlockFrames (checked in the .cpp)

   static AudioProbe& Instance();

   // Main thread. Replaces whatever was armed; nullptr entries leave a slot
   // unused. Leftover samples from an earlier capture are discarded first.
   void Arm(const AudioNode* const* nodes, int count);
   void Disarm();
   bool IsArmed() const { return mArmed.load(std::memory_order_relaxed); }

   // Main thread: up to maxCount floats (whole L,R pairs) from a slot.
   int Drain(int slot, float* out, int maxCount);
   // Main thread: samples dropped because a slot's ring was full since the last call.
   uint64_t TakeOverflows(int slot);
   // Sample rate of the most recent captured block (0 until one arrives).
   double SampleRate() const { return mSampleRate.load(std::memory_order_relaxed); }

   // Main thread. While set, the probe taps AudioEngine::ProcessOffline (an
   // offline capture renders the graph itself with the device stopped)
   // instead of the device callback. Only flipped with the device closed.
   void SetOffline(bool offline) { mOffline.store(offline, std::memory_order_relaxed); }
   bool Offline() const { return mOffline.load(std::memory_order_relaxed); }

   // Audio thread (RunTopology), once per block before any node runs.
   void BeginBlock(double sampleRate);
   // Audio thread, after `node` processed into `out` (may be null).
   void OnNodeProcessed(const AudioNode* node, const AudioBuffer* out, int numFrames);

private:
   struct Slot
   {
      std::atomic<const AudioNode*> node{ nullptr };
      AudioCaptureRing ring;
      float scratch[kMaxBlockFrames * 2] = {};
   };

   Slot mSlots[kSlots];
   std::atomic<bool> mArmed{ false };
   std::atomic<double> mSampleRate{ 0.0 };
   std::atomic<bool> mOffline{ false };
   bool mActiveThisBlock = false; // audio thread only
};
