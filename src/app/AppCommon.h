#pragma once
// Everything main.cpp used to include before its first declaration: the shared
// preamble of main.cpp and the translation units split out of it (src/app/).
#define GLFW_INCLUDE_NONE
#include "gl3.h"
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
   // No CF symbols remain in this file, but the include predates the Windows
   // port and macOS toolchains expect it in the preamble - keep it Apple-only.
   #include <CoreFoundation/CoreFoundation.h>
   // malloc_zone_statistics - used by INFINITE_FIELDSAMPLETEST's zero-
   // allocation check (reads the allocator's own counters rather than
   // overriding the process-wide operator new/delete).
   #include <malloc/malloc.h>
#endif

#include "imgui.h"
#include "platform/common/MidiCC14.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_node_editor.h"
#include "imgui_stdlib.h"
// For direct ImGuiInputTextState access: ImGui's own AutoSelectAll flag (and
// the select-all it triggers implicitly whenever focus arrives through
// SetKeyboardFocusHere's nav-activation path) can't be trusted to land
// before a same-frame keystroke is processed, so typed-param entry sets the
// cursor/selection state explicitly once the field is confirmed active.
#include "imgui_internal.h"
#include <cstdarg>
#include "TablerIcons.h"

#include "stb_image_write.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_set>
#include <filesystem>
#include <sys/stat.h>
#include "platform/AppPaths.h"
#include "platform/PlatformApp.h"
#include "IconsLucide.h"
#include "core/SysInfo.h"
#include "core/BenchReport.h"
#include "core/BenchFrameTail.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

// INFINITE_MIDIPARSETEST's pure-parser hook, so the test can drive
// MidiLinux.cpp's ALSA sequencer event->table/ring code with synthetic
// snd_seq_event_t values and no real /dev/snd/seq - see
// docs/plans/linux/phase-02-audio-midi.md 2.5.2 and
// docs/plans/linux/validation.md's P0 spike (no kernel sound modules on
// GitHub Actions/OrbStack, so this is the primary MIDI proof on CI). Linux
// only, same footing as the existing _WIN32 branches in this file for
// platform-specific test dispatch - see linux-parity SS0's note that main.cpp
// currently carries no __linux__ branches, which this is the first of,
// exactly because there is no portable way to exercise an ALSA-specific
// parser from a Platform:: call all three platforms implement.
#if defined(__linux__)
#include "platform/linux/MidiParseTestHooks.h"
#endif

// Displayed shortcut labels: the modifier key shown in menus and the
// shortcuts reference differs by platform (Cmd doesn't exist on Windows),
// while the underlying handling already accepts Ctrl on both (see cmdOrCtrl).
#if defined(__APPLE__)
   #define MODKEY "Cmd"
#else
   #define MODKEY "Ctrl"
#endif

// Test fixtures live in the OS temp directory; /tmp was hardcoded before
// the Windows port.
inline std::string TmpPath(const std::string& name)
{
   return AppPaths::TempDir() + "/" + name;
}
#include <functional>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <set>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "core/NodeFactory.h"
#include "core/CategoryColors.h"
#include "core/I18n.h"
#include "core/UiScale.h"
#include "core/SplashScreen.h"
#include "core/LauncherCard.h"
#include "core/GLUtil.h"
#include "core/GraphNode.h"
#include "core/FilterDefs.h"
#include "core/BlendModes.h"
#include "core/Transport.h"
#include "core/AudioTopologyRequest.h"
#include "core/Modulation.h"
#include "core/MovementLog.h"
#include "core/MovementStats.h"
#include "core/AudioSummary.h"
#include "core/ColorStats.h"
#include "core/ContactSheet.h"
#include "core/HeadlessJob.h"
#include "core/HeadlessNotes.h"
#include "core/PatchSchema.h"
#include "core/PatchExplain.h"

#include "core/Patch.h"
#include "core/AudioCable.h"
#include "core/NoteCable.h"

#ifndef INFINITE_VERSION_STRING
#define INFINITE_VERSION_STRING "0.0.0"
#endif

namespace app { using I18n::T; using I18n::TC; using I18n::L; using I18n::LC; }

namespace ed = ax::NodeEditor;
