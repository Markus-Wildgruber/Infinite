#pragma once

#include <string>
#include <vector>

#include "json.hpp"

// Sound -> synth settings: offline analysis of one reference sound (a single
// note, or a stretch of a recording) into the numbers a synth is set from -
// its fundamental, the level, frequency and decay of each harmonic partial,
// an ADSR fitted to its amplitude envelope, and its spectral envelope /
// brightness / noisiness. Used by the Sound Analyze node
// (nodes/SoundAnalyzeNode.h), which turns a profile into modulator outputs
// and into settings it writes onto a synth ("Apply to").
//
// Pure: no ImGui, no app state. Runs on the calling thread (a worker in the
// node), allocates freely, NOT real-time safe. A few seconds of audio analyse
// in well under a second.
//
// Methods, from their primary references (all textbook / open literature):
//   f0           YIN (de Cheveigne & Kawahara 2002) over the body of the note,
//                median of the frame estimates; refined from the partials.
//   partials     peaks of a long Hann-windowed FFT over the body, searched
//                harmonic by harmonic, frequency and level refined by
//                parabolic interpolation of the log magnitude (Smith & Serra,
//                "PARSHL", 1987).
//   inharmonicity  least-squares fit of the stiff-string law
//                f_k = k * F * sqrt(1 + B k^2) (Fletcher 1964): (f_k/k)^2 is
//                linear in k^2 with intercept F^2 and slope F^2 B.
//   decay        each partial's amplitude track from an STFT, a straight line
//                fitted to its dB level from the onset down to 35 dB below its
//                peak (or the noise floor); T60 = -60 / slope.
//   envelope     RMS envelope (trailing window of one period of f0, 1 ms hop);
//                attack from the 10 %..90 % rise, decay to 90 % of the way to
//                the sustain plateau, release from 90 %..10 % of the plateau -
//                each converted to the length of the linear ADSR segment that
//                has the same crossing times (Infinite's synth envelopes are
//                linear: audio/AudioVoice.h).
//   spectrum     log-spaced band levels (peak envelope through the partials,
//                smoothed), power-weighted spectral centroid, 85 % rolloff,
//                and noise-to-harmonic ratio (power outside the partials'
//                main lobes vs inside).
namespace SoundAnalysis
{
   constexpr int kMaxPartials = 32;
   constexpr int kEnvBands = 28;             // log-spaced kBandLowHz .. kBandHighHz
   constexpr float kBandLowHz = 40.0f;
   constexpr float kBandHighHz = 16000.0f;
   constexpr double kMaxSeconds = 20.0;      // longest region analysed
   constexpr int kEnvelopePoints = 256;      // SoundProfile::envelopeDb length (at most)

   struct Options
   {
      double startSeconds = 0.0;  // region start in the input
      double lengthSeconds = 0.0; // region length; <= 0 = to the end (capped at kMaxSeconds)
      double f0HintHz = 0.0;      // > 0: skip pitch detection, search partials around this
      int partials = 16;          // harmonics to measure, 1..kMaxPartials

      bool operator==(const Options& o) const
      {
         return startSeconds == o.startSeconds && lengthSeconds == o.lengthSeconds && f0HintHz == o.f0HintHz &&
                partials == o.partials;
      }
      bool operator!=(const Options& o) const { return !(*this == o); }
   };

   struct Partial
   {
      int harmonic = 0;        // k (1 = fundamental)
      bool present = false;    // a real peak was found above the noise floor
      float freqHz = 0.0f;     // measured peak frequency
      float ampDb = -120.0f;   // level at the onset, dB relative to the strongest partial (0 dB)
      float amp = 0.0f;        // the same, linear (0..1)
      float decaySec = 0.0f;   // T60 of the fitted decay; 0 = not measured (too short / sustains)
      bool sustains = false;   // slope flatter than 1 dB/s - no measurable decay
   };

   struct SoundProfile
   {
      bool valid = false;
      std::string error;       // why not, when !valid

      double sampleRate = 0.0;
      double startSeconds = 0.0; // the region actually analysed
      double seconds = 0.0;
      float peakDb = -120.0f;    // region peak, dBFS
      float peakTime = 0.0f;     // seconds from region start to the envelope peak

      // Pitch. f0Hz is the measured fundamental (f_1 = F sqrt(1 + B)).
      bool pitched = false;
      float f0Hz = 0.0f;
      float f0Clarity = 0.0f;     // 0..1, YIN periodicity (1 = perfectly periodic)
      float inharmonicity = 0.0f; // B of the stiff-string law (0 = harmonic; piano ~1e-4..1e-3)
      std::vector<Partial> partials; // Options::partials entries, k = 1..N (absent ones present=false)

      // Amplitude envelope, as linear ADSR segment lengths (see header).
      float attackMs = 0.0f;
      float decayMs = 0.0f;
      float sustain = 0.0f;       // 0..1 of the peak, linear amplitude
      float releaseMs = 0.0f;
      bool sustained = false;     // a level plateau was found (else sustain = 0, a decaying sound)
      bool releaseMeasured = false; // the plateau's release ended inside the region
      float envHopSeconds = 0.0f;   // spacing of envelopeDb
      std::vector<float> envelopeDb; // RMS envelope over the region, dB re its peak (<= kEnvelopePoints)

      // Spectrum of the body (from the envelope peak on).
      float bandHz[kEnvBands] = {}; // band centres
      float bandDb[kEnvBands] = {}; // smoothed spectral envelope, dB re its maximum (0 dB)
      float centroidHz = 0.0f;
      float rolloffHz = 0.0f;       // 85 % of the power lies below
      float brightness = 0.0f;      // 0..1: centroid on a log scale, 100 Hz -> 0, 10 kHz -> 1
      float noiseToHarmonicDb = 0.0f; // 10 log10(noise / harmonic power); capped at +60 (unpitched)
      float noisiness = 0.0f;       // noise / (noise + harmonic) power, 0..1 (1 when unpitched)
   };

   // `mono` is the whole input (frames long); Options picks the region.
   SoundProfile AnalyzeSound(const float* mono, size_t frames, double sampleRate, const Options& options);

   // Everything in the profile, rounded for a reader; partials listed only
   // when present; envelopeDb decimated to at most 64 points.
   nlohmann::json ToJson(const SoundProfile& p);

   // ---- helpers for configuring a synth from a profile ------------------

   // An Equation Synth formula over x in [0, 1) that sums the present
   // partials as sines at their relative linear amplitudes - the measured
   // spectrum as a single-cycle wave. Keeps at most `maxTerms` partials (the
   // loudest) and drops those below `floorDb`. "sin(2*pi*x)" when there are none.
   std::string AdditiveFormula(const SoundProfile& p, int maxTerms, float floorDb);

   // How far a harmonic spectrum (`amps[k-1]` = linear amplitude of harmonic
   // k, any scale) is from the profile's partials: amplitude-weighted mean
   // absolute dB difference over k = 1..min(count, partials), levels floored
   // at -60 dB. Lower is closer; 0 = identical.
   float HarmonicMismatchDb(const SoundProfile& p, const float* amps, int count);

   // Band centre of spectral-envelope band `band`.
   float BandCentreHz(int band);
}
