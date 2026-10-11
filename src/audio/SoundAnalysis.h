#pragma once

#include <atomic>
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
//   transient    the partials are taken out of the first ~150 ms after the
//                onset (sinusoidal analysis at the measured partial
//                frequencies: complex amplitudes fitted per short Hann frame
//                by iterated projection, interpolated linearly between frame
//                centres, subtracted - McAulay & Quatieri 1986 with fixed
//                frequencies; the "deterministic + residual" split of Serra &
//                Smith 1990). The residual's energy above its later (sustained)
//                level gives the burst: its decay rate from the residual
//                energy's mean time (an exponential's energy centroid is
//                1 / 2 alpha), its level at the onset by matching the energy
//                of an exponential with that rate.
//   air          sustained residual in the body spectrum: the bins outside the
//                harmonic main lobes, per third-octave band from 1 kHz, as a
//                median (robust to stray tonal peaks; a periodogram bin of
//                noise is exponentially distributed, mean = median / ln 2).
//                The highpass corner is where that noise starts to outweigh
//                the harmonics above it, moved up to where its own spectrum
//                comes within 6 dB of its maximum.
//   multi-note   AnalyzeNotes runs the single-note analysis on every isolated
//                note of a transcription and takes medians; key tracking is a
//                least-squares line (one pass of 3-sigma outlier rejection)
//                of level and harmonic balance against pitch.
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

      // Level of the harmonic part at the onset: the power sum of the
      // partials' onset levels, dBFS (-120 when unpitched). Unlike peakDb it
      // ignores other sounds in the region (a previous note's tail).
      float levelDb = -120.0f;

      // Attack transient: what is left in the first ~150 ms after the onset
      // once the partials are taken out - the strike / pluck / breath noise.
      bool transientMeasured = false; // pitched, and enough audio after the onset
      float onsetTime = 0.0f;         // seconds from region start to the detected onset
      float transient = 0.0f;         // 0..1: RMS of that residual at the onset / RMS of the harmonic part at its peak
      float transientDecayMs = 0.0f;  // its decay to -60 dB (exponential), 1..200 ms (0 = no burst)
      // The same burst measured through a 12 dB/oct highpass at noiseToneHz:
      // what a synth noise layer filtered there has to put out to match it.
      float transientHigh = 0.0f;
      float transientHighDecayMs = 0.0f;
      float attackCrestDb = 0.0f;     // peak / RMS of the whole signal over the first 80 ms from the onset

      // Air: sustained non-harmonic noise in the top of the spectrum (body
      // spectrum, bands from 1 kHz up).
      bool airMeasured = false;
      float noiseLevel = 0.0f;        // 0..1: RMS of the residual above noiseToneHz / RMS of the harmonic part (1 when unpitched)
      float noiseToneHz = 0.0f;       // highpass corner for that noise, 1000..16000 Hz
   };

   // `mono` is the whole input (frames long); Options picks the region.
   SoundProfile AnalyzeSound(const float* mono, size_t frames, double sampleRate, const Options& options);

   // Everything in the profile, rounded for a reader; partials listed only
   // when present; envelopeDb decimated to at most 64 points. Adds
   // "levelDb", "transient" {measured, onset, level, decayMs, crestDb,
   // high {level, decayMs}} and
   // "air" {measured, noiseLevel, noiseDb, noiseToneHz}.
   nlohmann::json ToJson(const SoundProfile& p);

   // ---- multi-note analysis ---------------------------------------------

   // One note of a transcription (e.g. Audio to MIDI / BasicPitch::Note).
   struct NoteEvent
   {
      double start = 0.0;     // seconds into the input
      double end = 0.0;       // seconds
      int midi = 60;
      float amplitude = 1.0f; // 0..1, any loudness measure (only compared between notes)
   };

   struct MultiNoteOptions
   {
      int partials = 16;               // per-note Options::partials
      int lowMidi = 21, highMidi = 108; // notes outside are not analysed (but still count as neighbours)
      double isolationBefore = 0.08;   // s: no other onset this long before a note's start...
      double isolationAfter = 0.15;    // s: ...or this long after it
      double preRoll = 0.025;          // s: each region starts this much before the note (onset times are ~10 ms coarse)
      double maxNoteSeconds = 1.5;     // region = [start - preRoll, min(next onset - 5 ms, start + this)]
      double minNoteSeconds = 0.06;    // shorter notes are skipped
      float minRelAmplitude = 0.5f;    // quieter than this x the median amplitude of the in-range notes: skipped
      int maxNotes = 40;               // at most this many analysed, spread over the pitches and the file
      int refMidi = 60;                // key tracking pivot; the merged partial levels are given at this note
   };

   struct NoteRow
   {
      double start = 0.0;
      double regionStart = 0.0, regionSeconds = 0.0; // what was analysed for it
      int midi = 60;
      float f0Hz = 0.0f;
      float levelDb = -120.0f;  // SoundProfile::levelDb
      bool hasH1 = false;
      float h1Db = -120.0f;     // the fundamental's onset level (power), dBFS
      float peakDb = -120.0f;   // SoundProfile::peakDb (region waveform peak)
      bool hasH2 = false, hasH3 = false;
      float h2h1Db = 0.0f;      // partial 2 minus partial 1, dB
      float h3h1Db = 0.0f;
      float transient = 0.0f;
      float noiseLevel = 0.0f;
   };

   // y = atRefDb + dbPerOct * (midi - refMidi) / 12, least squares.
   struct KeyFit
   {
      bool valid = false;  // >= 3 notes spanning >= 2 semitones
      int notes = 0;       // used in the fit (after outlier rejection)
      float dbPerOct = 0.0f;
      float atRefDb = 0.0f;
      float r2 = 0.0f;
   };

   struct MultiNoteResult
   {
      bool valid = false;
      std::string error;
      int notesGiven = 0, notesInRange = 0, notesIsolated = 0, notesAnalysed = 0, notesUsed = 0;
      int refMidi = 60;
      // Merged profile: medians over the used notes. Partial levels are
      // relative to the strongest, taken relative to each note's fundamental
      // with the fitted key tilt removed (h_k - keyTilt * (midi - refMidi) / 12
      // for k >= 2), i.e. the spectrum at refMidi; partial frequencies are the
      // median ratios f_k / f_1 placed on f0 = refMidi. Envelope, ADSR,
      // spectrum, transient, air and levelDb are medians.
      SoundProfile profile;
      std::vector<NoteRow> rows; // the used notes, by start time
      // The fundamental's level vs pitch - what a synth's key-level gain must
      // follow when its key tilt leaves the fundamental alone (Equation
      // Synth's does) - or, when fewer than 3 notes show a fundamental, the
      // whole harmonic level (levelDb).
      KeyFit keyLevel;
      bool keyLevelFromFundamental = false;
      KeyFit keyTilt;   // h2 - h1 vs pitch (positive: higher notes brighter)
      KeyFit keyTilt3;  // h3 - h1 vs pitch
   };

   // Analyses each reasonably isolated note of `notes` (any order) and merges
   // the results; see MultiNoteOptions for the rules. `cancel` may be null.
   MultiNoteResult AnalyzeNotes(const float* mono, size_t frames, double sampleRate, const std::vector<NoteEvent>& notes,
                                const MultiNoteOptions& options, const std::atomic<bool>* cancel = nullptr);

   // Counts, "keyTracking" {refMidi, levelDbPerOct, levelR2, levelNotes,
   // levelAtRefDb, levelOf, tiltDbPerOct, tiltR2, tiltNotes, tilt3DbPerOct,
   // tilt3R2, tilt3Notes} and "notes" (per-note rows {t, note, midi, f0Hz,
   // levelDb, h1Db, peakDb, h2h1Db, h3h1Db, transient, noiseLevel}, at most
   // `maxRows`). Not the merged profile (ToJson(r.profile) has that).
   nlohmann::json ToJson(const MultiNoteResult& r, size_t maxRows = 40);

   // ---- helpers for configuring a synth from a profile ------------------

   // An Equation Synth formula over x in [0, 1) that sums the present
   // partials as sines at their relative linear amplitudes - the measured
   // spectrum as a single-cycle wave. Keeps at most `maxTerms` partials (the
   // loudest) and drops those below `floorDb`. "sin(2*pi*x)" when there are none.
   std::string AdditiveFormula(const SoundProfile& p, int maxTerms, float floorDb);

   // RMS / peak of the single cycle AdditiveFormula(p, maxTerms, floorDb)
   // describes (1/sqrt 2 for a sine; the peak floored at 1, as Equation
   // Synth only ever scales a wave down to peak 1): converts a level measured
   // against the harmonic RMS (transient, noiseLevel) into one against the
   // wave's peak.
   float AdditiveRmsOverPeak(const SoundProfile& p, int maxTerms, float floorDb);

   // How far a harmonic spectrum (`amps[k-1]` = linear amplitude of harmonic
   // k, any scale) is from the profile's partials: amplitude-weighted mean
   // absolute dB difference over k = 1..min(count, partials), levels floored
   // at -60 dB. Lower is closer; 0 = identical.
   float HarmonicMismatchDb(const SoundProfile& p, const float* amps, int count);

   // Band centre of spectral-envelope band `band`.
   float BandCentreHz(int band);
}
