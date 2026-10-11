#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "audio/MidiFile.h"
#include "audio/dsp/BasicPitch.h"
#include "core/AudioCable.h"
#include "core/INode.h"

class AudioSongPlayer;
class AudioToMidiAudioNode;

// Audio to MIDI: load an audio file (one instrument or a voice works best),
// and it is transcribed to notes - melody and chords - by the Basic Pitch
// model (audio/dsp/BasicPitch.h) on a worker thread. The notes play out of
// the node on the transport like a MIDI File node, in real time against the
// original audio (they are placed at the project bpm, and re-placed if the
// bpm changes), and Export writes them as a .mid.
//
// The notes are locked to the transport clock: the song position is the
// transport's time minus `offsetMs`, so after a seek it plays the notes that
// belong there, in step with an Audio File following the transport. Audio
// can also come in live: patch any audio into the "audio" input, press
// Record, and on Stop the take is saved as a WAV (in the recordings folder)
// and transcribed like a loaded file.
//
// Two stages, two caches: the model output for the file (seconds of work,
// redone only when the file changes, including on patch load) and the notes
// decoded from it with the three thresholds (milliseconds, redone on a worker
// whenever a knob moves). Nothing but the file path and the knobs is saved.
//
// Windows/Linux only (ONNX Runtime); elsewhere the node says so and stays silent.
class AudioToMidiNode : public INode, public INoteSource
{
public:
   static INode* Create() { return new AudioToMidiNode(); }
   AudioToMidiNode();
   ~AudioToMidiNode() override;

   unsigned int GetOutputTexture() override { return 0; }
   int GetOutputWidth() const override { return 0; }
   int GetOutputHeight() const override { return 0; }
   void CookIfNeeded(int frameId) override;
   void VisitParams(ParamVisitor& v) override;
   const char* OutputLabel(int /*index*/) const override { return "notes"; }
   AudioCable* AudioInputSlot(int slot) override { return slot == 0 ? &input : nullptr; }
   const char* InputLabel(int slot) const override { return slot == 0 ? "audio" : nullptr; }
   bool RequiresAudioProcessing() const override { return mRecording; }

   AudioNode* GetAudioNode() override;

   AudioCable input; // live audio to record (see StartRecording)

   // Saved params. Names are patch keys.
   std::string path;          // the audio file
   float sensitivity = 0.5f;  // 0..1: higher starts notes on weaker onsets
   float hold = 0.5f;         // 0..1: higher keeps notes held through quieter passages
   float minLengthMs = 128.0f; // notes shorter than this are dropped
   bool loop = true;
   int lowNote = 21;           // MIDI range kept (21 = A0 .. 108 = C8, the model's whole range)
   int highNote = 108;
   int polyphony = 0;          // > 0: at most this many notes at once (the quietest give way)
   float offsetMs = 0.0f;      // shifts the notes against the transport (+ = later), e.g. to
                               // line up with a synth's latency or an Audio File's offset

   // Not saved: the body's line about the last export ("saved to ..." or the error).
   std::string exportMessage;

   enum class State { Empty, Converting, Ready, Failed };
   State GetState() const { return mState; }
   const std::string& Status() const { return mStatus; }
   float Progress() const { return mProgress.load(std::memory_order_relaxed); }
   double DurationSeconds() const { return mDurationSeconds; }
   const std::vector<BasicPitch::Note>& Notes() const { return mNotes; }
   // Main-thread readout of the playhead, in seconds into the file, -1 when stopped.
   double PlayheadSeconds() const;
   // Re-runs the model on the current file (e.g. after it changed on disk).
   void Reconvert() { mLoadedPath.clear(); }

   bool ExportMidi(const std::string& filePath, std::string& error) const;

   // Live input recording. Start needs an audio cable on the input; Stop
   // writes the take to a WAV and loads it (path), which transcribes it.
   bool IsRecording() const { return mRecording; }
   double RecordedSeconds() const;
   bool StartRecording(std::string& error);
   void StopRecording();

   // Knob values -> model thresholds (defaults land on Basic Pitch's own 0.5 / 0.3 / 127.7 ms).
   BasicPitch::Settings CurrentSettings() const;

   // INFINITE_AUDIOTOMIDITEST: note decoding on synthetic activations, the
   // song/beat mapping and loop length, MIDI export round trip through
   // MidiFile::Load, params save/load. Headless, no model needed.
   static bool RunSelfTest();

private:
   struct Retired
   {
      std::unique_ptr<MidiFile::Song> song;
      double retiredAt;
   };

   void StartConversion();
   void PollConversion();
   void PollNotes();
   void RebuildSong(double bpm);
   void CollectRetired(double now);

   void DrainRecording();

   std::unique_ptr<AudioToMidiAudioNode> mAudioNode;
   int mLastCookFrame = -1;

   bool mRecording = false;
   std::vector<float> mRecorded; // interleaved stereo
   double mRecordRate = 48000.0;

   State mState = State::Empty;
   std::string mStatus;
   std::string mLoadedPath;
   double mDurationSeconds = 0.0;

   // Stage 1: model output for mLoadedPath.
   struct ConvertResult
   {
      std::shared_ptr<const BasicPitch::Activations> activations;
      std::string error;
   };
   std::shared_ptr<std::atomic<bool>> mCancel;
   std::atomic<float> mProgress{ 0.0f };
   std::future<ConvertResult> mConvert;
   std::shared_ptr<const BasicPitch::Activations> mActivations;

   // Stage 2: notes for mNotesSettings.
   std::future<std::vector<BasicPitch::Note>> mDecode;
   BasicPitch::Settings mDecodingSettings;
   BasicPitch::Settings mNotesSettings;
   bool mHaveNotes = false;
   std::vector<BasicPitch::Note> mNotes;

   // What the audio thread plays: the notes on a beat grid at mSongBpm.
   std::unique_ptr<MidiFile::Song> mSong;
   double mSongBpm = 0.0;
   bool mSongDirty = false;
   std::vector<Retired> mRetired;
};
