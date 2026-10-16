#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The exact WAV layout AudioFileWriter writes (the portable AudioFileWriter.cpp
// and the macOS AudioFileWriter.mm share it), plus a strict reader the
// render-to-file self-test checks files with.
//
//   PCM 16/24-bit (44-byte header):
//     "RIFF" <fileBytes-8> "WAVE"
//     "fmt " 16  <1> <channels> <rate> <rate*blockAlign> <blockAlign> <bits>
//     "data" <dataBytes> <samples...> [pad byte when dataBytes is odd]
//   IEEE float 32-bit (58-byte header):
//     "RIFF" <fileBytes-8> "WAVE"
//     "fmt " 18  <3> <channels> <rate> <rate*blockAlign> <blockAlign> <32> <cbSize 0>
//     "fact" 4   <frames>
//     "data" <dataBytes> <samples...>
//
// No other chunk is ever written - no LIST/INFO, acid, smpl, bext, cue or
// tempo: a DAW must not read a tempo or loop points from a render.
// WAVE_FORMAT_EXTENSIBLE is not used; every reader takes these plain tags.
// Everything is little-endian, as RIFF requires (the samples are copied as
// the host's own float/integer layout, which is little-endian on every
// platform Infinite builds for).
namespace WavFormat
{
   enum Encoding
   {
      kPcm16 = 0,
      kPcm24 = 1,
      kFloat32 = 2
   };

   inline int BytesPerSample(int enc) { return enc == kPcm16 ? 2 : (enc == kPcm24 ? 3 : 4); }
   inline uint16_t FormatTag(int enc) { return enc == kFloat32 ? (uint16_t)3 : (uint16_t)1; }
   inline int HeaderBytes(int enc) { return enc == kFloat32 ? 58 : 44; }

   // The largest data chunk a RIFF file can describe: every size field is 32
   // bits and the RIFF size counts the header (minus 8) and the pad byte too.
   inline uint64_t MaxDataBytes(int enc) { return 0xFFFFFFFFull - (uint64_t)(HeaderBytes(enc) - 8) - 1ull; }

   namespace detail
   {
      inline void Put16(uint8_t*& p, uint32_t v)
      {
         p[0] = (uint8_t)(v & 0xff);
         p[1] = (uint8_t)((v >> 8) & 0xff);
         p += 2;
      }
      inline void Put32(uint8_t*& p, uint32_t v)
      {
         p[0] = (uint8_t)(v & 0xff);
         p[1] = (uint8_t)((v >> 8) & 0xff);
         p[2] = (uint8_t)((v >> 16) & 0xff);
         p[3] = (uint8_t)((v >> 24) & 0xff);
         p += 4;
      }
      inline void PutTag(uint8_t*& p, const char* tag)
      {
         std::memcpy(p, tag, 4);
         p += 4;
      }
      inline uint32_t Get32(const uint8_t* p)
      {
         return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
      }
      inline uint16_t Get16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }
   }

   // The whole header for `frames` sample frames. Written once with 0 when the
   // file opens and again with the real count when it closes.
   inline std::vector<uint8_t> BuildHeader(int enc, uint32_t sampleRate, int channels, uint64_t frames)
   {
      using namespace detail;
      const uint32_t bps = (uint32_t)BytesPerSample(enc);
      const uint32_t blockAlign = (uint32_t)std::max(1, channels) * bps;
      const uint64_t dataBytes64 = std::min<uint64_t>(frames * blockAlign, MaxDataBytes(enc));
      const uint32_t dataBytes = (uint32_t)dataBytes64;
      const uint32_t pad = dataBytes & 1u;
      const uint32_t riffSize = (uint32_t)(HeaderBytes(enc) - 8) + dataBytes + pad;
      std::vector<uint8_t> h((size_t)HeaderBytes(enc), 0);
      uint8_t* p = h.data();
      PutTag(p, "RIFF");
      Put32(p, riffSize);
      PutTag(p, "WAVE");
      PutTag(p, "fmt ");
      Put32(p, enc == kFloat32 ? 18u : 16u);
      Put16(p, FormatTag(enc));
      Put16(p, (uint32_t)std::max(1, channels));
      Put32(p, sampleRate);
      Put32(p, sampleRate * blockAlign);
      Put16(p, blockAlign);
      Put16(p, bps * 8u);
      if (enc == kFloat32)
      {
         Put16(p, 0); // cbSize: no extension
         PutTag(p, "fact");
         Put32(p, 4);
         Put32(p, (uint32_t)std::min<uint64_t>(frames, 0xFFFFFFFFull));
      }
      PutTag(p, "data");
      Put32(p, dataBytes);
      return h;
   }

   // `count` interleaved floats -> `count * BytesPerSample(enc)` bytes at `out`.
   // PCM clamps to [-1, 1] and rounds (16-bit: x 32767, 24-bit: x 8388607, the
   // same symmetric scaling the 16-bit path always used); float keeps the value
   // as is (a render may go over 0 dBFS) except a NaN/inf, written as 0.
   inline void Encode(int enc, const float* in, size_t count, uint8_t* out)
   {
      if (enc == kPcm16)
      {
         for (size_t i = 0; i < count; i++)
         {
            const float v = std::isfinite(in[i]) ? std::clamp(in[i], -1.0f, 1.0f) : 0.0f;
            const int32_t s = (int32_t)std::lround(v * 32767.0f);
            out[0] = (uint8_t)(s & 0xff);
            out[1] = (uint8_t)((s >> 8) & 0xff);
            out += 2;
         }
      }
      else if (enc == kPcm24)
      {
         for (size_t i = 0; i < count; i++)
         {
            const double v = std::isfinite(in[i]) ? std::clamp((double)in[i], -1.0, 1.0) : 0.0;
            const int32_t s = (int32_t)std::lround(v * 8388607.0);
            out[0] = (uint8_t)(s & 0xff);
            out[1] = (uint8_t)((s >> 8) & 0xff);
            out[2] = (uint8_t)((s >> 16) & 0xff);
            out += 3;
         }
      }
      else
      {
         for (size_t i = 0; i < count; i++)
         {
            const float v = std::isfinite(in[i]) ? in[i] : 0.0f;
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            uint8_t* p = out;
            detail::Put32(p, bits);
            out += 4;
         }
      }
   }

   // ---- reading (tests / tools) -------------------------------------------
   struct Info
   {
      bool ok = false;
      std::string error;
      uint16_t formatTag = 0;
      uint16_t channels = 0;
      uint32_t sampleRate = 0;
      uint32_t byteRate = 0;
      uint16_t blockAlign = 0;
      uint16_t bitsPerSample = 0;
      uint32_t fmtSize = 0;
      uint32_t riffSize = 0;
      bool hasFact = false;
      uint32_t factFrames = 0;
      size_t dataOffset = 0;
      uint32_t dataBytes = 0;
      uint64_t frames = 0;
      std::vector<std::string> chunks; // chunk ids in file order
   };

   // Walks every chunk of a whole file image; fails on anything malformed.
   inline Info Parse(const std::vector<uint8_t>& f)
   {
      using namespace detail;
      Info info;
      if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) != 0 || std::memcmp(f.data() + 8, "WAVE", 4) != 0)
      {
         info.error = "not a RIFF/WAVE file";
         return info;
      }
      info.riffSize = Get32(f.data() + 4);
      size_t pos = 12;
      bool haveFmt = false, haveData = false;
      while (pos + 8 <= f.size())
      {
         const std::string id((const char*)f.data() + pos, 4);
         const uint32_t size = Get32(f.data() + pos + 4);
         info.chunks.push_back(id);
         const size_t body = pos + 8;
         if (body + size > f.size())
         {
            info.error = "chunk '" + id + "' runs past the end of the file";
            return info;
         }
         if (id == "fmt " && size >= 16)
         {
            haveFmt = true;
            info.fmtSize = size;
            info.formatTag = Get16(f.data() + body);
            info.channels = Get16(f.data() + body + 2);
            info.sampleRate = Get32(f.data() + body + 4);
            info.byteRate = Get32(f.data() + body + 8);
            info.blockAlign = Get16(f.data() + body + 12);
            info.bitsPerSample = Get16(f.data() + body + 14);
         }
         else if (id == "fact" && size >= 4)
         {
            info.hasFact = true;
            info.factFrames = Get32(f.data() + body);
         }
         else if (id == "data")
         {
            haveData = true;
            info.dataOffset = body;
            info.dataBytes = size;
         }
         pos = body + size + (size & 1u);
      }
      if (!haveFmt || !haveData)
      {
         info.error = !haveFmt ? "no fmt chunk" : "no data chunk";
         return info;
      }
      if (info.blockAlign == 0)
      {
         info.error = "block align 0";
         return info;
      }
      info.frames = info.dataBytes / info.blockAlign;
      info.ok = true;
      return info;
   }

   // Sample `i` (interleaved index) of a parsed file as float.
   inline float SampleAt(const std::vector<uint8_t>& f, const Info& info, uint64_t i)
   {
      const uint8_t* p = f.data() + info.dataOffset + (size_t)i * (info.bitsPerSample / 8);
      if (info.formatTag == 3 && info.bitsPerSample == 32)
      {
         const uint32_t bits = detail::Get32(p);
         float v = 0.0f;
         std::memcpy(&v, &bits, 4);
         return v;
      }
      if (info.bitsPerSample == 24)
      {
         int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
         if (s & 0x800000)
            s -= 0x1000000;
         return (float)((double)s / 8388607.0);
      }
      if (info.bitsPerSample == 16)
         return (float)((int16_t)detail::Get16(p)) / 32767.0f;
      return 0.0f;
   }
}
