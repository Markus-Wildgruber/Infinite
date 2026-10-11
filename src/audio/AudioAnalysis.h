#pragma once

#include <string>
#include <vector>

#include "json.hpp"

// Sound as numbers: offline analysis of a captured or decoded signal, for the
// assistant's analyze_audio / analyze_file tools (app/graph/AudioProbeJobs.cpp)
// and anything else that wants measurements instead of pictures.
//
// Everything runs on the calling thread (main or worker), allocates freely and
// is NOT real-time safe. Levels are dBFS RMS (a full-scale sine reads -3 dB);
// band levels are the RMS of that band's share of the signal.
//
// Methods, from their primary references:
//   pitch  - YIN (de Cheveigne & Kawahara 2002), difference function via FFT.
//   onsets - spectral flux on log-compressed magnitudes, adaptive threshold.
//   tempo  - autocorrelation of the onset envelope, weighted towards 120 bpm.
//   key    - chroma vector correlated with the Krumhansl-Kessler key profiles.
namespace AudioAnalysis
{
   constexpr int kBands = 8; // log-spaced 20 Hz .. 20 kHz

   struct Options
   {
      double frameSeconds = 0.1; // timeline resolution
      bool rhythmAndKey = false; // tempo + key (worth it on several seconds of music)
   };

   struct Frame
   {
      float time = 0.0f;
      float rmsDb = -120.0f;
      float peakDb = -120.0f;
      float centroidHz = 0.0f;
      float pitchHz = 0.0f;      // 0 = unpitched / silent
      float pitchClarity = 0.0f; // 0..1, 1 - YIN's aperiodicity at the chosen lag
      float bandsDb[kBands] = {};
   };

   struct Result
   {
      double sampleRate = 0.0;
      double seconds = 0.0;
      float rmsDb = -120.0f, peakDb = -120.0f;
      float bandsDb[kBands] = {};
      float centroidHz = 0.0f;
      float silentRatio = 0.0f; // frames below -60 dB
      std::vector<Frame> frames;
      std::vector<float> onsets; // seconds

      float medianPitchHz = 0.0f; // over pitched frames
      float pitchedRatio = 0.0f;  // pitched frames / non-silent frames
      float lowPitchHz = 0.0f, highPitchHz = 0.0f; // 5th / 95th percentile

      // Options::rhythmAndKey
      float tempoBpm = 0.0f, tempoConfidence = 0.0f;
      int keyRoot = -1; // pitch class, 0 = C
      bool keyMinor = false;
      float keyConfidence = 0.0f; // correlation margin over the runner-up key
      float chroma[12] = {};
   };

   // Interleaved stereo (L,R,...) to mono.
   std::vector<float> MixToMono(const std::vector<float>& interleavedStereo);

   Result Analyze(const std::vector<float>& mono, double sampleRate, const Options& options);

   // Summary plus a column-wise timeline of at most `maxFrames` rows (0 = none).
   nlohmann::json ToJson(const Result& r, int maxFrames);

   // Two signals captured over the same span (same rate). Level/band
   // differences are A minus B; a positive lag means B lags A.
   nlohmann::json Compare(const Result& a, const Result& b, const std::vector<float>& monoA,
                          const std::vector<float>& monoB, double sampleRate);

   // "A4", "C#3 -12c" (cents shown when off by 5 or more). Empty for hz <= 0.
   std::string NoteName(float hz);
}
