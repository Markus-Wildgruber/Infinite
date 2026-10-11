#include "AudioProbe.h"

#include <algorithm>
#include <vector>

#include "AudioEngine.h"

static_assert(AudioProbe::kMaxBlockFrames == kAudioMaxBlockFrames, "probe scratch must hold one engine block");

namespace
{
   // Namespace-scope rather than a function-local static: the audio thread
   // may be the first caller, and a function-local static would initialise
   // (zero ~3 MB of rings, behind a guard lock) on the real-time thread.
   AudioProbe gProbe;
}

AudioProbe& AudioProbe::Instance()
{
   return gProbe;
}

void AudioProbe::Arm(const AudioNode* const* nodes, int count)
{
   Disarm();
   // Nothing writes a slot whose node is null, so the rings can be emptied.
   std::vector<float> sink(8192);
   for (Slot& s : mSlots)
   {
      while (s.ring.Read(sink.data(), (int)sink.size()) > 0)
      {
      }
      s.ring.overflowCount.store(0, std::memory_order_relaxed);
   }
   for (int i = 0; i < kSlots; i++)
      mSlots[i].node.store(i < count ? nodes[i] : nullptr, std::memory_order_relaxed);
   // Published last: BeginBlock acquires this, so every slot's node is
   // visible from the same block on.
   mArmed.store(true, std::memory_order_release);
}

void AudioProbe::Disarm()
{
   mArmed.store(false, std::memory_order_release);
   for (Slot& s : mSlots)
      s.node.store(nullptr, std::memory_order_relaxed);
}

int AudioProbe::Drain(int slot, float* out, int maxCount)
{
   if (slot < 0 || slot >= kSlots)
      return 0;
   return mSlots[slot].ring.Read(out, maxCount);
}

uint64_t AudioProbe::TakeOverflows(int slot)
{
   if (slot < 0 || slot >= kSlots)
      return 0;
   return mSlots[slot].ring.overflowCount.exchange(0, std::memory_order_relaxed);
}

void AudioProbe::BeginBlock(double sampleRate)
{
   mActiveThisBlock = mArmed.load(std::memory_order_acquire);
   if (mActiveThisBlock && sampleRate > 0.0)
      mSampleRate.store(sampleRate, std::memory_order_relaxed);
}

void AudioProbe::OnNodeProcessed(const AudioNode* node, const AudioBuffer* out, int numFrames)
{
   if (!mActiveThisBlock || node == nullptr)
      return;
   numFrames = std::min(numFrames, kMaxBlockFrames);
   for (Slot& s : mSlots)
   {
      if (s.node.load(std::memory_order_relaxed) != node)
         continue;
      // A node with no output buffer this block still advances time: write
      // silence so every slot keeps the same length.
      const float* l = (out != nullptr && out->numChannels > 0) ? out->channels[0] : nullptr;
      const float* r = (out != nullptr && out->numChannels > 1) ? out->channels[1] : l;
      for (int i = 0; i < numFrames; i++)
      {
         s.scratch[2 * i] = l != nullptr ? l[i] : 0.0f;
         s.scratch[2 * i + 1] = r != nullptr ? r[i] : 0.0f;
      }
      s.ring.Write(s.scratch, numFrames * 2);
   }
}
