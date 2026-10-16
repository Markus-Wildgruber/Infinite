#pragma once

#include "INode.h"

// What a cable reads while its source's group is muted (app/graph/Groups.cpp):
// a shared 1x1 transparent-black texture the app creates on the GL thread the
// first time any group closes. 0 until then (reads as "no input").
inline unsigned int gImageCableMutedTexture = 0;

// Typed patch cable carrying a texture handle between two nodes.
//
// The cable is typed at compile time: it only ever holds an INode*, so an
// image input can never be handed an audio or geometry source, and a consumer
// never has to discover what it is connected to by casting at cook time.
class ImageCable
{
public:
   // outputIndex selects which of the source's image outputs this cable
   // reads (build step 11, §5.3) - 0 for every pre-existing call site,
   // matching the pre-step-11 behaviour exactly.
   void Connect(INode* source, int outputIndex = 0) { mSource = source; mSourceOutput = outputIndex; }
   void Disconnect() { mSource = nullptr; mSourceOutput = 0; }
   INode* GetSource() const { return mSource; }
   int GetSourceOutput() const { return mSourceOutput; }
   bool IsConnected() const { return mSource != nullptr; }

   // Walks past bypassed nodes to the first one that is actually enabled. The
   // hop limit stops a bypassed feedback cycle from spinning forever.
   INode* Resolved() const
   {
      INode* node = mSource;
      for (int hops = 0; node != nullptr && node->bypassed && hops < 64; hops++)
         node = node->BypassSource();
      return (node != nullptr && node->bypassed) ? nullptr : node;
   }

   // Group mute gate, main thread only. Set every frame by the app
   // (UpdateGroupGates) on a cable whose source sits in a group the consumer
   // does not belong to: while *closed is true the source still cooks (a
   // muted group keeps running, so unmuting is instant), but the consumer
   // sees gImageCableMutedTexture instead of its pixels. nullptr = ungated.
   void SetGroupGate(const bool* closed) { mGateClosed = closed; }
   bool GroupGateClosed() const { return mGateClosed != nullptr && *mGateClosed; }

   // Pulls the upstream node's output for this frame, cooking it if needed.
   unsigned int Pull(int frameId)
   {
      INode* node = Resolved();
      if (node == nullptr)
         return 0;
      node->CookIfNeeded(frameId);
      if (GroupGateClosed())
         return gImageCableMutedTexture;
      return node->GetOutputTexture(ResolvedOutput(node));
   }

   // The texture this cable resolves to, without cooking. Every read of an
   // input's pixels goes through here or Pull(), never GetSource(): the raw
   // source may be bypassed, and a bypassed node no longer cooks, so its own
   // texture is whatever it last rendered - a frozen frame.
   unsigned int Texture() const
   {
      INode* node = Resolved();
      if (node != nullptr && GroupGateClosed())
         return gImageCableMutedTexture;
      return node ? node->GetOutputTexture(ResolvedOutput(node)) : 0;
   }

   int Width() const
   {
      INode* node = Resolved();
      return node ? node->GetOutputWidth() : 0;
   }
   int Height() const
   {
      INode* node = Resolved();
      return node ? node->GetOutputHeight() : 0;
   }

   // Current revision of whatever this cable resolves to. Call after Pull()
   // has cooked it this frame, so a node that just re-rendered has already
   // bumped its stamp before this is read for a downstream cache signature.
   unsigned long long Revision() const
   {
      INode* node = Resolved();
      // A constant while gated, distinct from any real stamp, so a consumer
      // that caches on its input revision re-renders once on mute and once
      // on unmute, and not every frame in between.
      if (node != nullptr && GroupGateClosed())
         return 0x6D7574656400000Full;
      return node ? node->TextureRevision() : 0;
   }

private:
   // A hop past a bypassed node lands on a different node, whose output
   // numbering has nothing to do with the one this cable chose - so it reads
   // that node's main output, as the audio resolver already does.
   int ResolvedOutput(INode* node) const { return node == mSource ? mSourceOutput : 0; }

   INode* mSource = nullptr;
   int mSourceOutput = 0;
   const bool* mGateClosed = nullptr;
};
