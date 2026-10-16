#pragma once
// Process, display, dialogs, image loading, crash log, document handling,
// single instance, URLs/HTTP and the Windows output window.
// Split out of Platform.h (docs/plans/compile-times/include-split.md, Phase 4);
// every Platform:: function still has macOS, Windows and Linux sides.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow; // opaque - keeps GLFW's headers out

namespace Platform
{
   // Holds an NSProcessInfo activity token that opts the process out of App
   // Nap for as long as the app runs. Without this, macOS treats a window
   // that isn't receiving input as idle/background and throttles its timer
   // sources - GLFW's event-polling and vsync among them - so a clock-driven
   // redraw (e.g. Switcher 3D's auto-switch) stalls until a mouse event
   // wakes the process back up, then bursts through the backlog at once.
   // Call once at startup and keep the app running for the token to matter.
   void PreventAppNap();

   // The user's OS language preferences, most preferred first, as BCP-47-style tags ("de-DE",
   // "zh-Hans-CN", "en"). Used only to pick the interface language on first launch; an explicit
   // choice in Settings always wins. Empty when the OS reports nothing. macOS: NSLocale
   // preferredLanguages; Windows: GetUserPreferredUILanguages; Linux: LANGUAGE / LC_ALL /
   // LC_MESSAGES / LANG.
   std::vector<std::string> PreferredLanguages();

   // Refresh clock of one display, used to pace projector (Output) windows to
   // the display they are on instead of to the canvas window's swap. (x, y)
   // is any point on that display in GLFW virtual-desktop coordinates
   // (glfwGetMonitorPos works); refreshHz is its current mode's rate.
   // Blocks until the display has begun `intervals` (>= 1) new refreshes
   // since this call last returned. If the caller is already past that
   // refresh it waits for the next one rather than returning mid-scanout, so
   // presents land on a whole number of refresh periods. It never relies on
   // a swap blocking, so it holds when every window is occluded or hidden.
   // Returns false without waiting when the display has no usable clock
   // (point on no display, rate <= 0, clock stalled); the caller then
   // presents unpaced for that frame. Main thread only.
   //   macOS: CVDisplayLink on that display.  Windows: DXGI WaitForVBlank on
   //   that display's output.  Linux: a steady timer at refreshHz (no
   //   portable vblank source; right rate, not locked to scanout phase).
   bool WaitForDisplayRefresh(int x, int y, double refreshHz, int intervals);
   // Releases the clock WaitForDisplayRefresh started. Call when the last
   // projector window closes; the next Wait starts a fresh one.
   void StopDisplayRefreshClock();

   // Current process resident set size in MB. Used by the INFINITE_BENCH
   // suite (docs/plans/perf/benchmark-suite.md) for the mem.rss_mb field -
   // not called anywhere on the hot path, so a syscall per call is fine.
   // Returns -1.0 if the platform has no cheap way to read it yet.
   double ProcessRssMb();

   // Memory the OS charges to this process, in MB, including pages it has
   // compressed or swapped out. RSS falls when macOS compresses memory under
   // pressure, so on an 8 GB machine it reads as the app freeing memory it
   // still holds. This is the number that tracks real usage and leaks:
   // phys_footprint on macOS (what Activity Monitor's "Memory" column shows),
   // PrivateUsage (commit charge) on Windows, VmRSS + VmSwap on Linux.
   // Returns -1.0 if it cannot be read.
   double ProcessFootprintMb();

   // Best-effort hardware model string ("MacBookPro18,3", a Windows
   // WMI/registry product name, or a Linux DMI string) for BENCH_JSON's
   // "machine" field. Returns "unknown" rather than failing - this is
   // diagnostic labelling only, never branched on.
   std::string HwModelString();

   // Trackpad pinch (macOS "magnify" gesture), drained as an incremental
   // delta since the last call - e.g. +0.02 for a small pinch-open, negative
   // for pinch-close. GLFW's Cocoa backend only forwards scrollWheel: (see
   // cocoa_window.m), never magnifyWithEvent:, so without this a trackpad
   // pinch produces no GLFW/ImGui event at all - not merely a wrong one.
   // Lazily attaches an NSMagnificationGestureRecognizer to the app's key
   // window the first time this is called from within a frame where one
   // exists yet. Always returns 0.0 on Windows (no equivalent gesture).
   double PollTrackpadMagnificationDelta();

   // Native open panel filtered to image types. Returns "" if cancelled.
   std::string OpenImageDialog();

   // Decodes any image format the OS understands (png/jpeg/tiff/heic/webp/raw/...)
   // into tightly packed RGBA8, already row-flipped for OpenGL's bottom-up
   // texture convention. Returns false and fills outError on failure.
   bool LoadImageRGBA(const std::string& path, std::vector<unsigned char>& outPixels,
                      int& outWidth, int& outHeight, std::string& outError);

   // Same decode/output contract as LoadImageRGBA (tightly packed RGBA8,
   // row-flipped for OpenGL's bottom-up convention), but for image bytes
   // already in memory rather than a file on disk - glTF/GLB textures are
   // either base64 data-URIs or raw bytes inside the .glb binary chunk,
   // neither of which is a path. Returns false and fills outError on failure.
   bool LoadImageRGBAFromMemory(const std::vector<unsigned char>& bytes,
                                std::vector<unsigned char>& outPixels,
                                int& outWidth, int& outHeight, std::string& outError);

   // Native open panel filtered to HDR equirectangular formats. ImageIO/NSImage
   // do not recognise .hdr, so this needs its own panel rather than reusing
   // OpenImageDialog - the same reason OpenModelDialog is separate from it.
   std::string OpenHdrDialog();

   // Decodes anything ImageIO can open (notably .exr, which macOS has read
   // natively since Ventura) into linear float RGB, via a CGBitmapContext in
   // the extended-linear-sRGB colour space so values above 1.0 survive rather
   // than clamping the way LoadImageRGBA's 8-bit path does. Not used for
   // .hdr: ImageIO does not read Radiance HDR, which is why that format goes
   // through stb_image instead.
   bool LoadImageFloatRGB(const std::string& path, std::vector<float>& outPixels,
                          int& outWidth, int& outHeight, std::string& outError);

   // ---- startup / crash diagnostics ---------------------------------------
   // Windows links the app with WIN32_EXECUTABLE (CMakeLists.txt) - a GUI
   // subsystem process with no console, so fprintf(stderr, ...) goes nowhere
   // and an unhandled exception unwinds silently: the window just vanishes.
   // macOS gets a `.ips` crash report and a visible terminal for free from
   // the OS, so both of these are real work on Windows and a no-op on macOS.

   // Installs a process-wide unhandled-exception handler that writes a
   // minidump plus a short text log to AppPaths::AppSupportDir() + "/crash/"
   // before the process dies, so a Windows crash leaves something to attach
   // to a report. Call once, early in main(), after argv-only setup. No-op
   // on macOS.
   void InstallCrashHandler();

   // Appends one line (a trailing newline is added) to a rolling
   // AppPaths::AppSupportDir() + "/log.txt", so a user who hits a startup
   // failure they can't see (see above) has something to attach to a report.
   // Best-effort: never throws, silently does nothing if the file can't be
   // opened. macOS and Linux also echo to stderr; Windows redirects stderr into
   // the same file.
   void AppendLogLine(const std::string& line);

   // Help > Reveal logs: shows AppPaths::AppSupportDir() + "/log.txt" selected in the OS file manager (the file is
   // created empty first so there is always something to select). Same file AppendLogLine writes on every platform.
   void RevealLogs();

   // Reports a fatal startup failure through every channel a user might
   // actually see: stderr (unconditionally, matches prior behavior) and,
   // on Windows only, a MessageBoxW plus AppendLogLine. macOS relies on the
   // stderr print alone, since it reaches a visible terminal/Console.app.
   // Intended for failures before the main window exists (glfwInit,
   // glfwCreateWindow, GL context/loader setup) - call this instead of a
   // bare fprintf(stderr, ...) at any of those sites.
   void ShowFatalError(const std::string& title, const std::string& message);

   // macOS Native Document Opening (Finder / AppleEvents)
   // Must be called before glfwInit() so the delegate methods exist on
   // GLFWApplicationDelegate by the time GLFW installs it as NSApp's delegate.
   void InitDocumentHandlingPreGlfw();
   // Must be called after glfwInit() to reinstall the Apple Event handler that
   // AppKit's -[NSApplication finishLaunching] (run inside glfwInit) clobbers.
   void InitDocumentHandlingPostGlfw();
   bool PollPendingOpenFile(std::string& outPath);

   // Single-instance launch. Call once at startup, before creating the window.
   // Returns true when this process should carry on as the app; false when a
   // running Infinite has been handed `patchPath` and this one should exit.
   // macOS always returns true: Launch Services already routes a second open to
   // the running app (see PollPendingOpenFile).
   bool ForwardOpenToRunningInstance(const std::string& patchPath);

   // ---- networking (update checker) ----------------------------------------

   // Hands a URL to the OS's default browser. Fire-and-forget: there is no
   // success callback, and a malformed or non-http(s) URL is dropped rather
   // than passed through, so this can never be used to launch a local
   // executable by way of a file:// or shell URL.
   void OpenExternalUrl(const std::string& url);

   // Shows `path` selected in the OS file manager (Finder / Explorer). Used by
   // the render queue's "Reveal" - the one thing a finished export wants that
   // the app itself can't show. Fire-and-forget and deliberately narrow: it
   // only ever selects an existing file in its own folder, it never opens or
   // executes it, and a path that does not exist is dropped.
   void RevealInFileManager(const std::string& path);

   // Blocking HTTPS GET. Call from a worker thread, never the render or audio
   // thread. Returns false on any transport, TLS, or non-2xx failure and fills
   // outError; outBody is only valid when it returns true. Bounded by
   // timeoutSeconds and a hard response-size cap so a hung or hostile endpoint
   // can't stall or balloon the caller.
   bool HttpGet(const std::string& url, const std::string& userAgent,
                std::string& outBody, std::string& outError,
                int timeoutSeconds = 10);

   // Streams an HTTPS GET to `destPath` (no size cap, nothing held in memory).
   // Call from a worker thread. `progress(done, total)` runs on the calling
   // thread as bytes arrive (total is 0 when the server does not say); return
   // false from it to cancel. Returns false on any failure, cancel or non-2xx and
   // fills outError; a partial destPath is deleted. timeoutSeconds bounds the
   // whole transfer.
   using HttpProgress = std::function<bool(uint64_t done, uint64_t total)>;
   bool HttpDownload(const std::string& url, const std::string& userAgent,
                     const std::string& destPath, const HttpProgress& progress,
                     std::string& outError, int timeoutSeconds = 600);

   // ---- output/projector window (Windows only) ----------------------------
   // Windows has no OS-level "make this app window fullscreen" affordance -
   // maximise keeps the title bar and taskbar, and an ordinary window stays
   // activatable/non-topmost in the normal Z-order, so it drops behind the
   // editor the moment focus moves back to it. macOS instead gets a real
   // fullscreen Space via NSWindow (handled entirely with GLFW_DECORATED/
   // GLFW_FLOATING window attribs in main.cpp, no Platform.mm entry point
   // needed), so these are declared for _WIN32 only - Platform.mm provides
   // no definition and none should be added.
   //
   // Windows projector policy: a normal decorated window while the user is
   // positioning it, then borderless + HWND_TOPMOST on fullscreen instead of
   // exclusive fullscreen. SWP_NOACTIVATE means the editor keeps keyboard
   // focus while the output stays visible on the other display.
#if defined(_WIN32)
   // Applies the borderless/topmost/cursor policy above. When w/h are both
   // > 0 the window is also moved and resized to exactly that virtual-screen
   // rect in the SAME SetWindowPos that applies the style change; otherwise
   // position and size are left alone.
   //
   // That combined call is not a convenience - it is the fix for a real
   // Windows-only defect. glfwSetWindowSize sets the CLIENT area, so sizing a
   // still-decorated window to the monitor's full video mode and only then
   // stripping the frame leaves a borderless window bigger than the monitor,
   // overhanging bottom-right. macOS has no equivalent problem: its
   // GLFW_DECORATED path keeps the content rect fixed when the frame goes
   // away, so main.cpp's non-Windows branch deliberately still sizes first.
   void ConfigureOutputWindow(GLFWwindow* window, bool borderless, bool topmost, bool hideCursor,
                              int x = 0, int y = 0, int w = 0, int h = 0);
   void ReassertOutputWindowTopmost(GLFWwindow* window);
   void SetWindowIconFromResource(GLFWwindow* window);
#endif
}
