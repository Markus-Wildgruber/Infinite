#pragma once
// MIDI input, MPE helpers, live note stream, MIDI clock.
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
   // ---- MIDI input ----
   // Listens to every connected class-compliant MIDI source (Akai/Nektar/Pioneer
   // controllers all enumerate this way on macOS, no vendor driver needed) and
   // keeps a running table of the last value received for every (device,
   // channel, controller) triple seen, plus which one arrived most recently —
   // that's what MIDI Learn polls. Computed on Core MIDI's callback thread,
   // read the same lock-free-ish way as AudioLevels above.
   //
   // `device` identifies the physical source endpoint (its MIDIEndpointRef,
   // opaque otherwise) - two controllers both defaulting to channel 1 would
   // otherwise be indistinguishable, which is exactly the bug this fixes:
   // every binding is now scoped to the device it was learned on, not just
   // its channel/controller number. 0 = no device / wildcard-unbound.
   using MidiDeviceId = unsigned int;

   struct MidiCCValue
   {
      MidiDeviceId device = 0;
      int channel = -1;    // 0-15, -1 = none seen yet
      int controller = -1; // CC number 0-127, or note number for Note-On/Off
      bool isNote = false; // true if this came from a Note-On rather than a CC
      float value01 = 0.0f;
   };

   // Pitch bend and channel aftertouch have no CC number of their own, so they
   // live in the same (device, channel, controller) table under numbers just
   // above the CC range (0-127). MIDI learn, bindings and MidiRead need no
   // special case. Pitch bend is 14-bit scaled to 0..1 with centre 0.5;
   // aftertouch is 0..1. Poly aftertouch is not modelled.
   constexpr int kMidiControllerPitchBend = 128;
   constexpr int kMidiControllerAftertouch = 129;
   // MPE (R597). Per-note pressure and slide arrive as channel aftertouch and
   // CC74 on a member channel (zero-based 1..14), so the whole-channel tables
   // above already hold them per channel. These two virtual controllers, only
   // written while an MPE-enabled node is listening (MidiMpeEnabled), follow
   // the most recently moved member channel on (device, channel 0) so one
   // ordinary binding can read "the note being played" without knowing which
   // member channel it landed on.
   constexpr int kMidiControllerNotePressure = 130;
   constexpr int kMidiControllerNoteSlide = 131;
   constexpr int kMidiSlideCC = 74;
   inline std::string MidiBindingName(bool isNote, int controller)
   {
      if (!isNote && controller == kMidiControllerPitchBend) return "Pitch Bend";
      if (!isNote && controller == kMidiControllerAftertouch) return "Aftertouch";
      if (!isNote && controller == kMidiControllerNotePressure) return "Note Pressure";
      if (!isNote && controller == kMidiControllerNoteSlide) return "Note Slide";
      return std::string(isNote ? "Note " : "CC ") + std::to_string(controller);
   }

   // MPE state shared by all three MIDI backends: one switch, and the latest
   // pitch bend per channel (-1..+1, centre 0). Header-only so the platform
   // files need no new symbol. The MIDI thread writes, the audio thread reads;
   // relaxed atomics are enough because each value stands alone.
   inline std::atomic<int>& MidiMpeEnabledCount()
   {
      static std::atomic<int> sCount { 0 };
      return sCount;
   }
   inline bool MidiMpeEnabled() { return MidiMpeEnabledCount().load(std::memory_order_relaxed) > 0; }
   inline std::atomic<float>& MidiChannelBendSlot(int channel)
   {
      static std::atomic<float> sBend[16] = {};
      return sBend[channel & 15];
   }
   inline void MidiStoreChannelBend(int channel, float bendMinus1To1)
   {
      MidiChannelBendSlot(channel).store(bendMinus1To1, std::memory_order_relaxed);
   }
   inline float MidiChannelBend(int channel) { return MidiChannelBendSlot(channel).load(std::memory_order_relaxed); }
   // Lower-zone member channels, zero-based. 0 is the master, 15 the upper-zone master.
   inline bool MidiIsMpeMemberChannel(int channel) { return channel >= 1 && channel <= 14; }
   inline float MidiBend14ToSigned(int lsb, int msb) { return (float)(((msb << 7) | lsb) - 8192) / 8192.0f; }

   bool MidiStart(std::string& outError);
   void MidiStop();
   bool MidiIsRunning();
   std::string MidiDeviceSummary(); // comma-joined names of connected sources, for status text
   std::string MidiDeviceName(MidiDeviceId device); // "" if not currently connected

   // Injects raw MIDI bytes (Note On, Note Off, CC, Clock) into the MIDI subsystem
   // for benchmarks and tests, identically to hardware MIDI arrival.
   void MidiInjectBytes(const unsigned char* data, size_t len, MidiDeviceId device = 0);

   // Current value for a specific (device, channel, controller) binding.
   // Returns false (value left at 0) if that binding has never been seen.
   bool MidiRead(MidiDeviceId device, int channel, int controller, bool isNote, float& outValue01);

   // Re-points a saved binding whose device is not connected at a connected
   // device that has sent the same (channel, controller/note, isNote) this
   // session. Lets bindings saved with a device id that no longer exists
   // (pre-0.4.4 macOS saved a per-launch handle) reattach on first touch
   // instead of needing a re-learn. controller < 0 matches any note on the
   // channel (keyboard-mode bindings). Returns true if `device` changed.
   bool MidiRebindStaleDevice(MidiDeviceId& device, int channel, int controller, bool isNote);

   // The most recent CC/note touched on ANY connected source since the last
   // call — this is what a node in "Learn" mode polls each frame. Returns false
   // if nothing has moved since the last poll (consumed on read, like a queue
   // of depth 1, so two Learn-mode nodes don't both grab the same event and one
   // doesn't silently miss it while learning).
   bool MidiPollLastTouched(MidiCCValue& outLast);

   // Monotonically increasing count of Note-On events seen for a specific
   // (device, channel, note), for nodes that need to know *when* a new hit
   // happened rather than just the currently-held velocity (MidiRead only
   // reports the latter). Returns 0 if that note has never been hit.
   unsigned int MidiNoteHitCount(MidiDeviceId device, int channel, int note);

   // The most recent Note-On seen on a whole (device, channel), regardless of
   // which note - for "keyboard" nodes that report whichever key is currently
   // being played rather than one fixed pad note. hitSeq increments on every
   // Note-On so callers can tell a repeat of the same note from a held one.
   // Returns false (out left default) if that device/channel has never seen a note.
   struct MidiLastNote
   {
      int note = -1;
      float velocity01 = 0.0f;
      unsigned int hitSeq = 0;
   };

   bool MidiChannelLastNote(MidiDeviceId device, int channel, MidiLastNote& out);

   // ---- live note stream ---------------------------------------------------
   // Every Note-On and Note-Off, in order, for the MIDI Notes node to turn
   // into NoteEvents. This is a different shape of data from everything above
   // it: MidiRead/MidiChannelLastNote sample a *current value* under a mutex,
   // which is right for a modulator polled once a frame and wrong for a synth,
   // where a missed note-off is a stuck voice. So the note stream is its own
   // lock-free ring, written by the CoreMIDI read thread and readable from the
   // audio thread with no lock at all.
   //
   // Multiple consumers are supported by giving each one its own cursor rather
   // than a shared read index: the ring's write counter is monotonic, and a
   // consumer that has fallen more than the ring's capacity behind is skipped
   // forward to the oldest still-valid entry (dropping what it missed) instead
   // of reading torn data. Note-Off (0x80) and Note-On-with-velocity-0 both
   // arrive here as isNoteOn == false.
   struct MidiNoteMessage
   {
      MidiDeviceId device = 0;
      int channel = 0; // 0-15
      int note = 0;    // 0-127
      float velocity01 = 0.0f;
      bool isNoteOn = false;
   };

   // Pass the same `cursor` back on every call; initialise it to 0 and it will
   // be fast-forwarded to the live window on the first read. Returns how many
   // messages were written to `out`. Audio-thread safe: no locks, no
   // allocation.
   int MidiReadNotesSince(unsigned long long& cursor, MidiNoteMessage* out, int maxCount);

   // Current write position of the note ring - the value a consumer that only
   // wants *future* notes should seed its cursor with.
   unsigned long long MidiNoteStreamPosition();

   // ---- MIDI clock ---------------------------------------------------------
   // MIDI Clock (status byte 0xF8) is a realtime system message sent 24 times
   // per quarter note by a clock source (DJ mixer, drum machine, DAW) whenever
   // it's running - distinct from the CC/Note messages above. BPM isn't sent
   // directly; it's derived from the wall-clock spacing between pulses, smoothed
   // over a rolling window. 0xFA/0xFC (Start/Stop) reset that window so a
   // stopped-then-restarted clock doesn't average across the gap.
   bool MidiClockIsPresent(); // pulses seen within the last couple of seconds
   float MidiClockBpm();      // smoothed estimate; 0 if not enough pulses yet

}
