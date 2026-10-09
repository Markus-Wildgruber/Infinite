#pragma once
// Benchmark scene builders (B1-B8) and xrun accounting for the bench harness.
// Split out of AppShared.h so AudioEngine.h is not in every app/ TU
// (docs/plans/compile-times/include-split.md, Phase 3).
#include "app/AppShared.h"
#include "audio/AudioEngine.h"

namespace app
{
void BenchFillXruns(Bench::BenchReport& report, const AudioEngine::XrunCounts& base);

void BuildBenchB1Audio(long numVoices, int bufferFrames = 256, float yOffset = 0.0f);

void BuildBenchB2Scene(const std::string& scaleStr, bool isAnim, int& outRender3DIdx, int& outOutputIdx,
                              int* outTwistIdx = nullptr, int* outMatIdx = nullptr, int* outCamIdx = nullptr,
                              bool useEmbossForGlitch = false);

void BuildBenchB4Scene(const std::string& scaleStr, const std::string& shadowStr, bool isAnim, int& outRender3DIdx, int& outOutputIdx, int& outCamIdx, int& outLfoIdx);

void BuildBenchB6Scene(int n, bool collapsed, float& outMaxX, float& outMaxY, int& outDragNodeIdx);

bool BuildBenchB8Scene(const std::vector<std::string>& clipPaths, bool withSyphon, bool withCamera, int windows,
                              std::vector<int>& outClipIdx, std::vector<int>& outOutputIdx,
                              int& outSyphonIdx, int& outCameraIdx, std::string& outError);

}
