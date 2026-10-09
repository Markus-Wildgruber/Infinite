// Platform::Child* on Windows: CreateProcessW with stdin/stdout pipes, inside a
// Job object so ChildKill ends the child and every process it started (claude
// spawns `Infinite --mcp` itself) in one call. See Platform.h.
#include "platform/Platform.h"

#include "WinCommon.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace Platform
{
   struct ChildProcess
   {
      HANDLE process = nullptr;
      HANDLE job = nullptr;
      HANDLE stdinWrite = nullptr;
      HANDLE stdoutRead = nullptr;
   };

   namespace
   {
      // One argument quoted by the rules CommandLineToArgvW / the MSVC CRT
      // parse with: backslashes are literal except before a quote, where they
      // are doubled; a quote is escaped with a backslash.
      std::wstring QuoteArg(const std::wstring& a)
      {
         if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return a;
         std::wstring out = L"\"";
         size_t backslashes = 0;
         for (wchar_t c : a)
         {
            if (c == L'\\')
            {
               backslashes++;
               continue;
            }
            if (c == L'"')
               out.append(backslashes * 2 + 1, L'\\');
            else
               out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
         }
         out.append(backslashes * 2, L'\\');
         out += L'"';
         return out;
      }

      void CloseIf(HANDLE& h)
      {
         if (h != nullptr && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
         h = nullptr;
      }

      bool IsFile(const std::wstring& p)
      {
         const DWORD attr = GetFileAttributesW(p.c_str());
         return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
      }
   }

   ChildProcess* ChildStart(const std::string& exe, const std::vector<std::string>& args,
                            const std::string& cwd, std::string& outError)
   {
      SECURITY_ATTRIBUTES sa = {};
      sa.nLength = sizeof(sa);
      sa.bInheritHandle = TRUE;

      HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
      if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0))
      {
         outError = "CreatePipe failed";
         CloseIf(inRead);
         CloseIf(inWrite);
         return nullptr;
      }
      // Our ends must not be inherited, or the child holds its own pipe open
      // and EOF never arrives when it exits.
      SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
      HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

      // Everything the child starts joins the job; closing the job's last
      // handle (ChildKill / ChildFree) terminates all of it.
      HANDLE job = CreateJobObjectW(nullptr, nullptr);
      if (job != nullptr)
      {
         JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
         info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
         SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
      }

      STARTUPINFOW si = {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = inRead;
      si.hStdOutput = outWrite;
      si.hStdError = nul;

      const std::wstring wexe = WinCommon::Utf8ToWide(exe);
      std::wstring cmd = QuoteArg(wexe);
      for (const std::string& a : args)
         cmd += L" " + QuoteArg(WinCommon::Utf8ToWide(a));
      std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
      cmdBuf.push_back(L'\0');
      const std::wstring wcwd = WinCommon::Utf8ToWide(cwd);

      PROCESS_INFORMATION pi = {};
      // Suspended until it is in the job, so nothing it starts can escape it.
      const BOOL ok = CreateProcessW(wexe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                     cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
      const DWORD createError = ok ? 0 : GetLastError(); // before CloseHandle can overwrite it
      CloseIf(inRead);
      CloseIf(outWrite);
      CloseIf(nul);
      if (!ok)
      {
         outError = "could not start " + exe + ": " + WinCommon::HrToString("CreateProcessW", HRESULT_FROM_WIN32(createError));
         CloseIf(inWrite);
         CloseIf(outRead);
         CloseIf(job);
         return nullptr;
      }
      if (job != nullptr)
         AssignProcessToJobObject(job, pi.hProcess);
      ResumeThread(pi.hThread);
      CloseHandle(pi.hThread);

      ChildProcess* child = new ChildProcess();
      child->process = pi.hProcess;
      child->job = job;
      child->stdinWrite = inWrite;
      child->stdoutRead = outRead;
      return child;
   }

   bool ChildWrite(ChildProcess* child, const std::string& data)
   {
      if (child == nullptr || child->stdinWrite == nullptr)
         return false;
      size_t sent = 0;
      while (sent < data.size())
      {
         DWORD n = 0;
         if (!WriteFile(child->stdinWrite, data.data() + sent, (DWORD)(data.size() - sent), &n, nullptr) || n == 0)
            return false;
         sent += n;
      }
      return true;
   }

   long ChildRead(ChildProcess* child, char* buf, size_t cap)
   {
      if (child == nullptr || child->stdoutRead == nullptr)
         return -1;
      DWORD n = 0;
      if (!ReadFile(child->stdoutRead, buf, (DWORD)cap, &n, nullptr))
         return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
      return (long)n;
   }

   void ChildKill(ChildProcess* child)
   {
      if (child == nullptr)
         return;
      // Ends the whole tree; the pipe's write ends die with it, which
      // returns ERROR_BROKEN_PIPE (EOF) to a ChildRead blocked on another thread.
      if (child->job != nullptr)
         TerminateJobObject(child->job, 1);
      else if (child->process != nullptr)
         TerminateProcess(child->process, 1);
   }

   void ChildFree(ChildProcess* child)
   {
      if (child == nullptr)
         return;
      CloseIf(child->stdinWrite);
      if (child->process != nullptr && WaitForSingleObject(child->process, 3000) == WAIT_TIMEOUT)
         ChildKill(child);
      CloseIf(child->stdoutRead);
      CloseIf(child->process);
      CloseIf(child->job); // KILL_ON_JOB_CLOSE: anything still alive in the tree ends here
      delete child;
   }

   std::string FindExecutable(const std::string& name)
   {
      const std::wstring file = WinCommon::Utf8ToWide(name) + L".exe";
      wchar_t found[MAX_PATH * 4];
      if (SearchPathW(nullptr, file.c_str(), nullptr, (DWORD)(sizeof(found) / sizeof(found[0])), found, nullptr) > 0)
         return WinCommon::WideToUtf8(found);
      // The native installer's folder, missing from PATH for an app started
      // before the installer edited it.
      if (const wchar_t* home = _wgetenv(L"USERPROFILE"))
      {
         const std::wstring p = std::wstring(home) + L"\\.local\\bin\\" + file;
         if (IsFile(p))
            return WinCommon::WideToUtf8(p);
      }
      return std::string();
   }
}
