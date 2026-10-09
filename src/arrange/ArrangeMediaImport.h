#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "platform/PlatformMedia.h"
#include "platform/PlatformAudio.h"

// Background decode for media files dropped onto the Arrange timeline
// (audio samples, video files, images) - see ArrangeMediaImport.cpp for the
// worker-thread implementation. Mirrors SampleScanner's plain-std::thread /
// mutex-guarded-result / main-thread try_lock-poll pattern (SampleScanner.h):
// decode has no real-time constraints and never touches ImGui, GL, or the
// audio graph, so it is safe on a worker thread. Unlike SampleScanner (one
// reused thread, one scan at a time), each import job gets its own detached
// thread, since dropping several files at once should decode them
// concurrently rather than queue behind each other.
namespace Arrange
{

enum class ImportMediaKind { Audio, Video, Image };

// One job's finished result, returned by MediaImportManager::PollResults().
// Only the fields relevant to `kind` are populated.
struct MediaImportResult
{
   uint64_t jobId = 0;
   ImportMediaKind kind = ImportMediaKind::Audio;
   bool success = false;
   std::string error;
   std::string path;
   double durationSeconds = 0.0; // audio + video only; 0 for image

   // Audio: full decode, ownership transferred to whoever pops this result -
   // same convention as AudioFileNode::Open's `new Platform::SampleBuffer()`.
   // Push it onto the node with AudioFilePlayerAudioNode::PushBuffer (which
   // takes ownership) or delete it.
   // Video: doubles as that video's own audio track, if it has one (many
   // don't - a normal outcome, not an error, left null in that case).
   Platform::SampleBuffer* audioBuffer = nullptr;

   // Video: already open and reading (Platform::VideoOpen ran on the worker
   // thread, including its synchronous AVAssetReader setup) - handing this
   // to VideoSourceNode needs no further blocking work on the main thread.
   // Caller takes ownership (Platform::VideoClose it, or hand it to the node).
   Platform::VideoHandle* videoHandle = nullptr;
   int width = 0;
   int height = 0;
   // First-frame thumbnail, decoded on the worker thread via VideoFrameAt(0)
   // right after opening, so the node has something to show immediately
   // without a synchronous decode of its own on the main thread.
   std::vector<unsigned char> videoFirstFrame;

   // Image: decoded RGBA8 pixels (Platform::LoadImageRGBA), ready for
   // ImageSourceNode::LoadFromDecoded.
   std::vector<unsigned char> imagePixels;
};

class MediaImportManager
{
public:
   // Main thread only. Starts decoding `path` in the background and returns
   // a job id - the caller should remember it (e.g. on the pending
   // Arrange::Clip) to match it against a later PollResults() entry.
   uint64_t StartImport(const std::string& path, ImportMediaKind kind);

   // Main thread only, call once per frame. Non-blocking (try_lock) - a
   // still-running job simply doesn't appear yet, and is picked up on a
   // later call once it finishes.
   std::vector<MediaImportResult> PollResults();

private:
   void ThreadMain(uint64_t jobId, std::string path, ImportMediaKind kind);

   std::mutex mMutex;
   std::vector<MediaImportResult> mFinished; // guarded by mMutex
   std::atomic<uint64_t> mNextJobId { 1 };
};

// Process-wide instance - every drop-import call site shares one manager,
// same as e.g. the docked panel's SampleScanner globals.
MediaImportManager& GetMediaImportManager();

} // namespace Arrange
