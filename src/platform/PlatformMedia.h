#pragma once
// Model loading, text outlines/raster, video decoding, background removal.
// Split out of Platform.h (docs/plans/compile-times/include-split.md, Phase 4);
// every Platform:: function still has macOS, Windows and Linux sides.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace Bench
{
   struct MediaDecodeStats; // core/BenchMediaIo.h
}

namespace Platform
{
   // ---- 3D model loading --------------------------------------------------
   // Goes through ModelIO for the same reason image decoding goes through
   // ImageIO: the OS already reads OBJ, PLY, STL, USD and USDZ, and a bundled
   // importer would be a far heavier dependency than the code it replaces.
   // Not covered: glTF and FBX, which ModelIO does not read.
   //
   // Returns interleaved position/normal/uv vertices and triangle indices,
   // matching the layout of the Vertex struct in core/Mesh.h. Normals are
   // generated when the file has none.
   struct ModelVertex
   {
      float px = 0, py = 0, pz = 0;
      float nx = 0, ny = 0, nz = 1;
      float u = 0, v = 0;
   };

   std::string OpenModelDialog();

   // Patch files. Save returns the chosen path, or "" if cancelled.
   std::string OpenPatchDialog();
   std::string SavePatchDialog(const std::string& suggestedName);

   // .field device files (and legacy .infdev). Same contract as the patch
   // dialogs above - Save returns the chosen path, or "" if cancelled.
   std::string OpenDeviceDialog();
   std::string SaveDeviceDialog(const std::string& suggestedName = "Untitled.field");

   // ---- text outlines -----------------------------------------------------
   // Glyph outlines for a laid-out string, flattened to polygons in font units
   // normalised so cap height is roughly 1. Curves are subdivided here because
   // CoreGraphics has no public path-flattening call.
   //
   // Winding is whatever the font format uses and is deliberately not
   // normalised here - TrueType winds outer contours clockwise and CFF the
   // other way. The triangulator decides outline-versus-hole by nesting depth
   // instead, which works for both.
   struct TextContour
   {
      std::vector<float> points; // x,y pairs
   };

   // Installed font families, sorted, for the Text nodes' font pickers.
   const std::vector<std::string>& AvailableFontFamilies();

   bool GetTextOutlines(const std::string& text, const std::string& fontName,
                        float letterSpacing, std::vector<TextContour>& outContours,
                        std::string& outError);

   struct TextRasterRequest
   {
      std::string text;
      std::string fontName;
      float fontSize = 24.0f;
      float color[3] = { 1, 1, 1 };
      float tracking = 0.0f;
      float posX = 0.5f;
      float posY = 0.5f;
      int align = 1; // 0 left, 1 center, 2 right, 3 justified
      bool wordWrap = false;
      bool fitToBox = false;
      float wrapWidth = 0.9f;  // fraction of canvas width
      float wrapHeight = 0.9f; // fraction of canvas height
      float lineSpacing = 1.0f;
      float outlineWidth = 0.0f;
      float outlineColor[3] = { 0, 0, 0 };
      bool outlineOnly = false;
      float scaleX = 1.0f;
      float scaleY = 1.0f;
   };

   void RasterizeText(const TextRasterRequest& req, int width, int height,
                      unsigned char* outPixelsRGBA, float& outFittedSize);

   bool LoadModel(const std::string& path, std::vector<ModelVertex>& outVertices,
                  std::vector<unsigned int>& outIndices, std::string& outError);

   // ---- video decoding ----------------------------------------------------
   // Opaque handle around a platform decoder (AVAssetReader, Media Foundation,
   // FFmpeg) that decodes ahead on its own thread. Frames are delivered as
   // RGBA8, tightly packed, already row-flipped for OpenGL.
   struct VideoHandle;

   std::string OpenVideoDialog();

   VideoHandle* VideoOpen(const std::string& path, std::string& outError);
   void VideoClose(VideoHandle* handle);
   int VideoWidth(VideoHandle* handle);
   int VideoHeight(VideoHandle* handle);
   double VideoDuration(VideoHandle* handle);

   // Hands back the newest decoded frame covering `seconds`. Returns true only
   // when that is a new frame (so the caller can skip re-uploading); false
   // leaves outPixels as they were. A jump (backward, or well ahead) asks the
   // decode thread to seek, which is how looping works; until it has the new
   // position the caller keeps its previous frame.
   bool VideoFrameAt(VideoHandle* handle, double seconds, std::vector<unsigned char>& outPixels);

   // True while the decoder has not yet read as far as the position last asked
   // for through VideoFrameAt - i.e. a false return from VideoFrameAt means
   // "not yet", not "never". Once it goes false the answer is settled and
   // waiting longer cannot change it, so it is safe to loop on.
   //
   // Decoding runs on its own thread on every platform, so a caller stepping
   // faster than real time (an offline render, a self-test) has to wait for
   // the decoder instead of racing past it - see VideoFrameAtExact. Realtime playback must NOT wait
   // on this - holding the previous frame is the whole point.
   bool VideoDecodeIsCatchingUp(VideoHandle* handle);

   // VideoFrameAt for offline renders, which must show the exact frame covering
   // `seconds` rather than whatever the decoder has reached: keeps asking until
   // the decoder has caught up (bounded, so a dead decoder cannot hang a render),
   // then asks once more so a frame that landed during the last wait is picked
   // up. Returns true if any call produced a new frame.
   inline bool VideoFrameAtExact(VideoHandle* handle, double seconds, std::vector<unsigned char>& outPixels)
   {
      bool produced = VideoFrameAt(handle, seconds, outPixels);
      if (!VideoDecodeIsCatchingUp(handle))
         return produced;
      const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (VideoDecodeIsCatchingUp(handle) && std::chrono::steady_clock::now() < giveUp)
      {
         if (VideoFrameAt(handle, seconds, outPixels))
            produced = true;
         else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return VideoFrameAt(handle, seconds, outPixels) || produced;
   }

   // B8 bench only: the decode stats this handle has been recording since
   // VideoOpen, or nullptr when Bench::MediaIoEnabled() was off at open (every
   // handle outside INFINITE_BENCH_B8). See core/BenchMediaIo.h for who writes
   // which field on which thread.
   Bench::MediaDecodeStats* VideoBenchStats(VideoHandle* handle);

   // Decodes a video container's audio track into the same planar-float
   // SampleBuffer (defined below) the audio-file path already produces.
   // Returns false with a reason in outError when the file has no audio
   // track - which is a normal case for a lot of VJ footage, not a failure -
   // vs. an actual decode failure; callers distinguish the two by checking
   // whether outError is exactly "no audio track in this file".
   //
   // Decodes the whole track up front, into memory (a three-minute stereo
   // 48kHz track is ~70MB - acceptable, and it's what SampleSlotT and the
   // sampler playback path already assume). Streaming decode from the audio
   // thread would be a much larger change and is deliberately not what this
   // does - don't "improve" it into one without deciding that separately.
   //
   // Main-thread only - not real-time safe. Cannot reuse DecodeAudioFileToBuffer:
   // that goes through AVAudioFile, an audio-file API that does not reliably
   // open movie containers, unlike AVAssetReader here (same class the video
   // decoder above already uses).
   struct SampleBuffer;
   bool DecodeVideoAudioTrackToBuffer(const std::string& path, SampleBuffer& outBuffer,
                                      std::string& outError);

   // ---- background removal ----
   // macOS uses Vision's on-device segmentation: no model download, no
   // network, no API key. Subject lifting (any salient foreground) needs
   // macOS 14; person segmentation works from macOS 12. Returns false with a
   // reason otherwise.
   //
   // Windows has no Vision equivalent, so it bundles a small on-device model
   // (u2netp, see assets/models/NOTICE.txt) run through ONNX Runtime with the
   // DirectML execution provider instead - still no network access or API
   // key at runtime, just a model file shipped with the build rather than
   // fetched from the OS. See src/platform/win/PlatformWin.cpp.
   enum class MattingMode
   {
      Subject, // any salient foreground object (macOS 14+)
      Person   // people only (macOS 12+)
   };

   bool SubjectMask(const std::vector<unsigned char>& rgbaPixels, int width, int height,
                    MattingMode mode, std::vector<unsigned char>& outMask,
                    std::string& outError);

   // Human-readable name of the compute path the background remover uses, for
   // the node UI - "Apple Vision (on-device)" on macOS, and on Windows either
   // "DirectML GPU (DX12)" or "CPU (DirectML unavailable)" depending on whether
   // the DirectML execution provider registered. Cheap to call every frame: it
   // does NOT construct the ONNX session, it only reports what the last (or
   // pending) SubjectMask call resolved to.
   std::string MattingBackend();
   const std::vector<std::string>& MattingModeNames();

}
