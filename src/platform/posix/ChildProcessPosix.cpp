// Platform::Child* on macOS and Linux: posix_spawn with stdin/stdout pipes in
// a new process group, so ChildKill ends the child and every process it
// started (claude spawns `Infinite --mcp` itself) with one kill(-pgid). See
// Platform.h. Shared by APPLE_SOURCES and LINUX_SOURCES.
#include "platform/Platform.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace Platform
{
   struct ChildProcess
   {
      pid_t pid = -1;
      int stdinFd = -1;
      int stdoutFd = -1;
      bool reaped = false;
   };

   namespace
   {
      void CloseFd(int& fd)
      {
         if (fd >= 0)
            close(fd);
         fd = -1;
      }

      bool IsExecutable(const std::string& p)
      {
         struct stat st;
         return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
      }
   }

   ChildProcess* ChildStart(const std::string& exe, const std::vector<std::string>& args,
                            const std::string& cwd, std::string& outError)
   {
      // Writing to a child that already exited raises SIGPIPE, whose default
      // action ends this whole process. Nothing in the app relies on it (the
      // RemoteControl socket replying to a vanished client is the same hole),
      // so ignore it process-wide: the write returns EPIPE instead.
      signal(SIGPIPE, SIG_IGN);

      int in[2] = { -1, -1 }, out[2] = { -1, -1 };
      if (pipe(in) != 0 || pipe(out) != 0)
      {
         outError = std::string("pipe failed: ") + std::strerror(errno);
         CloseFd(in[0]);
         CloseFd(in[1]);
         return nullptr;
      }
      // Our ends must not leak into the child (or any later child), or the
      // pipe never reports EOF when the child exits.
      fcntl(in[1], F_SETFD, FD_CLOEXEC);
      fcntl(out[0], F_SETFD, FD_CLOEXEC);

      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addclose(&actions, in[0]);
      posix_spawn_file_actions_addclose(&actions, out[1]);

      // Own process group (pgid = child pid): ChildKill signals the group.
      posix_spawnattr_t attr;
      posix_spawnattr_init(&attr);
      posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
      posix_spawnattr_setpgroup(&attr, 0);

      std::vector<std::string> argvStore;
      argvStore.push_back(exe);
      argvStore.insert(argvStore.end(), args.begin(), args.end());
      std::vector<char*> argv;
      for (std::string& a : argvStore)
         argv.push_back(a.data());
      argv.push_back(nullptr);

      // posix_spawn has no portable cwd action (addchdir is macOS 10.15+ /
      // glibc 2.29+ under different names), so switch around the spawn. The
      // panel spawns from the main thread only, so no other thread is
      // resolving relative paths meanwhile.
      std::string prevCwd;
      if (!cwd.empty())
      {
         char buf[4096];
         if (getcwd(buf, sizeof(buf)) != nullptr)
            prevCwd = buf;
         if (chdir(cwd.c_str()) != 0)
            prevCwd.clear();
      }
      pid_t pid = -1;
      const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);
      if (!prevCwd.empty())
         (void)chdir(prevCwd.c_str());
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attr);
      CloseFd(in[0]);
      CloseFd(out[1]);
      if (rc != 0)
      {
         outError = "could not start " + exe + ": " + std::strerror(rc);
         CloseFd(in[1]);
         CloseFd(out[0]);
         return nullptr;
      }

      ChildProcess* child = new ChildProcess();
      child->pid = pid;
      child->stdinFd = in[1];
      child->stdoutFd = out[0];
      return child;
   }

   bool ChildWrite(ChildProcess* child, const std::string& data)
   {
      if (child == nullptr || child->stdinFd < 0)
         return false;
      size_t sent = 0;
      while (sent < data.size())
      {
         // A dead child gives EPIPE here (SIGPIPE is ignored, see ChildStart).
         const ssize_t n = write(child->stdinFd, data.data() + sent, data.size() - sent);
         if (n < 0 && errno == EINTR)
            continue;
         if (n <= 0)
            return false;
         sent += (size_t)n;
      }
      return true;
   }

   long ChildRead(ChildProcess* child, char* buf, size_t cap)
   {
      if (child == nullptr || child->stdoutFd < 0)
         return -1;
      for (;;)
      {
         const ssize_t n = read(child->stdoutFd, buf, cap);
         if (n < 0 && errno == EINTR)
            continue;
         return (long)n;
      }
   }

   void ChildKill(ChildProcess* child)
   {
      if (child == nullptr || child->pid <= 0 || child->reaped)
         return;
      // The whole group: the child and the MCP server it started. Every
      // write end of the stdout pipe dies with them, so a ChildRead blocked
      // on another thread returns 0.
      kill(-child->pid, SIGTERM);
   }

   void ChildFree(ChildProcess* child)
   {
      if (child == nullptr)
         return;
      CloseFd(child->stdinFd);
      if (child->pid > 0 && !child->reaped)
      {
         // Closed stdin ends a stream-json claude on its own; give it a moment.
         int status = 0;
         for (int i = 0; i < 30 && waitpid(child->pid, &status, WNOHANG) == 0; i++)
            usleep(100 * 1000);
         if (waitpid(child->pid, &status, WNOHANG) == 0)
         {
            kill(-child->pid, SIGKILL);
            waitpid(child->pid, &status, 0);
         }
         child->reaped = true;
      }
      CloseFd(child->stdoutFd);
      delete child;
   }

   std::string FindExecutable(const std::string& name)
   {
      std::vector<std::string> dirs;
      if (const char* path = std::getenv("PATH"))
      {
         std::string p = path;
         size_t start = 0;
         while (start <= p.size())
         {
            const size_t colon = p.find(':', start);
            const std::string dir = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!dir.empty())
               dirs.push_back(dir);
            if (colon == std::string::npos)
               break;
            start = colon + 1;
         }
      }
      // A Finder/desktop-launched app gets a minimal PATH without the
      // per-user and Homebrew folders installers put tools in.
      if (const char* home = std::getenv("HOME"))
      {
         dirs.push_back(std::string(home) + "/.local/bin");
         dirs.push_back(std::string(home) + "/.claude/local");
         dirs.push_back(std::string(home) + "/bin");
      }
      dirs.push_back("/opt/homebrew/bin");
      dirs.push_back("/usr/local/bin");
      dirs.push_back("/usr/bin");
      for (const std::string& d : dirs)
      {
         const std::string candidate = d + "/" + name;
         if (IsExecutable(candidate))
            return candidate;
      }
      return std::string();
   }
}
