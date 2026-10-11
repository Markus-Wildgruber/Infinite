#include "EquationNode.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio/AudioBuffer.h"
#include "audio/AudioNode.h"
#include "audio/AudioVoice.h"
#include "audio/DspMath.h"
#include "audio/MeterRing.h"
#include "audio/NoteEvent.h"
#include "audio/NoteEventQueue.h"
#include "audio/ParamMailbox.h"
#include "audio/SampleSlot.h"
#include "audio/SynthModes.h"

namespace
{
   const std::vector<EquationNode::Preset> kPresets = {
      { "Sine (Pure)", "sin(2*pi*x)", EquationDsp::kDomainZeroToOne, 0.5f, 0.5f, 0.0f, 0.0f },
      { "Additive Harmonics", "sin(2*pi*x) + a*sin(4*pi*x) + b*sin(6*pi*x) + c*sin(8*pi*x)", EquationDsp::kDomainZeroToOne, 0.5f, 0.25f, 0.125f, 0.0f },
      { "Chebyshev T3/T4/T5", "4*x^3 - 3*x + a*(8*x^4 - 8*x^2 + 1) + b*(16*x^5 - 20*x^3 + 5*x)", EquationDsp::kDomainNegOneToOne, 0.5f, 0.25f, 0.0f, 0.0f },
      { "Phase Distortion / FM", "sin(2*pi*x + a*sin(2*pi*(1+b*4)*x))", EquationDsp::kDomainZeroToOne, 0.75f, 0.5f, 0.0f, 0.0f },
      { "Sinc Pulse (Dirichlet)", "sin(pi*(1+a*15)*x) / (pi*(1+a*15)*x + 0.0001)", EquationDsp::kDomainNegOneToOne, 0.5f, 0.0f, 0.0f, 0.0f },
      { "Exponential Chirp", "exp(-a*6*x) * sin(2*pi*(1+b*8)*x)", EquationDsp::kDomainZeroToOne, 0.5f, 0.5f, 0.0f, 0.0f },
      { "Weierstrass Fractal", "sin(2*pi*x) + a*sin(6*pi*x) + a^2*sin(18*pi*x) + a^3*sin(54*pi*x)", EquationDsp::kDomainZeroToOne, 0.5f, 0.0f, 0.0f, 0.0f },
      { "Tanh Soft Saturator", "tanh((1 + a*12) * sin(2*pi*x))", EquationDsp::kDomainZeroToOne, 0.5f, 0.0f, 0.0f, 0.0f },
      { "Pulse Width Morph", "if(x < a, 1, -1)", EquationDsp::kDomainZeroToOne, 0.5f, 0.0f, 0.0f, 0.0f },
      { "Formant Vowel Peak", "sin(2*pi*x) * exp(-a*25 * (x - b)^2)", EquationDsp::kDomainZeroToOne, 0.5f, 0.5f, 0.0f, 0.0f },
      { "Fold / Tangent Warp", "tan(x * pi * a * 0.96)", EquationDsp::kDomainNegOneToOne, 0.75f, 0.0f, 0.0f, 0.0f },
      { "Sawtooth Morph", "(x - floor(x)) * 2 - 1 + a * sin(2*pi*b*x)", EquationDsp::kDomainZeroToOne, 0.0f, 1.0f, 0.0f, 0.0f }
   };

   const std::vector<std::string> kDomainNames = {
      "[0, 1] Normalized", "[-pi, pi] Angular", "[-1, 1] Bipolar"
   };

   // filterType is stored as SynthModes::FilterType directly (same
   // convention as WavetableNode/OscillatorNode's engine.filterType) rather
   // than a node-local subset, so this synth gets every slope/shape the app
   // defines - LP/HP at 12/24/36 dB, BP12/24, notch12/24 - not just the
   // five that used to be hand-picked here.
   const std::vector<std::string>& kFilterNames = SynthModes::FilterTypeList();

   enum SmoothedParam
   {
      kParamFrequency = 0,
      kParamVolume,
      kParamPan,
      kParamGlide,
      kParamDetune,
      kParamStereoWidth,
      kParamCutoff,
      kParamResonance,
      kParamDrive,
      kParamFine,
      kParamTransient,
      kParamNoiseLevel,
      kNumSmoothedParams
   };

   constexpr float kVoicePhaseSeed[EquationNode::kMaxUnison] = {
      0.000000f, 0.618034f, 0.236068f, 0.854102f,
      0.472136f, 0.090170f, 0.708204f, 0.326238f
   };

   inline float MidiNoteToHz(int midiNote, float fineCents = 0.0f)
   {
      return 440.0f * powf(2.0f, (float)(midiNote - 69 + fineCents * 0.01f) / 12.0f);
   }

   // ---------------------------------------------------------------- noise & key
   // Unit-RMS white noise: xorshift32's uniform [-1, 1) has RMS 1/sqrt(3).
   constexpr float kNoiseUnitRms = 1.7320508f;
   // The tone leaves RenderVoice through a centred equal-power pan (unison 1),
   // so the mono noise gets the same per-channel factor to stay "relative to
   // the voice" in each channel.
   constexpr float kCentrePan = 0.70710678f;
   // A burst below this envelope value is inaudible (-100 dB) and is ended.
   constexpr float kBurstFloor = 1e-5f;
   // ln(1000): an exponential that falls 60 dB over T has coefficient
   // exp(-ln(1000) / (T * sampleRate)) per sample.
   constexpr double kLn1000 = 6.907755278982137;
   // Key-tracked gain and tilt limits (beyond ~15 dB a single shelf between
   // the fundamental and the 2nd harmonic can no longer separate them).
   constexpr float kKeyLevelMaxDb = 24.0f;
   constexpr float kKeyTiltMaxDb = 15.0f;
   constexpr double kKeyTiltShelfMaxDb = 24.0;
   // Shelf corner as a multiple of the voice fundamental, and the RBJ shelf
   // Q: 1.2 keeps harmonics 2..8 within ~1 dB of each other (a flat "tilt of
   // everything above the fundamental") for tilts up to ~12 dB.
   constexpr double kKeyTiltCornerRatio = 1.5;
   constexpr double kKeyTiltQ = 1.2;

   // |H(e^jw)|^2 of a biquad, from cos(w) and cos(2w).
   inline double BiquadMagSq(const DspMath::Biquad& bq, double cw, double c2w)
   {
      const double b0 = bq.b0, b1 = bq.b1, b2 = bq.b2, a1 = bq.a1, a2 = bq.a2;
      const double num = b0 * b0 + b1 * b1 + b2 * b2 + 2.0 * (b0 * b1 + b1 * b2) * cw + 2.0 * b0 * b2 * c2w;
      const double den = 1.0 + a1 * a1 + a2 * a2 + 2.0 * (a1 + a1 * a2) * cw + 2.0 * a2 * c2w;
      return den > 1e-30 ? std::max(num, 1e-30) / den : 1.0;
   }

   // Pitch-tracked harmonic tilt: an RBJ high shelf with its corner at
   // 1.5 x f0 (between the fundamental and the 2nd harmonic), whose gain is
   // corrected (a few multiplicative steps on the measured response) until
   // the 2nd harmonic sits exactly tiltDb above/below the fundamental, then
   // normalized so the fundamental itself stays at 0 dB - keyTilt moves the
   // harmonic balance only, keyLevel owns loudness. Writes coefficients only
   // (no filter state) into `out`, plus the fundamental-normalizing gain.
   // Called per note-on and when tilt or pitch moved since the last design
   // (once per block at most), never per sample.
   inline void DesignKeyTiltShelf(DspMath::Biquad& out, float& norm, double f0, double tiltDb, double sampleRate)
   {
      const double twoPiOverSr = 2.0 * M_PI / sampleRate;
      const double fc = std::min(kKeyTiltCornerRatio * f0, 0.45 * sampleRate);
      const double cw1 = cos(twoPiOverSr * f0);
      const double c2w1 = cos(2.0 * twoPiOverSr * f0);
      double g = tiltDb;
      if (2.0 * f0 < 0.49 * sampleRate)
      {
         const double cw2 = cos(twoPiOverSr * 2.0 * f0);
         const double c2w2 = cos(2.0 * twoPiOverSr * 2.0 * f0);
         for (int it = 0; it < 4; it++)
         {
            out.SetHighShelf(fc, kKeyTiltQ, g, sampleRate);
            const double d = 10.0 * log10(BiquadMagSq(out, cw2, c2w2) / BiquadMagSq(out, cw1, c2w1));
            if (d * tiltDb <= 0.0 || std::fabs(d - tiltDb) < 1e-3)
               break;
            g = std::clamp(g * tiltDb / d, -kKeyTiltShelfMaxDb, kKeyTiltShelfMaxDb);
         }
      }
      out.SetHighShelf(fc, kKeyTiltQ, g, sampleRate);
      norm = (float)(1.0 / sqrt(BiquadMagSq(out, cw1, c2w1)));
   }

   inline float KeyNoteForHz(float hz)
   {
      return 69.0f + 12.0f * log2f(std::max(hz, 8.0f) / 440.0f);
   }
}

const std::vector<EquationNode::Preset>& EquationNode::Presets() { return kPresets; }

const std::vector<std::string>& EquationNode::PresetNames()
{
   static std::vector<std::string> sNames;
   if (sNames.empty())
   {
      for (const auto& p : kPresets)
         sNames.push_back(p.name);
   }
   return sNames;
}

const std::vector<std::string>& EquationNode::DomainNames() { return kDomainNames; }
const std::vector<std::string>& EquationNode::FilterTypeNames() { return kFilterNames; }

// ---------------------------------------------------------------------------
// AudioEquationNode: Audio Thread DSP Synthesizer
// ---------------------------------------------------------------------------
class AudioEquationNode : public AudioNode
{
public:
   static constexpr int kMaxVoices = EquationNode::kMaxVoices;
   static constexpr int kMaxUnison = EquationNode::kMaxUnison;

   AudioEquationNode()
   {
      auto* initialBank = new EquationDsp::EquationBank();
      mBankSlot.Push(initialBank);
      mBankSlot.SwapIn();

      for (int v = 0; v < kMaxVoices; v++)
         mVoices[v].Reset(44100.0, v);
   }

   ~AudioEquationNode() override = default;

   void SetNoteInbox(NoteEventQueue* inbox, int cursor) override
   {
      mNoteInbox = inbox;
      mNoteCursor = cursor;
   }

   void PrepareToPlay(double sampleRate, int maxBlockSize) override
   {
      mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
      mMailbox.PrepareToPlay(mSampleRate);
      for (int v = 0; v < kMaxVoices; v++)
         mVoices[v].Reset(mSampleRate, v);
      mLastNoteHz = 0.0f;
      mHasLastNote = false;

      const int allocSize = std::max(maxBlockSize, 4096);
      mMonoScope.resize(allocSize, 0.0f);
   }

   void SwapBank(const EquationDsp::EquationBank& newBank)
   {
      mBankSlot.Push(new EquationDsp::EquationBank(newBank));
   }

   void DrainRetiredBanks()
   {
      mBankSlot.DrainRetired();
   }

   void PushParams(const EquationNode& n)
   {
      mUnison.store(std::clamp(n.unison, 1, kMaxUnison), std::memory_order_relaxed);
      mOctave.store(n.octave, std::memory_order_relaxed);
      mSemi.store(n.semi, std::memory_order_relaxed);
      mFilterType.store(n.filterType, std::memory_order_relaxed);
      mFilterAmount.store(n.filterAmount, std::memory_order_relaxed);

      mAmpAdsr[0].store(n.ampAttack, std::memory_order_relaxed);
      mAmpAdsr[1].store(n.ampDecay, std::memory_order_relaxed);
      mAmpAdsr[2].store(n.ampSustain, std::memory_order_relaxed);
      mAmpAdsr[3].store(n.ampRelease, std::memory_order_relaxed);

      mFilterAdsr[0].store(n.filterAttack, std::memory_order_relaxed);
      mFilterAdsr[1].store(n.filterDecay, std::memory_order_relaxed);
      mFilterAdsr[2].store(n.filterSustain, std::memory_order_relaxed);
      mFilterAdsr[3].store(n.filterRelease, std::memory_order_relaxed);

      mTransientDecayMs.store(n.transientDecay, std::memory_order_relaxed);
      mNoiseToneHz.store(n.noiseTone, std::memory_order_relaxed);
      mKeyLevel.store(n.keyLevel, std::memory_order_relaxed);
      mKeyTilt.store(n.keyTilt, std::memory_order_relaxed);

      const float values[kNumSmoothedParams] = {
         n.frequency,
         n.volume,
         n.pan,
         n.glide,
         n.detune,
         n.stereoWidth,
         n.cutoff,
         n.resonance,
         n.drive,
         n.fine,
         n.transient,
         n.noiseLevel
      };

      for (int i = 0; i < kNumSmoothedParams; i++)
         mMailbox.Push(i, values[i]);
   }

   int AllocateVoice()
   {
      for (int v = 0; v < kMaxVoices; v++)
      {
         if (!mVoices[v].active)
            return v;
      }
      uint64_t oldestAge = UINT64_MAX;
      int oldestIdx = 0;
      for (int v = 0; v < kMaxVoices; v++)
      {
         if (mVoices[v].age < oldestAge)
         {
            oldestAge = mVoices[v].age;
            oldestIdx = v;
         }
      }
      return oldestIdx;
   }

   void ProcessBlock(const AudioBuffer* const* /*inputs*/, int /*numInputs*/, AudioBuffer& output) override
   {
      const int numFrames = output.numFrames;
      if (numFrames <= 0 || output.numChannels < 2)
         return;

      float* outL = output.channels[0];
      float* outR = output.channels[1];
      std::fill_n(outL, numFrames, 0.0f);
      std::fill_n(outR, numFrames, 0.0f);

      mBankSlot.SwapIn();
      const EquationDsp::EquationBank& bank = *mBankSlot.Active();

      const int unison = mUnison.load(std::memory_order_relaxed);
      const int octave = mOctave.load(std::memory_order_relaxed);
      const int semi = mSemi.load(std::memory_order_relaxed);
      const int filterType = mFilterType.load(std::memory_order_relaxed);
      const float filterAmount = mFilterAmount.load(std::memory_order_relaxed);

      const float ampA = mAmpAdsr[0].load(std::memory_order_relaxed);
      const float ampD = mAmpAdsr[1].load(std::memory_order_relaxed);
      const float ampS = mAmpAdsr[2].load(std::memory_order_relaxed);
      const float ampR = mAmpAdsr[3].load(std::memory_order_relaxed);

      const float fltA = mFilterAdsr[0].load(std::memory_order_relaxed);
      const float fltD = mFilterAdsr[1].load(std::memory_order_relaxed);
      const float fltS = mFilterAdsr[2].load(std::memory_order_relaxed);
      const float fltR = mFilterAdsr[3].load(std::memory_order_relaxed);

      if (mNoteInbox != nullptr)
      {
         NoteEvent evts[64];
         const int numEvts = mNoteInbox->Pop(mNoteCursor, evts, 64);
         for (int i = 0; i < numEvts; i++)
         {
            const auto& event = evts[i];
            if (event.isNoteOn && event.velocity > 0.0f)
            {
               const int voiceIdx = AllocateVoice();
               Voice& v = mVoices[voiceIdx];
               const bool wasInactive = !v.active;
               v.active = true;
               v.held = true;
               v.note = event.note;
               v.voiceId = event.voiceId;
               v.velocity = event.velocity;
               v.age = ++mAgeCounter;
               v.amp.SetADSR(ampA, ampD, ampS, ampR);
               v.amp.NoteOn();
               v.filt.SetADSR(fltA, fltD, fltS, fltR);
               v.filt.NoteOn();

               const float baseHz = MidiNoteToHz(event.note + octave * 12 + semi);
               v.targetFreq = baseHz;
               const float glide = mMailbox.SmoothedValue(kParamGlide);
               if (wasInactive)
               {
                  if (mHasLastNote && glide > 0.001f)
                     v.currentFreq = mLastNoteHz;
                  else
                     v.currentFreq = baseHz;
               }
               else
               {
                  if (glide <= 0.001f)
                     v.currentFreq = baseHz;
               }
               mLastNoteHz = baseHz;
               mHasLastNote = true;

               // Key tracking reads the transposed note; a fresh note snaps
               // its key gain and redesigns its tilt shelf for the new pitch
               // (a stolen voice keeps its shelf state for continuity, a
               // silent one starts it from zero).
               v.keyNote = (float)(event.note + octave * 12 + semi);
               v.keyFresh = wasInactive; // a stolen, still-sounding voice ramps instead
               v.shelfHz = 0.0f;
               if (wasInactive)
               {
                  v.shelfActive = false;
                  v.noiseHp.Reset();
               }
               v.burstEnv = 1.0f;
            }
            else if (!event.isNoteOn && !event.bendUpdate)
            {
               for (int v = 0; v < kMaxVoices; v++)
               {
                  if (mVoices[v].active && (mVoices[v].voiceId == event.voiceId || mVoices[v].note == event.note) && mVoices[v].held)
                  {
                     mVoices[v].held = false;
                     mVoices[v].amp.NoteOff();
                     mVoices[v].filt.NoteOff();
                  }
               }
            }
         }
      }

      int activeCount = 0;
      const bool isNoteDriven = (mNoteInbox != nullptr);
      if (mMonoScope.size() < (size_t)numFrames)
         mMonoScope.resize(numFrames, 0.0f);

      // ---- Noise & key: per-block setup (coefficients, gain ramps, shelves).
      const float transientDecayMs = std::clamp(mTransientDecayMs.load(std::memory_order_relaxed), 1.0f, 200.0f);
      const float burstCoef = (float)exp(-kLn1000 / ((double)transientDecayMs * 0.001 * mSampleRate));
      const float noiseToneHz = std::min(std::clamp(mNoiseToneHz.load(std::memory_order_relaxed), 200.0f, 16000.0f),
                                         (float)(mSampleRate * 0.45));
      const float keyLevel = std::clamp(mKeyLevel.load(std::memory_order_relaxed), -12.0f, 12.0f);
      const float keyTilt = std::clamp(mKeyTilt.load(std::memory_order_relaxed), -12.0f, 12.0f);
      DspMath::TptSvf noiseHpProto;
      noiseHpProto.SetSampleRate(mSampleRate);
      noiseHpProto.SetCutoff(noiseToneHz, 0.70710678f);
      const float invFrames = 1.0f / (float)numFrames;
      float keyGainTarget[kMaxVoices];
      float keyGainInc[kMaxVoices];
      for (int vIdx = 0; vIdx < kMaxVoices; vIdx++)
      {
         Voice& v = mVoices[vIdx];
         v.noiseHp.g = noiseHpProto.g;
         v.noiseHp.k = noiseHpProto.k;
         keyGainTarget[vIdx] = v.keyGain;
         keyGainInc[vIdx] = 0.0f;
         const bool renders = isNoteDriven ? v.active : (vIdx == 0);
         if (!renders)
            continue;
         if (!isNoteDriven)
            v.keyNote = KeyNoteForHz(v.currentFreq);
         PrepareKeyTracking(v, keyLevel, keyTilt, invFrames, keyGainTarget[vIdx], keyGainInc[vIdx]);
      }

      for (int i = 0; i < numFrames; i++)
      {
         const float baseFreq = mMailbox.SmoothedValue(kParamFrequency);
         const float volume = mMailbox.SmoothedValue(kParamVolume);
         const float pan = mMailbox.SmoothedValue(kParamPan);
         const float glide = mMailbox.SmoothedValue(kParamGlide);
         const float detune = mMailbox.SmoothedValue(kParamDetune);
         const float stereoWidth = mMailbox.SmoothedValue(kParamStereoWidth);
         const float cutoff = mMailbox.SmoothedValue(kParamCutoff);
         const float resonance = mMailbox.SmoothedValue(kParamResonance);
         const float drive = mMailbox.SmoothedValue(kParamDrive);
         const float fine = mMailbox.SmoothedValue(kParamFine);
         const float transient = std::clamp(mMailbox.SmoothedValue(kParamTransient), 0.0f, 1.0f);
         const float noiseLevel = std::clamp(mMailbox.SmoothedValue(kParamNoiseLevel), 0.0f, 1.0f);

         float frameSumL = 0.0f;
         float frameSumR = 0.0f;

         const bool glideActive = glide > 0.001f;
         const float glideCoef = glideActive
            ? expf(-1.0f / (float)(mSampleRate * glide))
            : 0.0f;

         if (!isNoteDriven)
         {
            Voice& v = mVoices[0];
            v.active = true;
            v.held = true;
            const float targetHz = baseFreq * powf(2.0f, (float)(octave * 12 + semi) / 12.0f + fine * 0.01f / 12.0f);
            v.currentFreq = targetHz;

            float vOutL = 0.0f, vOutR = 0.0f;
            RenderVoice(v, bank, v.currentFreq, unison, detune, stereoWidth, filterType, cutoff, resonance, 0.0f, filterAmount, vOutL, vOutR);
            ApplyKeyTilt(v, vOutL, vOutR);
            v.keyGain += keyGainInc[0];
            // Free-running: no envelope, no velocity - the noise follows the
            // constant voice level (and a burst left over from note mode).
            const float noiseOut = RenderNoise(v, noiseLevel + transient * v.burstEnv, burstCoef) * v.keyGain;
            frameSumL += vOutL * v.keyGain + noiseOut;
            frameSumR += vOutR * v.keyGain + noiseOut;
         }
         else
         {
            for (int vIdx = 0; vIdx < kMaxVoices; vIdx++)
            {
               Voice& v = mVoices[vIdx];
               if (!v.active)
                  continue;

               const float ampEnv = v.amp.Process();
               const float filtEnv = v.filt.Process();

               if (!v.amp.IsActive())
               {
                  v.active = false;
                  continue;
               }

               if (glideActive)
               {
                  v.currentFreq = v.targetFreq + glideCoef * (v.currentFreq - v.targetFreq);
               }
               else
               {
                  v.currentFreq = v.targetFreq;
               }

               const float voiceHz = v.currentFreq * powf(2.0f, fine * 0.01f / 12.0f);
               float vOutL = 0.0f, vOutR = 0.0f;
               RenderVoice(v, bank, voiceHz, unison, detune, stereoWidth, filterType, cutoff, resonance, filtEnv, filterAmount, vOutL, vOutR);
               ApplyKeyTilt(v, vOutL, vOutR);

               // Noise sits after the voice filter (a lowpassed tone keeps
               // its air) and before drive/volume/pan. The burst ignores the
               // amp envelope (a strike is instant even under a slow attack);
               // the sustained layer follows it.
               v.keyGain += keyGainInc[vIdx];
               const float voiceGain = v.velocity * v.keyGain;
               const float gain = ampEnv * voiceGain;
               const float noiseOut = RenderNoise(v, noiseLevel * ampEnv + transient * v.burstEnv, burstCoef) * voiceGain;
               frameSumL += vOutL * gain + noiseOut;
               frameSumR += vOutR * gain + noiseOut;
            }
         }

         if (drive > 0.01f)
         {
            const float driveGain = 1.0f + drive * 5.0f;
            frameSumL = tanhf(frameSumL * driveGain) / (1.0f + drive * 0.5f);
            frameSumR = tanhf(frameSumR * driveGain) / (1.0f + drive * 0.5f);
         }

         float panL, panR;
         DspMath::EqualPowerPan(pan, panL, panR);
         outL[i] = frameSumL * volume * panL;
         outR[i] = frameSumR * volume * panR;
         mMonoScope[i] = (outL[i] + outR[i]) * 0.5f;
      }

      mScopeRing.Write(mMonoScope.data(), numFrames);

      // Land every ramped key gain exactly on its target (no float drift).
      for (int vIdx = 0; vIdx < kMaxVoices; vIdx++)
         mVoices[vIdx].keyGain = keyGainTarget[vIdx];

      if (isNoteDriven)
      {
         for (int vIdx = 0; vIdx < kMaxVoices; vIdx++)
         {
            if (mVoices[vIdx].active)
               activeCount++;
         }
      }
      else
      {
         activeCount = 1;
      }
      mActiveVoices.store(activeCount, std::memory_order_relaxed);
   }

   MeterRing& ScopeRing() { return mScopeRing; }
   int ActiveVoices() const { return mActiveVoices.load(std::memory_order_relaxed); }

private:
   struct Voice
   {
      bool active = false;
      bool held = false;
      int note = -1;
      int voiceId = 0;
      float velocity = 0.0f;
      float currentFreq = 220.0f;
      float targetFreq = 220.0f;
      uint64_t age = 0;
      double phase[kMaxUnison] = {};
      Envelope amp;
      Envelope filt;
      DspMath::TptSvf filter[2][SynthModes::kMaxFilterStages];
      DspMath::CombFilter comb[2];

      // Key tracking: the note the key params are measured from, the
      // current (block-ramped) key gain, and whether this voice still needs
      // its gain snapped (a fresh note-on) instead of ramped.
      float keyNote = 60.0f;
      float keyGain = 1.0f;
      bool keyFresh = true;
      // Key tilt shelf (one per channel, shared coefficients). shelfDb/shelfHz
      // are what the current coefficients were designed for.
      DspMath::Biquad shelf[2];
      float shelfNorm = 1.0f;
      float shelfDb = 0.0f;
      float shelfHz = 0.0f;
      bool shelfActive = false;
      // Noise layer: per-voice generator (seeded per voice index, so voices
      // are decorrelated and renders are deterministic), its highpass, and
      // the note-on burst envelope (1 at note-on, exponential decay).
      DspMath::WhiteNoise noise;
      DspMath::TptSvf noiseHp;
      float burstEnv = 0.0f;

      void Reset(double sampleRate, int index)
      {
         keyNote = 60.0f;
         keyGain = 1.0f;
         keyFresh = true;
         shelf[0].Reset();
         shelf[1].Reset();
         shelfNorm = 1.0f;
         shelfDb = 0.0f;
         shelfHz = 0.0f;
         shelfActive = false;
         noise.state = 0x9E3779B9u ^ ((uint32_t)(index + 1) * 0x85EBCA6Bu);
         if (noise.state == 0u)
            noise.state = 0x2545F491u;
         noiseHp.SetSampleRate(sampleRate);
         noiseHp.SetCutoff(3500.0f, 0.70710678f);
         noiseHp.Reset();
         burstEnv = 0.0f;

         active = false;
         held = false;
         note = -1;
         voiceId = 0;
         velocity = 0.0f;
         currentFreq = 220.0f;
         targetFreq = 220.0f;
         age = 0;
         for (int u = 0; u < kMaxUnison; u++)
            phase[u] = 0.0;
         amp.SetSampleRate(sampleRate);
         filt.SetSampleRate(sampleRate);
         for (int c = 0; c < 2; c++)
         {
            for (int s = 0; s < SynthModes::kMaxFilterStages; s++)
            {
               filter[c][s].SetSampleRate(sampleRate);
               filter[c][s].Reset();
            }
            comb[c].Reset();
         }
      }
   };

   void RenderVoice(Voice& v, const EquationDsp::EquationBank& bank, float baseFreq, int unison,
                    float detuneCents, float stereoWidth, int filterType, float cutoffHz,
                    float resonance, float filtEnv, float filterAmount, float& outL, float& outR)
   {
      outL = 0.0f;
      outR = 0.0f;
      if (baseFreq <= 0.0f)
         return;

      float sumL = 0.0f;
      float sumR = 0.0f;

      for (int u = 0; u < unison; u++)
      {
         const float spread = (unison > 1)
            ? detuneCents * 0.5f * (2.0f * (float)u / (float)(unison - 1) - 1.0f)
            : 0.0f;
         const float voiceFreq = baseFreq * powf(2.0f, spread / 1200.0f);
         const double inc = (double)voiceFreq / mSampleRate;

         const double randOffset = (double)kVoicePhaseSeed[u] * 0.5;
         const double readPhase = v.phase[u] + randOffset;

         const float s = EquationDsp::SampleBank(bank, readPhase, inc);

         v.phase[u] += inc;
         v.phase[u] -= floor(v.phase[u]);

         const float spreadPan = (unison > 1)
            ? stereoWidth * (2.0f * (float)u / (float)(unison - 1) - 1.0f)
            : 0.0f;
         float panL, panR;
         DspMath::EqualPowerPan(std::clamp(spreadPan, -1.0f, 1.0f), panL, panR);
         sumL += s * panL;
         sumR += s * panR;
      }

      const float norm = 1.0f / sqrtf((float)unison);
      sumL *= norm;
      sumR *= norm;

      const float modulatedCutoff = cutoffHz * exp2f(filterAmount * filtEnv);
      const float clampedHz = std::clamp(modulatedCutoff, 20.0f, 20000.0f);

      if (SynthModes::IsCombFilter(filterType))
      {
         const bool negative = SynthModes::CombIsNegative(filterType);
         float* chans[2] = { &outL, &outR };
         float in[2] = { sumL, sumR };
         for (int c = 0; c < 2; c++)
         {
            v.comb[c].SetParams(clampedHz, DspMath::CombFeedbackFromResonance(resonance), negative, mSampleRate);
            *chans[c] = v.comb[c].Process(in[c]);
         }
         return;
      }

      const int filterStages = SynthModes::FilterStages(filterType);
      if (filterStages > 0)
      {
         const float q = 0.707f + resonance * 9.0f;
         const int filterShape = SynthModes::FilterShapeOf(filterType);

         float* chans[2] = { &outL, &outR };
         float in[2] = { sumL, sumR };
         for (int c = 0; c < 2; c++)
         {
            float x = in[c];
            for (int s = 0; s < filterStages; s++)
            {
               v.filter[c][s].SetCutoff(clampedHz, q);
               const auto res = v.filter[c][s].Process(x);
               switch (filterShape)
               {
                  case SynthModes::kShapeHigh:  x = res.high;  break;
                  case SynthModes::kShapeBand:  x = res.band;  break;
                  case SynthModes::kShapeNotch: x = res.notch; break;
                  default:                      x = res.low;  break;
               }
            }
            *chans[c] = x;
         }
      }
      else
      {
         outL = sumL;
         outR = sumR;
      }
   }

   // Key tracking for one voice, once per block: the key gain's target for
   // this block (ramped linearly from the current gain, or snapped on a
   // fresh voice) and, when keyTilt * octaves is non-zero, a tilt shelf
   // designed for the voice's current pitch - redesigned only when the tilt
   // or the pitch (glide) moved since the last design.
   void PrepareKeyTracking(Voice& v, float keyLevel, float keyTilt, float invFrames, float& target, float& inc)
   {
      const float octavesFromC4 = (v.keyNote - 60.0f) / 12.0f;
      const float levelDb = std::clamp(keyLevel * octavesFromC4, -kKeyLevelMaxDb, kKeyLevelMaxDb);
      target = (levelDb == 0.0f) ? 1.0f : powf(10.0f, levelDb / 20.0f);
      if (v.keyFresh)
      {
         v.keyGain = target;
         v.keyFresh = false;
         inc = 0.0f;
      }
      else
      {
         inc = (target - v.keyGain) * invFrames;
      }

      const float tiltDb = std::clamp(keyTilt * octavesFromC4, -kKeyTiltMaxDb, kKeyTiltMaxDb);
      if (std::fabs(tiltDb) < 0.01f)
      {
         v.shelfActive = false;
         return;
      }
      const float f0 = std::max(v.currentFreq, 8.0f);
      const bool stale = !v.shelfActive || std::fabs(tiltDb - v.shelfDb) > 0.01f ||
                         std::fabs(f0 - v.shelfHz) > 0.002f * f0;
      if (!stale)
         return;
      DspMath::Biquad proto;
      float norm = 1.0f;
      DesignKeyTiltShelf(proto, norm, (double)f0, (double)tiltDb, mSampleRate);
      if (!v.shelfActive)
      {
         v.shelf[0].Reset();
         v.shelf[1].Reset();
      }
      for (DspMath::Biquad& s : v.shelf)
      {
         s.b0 = proto.b0;
         s.b1 = proto.b1;
         s.b2 = proto.b2;
         s.a1 = proto.a1;
         s.a2 = proto.a2;
      }
      v.shelfNorm = norm;
      v.shelfDb = tiltDb;
      v.shelfHz = f0;
      v.shelfActive = true;
   }

   static void ApplyKeyTilt(Voice& v, float& l, float& r)
   {
      if (!v.shelfActive)
         return;
      l = v.shelf[0].Process(l) * v.shelfNorm;
      r = v.shelf[1].Process(r) * v.shelfNorm;
   }

   // One sample of the voice's noise layer at `amount` (already the sum of
   // the sustained and burst levels), per channel, before the voice gain.
   // Nothing is generated at amount 0, so the default (both layers off)
   // leaves the tone bit-identical. The burst envelope always advances, so a
   // burst fired while transient was 0 cannot linger and pop out later.
   static float RenderNoise(Voice& v, float amount, float burstCoef)
   {
      float out = 0.0f;
      if (amount > 0.0f)
         out = v.noiseHp.Process(v.noise.Next() * kNoiseUnitRms).high * amount * kCentrePan;
      if (v.burstEnv > 0.0f)
      {
         v.burstEnv *= burstCoef;
         if (v.burstEnv < kBurstFloor)
            v.burstEnv = 0.0f;
      }
      return out;
   }

   double mSampleRate = 44100.0;
   NoteEventQueue* mNoteInbox = nullptr;
   int mNoteCursor = 0;

   ParamMailbox mMailbox;
   SampleSlotT<EquationDsp::EquationBank> mBankSlot;
   MeterRing mScopeRing;
   std::vector<float> mMonoScope;

   std::atomic<int> mUnison{1};
   std::atomic<int> mOctave{0};
   std::atomic<int> mSemi{0};
   std::atomic<int> mFilterType{1};
   std::atomic<float> mFilterAmount{0.0f};

   std::atomic<float> mAmpAdsr[4]{ 5.0f, 250.0f, 0.75f, 200.0f };
   std::atomic<float> mFilterAdsr[4]{ 5.0f, 300.0f, 0.4f, 250.0f };

   // Per-block (not smoothed) noise/key params: each is consumed once per
   // block (a decay coefficient, a filter corner, a gain target ramped
   // across the block, a shelf design), so a per-sample smoother would buy
   // nothing and ParamMailbox's one-call-per-sample contract would not hold.
   std::atomic<float> mTransientDecayMs{ 15.0f };
   std::atomic<float> mNoiseToneHz{ 3500.0f };
   std::atomic<float> mKeyLevel{ 0.0f };
   std::atomic<float> mKeyTilt{ 0.0f };

   Voice mVoices[kMaxVoices];
   uint64_t mAgeCounter = 0;
   float mLastNoteHz = 0.0f;
   bool mHasLastNote = false;
   std::atomic<int> mActiveVoices{0};
};

// ---------------------------------------------------------------------------
// EquationNode: Main Thread State & Graph Interface
// ---------------------------------------------------------------------------
EquationNode::EquationNode()
   : mAudioNode(std::make_unique<AudioEquationNode>())
{
   mPreviewCurve.resize(EquationDsp::kFrameSize, 0.0f);
   CompileEquation();
   RebuildBank();
}

EquationNode::~EquationNode() = default;

AudioNode* EquationNode::GetAudioNode()
{
   return mAudioNode.get();
}

int EquationNode::ReadScope(float* out, int capacity)
{
   if (!mAudioNode || !out || capacity <= 0)
      return 0;
   return mAudioNode->ScopeRing().Read(out, capacity);
}

int EquationNode::ActiveVoices() const
{
   return mAudioNode ? mAudioNode->ActiveVoices() : 0;
}

void EquationNode::LoadPreset(int index)
{
   if (index < 0 || index >= (int)kPresets.size())
      return;
   const auto& p = kPresets[index];
   presetIndex = index;
   formula = p.formula;
   domainMode = p.domainMode;
   knobA = p.a;
   knobB = p.b;
   knobC = p.c;
   knobD = p.d;
   CompileEquation();
   RebuildBank();
}

bool EquationNode::CompileEquation()
{
   EquationDsp::AstNodePtr newAst;
   std::string err;
   if (EquationDsp::Parser::Parse(formula, newAst, err))
   {
      mAst = newAst;
      mLastError.clear();
      return true;
   }
   mLastError = err;
   return false;
}

void EquationNode::RebuildBank()
{
   if (!mAst)
   {
      if (!CompileEquation() && !mAst)
      {
         // Create fallback sine AST if none exists
         EquationDsp::Parser::Parse("sin(2*pi*x)", mAst, mLastError);
      }
   }

   if (mAst)
   {
      EquationDsp::EquationBank bank;
      EquationDsp::BuildBankFromAst(bank, *mAst, domainMode, knobA, knobB, knobC, knobD, 0.0f);
      mPreviewCurve = bank.previewCurve;
      if (mAudioNode)
         mAudioNode->SwapBank(bank);
   }
}

void EquationNode::CookIfNeeded(int frameId)
{
   if (frameId == mLastCookFrame)
      return;
   mLastCookFrame = frameId;

   // Check if preset or formula or parameter knobs changed
   const bool formulaChanged = (formula != mLastFormula);
   const bool domainChanged = (domainMode != mLastDomainMode);
   const bool knobsChanged = (knobA != mLastKnobA || knobB != mLastKnobB || knobC != mLastKnobC || knobD != mLastKnobD);

   if (formulaChanged || domainChanged || knobsChanged)
   {
      if (formulaChanged)
         CompileEquation();

      RebuildBank();

      mLastFormula = formula;
      mLastDomainMode = domainMode;
      mLastKnobA = knobA;
      mLastKnobB = knobB;
      mLastKnobC = knobC;
      mLastKnobD = knobD;
   }

   if (mAudioNode)
   {
      mAudioNode->DrainRetiredBanks();
      mAudioNode->PushParams(*this);

      // Decimated scope read (up to 30 Hz refresh)
      float tempScope[kScopeCapacity];
      const int read = mAudioNode->ScopeRing().Read(tempScope, kScopeCapacity);
      if (read > 0)
      {
         const int toCopy = std::min(read, kScopeCapacity);
         std::copy_n(tempScope, toCopy, scopeCache);
         scopeCacheCount = toCopy;
      }
   }
}

void EquationNode::VisitParams(ParamVisitor& v)
{
   v.Text("formula", formula);
   v.Int("preset", presetIndex);
   v.Int("domainMode", domainMode);

   v.Float("knobA", knobA);
   v.Float("knobB", knobB);
   v.Float("knobC", knobC);
   v.Float("knobD", knobD);

   v.Float("volume", volume);
   v.Float("pan", pan);
   v.Float("frequency", frequency);
   v.Int("octave", octave);
   v.Int("semi", semi);
   v.Float("fine", fine);
   v.Float("glide", glide);

   v.Int("unison", unison);
   v.Float("detune", detune);
   v.Float("stereoWidth", stereoWidth);

   v.Float("ampAttack", ampAttack);
   v.Float("ampDecay", ampDecay);
   v.Float("ampSustain", ampSustain);
   v.Float("ampRelease", ampRelease);

   v.Int("filterType", filterType);
   v.Float("cutoff", cutoff);
   v.Float("resonance", resonance);
   v.Float("filterAmount", filterAmount);
   v.Float("filterAttack", filterAttack);
   v.Float("filterDecay", filterDecay);
   v.Float("filterSustain", filterSustain);
   v.Float("filterRelease", filterRelease);

   v.Float("drive", drive);

   v.Float("transient", transient);
   v.Float("transientDecay", transientDecay);
   v.Float("noiseLevel", noiseLevel);
   v.Float("noiseTone", noiseTone);
   v.Float("keyLevel", keyLevel);
   v.Float("keyTilt", keyTilt);
}
