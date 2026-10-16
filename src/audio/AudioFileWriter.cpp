#include "AudioFileWriter.h"
#include "WavFormat.h"
#include "shine.h"
#include "platform/AppPaths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <chrono>
#include <ctime>
#include <atomic>
#include <sys/stat.h>

#if defined(_WIN32)
   #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
   #endif
   #ifndef NOMINMAX
      #define NOMINMAX
   #endif
   #include <windows.h>
#endif

// Portable replacement for the former AudioFileWriter.mm: same WAV (raw RIFF)
// and MP3 (shine) paths, with FLAC written by libFLAC (fetched by CMake on
// Windows - dr_libs is decode-only, so it can't encode) instead of CoreAudio's
// ExtAudioFile. libFLAC is linked here and only here.
#include <FLAC/stream_encoder.h>

namespace
{
   int16_t FloatToPcm16(float v)
   {
      const float clamped = std::clamp(v, -1.0f, 1.0f);
      return (int16_t)std::lround(clamped * 32767.0f);
   }

   std::string ToLower(const std::string& str)
   {
      std::string res = str;
      for (char& c : res)
         c = (char)tolower((unsigned char)c);
      return res;
   }

   // mExtAudioFile keeps its name (header stability) but stores a libFLAC
   // stream encoder since the port off ExtAudioFile.
   // The TYPE is capitalized (FLAC__StreamEncoder) even though the functions
   // are lower_case - libFLAC naming is inconsistent by design.
   FLAC__StreamEncoder* AsFlac(void* handle) { return static_cast<FLAC__StreamEncoder*>(handle); }

   // fopen() on Windows reads a narrow path in the ANSI code page, so a UTF-8
   // path under a non-ASCII folder ("C:\\Users\\Müller\\Desktop") fails to open
   // and the whole export dies with "could not create". Convert and use
   // _wfopen; every other platform's fopen already takes UTF-8.
   FILE* OpenFileUtf8(const std::string& path, const char* mode)
   {
#if defined(_WIN32)
      const int pn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
      const int mn = MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
      if (pn <= 0 || mn <= 0)
         return nullptr;
      std::wstring wpath((size_t)pn, L'\0'), wmode((size_t)mn, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), pn);
      MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode.data(), mn);
      return _wfopen(wpath.c_str(), wmode.c_str());
#else
      return fopen(path.c_str(), mode);
#endif
   }

   void StatFileSize(const std::string& path, int64_t& outSize)
   {
#if defined(_WIN32)
      struct _stat64 st;
      const int pn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
      if (pn > 0)
      {
         std::wstring wpath((size_t)pn, L'\0');
         MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), pn);
         if (_wstat64(wpath.c_str(), &st) == 0)
            outSize = st.st_size;
      }
#else
      struct stat st;
      if (stat(path.c_str(), &st) == 0)
         outSize = st.st_size;
#endif
   }
}

AudioFileWriter::AudioFileWriter() = default;

AudioFileWriter::~AudioFileWriter()
{
   Close();
}

bool AudioFileWriter::IsOpen() const
{
   return mFile != nullptr || mExtAudioFile != nullptr || (mMp3File != nullptr && mShineHandle != nullptr);
}

AudioFileWriter::Format AudioFileWriter::FormatFromPath(const std::string& path)
{
   std::string lower = ToLower(path);
   if (lower.length() >= 5 && lower.substr(lower.length() - 5) == ".flac")
      return Format::Flac;
   if (lower.length() >= 4 && lower.substr(lower.length() - 4) == ".mp3")
      return Format::Mp3;
   return Format::Wav;
}

const char* AudioFileWriter::ExtensionForFormat(Format format)
{
   switch (format)
   {
      case Format::Flac: return "flac";
      case Format::Mp3:  return "mp3";
      case Format::Wav:
      default:           return "wav";
   }
}

int64_t AudioFileWriter::BytesWritten() const
{
   if (mFormat == Format::Wav)
      return mFramesWritten * mNumChannels * WavFormat::BytesPerSample((int)mWavEncoding);
   return mBytesWrittenDirect;
}

bool AudioFileWriter::Open(const std::string& path, double sampleRate, int numChannels, Format format,
                           WavEncoding wavEncoding)
{
   Close();
   mWavEncoding = wavEncoding;
   mWriteOk = true;

   if (sampleRate <= 0.0 || numChannels <= 0)
      return false;

   mPath = path;
   mSampleRate = sampleRate;
   mNumChannels = std::max(1, numChannels);
   mFramesWritten = 0;
   mBytesWrittenDirect = 0;

   if (format == Format::Auto)
      mFormat = FormatFromPath(path);
   else
      mFormat = format;

   if (mFormat == Format::Flac)
   {
      FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
      if (encoder != nullptr)
      {
         FLAC__stream_encoder_set_channels(encoder, (uint32_t)mNumChannels);
         FLAC__stream_encoder_set_bits_per_sample(encoder, 16); // matches the WAV path
         FLAC__stream_encoder_set_sample_rate(encoder, (uint32_t)std::lround(sampleRate));
         FLAC__stream_encoder_set_compression_level(encoder, 5);
#if defined(_WIN32)
         // init_file() takes a narrow path; hand libFLAC an already-open FILE
         // instead (it closes it in finish(), and "w+b" lets it rewrite
         // STREAMINFO at the end).
         FILE* flacFile = OpenFileUtf8(path, "w+b");
         const bool flacOk = flacFile != nullptr &&
            FLAC__stream_encoder_init_FILE(encoder, flacFile, nullptr, nullptr) ==
               FLAC__STREAM_ENCODER_INIT_STATUS_OK;
#else
         const bool flacOk = FLAC__stream_encoder_init_file(encoder, path.c_str(), nullptr, nullptr) ==
                             FLAC__STREAM_ENCODER_INIT_STATUS_OK;
#endif
         if (flacOk)
         {
            mExtAudioFile = encoder;
            return true;
         }
         FLAC__stream_encoder_delete(encoder);
      }
      // Same "fall back to WAV if the encoder refuses the format" behavior
      // the ExtAudioFile path had.
      mFormat = Format::Wav;
   }

   if (mFormat == Format::Mp3)
   {
      shine_config_t config;
      shine_set_config_mpeg_defaults(&config.mpeg);
      config.wave.samplerate = (int)std::lround(sampleRate);
      config.wave.channels = (mNumChannels == 1) ? PCM_MONO : PCM_STEREO;
      config.mpeg.bitr = 192;

      shine_t shine = shine_initialize(&config);
      if (!shine)
      {
         mFormat = Format::Wav;
      }
      else
      {
         mMp3File = OpenFileUtf8(path, "wb");
         if (!mMp3File)
         {
            shine_close(shine);
            return false;
         }
         mShineHandle = (void*)shine;
         return true;
      }
   }

   // Default / WAV format
   mFormat = Format::Wav;
   mFile = OpenFileUtf8(path, "wb");
   if (mFile == nullptr)
      return false;

   // Sizes are placeholders until CloseWav rewrites the header (WavFormat.h).
   const std::vector<uint8_t> header =
      WavFormat::BuildHeader((int)mWavEncoding, (uint32_t)std::lround(sampleRate), mNumChannels, 0);
   if (fwrite(header.data(), 1, header.size(), mFile) != header.size())
   {
      fclose(mFile);
      mFile = nullptr;
      return false;
   }
   return true;
}

void AudioFileWriter::Append(const float* interleaved, int frames)
{
   if (frames <= 0 || !IsOpen())
      return;

   if (mFormat == Format::Flac && mExtAudioFile != nullptr)
   {
      // Interleaved float in (same client shape as the old ExtAudioFileWrite),
      // converted to the 16-bit PCM the encoder was configured for.
      const int numSamples = frames * mNumChannels;
      static thread_local std::vector<FLAC__int32> scratch;
      scratch.resize((size_t)numSamples);
      for (int i = 0; i < numSamples; i++)
         scratch[i] = (FLAC__int32)FloatToPcm16(interleaved[i]);

      FLAC__stream_encoder_process_interleaved(AsFlac(mExtAudioFile), scratch.data(),
                                               (uint32_t)frames);
      mFramesWritten += frames;
      mBytesWrittenDirect += (int64_t)(frames * mNumChannels * 1.5); // approximate FLAC compressed size
      return;
   }

   if (mFormat == Format::Mp3 && mShineHandle != nullptr && mMp3File != nullptr)
   {
      const int numSamples = frames * mNumChannels;
      static thread_local std::vector<int16_t> scratch;
      scratch.resize((size_t)numSamples);
      for (int i = 0; i < numSamples; i++)
         scratch[i] = FloatToPcm16(interleaved[i]);

      shine_t s = (shine_t)mShineHandle;
      const int samplesPerPass = shine_samples_per_pass(s) * mNumChannels;
      int offset = 0;
      while (offset + samplesPerPass <= numSamples)
      {
         int written = 0;
         unsigned char* data = shine_encode_buffer_interleaved(s, scratch.data() + offset, &written);
         if (written > 0 && data != nullptr)
         {
            fwrite(data, 1, (size_t)written, mMp3File);
            mBytesWrittenDirect += written;
         }
         offset += samplesPerPass;
      }
      if (offset < numSamples)
      {
         int remainingSamples = numSamples - offset;
         std::vector<int16_t> padded(samplesPerPass, 0);
         std::memcpy(padded.data(), scratch.data() + offset, remainingSamples * sizeof(int16_t));
         int written = 0;
         unsigned char* data = shine_encode_buffer_interleaved(s, padded.data(), &written);
         if (written > 0 && data != nullptr)
         {
            fwrite(data, 1, (size_t)written, mMp3File);
            mBytesWrittenDirect += written;
         }
      }
      mFramesWritten += frames;
      return;
   }

   if (mFormat == Format::Wav && mFile != nullptr)
   {
      const size_t numSamples = (size_t)frames * (size_t)mNumChannels;
      const int enc = (int)mWavEncoding;
      static thread_local std::vector<uint8_t> scratch;
      scratch.resize(numSamples * (size_t)WavFormat::BytesPerSample(enc));
      WavFormat::Encode(enc, interleaved, numSamples, scratch.data());
      if (fwrite(scratch.data(), 1, scratch.size(), mFile) != scratch.size())
         mWriteOk = false;
      mFramesWritten += frames;
   }
}

void AudioFileWriter::CloseFlac()
{
   if (mExtAudioFile != nullptr)
   {
      // finish() pads the final block, flushes, and writes the STREAMINFO
      // sample counts; delete() releases the encoder itself.
      FLAC__StreamEncoder* encoder = AsFlac(mExtAudioFile);
      FLAC__stream_encoder_finish(encoder);
      FLAC__stream_encoder_delete(encoder);
      mExtAudioFile = nullptr;

      // Update actual file size
      StatFileSize(mPath, mBytesWrittenDirect);
   }
}

void AudioFileWriter::CloseMp3()
{
   if (mShineHandle != nullptr)
   {
      shine_t s = (shine_t)mShineHandle;
      int written = 0;
      unsigned char* data = shine_flush(s, &written);
      if (written > 0 && data != nullptr && mMp3File != nullptr)
      {
         fwrite(data, 1, (size_t)written, mMp3File);
         mBytesWrittenDirect += written;
      }
      shine_close(s);
      mShineHandle = nullptr;
   }
   if (mMp3File != nullptr)
   {
      fclose(mMp3File);
      mMp3File = nullptr;

      StatFileSize(mPath, mBytesWrittenDirect);
   }
}

void AudioFileWriter::CloseWav()
{
   if (mFile == nullptr)
      return;

   // RIFF chunks are word aligned: an odd-sized data chunk (24-bit mono with
   // an odd frame count) gets one pad byte, which the RIFF size counts.
   const int enc = (int)mWavEncoding;
   const uint64_t dataBytes = (uint64_t)mFramesWritten * (uint64_t)mNumChannels * (uint64_t)WavFormat::BytesPerSample(enc);
   if ((dataBytes & 1u) != 0)
   {
      const uint8_t zero = 0;
      if (fwrite(&zero, 1, 1, mFile) != 1)
         mWriteOk = false;
   }
   const std::vector<uint8_t> header =
      WavFormat::BuildHeader(enc, (uint32_t)std::lround(mSampleRate), mNumChannels, (uint64_t)mFramesWritten);
   if (fseek(mFile, 0, SEEK_SET) != 0 || fwrite(header.data(), 1, header.size(), mFile) != header.size())
      mWriteOk = false;
   if (fclose(mFile) != 0)
      mWriteOk = false;
   mFile = nullptr;
}

void AudioFileWriter::Close()
{
   CloseWav();
   CloseFlac();
   CloseMp3();
}

namespace AudioRecordings
{
   std::string GetRecordingsDirectory()
   {
      std::string base = AppPaths::AppSupportDir();
      if (base.empty())
         base = AppPaths::TempDir() + "/Infinite";

      std::string dir = base + "/Recordings";
      AppPaths::EnsureDir(dir);
      return dir;
   }

   std::string GenerateFilePath(const std::string& prefix, const std::string& extension)
   {
      const std::string dir = GetRecordingsDirectory();
      auto now = std::chrono::system_clock::now();
      auto timeT = std::chrono::system_clock::to_time_t(now);
      std::tm tmVal {};
#if defined(_WIN32)
      localtime_s(&tmVal, &timeT);
#else
      localtime_r(&timeT, &tmVal);
#endif

      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
      static std::atomic<uint32_t> sCounter{ 0 };
      uint32_t count = ++sCounter;

      char filename[256];
      snprintf(filename, sizeof(filename), "recording_%s_%04d%02d%02d_%02d%02d%02d_%03d_%u.%s",
               prefix.c_str(),
               tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
               tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec,
               (int)ms.count(), count, extension.c_str());

      return dir + "/" + filename;
   }

   bool WriteWav(const std::string& path, const float* data, int frames, double sampleRate, int numChannels)
   {
      if (data == nullptr || frames <= 0 || sampleRate <= 0.0)
         return false;

      AudioFileWriter writer;
      if (!writer.Open(path, sampleRate, numChannels, AudioFileWriter::Format::Wav))
         return false;

      writer.Append(data, frames);
      writer.Close();
      return true;
   }
}
