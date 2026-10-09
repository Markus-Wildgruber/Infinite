#include "ArrangeMediaImport.h"
#include "platform/PlatformApp.h"

#include <thread>
#include <utility>

namespace Arrange
{

uint64_t MediaImportManager::StartImport(const std::string& path, ImportMediaKind kind)
{
   const uint64_t jobId = mNextJobId.fetch_add(1, std::memory_order_relaxed);
   std::thread([this, jobId, path, kind]() { ThreadMain(jobId, path, kind); }).detach();
   return jobId;
}

void MediaImportManager::ThreadMain(uint64_t jobId, std::string path, ImportMediaKind kind)
{
   MediaImportResult result;
   result.jobId = jobId;
   result.kind = kind;
   result.path = path;

   if (kind == ImportMediaKind::Audio)
   {
      auto* buffer = new Platform::SampleBuffer();
      std::string error;
      // Full dr_wav/dr_mp3/dr_flac-or-AVAudioFile decode, run here off the
      // main thread. Platform::DecodeAudioFileToBuffer's own doc comment
      // says "main-thread only", but its macOS implementation is a plain
      // AVAudioFile/AVAudioPCMBuffer read inside an @autoreleasepool with no
      // run-loop or dispatch dependency - that comment describes its
      // existing call sites, not a hard constraint, so calling it from a
      // worker thread here is safe.
      if (Platform::DecodeAudioFileToBuffer(path, *buffer, error))
      {
         result.success = true;
         result.audioBuffer = buffer;
         result.durationSeconds = buffer->sampleRate > 0.0
            ? (double)buffer->numFrames / buffer->sampleRate : 0.0;
      }
      else
      {
         delete buffer;
         result.error = error;
      }
   }
   else if (kind == ImportMediaKind::Video)
   {
      std::string error;
      // VideoOpen's own implementation ([AVAsset assetWithURL:] plus a
      // synchronous AVAssetReader setup) touches no run loop either, so it
      // is likewise safe here - same reasoning as the audio decode above.
      Platform::VideoHandle* handle = Platform::VideoOpen(path, error);
      if (handle != nullptr)
      {
         result.success = true;
         result.width = Platform::VideoWidth(handle);
         result.height = Platform::VideoHeight(handle);
         result.durationSeconds = Platform::VideoDuration(handle);
         Platform::VideoFrameAt(handle, 0.0, result.videoFirstFrame);
         result.videoHandle = handle;

         // The video's own audio track, if it has one - a lot of VJ-style
         // footage doesn't, which DecodeVideoAudioTrackToBuffer treats as a
         // normal outcome rather than a failure. Decoded here too so
         // VideoSourceNode's synchronous LoadAudioTrack() never has to run
         // on the main thread for a dropped file.
         auto* audioBuf = new Platform::SampleBuffer();
         std::string audioError;
         if (Platform::DecodeVideoAudioTrackToBuffer(path, *audioBuf, audioError))
            result.audioBuffer = audioBuf;
         else
            delete audioBuf;
      }
      else
      {
         result.error = error;
      }
   }
   else // Image
   {
      std::string error;
      int w = 0, h = 0;
      std::vector<unsigned char> pixels;
      if (Platform::LoadImageRGBA(path, pixels, w, h, error))
      {
         result.success = true;
         result.imagePixels = std::move(pixels);
         result.width = w;
         result.height = h;
      }
      else
      {
         result.error = error;
      }
   }

   std::lock_guard<std::mutex> lock(mMutex);
   mFinished.push_back(std::move(result));
}

std::vector<MediaImportResult> MediaImportManager::PollResults()
{
   std::vector<MediaImportResult> out;
   std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
   if (!lock.owns_lock() || mFinished.empty())
      return out;
   out.swap(mFinished);
   return out;
}

MediaImportManager& GetMediaImportManager()
{
   static MediaImportManager sInstance;
   return sInstance;
}

} // namespace Arrange
