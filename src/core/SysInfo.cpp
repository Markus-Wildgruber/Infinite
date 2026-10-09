#include "SysInfo.h"
#include "audio/PluginScanner.h"
#include "core/gl3.h"
#include "platform/PlatformAudio.h"
#include "platform/PlatformPlugins.h"
#include "platform/PlatformMidi.h"
#include "platform/PlatformVideoIO.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
   #include <windows.h>
#else
   #include <sys/utsname.h>
#endif

#ifndef INFINITE_VERSION_STRING
   #define INFINITE_VERSION_STRING "dev"
#endif

#if defined(__linux__)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
namespace Platform { extern bool HasGuiDialogHelper(); }
#endif

namespace SysInfo
{
   void PrintAndExit(GLFWwindow* /*window*/)
   {
      std::printf("================ Infinite System Info ================\n");
      std::printf("Infinite version: %s\n", INFINITE_VERSION_STRING);

#if defined(_WIN32)
      std::printf("OS: Windows\n");
#else
      struct utsname un {};
      if (uname(&un) == 0)
      {
         std::printf("OS: %s %s %s (%s)\n", un.sysname, un.release, un.version, un.machine);
      }
      else
      {
         std::printf("OS: POSIX (uname failed)\n");
      }
#endif

      int platform = glfwGetPlatform();
      const char* platformName = "Unknown";
      if (platform == GLFW_PLATFORM_WIN32) platformName = "Win32";
      else if (platform == GLFW_PLATFORM_COCOA) platformName = "Cocoa (macOS)";
      else if (platform == GLFW_PLATFORM_WAYLAND) platformName = "Wayland";
      else if (platform == GLFW_PLATFORM_X11) platformName = "X11";
      else if (platform == GLFW_PLATFORM_NULL) platformName = "Null";
      std::printf("GLFW Platform: %s\n", platformName);

      const GLubyte* vendor = glGetString(GL_VENDOR);
      const GLubyte* renderer = glGetString(GL_RENDERER);
      const GLubyte* version = glGetString(GL_VERSION);
      const GLubyte* glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);

      std::printf("GL Vendor:    %s\n", vendor ? (const char*)vendor : "null");
      std::printf("GL Renderer:  %s\n", renderer ? (const char*)renderer : "null");
      std::printf("GL Version:   %s\n", version ? (const char*)version : "null");
      std::printf("GLSL Version: %s\n", glslVersion ? (const char*)glslVersion : "null");
#if defined(__linux__)
      std::printf("Dialog helper: %s\n", Platform::HasGuiDialogHelper() ? "yes" : "no (zenity/kdialog missing)");
      std::printf("FFmpeg (libavutil): %s\n", av_version_info());
#if defined(INFINITE_ENABLE_GPL_CODECS)
      std::printf("H.264 encoder: %s\n", avcodec_find_encoder_by_name("libx264") != nullptr
                                             ? "libx264 (present)" : "libx264 (MISSING)");
#else
      std::printf("H.264 encoder: none (INFINITE_ENABLE_GPL_CODECS=OFF)\n");
#endif
      std::printf("AAC encoder: %s\n", avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr
                                          ? "present" : "MISSING");
      {
         const std::vector<Platform::CameraDeviceInfo> cams = Platform::CameraListDevices();
         std::printf("Cameras found: %zu\n", cams.size());
         for (const auto& cam : cams)
            std::printf("  - %s (%s)%s\n", cam.localizedName.c_str(), cam.uniqueId.c_str(),
                       cam.isDefault ? " [default]" : "");
      }
#endif

      // Audio backend/device and MIDI port enumeration - portable, since
      // AudioListDevices/MidiStart/MidiDeviceSummary are already implemented
      // on all three platforms (docs/plans/linux/phase-02-audio-midi.md
      // 2.5.3). This is diagnostics only: MidiStart's device list here comes
      // from whatever was already running, or a short-lived probe start/stop
      // if nothing was, so INFINITE_SYSINFO stays a read-only snapshot.
      const std::vector<Platform::AudioDeviceInfo> audioDevices = Platform::AudioListDevices();
      std::printf("Audio devices: %d\n", (int)audioDevices.size());
      for (const auto& dev : audioDevices)
      {
         std::printf("  [%u] %s%s%s (in=%d out=%d)\n", dev.deviceId, dev.name.c_str(),
                     dev.isInput ? " [input]" : "", dev.isOutput ? " [output]" : "",
                     dev.inputChannels, dev.outputChannels);
      }

      const bool midiWasRunning = Platform::MidiIsRunning();
      std::string midiError;
      if (!midiWasRunning)
         Platform::MidiStart(midiError);
      std::printf("MIDI ports: %s\n", Platform::MidiDeviceSummary().c_str());
      if (!midiWasRunning)
         Platform::MidiStop();

      // VST3 hosting - portable across all three platforms (declared and
      // implemented on macOS/Windows/Linux; a no-op that returns empty when
      // built with INFINITE_ENABLE_VST3=OFF), so this section carries no
      // platform #if of its own, matching the audio/MIDI sections above.
      // Folders and the live scan count use PluginScanner::DefaultVST3Folders
      // / Platform::EnumerateVST3Plugins directly rather than a cached index,
      // for the same "read-only live probe" reason the audio/MIDI sections
      // above do a short-lived probe instead of trusting a stale cache.
      {
         const std::vector<std::string> folders = PluginScanner::DefaultVST3Folders();
         std::printf("VST3 search folders: %d\n", (int)folders.size());
         for (const std::string& f : folders)
            std::printf("  - %s\n", f.c_str());

         std::vector<Platform::PluginDesc> vst3Found;
         Platform::EnumerateVST3Plugins(folders, vst3Found);
         std::printf("VST3 plugins found (scan count): %d\n", (int)vst3Found.size());

         const std::vector<std::string> blocklist = Platform::VST3Blocklist();
         std::printf("VST3 blocklist count: %d\n", (int)blocklist.size());
         for (const std::string& b : blocklist)
            std::printf("  - %s\n", b.c_str());
      }

      std::printf("======================================================\n");
      std::fflush(stdout);
      std::exit(0);
   }

   void CrashTest()
   {
      volatile int* bad = nullptr;
      *bad = 42;
   }
}
