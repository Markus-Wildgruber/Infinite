#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Multi-format audio file writer supporting WAV (.wav), FLAC (.flac), and MP3 (.mp3).
// Main thread only - Append is called from the per-frame drain of an AudioCaptureRing.
class AudioFileWriter
{
public:
   enum class Format
   {
      Auto,
      Wav,
      Flac,
      Mp3
   };

   // Sample encoding of a WAV file (ignored for FLAC/MP3). Pcm16/Pcm24 are
   // WAVE_FORMAT_PCM (fmt chunk 16 bytes); Float32 is WAVE_FORMAT_IEEE_FLOAT
   // (fmt chunk 18 bytes, cbSize 0) followed by the fact chunk its spec
   // requires. Nothing else is ever written: no LIST/acid/smpl/bext/cue.
   // Layout in src/audio/WavFormat.h.
   enum class WavEncoding
   {
      Pcm16,
      Pcm24,
      Float32
   };

   AudioFileWriter();
   ~AudioFileWriter();

   bool IsOpen() const;

   // Opens `path` in the format deduced from extension (or explicitly specified).
   // Returns false if the file couldn't be created or initialized.
   // wavEncoding picks the WAV sample format; FLAC follows it (Pcm16 -> 16-bit,
   // Pcm24 and Float32 -> 24-bit); MP3 ignores it.
   bool Open(const std::string& path, double sampleRate, int numChannels, Format format = Format::Auto,
             WavEncoding wavEncoding = WavEncoding::Pcm16);
   // The same by bit depth: 16 or 24 (PCM, WAV and FLAC), 32 = float WAV (24-bit FLAC).
   bool Open(const std::string& path, double sampleRate, int numChannels, Format format, int bitDepth);

   // Converts `frames` interleaved float samples to the appropriate destination format and appends them.
   void Append(const float* interleaved, int frames);

   // Finalizes file headers / tags / encoders and closes the file. Safe to call multiple times.
   void Close();

   int64_t FramesWritten() const { return mFramesWritten; }
   int64_t BytesWritten() const;
   const std::string& Path() const { return mPath; }
   Format GetFormat() const { return mFormat; }
   WavEncoding GetWavEncoding() const { return mWavEncoding; }
   // False once a write to the file failed (disk full, removed volume): the
   // caller should stop and report it; Close() still fixes up the header.
   bool WriteOk() const { return mWriteOk; }

   static Format FormatFromPath(const std::string& path);
   static const char* ExtensionForFormat(Format format);

private:
   void CloseWav();
   void CloseFlac();
   void CloseMp3();

   std::string mPath;
   Format mFormat = Format::Wav;
   int mNumChannels = 2;
   int mBitDepth = 16;
   double mSampleRate = 44100.0;
   int64_t mFramesWritten = 0;
   int64_t mBytesWrittenDirect = 0;
   WavEncoding mWavEncoding = WavEncoding::Pcm16;
   bool mWriteOk = true;

   // WAV state
   FILE* mFile = nullptr;

   // FLAC state (libFLAC stream encoder on Windows, named for the CoreAudio
   // handle it held before the port)
   void* mExtAudioFile = nullptr;

   // MP3 state
   void* mShineHandle = nullptr;
   FILE* mMp3File = nullptr;
};

// Aliased as WavWriter for backwards compatibility with any existing nodes
using WavWriter = AudioFileWriter;

namespace AudioRecordings
{
   std::string GetRecordingsDirectory();
   std::string GenerateFilePath(const std::string& prefix, const std::string& extension = "wav");
   bool WriteWav(const std::string& path, const float* data, int frames, double sampleRate, int numChannels = 1);
}
