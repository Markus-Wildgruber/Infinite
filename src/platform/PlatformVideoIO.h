#pragma once
// Movie recording, movie inspection, Syphon sharing, live camera input.
// Split out of Platform.h (docs/plans/compile-times/include-split.md, Phase 4);
// every Platform:: function still has macOS, Windows and Linux sides.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace Platform
{
   // ---- video recording ---------------------------------------------------
   struct RecorderHandle;

   // audioPath is optional. When given, its samples are read independently of
   // any playback the Audio File node is doing and muxed in alongside the
   // video - recording does not depend on the file actually being audible.
   // When liveAudioSampleRate > 0, an audio track is configured to receive
   // streaming audio via RecorderAppendAudio.
   RecorderHandle* RecorderStart(const std::string& path, int width, int height,
                                 int fps, std::string& outError,
                                 const std::string& audioPath = std::string(),
                                 bool loopAudio = true,
                                 double liveAudioSampleRate = 0.0,
                                 int liveAudioChannels = 2,
                                 bool proRes4444 = false);
   // proRes4444 writes Apple ProRes 4444 with the frame's alpha kept (a .mov
   // only). macOS only: the other platforms refuse it through outError.
   // `pixels` is BGRA8 bottom-up by default - the GPU's native glReadPixels
   // format (GL_BGRA/GL_UNSIGNED_INT_8_8_8_8_REV) on essentially all desktop
   // GPUs, which turns the readback into a straight blit instead of a
   // driver-side conversion. See RecorderSetInputIsBgra below for the RGBA
   // fallback path.
   bool RecorderAppend(RecorderHandle* handle, const std::vector<unsigned char>& pixels);

   // Tells the recorder whether pixels handed to RecorderAppend for the rest
   // of this take are BGRA8 (the default assumed at RecorderStart) or RGBA8 -
   // the fallback a caller uses when its GPU driver rejects a native BGRA
   // glReadPixels. Call before the first RecorderAppend of the take; the
   // format is fixed for the take's whole duration once appends begin.
   void RecorderSetInputIsBgra(RecorderHandle* handle, bool isBgra);

   // Hands the caller a buffer from the recorder's own pool, already sized
   // width*height*4. Recycling these is what lets the render thread hand off
   // ownership instead of copying into the encoder's memory on the spot.
   std::vector<unsigned char> RecorderAcquireFrameBuffer(RecorderHandle* handle);

   // Takes ownership. `repeatCount` writes the same frame that many times, for
   // the constant-frame-rate padding case; 1 is the normal path.
   bool RecorderAppend(RecorderHandle* handle, std::vector<unsigned char>&& pixels,
                       int repeatCount = 1);

   // Frames handed to the encoder that it has not written yet.
   int RecorderPendingFrameCount(RecorderHandle* handle);
   // Frames the encoder could not accept and discarded, for the whole take.
   int RecorderDroppedFrameCount(RecorderHandle* handle);

   // Test-only: overrides the queue's byte-budget admission ceiling (normally
   // a fixed 256MB) for this handle. Real hardware encoders drain fast enough
   // that reproducing a genuine over-budget rejection in a self-test needs an
   // artificially small ceiling rather than an artificially slow consumer -
   // see RunRecExportTest's `starved` variant in main.cpp.
   void RecorderSetTestQueueByteBudget(RecorderHandle* handle, size_t bytes);

   // Appends interleaved float audio frames to the movie's audio track.
   bool RecorderAppendAudio(RecorderHandle* handle, const float* interleavedSamples, int numFrames);

   // Drains the encoder queue and joins its worker before returning, so the
   // movie is complete rather than truncated. outFrameCount/outDroppedCount,
   // if given, are filled with the final totals after that drain - reading
   // RecorderFrameCount() beforehand can race the worker's last few writes.
   bool RecorderStop(RecorderHandle* handle, std::string& outError,
                     int* outFrameCount = nullptr, int* outDroppedCount = nullptr);

   // Aborts a take instead of finishing it: discards every frame still queued
   // for the encoder, skips the flush/finalize entirely, and deletes the
   // partial output file. Returns as fast as the encoder can be unblocked -
   // at worst one in-flight frame - where RecorderStop's cost scales with
   // however deep the queue happens to be. That difference is the whole
   // point: an offline render can queue frames far faster than the encoder
   // drains them, so routing Cancel through RecorderStop made cancelling a
   // long take take about as long as finishing it. The handle is destroyed
   // either way; nothing usable is left behind on disk.
   void RecorderCancel(RecorderHandle* handle);

   // Whether an append of `bytes` would be admitted right now, or dropped by
   // the encoder queue's byte-budget ceiling. The live path has no use for
   // this - it cannot stall a real-time take, so it accepts the drop - but an
   // offline render must never drop a frame it has already advanced the
   // transport and the audio clock for, so it asks first and waits instead.
   bool RecorderQueueHasRoom(RecorderHandle* handle, size_t bytes);

   // Pushes whatever live audio is parked in the recorder's own backlog into
   // the writer, if the writer will take it now. RecorderAppendAudio already
   // tries this on every append, so a take that keeps appending never needs
   // to call it - but a caller that has STOPPED appending (an offline render
   // waiting for the video queue to drain) does: on macOS the writer holds
   // the video input not-ready until the audio track catches up, and the only
   // thing that ever flushed parked audio was the next append. Waiting for
   // the encoder while the encoder waits for audio nobody will hand it is a
   // deadlock, and this is what breaks it. No-op where the platform's writer
   // takes audio synchronously (Windows).
   void RecorderFlushPendingAudio(RecorderHandle* handle);

   // One-line snapshot of the recorder's internal state - writer status, both
   // inputs' readiness, queue depth, audio backlog. Diagnostic only: the
   // difference between "the encoder is slow" and "the encoder is wedged" is
   // invisible from outside, and a stalled offline render is exactly when it
   // matters.
   // Asks the encoder to drain whatever is queued, right now, without waiting
   // for the writer to hand out a readiness callback.
   //
   // The macOS encoder runs off AVAssetWriterInput's pull API
   // (requestMediaDataWhenReadyOnQueue:), whose contract is that the block is
   // re-invoked when an input that went not-ready becomes ready again. In an
   // offline render that re-invocation has been observed never to arrive: the
   // video input reports not-ready with the writer still in
   // AVAssetWriterStatusWriting, its audio track fully written and ahead of
   // the picture, one frame queued, and every encoder thread idle. Nothing
   // else in the process is waiting on anything - the take simply stops.
   //
   // A live take rides through it (its next append re-arms the queue, and it
   // drops frames rather than waiting); an offline take cannot, because it
   // stops appending precisely while it waits for room. So the waiting side
   // kicks the encoder itself rather than trusting the callback to come.
   // Cheap and idempotent: a kick with nothing to drain returns immediately.
   void RecorderKickEncoder(RecorderHandle* handle);

   // Marks the take's audio track complete without finishing the video track
   // or the file. Only meaningful for an offline render, whose audio is
   // generated on a fixed budget and therefore finishes before the picture
   // does: the writer will not take more video while it is still expecting
   // audio for that stretch of the timeline, so the last frames of a take
   // whose audio has run out cannot be written until the audio input is
   // told there is no more coming. Idempotent, and appending audio after it
   // is a no-op rather than an error.
   void RecorderFinishAudioInput(RecorderHandle* handle);

   std::string RecorderDebugState(RecorderHandle* handle);
   int RecorderFrameCount(RecorderHandle* handle);

   // Inspects a finished recording. Used by the audio-mux self-test, and handy
   // for the UI later if recording ever needs to report back what it actually
   // wrote rather than what was asked for.
   struct MovieInfo
   {
      bool hasVideo = false;
      bool hasAudio = false;
      double duration = 0.0;
      int frameCount = 0;
      // Per-track durations, not just the asset's overall one (which is the
      // longer of the two). An export that wrote all its video but lost part
      // of its audio - or the reverse - has a perfectly plausible overall
      // duration and two tracks that disagree; this is the only way to see
      // that from outside the process.
      double videoDuration = 0.0;
      double audioDuration = 0.0;
   };
   MovieInfo InspectMovie(const std::string& path);

   // ---- Syphon inter-app video sharing ------------------------------------
   struct SyphonServerHandle;
   struct SyphonClientHandle;

   struct SyphonServerInfo
   {
      std::string appName;
      std::string serverName;
      std::string uuid;
   };

   // Syphon Server (Broadcast)
   SyphonServerHandle* SyphonServerCreate(const std::string& serverName);
   void SyphonServerUpdateName(SyphonServerHandle* handle, const std::string& serverName);
   void SyphonServerPublish(SyphonServerHandle* handle, unsigned int textureId, int width, int height, bool flipped = false);
   // True only when some receiver is known to be reading this server. Never
   // a guess: where the backend cannot see receivers it returns false, and
   // SyphonServerCanReportClients() says so.
   bool SyphonServerHasClients(SyphonServerHandle* handle);
   // Whether SyphonServerHasClients can see receivers at all. macOS (Syphon)
   // yes. Windows (Spout: receivers just read shared memory, no handshake)
   // and Linux (no sharing backend) no, so a UI must say "not reported"
   // rather than "waiting".
   bool SyphonServerCanReportClients();
   void SyphonServerDestroy(SyphonServerHandle* handle);

   // Syphon Directory & Client (Receiver)
   std::vector<SyphonServerInfo> SyphonGetAvailableServers();
   SyphonClientHandle* SyphonClientCreate();
   bool SyphonClientConnect(SyphonClientHandle* handle, const std::string& appName, const std::string& serverName, const std::string& uuid = "");
   bool SyphonClientIsConnected(SyphonClientHandle* handle);
   bool SyphonClientHasNewFrame(SyphonClientHandle* handle);
   // Returns the GL_TEXTURE_RECTANGLE texture name and dimensions from the latest Syphon frame, or 0 if none.
   unsigned int SyphonClientGetFrameTexture(SyphonClientHandle* handle, int& outWidth, int& outHeight);
   void SyphonClientDestroy(SyphonClientHandle* handle);

   // ---- live camera input (for Video In node) -----------------------------
   struct CameraDeviceInfo
   {
      std::string uniqueId;
      std::string localizedName;
      bool isDefault = false;
   };

   enum class CameraResolution
   {
      Auto = 0,
      Res1080p,
      Res720p,
      Res480p,
      Count
   };

   struct CameraHandle;

   // Camera permission as the OS reports it right now. Read-only: never
   // raises a permission prompt (CameraOpen is what asks, on macOS). Windows
   // and Linux have no per-app prompt on this path, so they always answer
   // Authorized and a missing/blocked device surfaces from CameraOpen instead.
   enum class CameraAuthorization
   {
      Authorized,
      Denied,
      Restricted,
      NotDetermined,
   };
   CameraAuthorization CameraAuthorizationStatus();

   std::vector<CameraDeviceInfo> CameraListDevices();
   CameraHandle* CameraOpen(const std::string& deviceId, CameraResolution res, bool mirrorX, std::string& outError);
   void CameraClose(CameraHandle* handle);
   bool CameraIsRunning(CameraHandle* handle);
   void CameraSetMirror(CameraHandle* handle, bool mirrorX);
   void CameraSetResolution(CameraHandle* handle, CameraResolution res);

   // Drains the latest frame into outPixels (RGBA8, GL bottom-up).
   // Returns true only when a new frame was received and written.
   bool CameraReadFrame(CameraHandle* handle, std::vector<unsigned char>& outPixels,
                        int& outWidth, int& outHeight, unsigned long long& outFrameSeq);

}
