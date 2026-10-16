#pragma once

#include <string>
#include <vector>

#include "core/INode.h"

// Shared bodies for INode::PressButton (the assistant's press_button). Each
// helper runs exactly what the matching card button runs; the node's own
// PressButton chains them and ends with Unknown(). A helper returns true when
// it recognised `name`, with the press's result in `ok` (false + err when the
// press is not possible right now, the way the card would grey it out).
namespace NodeButtons
{
   // "unknown button 'x' (<what> has: a, b, c)". Always returns false.
   inline bool Unknown(const INode& node, const char* what, const std::string& name, std::string& err)
   {
      std::string list;
      for (const std::string& b : node.ButtonNames())
         list += (list.empty() ? "" : ", ") + b;
      err = "unknown button '" + name + "' (" + what + " has: " + list + ")";
      return false;
   }

   // The sampler family's Record/Stop toggle: "record" starts capturing the
   // audio input, "stop" ends the capture (and loads it).
   template <typename N>
   bool RecordToggle(N& n, const std::string& name, std::string& err, bool& ok)
   {
      if (name == "record")
      {
         ok = !n.IsRecording();
         if (ok)
            n.StartRecording();
         else
            err = "already recording";
         return true;
      }
      if (name == "stop")
      {
         ok = n.IsRecording();
         if (ok)
            n.StopRecording();
         else
            err = "not recording";
         return true;
      }
      return false;
   }

   // The sampler family's Audition/Stop toggle: "audition" runs `start`
   // (the card's trigger call), "stop_audition" stops the preview voice.
   template <typename N, typename StartFn>
   bool AuditionToggle(N& n, const std::string& name, std::string& err, bool& ok, StartFn start)
   {
      if (name == "audition")
      {
         ok = !n.IsPlaying();
         if (ok)
            start();
         else
            err = "already playing (stop_audition stops it)";
         return true;
      }
      if (name == "stop_audition")
      {
         ok = n.IsPlaying();
         if (ok)
            n.StopPreview();
         else
            err = "not playing";
         return true;
      }
      return false;
   }

   // A plain momentary action: runs `fn`, always possible.
   template <typename Fn>
   bool Action(const char* button, const std::string& name, bool& ok, Fn fn)
   {
      if (name != button)
         return false;
      fn();
      ok = true;
      return true;
   }
}
