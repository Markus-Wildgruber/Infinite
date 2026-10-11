#include "AudioTextureNode.h"
#include "audio/dsp/WaveTerrainDsp.h"
#include "audio/dsp/PortableFft.h"
#include <algorithm>
#include <cmath>

static const std::vector<std::string> kModeNames = { "Waveform", "Spectrum", "Spectrogram" };
static const std::vector<std::string> kWindowSizeNames = { "512", "1024", "2048", "4096" };
static const std::vector<std::string> kFftSizeNames = { "512", "1024", "2048", "4096", "8192" };
const int AudioTextureNode::kWindowSizes[] = { 512, 1024, 2048, 4096 };
const int AudioTextureNode::kFftSizes[] = { 512, 1024, 2048, 4096, 8192 };

const std::vector<std::string>& AudioTextureNode::ModeNames() { return kModeNames; }
const std::vector<std::string>& AudioTextureNode::WindowSizeNames() { return kWindowSizeNames; }
const std::vector<std::string>& AudioTextureNode::FftSizeNames() { return kFftSizeNames; }

// ---------------------------------------------------------------------------
// AudioTextureAudioSink: Audio Thread Sink
// ---------------------------------------------------------------------------
class AudioTextureAudioSink : public AudioNode
{
public:
   AudioTextureAudioSink() = default;
   ~AudioTextureAudioSink() override = default;

   void ProcessBlock(const AudioBuffer* const* inputs, int numInputs, AudioBuffer& output) override
   {
      const int numFrames = output.numFrames;
      if (numFrames <= 0)
         return;

      for (int ch = 0; ch < output.numChannels; ch++)
      {
         if (output.channels[ch])
            std::fill_n(output.channels[ch], numFrames, 0.0f);
      }

      const AudioBuffer* inBuf = nullptr;
      if (inputs != nullptr)
      {
         for (int s = 0; s < numInputs; s++)
         {
            if (inputs[s] != nullptr && inputs[s]->channels[0] != nullptr)
            {
               inBuf = inputs[s];
               break;
            }
         }
      }

      if (inBuf != nullptr)
      {
         const float* inL = inBuf->channels[0];
         const float* inR = (inBuf->numChannels > 1 && inBuf->channels[1] != nullptr) ? inBuf->channels[1] : inL;

         // Fixed stack chunk, not a per-block std::vector: this runs on the
         // audio thread, which must never allocate.
         constexpr int kChunk = 256;
         float mono[kChunk];
         for (int start = 0; start < numFrames; start += kChunk)
         {
            const int count = std::min(kChunk, numFrames - start);
            for (int i = 0; i < count; i++)
            {
               const int f = start + i;
               mono[i] = (inL[f] + inR[f]) * 0.5f;
               if (output.channels[0]) output.channels[0][f] = inL[f];
               if (output.numChannels > 1 && output.channels[1]) output.channels[1][f] = inR[f];
            }
            mMeterRing.Write(mono, count);
         }
      }
   }

   int ReadSamples(float* out, int capacity)
   {
      return mMeterRing.Read(out, capacity);
   }

private:
   MeterRing mMeterRing;
};

// ---------------------------------------------------------------------------
// AudioTextureNode (Main Thread INode Image Source)
// ---------------------------------------------------------------------------
AudioTextureNode::AudioTextureNode()
{
   mAudioSink = std::make_unique<AudioTextureAudioSink>();
   mHistory.assign(kMaxFftSize, 0.0f);
   mReadBuf.assign(MeterRing::kCapacity, 0.0f);
   mSmoothedData.assign(1024, 0.0f);
   mFftRe.assign(kMaxFftSize, 0.0f);
   mFftIm.assign(kMaxFftSize, 0.0f);
   mFft = std::make_unique<PortableFft::RealFft>();
   int maxLog2 = 0;
   while ((1 << maxLog2) < kMaxFftSize)
      maxLog2++;
   mFft->Prepare(maxLog2);
}

AudioTextureNode::~AudioTextureNode()
{
   DestroyTexture();
}

AudioNode* AudioTextureNode::GetAudioNode()
{
   if (!mAudioSink)
      mAudioSink = std::make_unique<AudioTextureAudioSink>();
   return mAudioSink.get();
}

unsigned int AudioTextureNode::GetOutputTexture()
{
   if (!mAudioInput.IsConnected())
      return 0;
   return mTexture;
}

int AudioTextureNode::GetOutputWidth() const
{
   return mTexW;
}

int AudioTextureNode::GetOutputHeight() const
{
   return mTexH;
}

float AudioTextureNode::SampleRate() const
{
   // Stamped by RebuildAudioTopology on the main thread whenever it prepares
   // the sink; never written by the audio thread.
   if (mAudioSink && mAudioSink->preparedForSampleRate > 0.0)
      return (float)mAudioSink->preparedForSampleRate;
   return 44100.0f;
}

void AudioTextureNode::ResolveDbRange(float& outFloor, float& outCeiling) const
{
   outFloor = std::clamp(dbFloor, -120.0f, -20.0f);
   outCeiling = std::clamp(dbCeiling, -40.0f, 12.0f);
   // The two ranges overlap (-40..-20); keep at least 1 dB of span so the
   // normalisation below never divides by zero or inverts.
   if (outCeiling < outFloor + 1.0f)
      outCeiling = outFloor + 1.0f;
}

void AudioTextureNode::ResolveFreqRange(float sampleRate, float& outMin, float& outMax) const
{
   const float nyquist = std::max(sampleRate * 0.5f, 400.0f);
   outMin = std::clamp(freqMin, 10.0f, 2000.0f);
   outMax = std::min(std::clamp(freqMax, 200.0f, 24000.0f), nyquist);
   // Keep max at least a semitone above min (an inverted or zero-width log
   // axis has no meaning); widen to an octave, staying under Nyquist.
   if (outMax < outMin * 1.0595f)
   {
      outMax = std::min(nyquist, outMin * 2.0f);
      if (outMax < outMin * 1.0595f)
         outMin = outMax * 0.5f;
   }
}

int AudioTextureNode::FftSize() const
{
   return kFftSizes[std::clamp(fftSizeIndex, 0, kFftSizeCount - 1)];
}

bool AudioTextureNode::EnsureTexture(int w, int h)
{
   if (mTexture != 0 && mTexW == w && mTexH == h)
      return false;

   if (mTexture != 0)
      glDeleteTextures(1, &mTexture);

   mTexW = w;
   mTexH = h;

   glGenTextures(1, &mTexture);
   glBindTexture(GL_TEXTURE_2D, mTexture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mTexW, mTexH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
   glBindTexture(GL_TEXTURE_2D, 0);
   return true;
}

void AudioTextureNode::DestroyTexture()
{
   if (mTexture != 0)
   {
      glDeleteTextures(1, &mTexture);
      mTexture = 0;
      mTexW = 0;
      mTexH = 0;
   }
}

void AudioTextureNode::UploadPixels(int w, int h)
{
   glBindTexture(GL_TEXTURE_2D, mTexture);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, mPixels.data());
   glBindTexture(GL_TEXTURE_2D, 0);
}

void AudioTextureNode::PushHistory(const float* samples, int count)
{
   if (count <= 0)
      return;
   const int n = (int)mHistory.size();
   if (count >= n)
   {
      std::copy(samples + (count - n), samples + count, mHistory.begin());
      return;
   }
   std::copy(mHistory.begin() + count, mHistory.end(), mHistory.begin());
   std::copy(samples, samples + count, mHistory.end() - count);
}

// Hann-windowed FFT of the newest fftSize history samples, written into
// mSmoothedData[0..fftSize/2) as dB-mapped 0..1 values after smoothing.
void AudioTextureNode::AnalyzeSpectrum(int fftSize, float sWeight, float dbLo, float dbHi)
{
   const int specSize = fftSize / 2;
   if ((int)mSmoothedData.size() != specSize)
      mSmoothedData.assign(specSize, 0.0f);

   if ((int)mHann.size() != fftSize)
   {
      mHann.resize(fftSize);
      for (int i = 0; i < fftSize; i++)
         mHann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979323846f * (float)i / (float)(fftSize - 1)));
   }

   const float* win = mHistory.data() + (mHistory.size() - (size_t)fftSize);
   const float* outRe = nullptr;
   const float* outIm = nullptr;
   float norm = 0.0f;
   const bool radix1024 = (fftSize == WaveTerrainDsp::Radix2FFT::N);
   if (radix1024)
   {
      // The original Spectrum path, kept for 1024 so the default output is
      // bit-identical to what saved patches rendered before.
      for (int i = 0; i < fftSize; i++)
      {
         mFftRe[i] = win[i] * mHann[i];
         mFftIm[i] = 0.0f;
      }
      WaveTerrainDsp::Radix2FFT::Instance().Forward(mFftRe.data(), mFftIm.data());
      outRe = mFftRe.data();
      outIm = mFftIm.data();
      norm = 2.0f / (float)fftSize;
   }
   else
   {
      // PortableFft writes a x2-scaled split spectrum (real half, then imag
      // half here), so 1/N gives the same 2/N single-sided amplitude.
      for (int i = 0; i < fftSize; i++)
         mFftRe[i] = win[i] * mHann[i];
      int log2N = 0;
      while ((1 << log2N) < fftSize)
         log2N++;
      mFft->Forward(mFftRe.data(), log2N, mFftIm.data(), mFftIm.data() + specSize);
      outRe = mFftIm.data();
      outIm = mFftIm.data() + specSize;
      norm = 1.0f / (float)fftSize;
   }

   // Log-compress each bin's magnitude before smoothing. A harmonically
   // sparse source (e.g. a wavetable) is near-zero in linear magnitude
   // between its harmonics, so almost the whole spectrum would read as
   // black without dB compression.
   const float dbSpan = dbHi - dbLo;
   for (int i = 0; i < specSize; i++)
   {
      // PortableFft packs Nyquist into bin 0's imaginary slot; DC has none.
      const float im = (!radix1024 && i == 0) ? 0.0f : outIm[i];
      const float mag = sqrtf(outRe[i] * outRe[i] + im * im) * norm * gain;
      const float dB = 20.0f * log10f(mag + 1e-6f);
      const float specVal = std::clamp((dB - dbLo) / dbSpan, 0.0f, 1.0f); // dbFloor..dbCeiling -> 0..1
      mSmoothedData[i] = mSmoothedData[i] * sWeight + specVal * (1.0f - sWeight);
   }
}

// Per spectrogram row: the fractional bin at its log-spaced centre frequency,
// and the whole bins its half-row band covers. Low rows span less than a bin
// and interpolate; high rows span many and take the peak, so a narrow partial
// between two row centres still shows instead of being stepped over.
void AudioTextureNode::BuildRowMap(int fftSize, float sampleRate, float fLo, float fHi)
{
   const int specSize = fftSize / 2;
   const float binWidth = sampleRate / (float)fftSize;
   const float ratio = fHi / fLo;
   const float step = 1.0f / (float)(kSpectrogramRows - 1);
   for (int r = 0; r < kSpectrogramRows; r++)
   {
      const float pos = (float)r * step;
      const float fc = fLo * std::pow(ratio, pos);
      const float fa = fLo * std::pow(ratio, pos - 0.5f * step);
      const float fb = fLo * std::pow(ratio, pos + 0.5f * step);
      mRowBinCentre[r] = std::clamp(fc / binWidth, 0.0f, (float)(specSize - 1));
      mRowBinLo[r] = std::clamp((int)std::ceil(fa / binWidth), 0, specSize - 1);
      mRowBinHi[r] = std::clamp((int)std::floor(fb / binWidth), 0, specSize - 1);
   }
}

void AudioTextureNode::WriteSpectrogramColumn(int specSize)
{
   if (mSpecCols <= 0)
      return;
   uint8_t* col = mSpecRing.data() + (size_t)mSpecWriteCol * kSpectrogramRows;
   for (int r = 0; r < kSpectrogramRows; r++)
   {
      const float binF = mRowBinCentre[r];
      const int bin0 = std::clamp((int)binF, 0, specSize - 1);
      const int bin1 = std::clamp(bin0 + 1, 0, specSize - 1);
      const float frac = binF - (float)bin0;
      float v = mSmoothedData[bin0] * (1.0f - frac) + mSmoothedData[bin1] * frac;
      for (int b = mRowBinLo[r]; b <= mRowBinHi[r]; b++)
         v = std::max(v, mSmoothedData[b]);
      col[r] = (uint8_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f);
   }
   mSpecWriteCol = (mSpecWriteCol + 1) % mSpecCols;
   mSpecDirty = true;
}

void AudioTextureNode::CookIfNeeded(int frameId)
{
   if (mLastCookFrame == frameId)
      return;
   mLastCookFrame = frameId;

   if (!mAudioInput.IsConnected())
      return;

   const int curMode = std::clamp(mode, 0, kModeCount - 1);
   const bool enteredMode = (curMode != mLastMode);
   mLastMode = curMode;
   if (enteredMode)
   {
      std::fill(mSmoothedData.begin(), mSmoothedData.end(), 0.0f);
      // A 4096 Waveform stages 64 MB; don't keep that around for a mode
      // that needs a fraction of it.
      std::vector<uint8_t>().swap(mPixels);
   }

   const float sWeight = std::clamp(smoothing, 0.0f, 0.99f);
   const float sampleRate = SampleRate();
   float dbLo = 0.0f;
   float dbHi = 0.0f;
   ResolveDbRange(dbLo, dbHi);
   float fLo = 0.0f;
   float fHi = 0.0f;
   ResolveFreqRange(sampleRate, fLo, fHi);
   const int fftSize = FftSize();
   const int specSize = fftSize / 2;

   if (curMode == kModeSpectrogram)
   {
      // Entering the mode starts from a clean (black) history rather than
      // whatever columns were left from the last time it was shown.
      const int cols = std::clamp(history, kHistoryMin, kHistoryMax);
      if (enteredMode || cols != mSpecCols)
      {
         mSpecCols = cols;
         mSpecRing.assign((size_t)cols * kSpectrogramRows, 0);
         mSpecWriteCol = 0;
         mSpecDirty = true;
      }
      BuildRowMap(fftSize, sampleRate, fLo, fHi);
   }

   // Drain everything the audio thread queued (the ring holds less than one
   // read's worth) so every mode shows the newest audio rather than lagging.
   const int readCount = mAudioSink ? mAudioSink->ReadSamples(mReadBuf.data(), (int)mReadBuf.size()) : 0;
   if (curMode == kModeSpectrogram)
   {
      // One analysis frame per kSpectrogramHop input samples, wherever the
      // hop boundary falls inside this batch, so the time axis runs at audio
      // rate regardless of how often the UI cooks.
      int pos = 0;
      while (pos < readCount)
      {
         const int take = std::min(readCount - pos, kSpectrogramHop - mHopFill);
         PushHistory(mReadBuf.data() + pos, take);
         pos += take;
         mHopFill += take;
         if (mHopFill >= kSpectrogramHop)
         {
            mHopFill = 0;
            AnalyzeSpectrum(fftSize, sWeight, dbLo, dbHi);
            WriteSpectrogramColumn(specSize);
         }
      }
   }
   else
   {
      PushHistory(mReadBuf.data(), readCount);
   }

   if (curMode == kModeWaveform)
   {
      const int winSize = kWindowSizes[std::clamp(windowSizeIndex, 0, kWindowSizeCount - 1)];
      const float* win = mHistory.data() + (mHistory.size() - (size_t)winSize);
      const int W = winSize;
      const int H = winSize;
      EnsureTexture(W, H);

      if ((int)mSmoothedData.size() != W)
         mSmoothedData.assign(W, 0.0f);

      mPixels.resize((size_t)W * H * 4);
      for (int i = 0; i < W; i++)
      {
         const float raw = win[i] * gain;
         mSmoothedData[i] = mSmoothedData[i] * sWeight + raw * (1.0f - sWeight);
         const float norm = std::clamp((mSmoothedData[i] + 1.0f) * 0.5f, 0.0f, 1.0f);
         const uint8_t byte = (uint8_t)(norm * 255.0f);

         mPixels[i * 4 + 0] = byte;
         mPixels[i * 4 + 1] = byte;
         mPixels[i * 4 + 2] = byte;
         mPixels[i * 4 + 3] = 255;
      }

      for (int y = 1; y < H; y++)
      {
         std::copy(mPixels.begin(), mPixels.begin() + (size_t)W * 4, mPixels.begin() + (size_t)y * W * 4);
      }

      UploadPixels(W, H);
      mRevision++;
   }
   else if (curMode == kModeSpectrum)
   {
      const int W = 512;
      const int H = 512;
      EnsureTexture(W, H);

      AnalyzeSpectrum(fftSize, sWeight, dbLo, dbHi);

      const float binWidth = sampleRate / (float)fftSize;
      const float ratio = fHi / fLo;

      mPixels.resize((size_t)W * H * 4);
      for (int x = 0; x < W; x++)
      {
         // Sample by log frequency (freqMin..freqMax, default 20Hz-20kHz)
         // rather than linear bin index, so harmonic content spreads across
         // the visible width instead of crowding into the first few columns.
         const float pos = (float)x / (float)(W - 1);
         const float freq = fLo * std::pow(ratio, pos);
         const float binF = std::clamp(freq / binWidth, 0.0f, (float)(specSize - 1));
         const int bin0 = std::clamp((int)binF, 0, specSize - 1);
         const int bin1 = std::clamp(bin0 + 1, 0, specSize - 1);
         const float frac = binF - (float)bin0;
         const float norm = mSmoothedData[bin0] * (1.0f - frac) + mSmoothedData[bin1] * frac;
         const uint8_t byte = (uint8_t)(std::clamp(norm, 0.0f, 1.0f) * 255.0f);

         mPixels[x * 4 + 0] = byte;
         mPixels[x * 4 + 1] = byte;
         mPixels[x * 4 + 2] = byte;
         mPixels[x * 4 + 3] = 255;
      }

      for (int y = 1; y < H; y++)
      {
         std::copy(mPixels.begin(), mPixels.begin() + (size_t)W * 4, mPixels.begin() + (size_t)y * W * 4);
      }

      UploadPixels(W, H);
      mRevision++;
   }
   else if (curMode == kModeSpectrogram)
   {
      // Time on X (oldest column at x = 0, newest at the right edge), log
      // frequency on Y (row 0 = freqMin, which GL samples at v = 0 - the
      // bottom of the previews). Re-uploaded only when a column landed or
      // the texture was just (re)allocated, so an idle spectrogram keeps its
      // TextureRevision and downstream caches hold.
      const int W = mSpecCols;
      const int H = kSpectrogramRows;
      if (EnsureTexture(W, H))
         mSpecDirty = true;

      if (mSpecDirty)
      {
         mPixels.resize((size_t)W * H * 4);
         for (int y = 0; y < H; y++)
         {
            uint8_t* dst = mPixels.data() + (size_t)y * W * 4;
            for (int x = 0; x < W; x++)
            {
               const int srcCol = (mSpecWriteCol + x) % W;
               const uint8_t byte = mSpecRing[(size_t)srcCol * H + y];
               dst[x * 4 + 0] = byte;
               dst[x * 4 + 1] = byte;
               dst[x * 4 + 2] = byte;
               dst[x * 4 + 3] = 255;
            }
         }

         UploadPixels(W, H);
         mRevision++;
         mSpecDirty = false;
      }
   }
}
