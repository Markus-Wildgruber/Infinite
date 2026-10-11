#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "gl3.h"

#include "core/INode.h"
#include "core/AudioCable.h"
#include "audio/AudioNode.h"
#include "audio/MeterRing.h"

class AudioTextureAudioSink;
namespace PortableFft { class RealFft; }

class AudioTextureNode : public INode
{
public:
   // Saved as an int - append only, never reorder.
   enum Mode
   {
      kModeWaveform = 0,
      kModeSpectrum,
      kModeSpectrogram, // scrolling time x log-frequency image
      kModeCount
   };

   static const std::vector<std::string>& ModeNames();
   static const std::vector<std::string>& WindowSizeNames(); // Waveform only
   static const std::vector<std::string>& FftSizeNames();    // Spectrum / Spectrogram
   static const int kWindowSizes[];
   static const int kFftSizes[];
   static constexpr int kWindowSizeCount = 4;
   static constexpr int kFftSizeCount = 5;
   static constexpr int kMaxFftSize = 8192;

   // Spectrogram geometry: one texture row per log-spaced frequency band,
   // one column per analysis frame. A frame is taken every kSpectrogramHop
   // input samples (not per UI frame), so the scroll speed is a property of
   // the audio, not of the render rate: ~86 columns/s at 44.1 kHz.
   static constexpr int kSpectrogramRows = 256;
   static constexpr int kSpectrogramHop = 512;
   static constexpr int kHistoryMin = 64;
   static constexpr int kHistoryMax = 1024;

   static INode* Create() { return new AudioTextureNode(); }
   AudioTextureNode();
   ~AudioTextureNode() override;

   unsigned int GetOutputTexture() override;
   int GetOutputWidth() const override;
   int GetOutputHeight() const override;
   unsigned long long TextureRevision() const override { return mRevision; }
   void CookIfNeeded(int frameId) override;

   AudioNode* GetAudioNode();
   AudioCable& GetAudioInput() { return mAudioInput; }
   AudioCable* AudioInputSlot(int slot) override { return slot == 0 ? &mAudioInput : nullptr; }
   bool RequiresAudioProcessing() const override { return mAudioInput.IsConnected(); }

   void VisitParams(ParamVisitor& v) override
   {
      v.Int("mode", mode);
      v.Int("windowSizeIndex", windowSizeIndex);
      v.Float("gain", gain);
      v.Float("smoothing", smoothing);
      v.Int("fftSizeIndex", fftSizeIndex);
      v.Float("dbFloor", dbFloor);
      v.Float("dbCeiling", dbCeiling);
      v.Float("freqMin", freqMin);
      v.Float("freqMax", freqMax);
      v.Int("history", history);
   }

   // Main thread. The sample rate the audio sink was last prepared at
   // (44.1 kHz until the topology has prepared it), and the dB / frequency
   // window the spectrum modes actually render after clamping: ceiling kept
   // above floor, max frequency kept at or below Nyquist and above min.
   float SampleRate() const;
   void ResolveDbRange(float& outFloor, float& outCeiling) const;
   void ResolveFreqRange(float sampleRate, float& outMin, float& outMax) const;
   int FftSize() const;

   int mode = kModeWaveform;
   int windowSizeIndex = 1; // 1024 default (Waveform)
   float gain = 1.0f;
   float smoothing = 0.0f;
   // Spectrum / Spectrogram analysis. The defaults reproduce the original
   // fixed 1024-point, -60..0 dB, 20 Hz..20 kHz Spectrum mode exactly; a
   // separate index from windowSizeIndex so an old Spectrum patch keeps its
   // 1024-point transform whatever its (then ignored) window setting was.
   int fftSizeIndex = 1;      // 512..8192, 1024 default
   float dbFloor = -60.0f;    // -120..-20 dB
   float dbCeiling = 0.0f;    // -40..+12 dB
   float freqMin = 20.0f;     // 10..2000 Hz
   float freqMax = 20000.0f;  // 200..24000 Hz, clamped to Nyquist
   int history = 256;         // Spectrogram columns, kHistoryMin..kHistoryMax

private:
   bool EnsureTexture(int w, int h);
   void DestroyTexture();
   void PushHistory(const float* samples, int count);
   void AnalyzeSpectrum(int fftSize, float sWeight, float dbLo, float dbHi);
   void BuildRowMap(int fftSize, float sampleRate, float fLo, float fHi);
   void WriteSpectrogramColumn(int specSize);
   void UploadPixels(int w, int h);

   std::unique_ptr<AudioTextureAudioSink> mAudioSink;
   AudioCable mAudioInput;

   unsigned int mTexture = 0;
   int mTexW = 0;
   int mTexH = 0;
   unsigned long long mRevision = 0;

   // Newest kMaxFftSize input samples, oldest first. Every mode reads its
   // window off the tail.
   std::vector<float> mHistory;
   std::vector<float> mReadBuf;     // MeterRing drain scratch
   std::vector<float> mSmoothedData;
   std::vector<float> mFftRe;
   std::vector<float> mFftIm;
   std::vector<float> mHann;        // cached for the current analysis size
   std::vector<uint8_t> mPixels;    // RGBA upload staging, reused across cooks
   std::unique_ptr<PortableFft::RealFft> mFft; // sizes other than 1024

   // Spectrogram ring: column-major, kSpectrogramRows grey bytes per column,
   // row 0 = lowest frequency. mSpecWriteCol is the next column to fill;
   // the oldest column is the one it is about to overwrite.
   std::vector<uint8_t> mSpecRing;
   int mSpecCols = 0;
   int mSpecWriteCol = 0;
   int mHopFill = 0;
   bool mSpecDirty = false;
   std::array<float, kSpectrogramRows> mRowBinCentre {};
   std::array<int, kSpectrogramRows> mRowBinLo {};
   std::array<int, kSpectrogramRows> mRowBinHi {};

   int mLastMode = -1;
   int mLastCookFrame = -1;
};
