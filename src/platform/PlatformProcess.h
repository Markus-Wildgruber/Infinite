#pragma once
// Long-running child processes over pipes (the assistant panel's `claude`).
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
   // A long-running child process talking over pipes (the assistant panel's
   // `claude` subprocess). stdin and stdout are pipes; stderr is discarded.
   // The child and everything it starts in turn (claude's own MCP server
   // child) form one group that ChildKill ends together. Implemented in
   // posix/ChildProcessPosix.cpp (macOS, Linux) and win/ChildProcessWin.cpp.
   struct ChildProcess;
   // `args` excludes argv[0]. `cwd` empty = inherit. nullptr + outError on failure.
   ChildProcess* ChildStart(const std::string& exe, const std::vector<std::string>& args,
                            const std::string& cwd, std::string& outError);
   // Blocking write of all of `data`; false once the child's stdin is closed or gone.
   bool ChildWrite(ChildProcess* child, const std::string& data);
   // Blocking read of up to `cap` bytes of stdout: > 0 bytes read, 0 at EOF
   // (child exited or was killed), < 0 on error. Safe to call from a worker
   // thread while another thread calls ChildKill, which unblocks it with EOF.
   long ChildRead(ChildProcess* child, char* buf, size_t cap);
   // Terminates the child and every process it started. Idempotent.
   void ChildKill(ChildProcess* child);
   // Waits for the child to exit and frees it. Call after ChildRead returned
   // <= 0 (or after ChildKill) and once no thread is inside ChildRead.
   void ChildFree(ChildProcess* child);
   // Absolute path of an executable named `name` (no extension; ".exe" is
   // added on Windows) on PATH or in the per-user install folders a GUI app's
   // PATH often lacks (~/.local/bin, /opt/homebrew/bin, ...). "" when absent.
   std::string FindExecutable(const std::string& name);
}
