#include "AudioToMidiNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "audio/AudioCaptureRing.h"
#include "audio/AudioFileWriter.h"
#include "core/AudioDecodeCache.h"
#include "core/AudioTopologyRequest.h"
#include "core/Transport.h"
#include "nodes/NoteSongPlayer.h"
#include "platform/PlatformAudio.h"

// The song player plus a tap on the "audio" input for live recording: while
// the ring is enabled, every block of the input is written to it as
// interleaved stereo for the main thread to collect (DrainRecording).
class AudioToMidiAudioNode : public AudioSongPlayer
{
public:
   void PrepareToPlay(double sampleRate, int maxBlockSize) override
   {
      AudioSongPlayer::PrepareToPlay(sampleRate, maxBlockSize);
      mRate.store(sampleRate, std::memory_order_relaxed);
   }

   void ProcessBlock(const AudioBuffer* const* inputs, int numInputs, AudioBuffer& output) override
   {
      if (ring.enabled.load(std::memory_order_relaxed) && numInputs > 0 && inputs != nullptr && inputs[0] != nullptr)
      {
         const AudioBuffer& in = *inputs[0];
         const int frames = std::min(in.numFrames, kMaxFrames);
         for (int i = 0; i < frames; i++)
         {
            const float l = in.numChannels > 0 ? in.channels[0][i] : 0.0f;
            mScratch[(size_t)i * 2] = l;
            mScratch[(size_t)i * 2 + 1] = in.numChannels > 1 ? in.channels[1][i] : l;
         }
         ring.Write(mScratch, frames * 2);
      }
      AudioSongPlayer::ProcessBlock(inputs, numInputs, output);
   }

   double Rate() const { return mRate.load(std::memory_order_relaxed); }

   AudioCaptureRing ring;

private:
   static constexpr int kMaxFrames = 8192;
   float mScratch[kMaxFrames * 2] = {};
   std::atomic<double> mRate { 48000.0 };
};

namespace
{
   double NowSeconds()
   {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
   }

   // How long a replaced song is kept alive after the audio thread was handed
   // its successor. One block is all it can still be reading; this is ample.
   constexpr double kRetireSeconds = 2.0;

   bool IsReady(const std::future<std::vector<BasicPitch::Note>>& f)
   {
      return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
   }

   MidiFile::Song BuildSong(const std::vector<BasicPitch::Note>& notes, double bpm)
   {
      MidiFile::Song song;
      const double beatsPerSecond = bpm / 60.0;
      song.events.reserve(notes.size() * 2);
      for (const BasicPitch::Note& n : notes)
      {
         MidiFile::Event on;
         on.beat = (double)n.start * beatsPerSecond;
         on.note = (uint8_t)std::clamp(n.pitch, 0, 127);
         on.velocity = (uint8_t)std::clamp((int)std::lround(127.0f * n.amplitude), 1, 127);
         on.on = true;
         MidiFile::Event off = on;
         off.beat = std::max(on.beat, (double)n.end * beatsPerSecond);
         off.velocity = 0;
         off.on = false;
         song.events.push_back(on);
         song.events.push_back(off);
         song.lowNote = std::min(song.lowNote, on.note);
         song.highNote = std::max(song.highNote, on.note);
         song.lengthBeats = std::max(song.lengthBeats, off.beat);
      }
      // By beat, note-offs first at the same beat (MidiFile::Song's contract).
      std::stable_sort(song.events.begin(), song.events.end(), [](const MidiFile::Event& a, const MidiFile::Event& b)
                       { return a.beat != b.beat ? a.beat < b.beat : (!a.on && b.on); });
      song.noteCount = (int)notes.size();
      song.trackCount = notes.empty() ? 0 : 1;
      return song;
   }
}

AudioToMidiNode::AudioToMidiNode() = default;

AudioToMidiNode::~AudioToMidiNode()
{
   if (mAudioNode)
      mAudioNode->ring.enabled.store(false);
   // The workers reference this node's progress counter: stop and join them first.
   if (mCancel)
      mCancel->store(true);
   if (mConvert.valid())
      mConvert.wait();
   if (mDecode.valid())
      mDecode.wait();
}

BasicPitch::Settings AudioToMidiNode::CurrentSettings() const
{
   BasicPitch::Settings s;
   // sensitivity 0.5 -> onset 0.5 and hold 0.5 -> frame 0.3: Basic Pitch's defaults.
   s.onsetThreshold = std::clamp(0.9f - 0.8f * sensitivity, 0.05f, 0.95f);
   s.frameThreshold = std::clamp(0.55f - 0.5f * hold, 0.05f, 0.6f);
   s.minNoteMs = std::clamp(minLengthMs, 10.0f, 1000.0f);
   s.lowMidi = std::clamp(lowNote, BasicPitch::kLowestMidi, BasicPitch::kLowestMidi + BasicPitch::kKeys - 1);
   s.highMidi = std::clamp(highNote, s.lowMidi, BasicPitch::kLowestMidi + BasicPitch::kKeys - 1);
   s.maxPolyphony = std::clamp(polyphony, 0, 16);
   return s;
}

void AudioToMidiNode::StartConversion()
{
   if (mCancel)
      mCancel->store(true);
   if (mConvert.valid())
      mConvert.wait(); // a cancelled run stops within one model window
   mConvert = std::future<ConvertResult>();
   // A decode of the previous file's activations must not land on this one.
   if (mDecode.valid())
      mDecode.wait();
   mDecode = std::future<std::vector<BasicPitch::Note>>();
   mActivations.reset();
   mHaveNotes = false;
   mNotes.clear();
   mSongDirty = true;
   mProgress.store(0.0f);
   mDurationSeconds = 0.0;
   mLoadedPath = path;

   if (path.empty())
   {
      mState = State::Empty;
      mStatus.clear();
      return;
   }

   // Decoding stays on the main thread (Platform's decoders are main-thread
   // only); resampling and the model run on the worker.
   auto buffer = std::make_shared<Platform::SampleBuffer>();
   std::string err;
   if (!AudioDecodeCache::DecodeCached(path, *buffer, err) || buffer->numFrames <= 0 || buffer->channels <= 0)
   {
      mState = State::Failed;
      mStatus = err.empty() ? "could not read the audio file" : err;
      return;
   }
   mDurationSeconds = (double)buffer->numFrames / std::max(1.0, buffer->sampleRate);

   mState = State::Converting;
   mStatus = "converting...";
   mCancel = std::make_shared<std::atomic<bool>>(false);
   std::shared_ptr<std::atomic<bool>> cancel = mCancel;
   std::atomic<float>* progress = &mProgress;
   mConvert = std::async(std::launch::async, [buffer, cancel, progress]() -> ConvertResult
   {
      ConvertResult r;
      std::vector<float> input;
      if (!BasicPitch::ToModelInput(buffer->channelData.data(), buffer->channels, buffer->numFrames, buffer->sampleRate,
                                    input, cancel.get()))
      {
         r.error = cancel->load() ? "cancelled" : "could not prepare the audio";
         return r;
      }
      auto act = std::make_shared<BasicPitch::Activations>();
      if (!BasicPitch::RunModel(input, *act, r.error, progress, cancel.get()))
         return r;
      r.activations = std::move(act);
      return r;
   });
}

void AudioToMidiNode::PollConversion()
{
   if (!mConvert.valid() || mConvert.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
   ConvertResult r = mConvert.get();
   if (!r.activations)
   {
      mState = State::Failed;
      mStatus = r.error;
      return;
   }
   mActivations = std::move(r.activations);
   mState = State::Ready;
   mStatus.clear();
}

void AudioToMidiNode::PollNotes()
{
   if (IsReady(mDecode))
   {
      mNotes = mDecode.get();
      mNotesSettings = mDecodingSettings;
      mHaveNotes = true;
      mSongDirty = true;
   }
   // One decode at a time; a knob that kept moving is picked up by the next one.
   const BasicPitch::Settings want = CurrentSettings();
   if (mActivations && !mDecode.valid() && (!mHaveNotes || mNotesSettings != want))
   {
      mDecodingSettings = want;
      std::shared_ptr<const BasicPitch::Activations> act = mActivations;
      mDecode = std::async(std::launch::async, [act, want]() { return BasicPitch::NotesFromActivations(*act, want); });
   }
}

void AudioToMidiNode::RebuildSong(double bpm)
{
   if (mSong)
      mRetired.push_back({ std::move(mSong), NowSeconds() });
   mSong.reset();
   if (mHaveNotes)
      mSong = std::make_unique<MidiFile::Song>(BuildSong(mNotes, bpm));
   mSongBpm = bpm;
   mSongDirty = false;
}

void AudioToMidiNode::CollectRetired(double now)
{
   mRetired.erase(std::remove_if(mRetired.begin(), mRetired.end(),
                                 [now](const Retired& r) { return now - r.retiredAt > kRetireSeconds; }),
                  mRetired.end());
}

void AudioToMidiNode::CookIfNeeded(int frameId)
{
   if (frameId == mLastCookFrame)
      return;
   mLastCookFrame = frameId;
   if (!mAudioNode)
      mAudioNode = std::make_unique<AudioToMidiAudioNode>();
   if (mRecording)
   {
      DrainRecording();
      // A recording ends with its cable, or at ten minutes.
      if (!input.IsConnected() || RecordedSeconds() > 600.0)
         StopRecording();
   }

   // A path set by a patch load, the file picker or a drop is read here, once per change.
   if (path != mLoadedPath)
      StartConversion();
   PollConversion();
   PollNotes();

   const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
   if (mSongDirty || std::fabs(bpm - mSongBpm) > 1e-6)
      RebuildSong(bpm);
   if (!mRetired.empty())
      CollectRetired(NowSeconds());

   // The loop is the whole file, so the notes stay in step with it.
   const double loopBeats = std::max(1.0, mDurationSeconds * bpm / 60.0);
   mAudioNode->PushSong(mSong.get(), loopBeats, 0, 0.0f, loop, 1.0f, 1.0f, /*lockToTransport=*/true,
                        std::clamp(offsetMs, -2000.0f, 2000.0f) / 1000.0f);
}

double AudioToMidiNode::RecordedSeconds() const
{
   return (double)(mRecorded.size() / 2) / std::max(1.0, mRecordRate);
}

bool AudioToMidiNode::StartRecording(std::string& error)
{
   if (!input.IsConnected())
   {
      error = "patch some audio into the \"audio\" input first";
      return false;
   }
   if (!mAudioNode)
      mAudioNode = std::make_unique<AudioToMidiAudioNode>();
   float sink[4096];
   while (mAudioNode->ring.Read(sink, 4096) > 0)
   {
   } // anything left from an earlier take
   mRecorded.clear();
   mRecordRate = mAudioNode->Rate();
   mRecording = true;
   mAudioNode->ring.enabled.store(true);
   AudioTopologyRequest::Request(); // RequiresAudioProcessing() just turned true
   return true;
}

void AudioToMidiNode::DrainRecording()
{
   if (!mAudioNode)
      return;
   float chunk[8192];
   int n;
   while ((n = mAudioNode->ring.Read(chunk, 8192)) > 0)
      mRecorded.insert(mRecorded.end(), chunk, chunk + n);
}

void AudioToMidiNode::StopRecording()
{
   if (!mRecording)
      return;
   DrainRecording();
   mAudioNode->ring.enabled.store(false);
   mRecording = false;
   AudioTopologyRequest::Request();
   const int frames = (int)(mRecorded.size() / 2);
   if (frames < (int)(mRecordRate * 0.25))
   {
      mState = State::Failed;
      mStatus = "recording too short";
      mRecorded.clear();
      return;
   }
   const std::string file = AudioRecordings::GenerateFilePath("audio-to-midi");
   if (!AudioRecordings::WriteWav(file, mRecorded.data(), frames, mRecordRate, 2))
   {
      mState = State::Failed;
      mStatus = "could not save the recording";
      mRecorded.clear();
      return;
   }
   mRecorded.clear();
   mRecorded.shrink_to_fit();
   path = file; // the next cook transcribes it, and the patch remembers it
}

double AudioToMidiNode::PlayheadSeconds() const
{
   if (!mAudioNode || mSongBpm <= 0.0)
      return -1.0;
   const double beats = mAudioNode->Playhead();
   return beats < 0.0 ? -1.0 : beats * 60.0 / mSongBpm;
}

bool AudioToMidiNode::ExportMidi(const std::string& filePath, std::string& error) const
{
   if (!mHaveNotes || mNotes.empty())
   {
      error = "no notes to export";
      return false;
   }
   const double bpm = std::max(1.0, (double)Transport::Instance().Tempo());
   return MidiFile::Save(filePath, BuildSong(mNotes, bpm), bpm, error);
}

void AudioToMidiNode::VisitParams(ParamVisitor& v)
{
   v.Text("path", path);
   v.Float("sensitivity", sensitivity);
   v.Float("hold", hold);
   v.Float("minLength", minLengthMs);
   v.Bool("loop", loop);
   v.Int("lowNote", lowNote);
   v.Int("highNote", highNote);
   v.Int("polyphony", polyphony);
   v.Float("offsetMs", offsetMs);
}

AudioNode* AudioToMidiNode::GetAudioNode()
{
   if (!mAudioNode)
      mAudioNode = std::make_unique<AudioToMidiAudioNode>();
   return mAudioNode.get();
}

// ---------------------------------------------------------------- self-test

bool AudioToMidiNode::RunSelfTest()
{
   bool ok = true;
   auto check = [&ok](bool cond, const char* what)
   {
      if (!cond)
      {
         printf("AUDIOTOMIDITEST FAIL: %s\n", what);
         ok = false;
      }
   };

   // 1. Note decoding on hand-made activations: a note with a clear onset
   // (A4, frames 50..149), a sustained note (C4, 200..279) and a blip too
   // short to keep (frames 10..14).
   {
      BasicPitch::Activations a;
      a.frames = 300;
      a.note.assign((size_t)a.frames * BasicPitch::kKeys, 0.0f);
      a.onset.assign((size_t)a.frames * BasicPitch::kKeys, 0.0f);
      auto cell = [](int t, int midi) { return (size_t)t * BasicPitch::kKeys + (size_t)(midi - BasicPitch::kLowestMidi); };
      for (int t = 50; t < 150; t++)
         a.note[cell(t, 69)] = 0.8f;
      a.onset[cell(49, 69)] = 0.3f;
      a.onset[cell(50, 69)] = 0.9f;
      a.onset[cell(51, 69)] = 0.3f;
      for (int t = 200; t < 280; t++)
         a.note[cell(t, 60)] = 0.7f;
      for (int t = 10; t < 15; t++)
         a.note[cell(t, 51)] = 0.9f;

      AudioToMidiNode defaults;
      const std::vector<BasicPitch::Note> notes = BasicPitch::NotesFromActivations(a, defaults.CurrentSettings());
      check(notes.size() == 2, "expected exactly two notes (the short blip dropped)");
      if (notes.size() == 2)
      {
         const double fps = 22050.0 / 256.0;
         check(notes[0].pitch == 69 && std::fabs(notes[0].start - 50.0 / fps) < 0.02 && std::fabs(notes[0].end - 150.0 / fps) < 0.03,
               "A4 note pitch/start/end");
         check(std::fabs(notes[0].amplitude - 0.8f) < 1e-3f, "A4 amplitude is the mean activation");
         check(notes[1].pitch == 60 && std::fabs(notes[1].start - 200.0 / fps) < 0.03 && notes[1].end > notes[1].start + 0.8f,
               "C4 sustained note");
      }

      // A minimum length above both notes (100 and 80 frames) drops them all.
      BasicPitch::Settings strict = defaults.CurrentSettings();
      strict.minNoteMs = 1500.0f;
      check(BasicPitch::NotesFromActivations(a, strict).empty(), "min length 1.5 s drops every note");

      // Pitch range and polyphony: a quieter E4 (frames 60..139) under the A4.
      {
         BasicPitch::Activations b = a;
         for (int t = 60; t < 140; t++)
            b.note[cell(t, 64)] = 0.5f;
         b.onset[cell(59, 64)] = 0.3f;
         b.onset[cell(60, 64)] = 0.9f;
         b.onset[cell(61, 64)] = 0.3f;
         AudioToMidiNode knobs;
         check(BasicPitch::NotesFromActivations(b, knobs.CurrentSettings()).size() == 3, "overlapping E4 found");
         knobs.polyphony = 1;
         const std::vector<BasicPitch::Note> mono = BasicPitch::NotesFromActivations(b, knobs.CurrentSettings());
         check(mono.size() == 2 && mono[0].pitch == 69, "polyphony 1 drops the quieter overlapping note");
         knobs.polyphony = 0;
         knobs.lowNote = 62;
         const std::vector<BasicPitch::Note> high = BasicPitch::NotesFromActivations(b, knobs.CurrentSettings());
         check(high.size() == 2 && high[0].pitch >= 62 && high[1].pitch >= 62, "low note 62 drops C4");
      }

      // 2. Beat mapping + export round trip.
      const MidiFile::Song song = BuildSong(notes, 120.0);
      check(song.noteCount == (int)notes.size() && song.events.size() == notes.size() * 2, "song has an on/off per note");
      if (!notes.empty())
         check(std::fabs(song.events.front().beat - notes[0].start * 2.0) < 1e-6, "seconds map to beats at the bpm");

      const std::string tmp = (std::filesystem::temp_directory_path() / "infinite_audiotomidi_test.mid").u8string();
      std::string err;
      check(MidiFile::Save(tmp, song, 120.0, err), "MidiFile::Save");
      MidiFile::Song back;
      check(MidiFile::Load(tmp, back, err), "MidiFile::Load of the export");
      check(back.noteCount == song.noteCount, "export keeps every note");
      if (!back.events.empty() && !song.events.empty())
         check(std::fabs(back.events.front().beat - song.events.front().beat) < 1.0 / 480.0 + 1e-9, "export timing within a tick");
      std::error_code ec;
      std::filesystem::remove(std::filesystem::u8path(tmp), ec);
   }

   // 3. End to end through the real model, when this build and install have it:
   // two seconds of a harmonic A4 at 44.1 kHz must come out as MIDI 69.
   {
      const int sr = 44100, n = sr * 2;
      std::vector<float> tone((size_t)n);
      const double kPi = 3.14159265358979323846;
      for (int i = 0; i < n; i++)
      {
         const double t = (double)i / sr;
         const double env = std::min(1.0, t / 0.01) * std::min(1.0, (2.0 - t) / 0.05);
         tone[(size_t)i] = (float)(env * (0.5 * std::sin(2 * kPi * 440 * t) + 0.25 * std::sin(2 * kPi * 880 * t) +
                                          0.12 * std::sin(2 * kPi * 1320 * t)));
      }
      std::vector<float> input;
      check(BasicPitch::ToModelInput(tone.data(), 1, n, sr, input, nullptr) && std::abs((int)input.size() - 44100) <= 1,
            "resample 44.1k -> 22.05k length");
      BasicPitch::Activations act;
      std::string err;
      if (!BasicPitch::RunModel(input, act, err, nullptr, nullptr))
         printf("AUDIOTOMIDITEST model stage skipped: %s\n", err.c_str());
      else
      {
         AudioToMidiNode defaults;
         const std::vector<BasicPitch::Note> notes = BasicPitch::NotesFromActivations(act, defaults.CurrentSettings());
         float longest = 0.0f;
         int pitch = -1;
         for (const BasicPitch::Note& note : notes)
            if (note.end - note.start > longest)
            {
               longest = note.end - note.start;
               pitch = note.pitch;
            }
         printf("AUDIOTOMIDITEST model: %d frames, %d notes, longest %.2fs at MIDI %d\n", act.frames, (int)notes.size(),
                longest, pitch);
         check(act.frames > 150 && act.frames < 190, "about 86 frames per second of audio");
         check(pitch == 69 && longest > 1.0f, "a 440 Hz tone transcribes to a long A4");
      }
   }

   // 4. Transport lock: after a seek the player plays the notes that belong
   // at the new time (it used to restart the same pattern on every jump).
   {
      Transport& tr = Transport::Instance();
      const bool wasPlaying = tr.IsPlaying();
      const float wasBpm = tr.Tempo();
      tr.SetTempo(120.0f);
      tr.SetOfflineMode(true, 48000.0);
      tr.SetPlaying(true);
      // An A4 at 1 s and a C5 at 5 s, half a second each.
      const std::vector<BasicPitch::Note> ns = { { 1.0f, 1.5f, 69, 0.8f }, { 5.0f, 5.5f, 72, 0.8f } };
      const MidiFile::Song song = BuildSong(ns, 120.0);
      AudioSongPlayer player;
      player.PrepareToPlay(48000.0, 256);
      NoteEventQueue* out = player.NoteOutbox();
      const int cursor = out->RegisterConsumer();
      player.PushSong(&song, 20.0, 0, 0.0f, false, 1.0f, 1.0f, /*lockToTransport=*/true, 0.0f);
      float l[256] = {}, r[256] = {};
      float* chans[2] = { l, r };
      AudioBuffer buf;
      buf.channels = chans;
      buf.numChannels = 2;
      buf.numFrames = 256;
      auto runFor = [&](double seconds)
      {
         std::vector<int> ons;
         const int blocks = (int)(seconds * 48000.0 / 256.0);
         for (int b = 0; b < blocks; b++)
         {
            tr.BeginOfflineAudioBlock(256);
            player.ProcessBlock(nullptr, 0, buf);
            tr.EndOfflineAudioBlock();
            NoteEvent ev[16];
            int n;
            while ((n = out->Pop(cursor, ev, 16)) > 0)
               for (int i = 0; i < n; i++)
                  if (ev[i].isNoteOn)
                     ons.push_back(ev[i].note);
         }
         return ons;
      };
      tr.Seek(4.5);
      const std::vector<int> later = runFor(1.0);
      check(later.size() == 1 && later[0] == 72, "after a seek to 4.5 s the C5 at 5 s plays, not the A4");
      tr.Seek(0.5);
      const std::vector<int> earlier = runFor(1.0);
      check(earlier.size() == 1 && earlier[0] == 69, "seeking back to 0.5 s plays the A4 at 1 s");
      player.PushSong(&song, 20.0, 0, 0.0f, false, 1.0f, 1.0f, true, 0.6f); // offset: everything 0.6 s later
      tr.Seek(4.8);
      const std::vector<int> offset = runFor(0.7);
      check(offset.empty(), "a 0.6 s offset moves the C5 to 5.6 s");
      player.PushSong(nullptr, 20.0, 0, 0.0f, false, 1.0f, 1.0f, true, 0.0f);
      tr.SetOfflineMode(false);
      tr.NotifyAudioEngineStopped();
      tr.SetTempo(wasBpm);
      tr.SetPlaying(wasPlaying);
   }

   printf("AUDIOTOMIDITEST %s\n", ok ? "OK" : "FAIL");
   return ok;
}
