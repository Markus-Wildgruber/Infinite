#pragma once
// Self-test entry points and the audio-sweep discovery helpers they share.
// Split out of AppShared.h so the DSP/effect headers the fixtures need are
// not in every app/ TU (docs/plans/compile-times/include-split.md, Phase 3).
#include "app/AppShared.h"
#include "audio/AudioBuffer.h"
#include "audio/EffectDefs.h"
#include "nodes/AudioEffectNode.h"

namespace app
{
// ================================================== Audio node sweep discovery
//
// Shared by INFINITE_AUDIOPARAMSWEEPTEST and INFINITE_AUDIOTEARDOWNSWEEPTEST:
// every node type is read out of NodeFactory - the same registry
// RegisterNodes() populates - rather than a hand-maintained list, so a node
// added later (any remaining row of docs/plans/audio/README.md §3) is
// covered by both sweeps without anyone editing this file. Which of a node's
// interfaces it answers (IAudioSource, INoteSource, AudioNodeForNotePorts(),
// AudioInputSlot/NoteInputSlot) is probed the same way InputCountFor already
// probes AudioInputSlot/NoteInputSlot generically for pin counts - see
// .claude/skills/new-audio-node/SKILL.md §3's step 9 note that audio and note
// pins share one slot index space.
struct AudioNodeShape
{
   bool isAudioSource = false;
   bool isNoteSource = false;
   bool isModulator = false;
   bool hasNotePorts = false; // AudioNodeForNotePorts() != nullptr (NoteToCVNode)
   int audioInputSlots = 0;   // highest AudioInputSlot(i) != nullptr index + 1
   int noteInputSlots = 0;    // highest NoteInputSlot(i) != nullptr index + 1
   int firstNoteInputSlot = -1; // actual slot index of the first live NoteInputSlot(); -1 if none.
                                 // Nodes with note-only input (no AudioInputSlot at all) can have
                                 // their note pin start at a nonzero slot - e.g. WaveTerrainNode and
                                 // ImageSpectralSynthNode both only answer NoteInputSlot(1). Callers
                                 // that need to actually connect a cable must use this, not slot 0.

   // Does this node own (or reach) an AudioNode at all? AUDIOPARAMSWEEPTEST's
   // "reaches the audio thread" check needs one to drive; a node with none
   // (Audio Out - a pure topology terminal, no ProcessBlock of its own) has
   // nothing to test there.
   bool HasAudioNode() const { return isAudioSource || isNoteSource || hasNotePorts; }

   // Does this node touch the audio/note cable graph at all, as either side
   // of a cable? AUDIOTEARDOWNSWEEPTEST's teardown invariant applies to every
   // one of these, including pure terminals like Audio Out.
   bool ParticipatesInAudioGraph() const
   {
      return isAudioSource || isNoteSource || hasNotePorts || audioInputSlots > 0 || noteInputSlots > 0;
   }
};


struct AudioSweepCandidate
{
   std::string name;
   std::string category;
   AudioNodeShape shape;
};

std::vector<AudioSweepCandidate> DiscoverAudioSweepCandidates(
   const std::function<bool(const AudioNodeShape&)>& include);

bool RunGainFixture();

bool RunFilterFixture();

bool RunOscWaveformFixture();

bool RunWavetableFixture();

bool RunEnvelopeFixture();

bool RunVoiceStealFixture();

bool RunMusicTimeFixture();

bool RunAudioFilterFixture();

bool RunDynamicsFixture();


// Delay's exit criterion, same rig shape as DynamicsTest above (real
// AudioEffectNode -> AudioEffectRuntime -> DelayKernel pipeline, not the
// kernel's internals in isolation).
namespace DelayTest
{
   inline std::unique_ptr<AudioEffectNode> MakeDelayNode(bool bounce, double sampleRate)
   {
      const EffectDef* def = nullptr;
      for (const EffectDef& d : GetEffectDefs())
         if (d.name == "Delay")
            def = &d;
      auto node = std::make_unique<AudioEffectNode>(*def);
      *node->ParamPtr("bounce") = bounce ? 1.0f : 0.0f;
      node->mix = 1.0f; // 100% wet so the fixture measures the delayed tap directly, not a dry/wet blend
      node->GetAudioNode()->PrepareToPlay(sampleRate, 512);
      node->CookIfNeeded(1);
      return node;
   }

   // Runs `numSamples` frames of `numChannels`, one sample at a time (block
   // size 1) through the node, capturing every channel's output.
   template <typename SampleFn>
   void RunSamples(AudioEffectNode& node, int numSamples, int numChannels, SampleFn getSample,
                    std::vector<std::vector<float>>* outSamples)
   {
      AudioNode* audioNode = node.GetAudioNode();
      float inVal[2] = { 0.0f, 0.0f };
      float outVal[2] = { 0.0f, 0.0f };
      float* inPtrs[2] = { &inVal[0], &inVal[1] };
      float* outPtrs[2] = { &outVal[0], &outVal[1] };
      AudioBuffer inBuffer;
      inBuffer.channels = inPtrs;
      inBuffer.numChannels = numChannels;
      inBuffer.numFrames = 1;
      AudioBuffer outBuffer;
      outBuffer.channels = outPtrs;
      outBuffer.numChannels = numChannels;
      outBuffer.numFrames = 1;
      const AudioBuffer* inputs[1] = { &inBuffer };
      if (outSamples != nullptr)
         outSamples->assign(numChannels, {});
      for (int i = 0; i < numSamples; i++)
      {
         for (int ch = 0; ch < numChannels; ch++)
            inVal[ch] = getSample(i, ch);
         audioNode->ProcessBlock(inputs, 1, outBuffer);
         if (outSamples != nullptr)
            for (int ch = 0; ch < numChannels; ch++)
               (*outSamples)[ch].push_back(outVal[ch]);
      }
   }
}

bool RunDelayFixture();

bool RunReverbFixture();

bool RunWavetableShaperFixture();

bool RunSamplerFixture();

bool RunPaulStretchFixture();

bool RunResonatorFixture();

bool RunMetallicDecayFixture();

bool RunCycleShaperFixture();

bool RunMidiFileFixture();

bool RunMpeFixture();

bool RunShapeResonatorFixture();

bool RunSpectrumSlideFixture();

bool RunKeySnapFixture();

bool RunSpecBlurFixture();

bool RunGrainMolderFixture();

bool RunMolderFixture();

bool RunSpoutLoopTest();

bool RunGranularFixture();

int RunDspTest();

int RunFieldTest();

int RunFieldElementTest();

int RunFieldParamTest();

int RunFieldStateTest();

int RunFieldTransferTest();

int RunFieldSampleTest();

int RunFieldPinDeclTest();

void RunRecSyncTest();

void RunAudioRingTest();

void RunVideoExactTest();

void RunRecExportTest(int width, int height, bool starved, const char* label);

bool RunAudioPdcTest();
bool RunI18nTest();

#if defined(__linux__)
int RunMidiParseTest();
#endif

int RunCVRecorderTest();

int RunMidiCC14Test();
int RunNdiTest();

int RunAudioParamSweepTest();

bool RunFmModeDebugCheck();

bool RunFmRenderCheck();

bool RunBrowserSortTest();

bool RunAppearanceSelfTest();

int RunPluginScanTest();

int RunMidiBendTest();

int RunPluginNodeHandleTest();

#if INFINITE_ENABLE_VST3
int RunVST3ScanTest();
#endif

#if INFINITE_ENABLE_VST3
#if defined(__linux__)
int RunVST3EditorShotTest();
#endif
#endif

#if INFINITE_ENABLE_VST3
#if defined(__linux__)
int RunVST3BlocklistTest();
#endif
#endif

void RunRpcBatchTest();

void RunPatchWatchTest();

int RunAutosaveMarkerTest();

int RunRemoveBgTest();

int RunNetworkTest();

bool RunPerfPanelSelfTest();

#if defined(__linux__)
int RunCameraConvTest();
#endif

#if defined(__linux__)
int RunHostEnvTest();
#endif

int RunSyphonPatchTest();

int RunPatchLayoutTest();

}
