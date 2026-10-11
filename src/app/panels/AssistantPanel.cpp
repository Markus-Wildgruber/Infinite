// Assistant panel: a right-docked chat that runs the user's own Claude Code
// (`claude` CLI) as a child process and lets it edit the open patch through
// this instance's own `Infinite --mcp` bridge (core/McpBridge.cpp ->
// RemoteControl -> Rpc.cpp). Infinite never sees Claude credentials: the
// child uses whatever login the user's Claude Code already has.
//
// Process model: one long-lived `claude -p` with stream-json in and out. Each
// message is one stdin line; a reader thread splits stdout into lines and the
// panel parses them on the main thread every frame. Stop kills the process
// tree; the next message starts a new one with --resume, so the conversation
// carries on.
//
// Editing or retrying an earlier question rewinds Claude's side too: every
// user entry remembers the last assistant message before it (session id +
// message uuid from the stream), and the rewound turn resumes that session at
// that message (--resume-session-at) as a fork, so the original transcript is
// left intact and the dropped answers are gone from Claude's context. Patch
// edits those answers made stay; Undo removes them as usual.
//
// History: each chat is one JSON file in <AppSupport>/assistant/chats, written
// whenever its log changes; the newest one reopens on launch. Claude Code keeps
// its own transcript per session id, so a reopened chat resumes with Claude
// remembering it. If that transcript is gone (cleaned up, other machine), the
// first start fails before `init`, and the chat carries on as a new session.
#include "app/AppShared.h"

#include "core/RemoteControl.h"
#include "platform/PlatformProcess.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cfloat>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace app
{
#if defined(INFINITE_AI_SIDEBAR)
   namespace
   {
      using json = nlohmann::json;

      // Appended to Claude Code's own system prompt. The tool descriptions
      // and the bridge's MCP instructions carry the how-to; this is the role.
      const char* kSystemPrompt =
         "You are the assistant panel inside Infinite, a node-based app for real-time visuals and sound. "
         "The user is looking at their patch while you work and sees every change appear live. "
         "Use the infinite tools to inspect and edit it; you have no other tools. "
         "Answer in the user's language, briefly. Before a large change, say in one sentence what you will do. "
         "After changing anything, say what you changed in plain words (node names, not indices). "
         "If a request is ambiguous, ask one short question instead of guessing. "
         "Never delete or replace existing work unless the user asked for it.";

      struct Entry
      {
         enum Kind { User, Assistant, Tool, Error, Info };
         Kind kind;
         std::string text;
         // User entries only: where Claude's conversation stood just before
         // this message, to rewind to on Edit/Retry. Empty uuid = the start.
         std::string anchorSession;
         std::string anchorUuid;
      };

      struct ChatMeta
      {
         std::string id;
         std::string title;
         long long updated = 0; // unix seconds
      };

      struct Session
      {
         Platform::ChildProcess* child = nullptr;
         std::thread reader;
         std::mutex mutex;
         std::deque<std::string> lines; // complete stdout lines, filled by `reader`
         std::atomic<bool> readerDone{ false };

         std::string chatId;    // file name (no extension) in the chats folder
         std::string sessionId; // for --resume after Stop / a crash / a relaunch
         std::string lastAssistantUuid; // newest assistant message in sessionId
         std::string resumeAt;  // one-shot: next start forks sessionId at this message
         bool resumedStart = false; // the running process was started with --resume
         bool gotInit = false;      // ...and has said hello since
         std::string lastSent;      // newest message written to stdin, resent if a resume fails
         bool busy = false;     // a turn is running
         double costUsd = 0.0;
         std::vector<Entry> log;
         bool loaded = false;   // newest saved chat restored
         bool dirty = false;    // log/session changed since the last save

         char input[16384] = {};
         // Soft wrap state for `input` (see InputCallback): positions of the
         // '\n's the panel inserted itself, the buffer as last wrapped, and the
         // width it was wrapped at (< 0 = wrap again before the next draw).
         std::vector<int> softBreaks;
         std::string wrappedInput;
         float wrapWidth = 0.0f;
         float wrappedAt = -1.0f;
         int inputGen = 0;          // bumped when the panel replaces the text, so ImGui drops its edit copy
         bool inputActive = false;  // the input box had keyboard focus last frame
         float inputHeight = 0.0f;  // drag-resizable; 0 = four lines
         bool uiLoaded = false;     // panel width / input height read from ui.json
         int menuIndex = -1;        // entry whose right-click menu is open
         int editIndex = -1;    // log index of the user entry being edited, or -1
         int copiedIndex = -2;  // entry whose Copy was just clicked (-1 = Copy all)
         double copiedTime = 0.0;
         bool scrollToBottom = false;
         bool focusInput = true;
         std::vector<ChatMeta> history; // filled when the History popup opens

         ~Session() { StopProcess(); }

         void StopProcess()
         {
            if (child == nullptr)
               return;
            Platform::ChildKill(child);
            if (reader.joinable())
               reader.join(); // ChildKill unblocks its read with EOF
            Platform::ChildFree(child);
            child = nullptr;
            busy = false;
         }
      };

      Session& S()
      {
         static Session s;
         return s;
      }

      const std::string& ClaudePath()
      {
         static const std::string path = Platform::FindExecutable("claude");
         return path;
      }

      std::string AssistantDir() { return AppPaths::AppSupportDir() + "/assistant"; }
      std::string ChatsDir() { return AssistantDir() + "/chats"; }
      std::filesystem::path ChatPath(const std::string& id) { return std::filesystem::u8path(ChatsDir() + "/" + id + ".json"); }

      long long NowSeconds()
      {
         return (long long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      }

      // Sortable and unique enough: 20261009-153012-4f2a.
      std::string NewChatId()
      {
         const std::time_t t = (std::time_t)NowSeconds();
         std::tm tm{};
#if defined(_WIN32)
         localtime_s(&tm, &t);
#else
         localtime_r(&t, &tm);
#endif
         char stamp[32];
         std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
         static std::mt19937 rng{ std::random_device{}() };
         char suffix[8];
         snprintf(suffix, sizeof(suffix), "%04x", (unsigned)(rng() & 0xffff));
         return std::string(stamp) + "-" + suffix;
      }

      std::string ChatTitle(const std::vector<Entry>& log)
      {
         for (const Entry& e : log)
            if (e.kind == Entry::User)
            {
               std::string t = e.text.substr(0, e.text.find('\n'));
               if (t.size() > 60)
               {
                  size_t cut = 57;
                  while (cut > 0 && (t[cut] & 0xC0) == 0x80) // don't split a UTF-8 sequence
                     --cut;
                  t = t.substr(0, cut) + "...";
               }
               return t;
            }
         return T("New chat");
      }

      void SaveChat(Session& s)
      {
         s.dirty = false;
         // An empty chat is not worth a history row.
         if (s.log.empty() || s.chatId.empty())
            return;
         json entries = json::array();
         for (const Entry& e : s.log)
         {
            json j = { {"k", (int)e.kind}, {"t", e.text} };
            if (!e.anchorSession.empty())
               j["as"] = e.anchorSession;
            if (!e.anchorUuid.empty())
               j["au"] = e.anchorUuid;
            entries.push_back(std::move(j));
         }
         const json doc = { {"version", 1}, {"title", ChatTitle(s.log)}, {"updated", NowSeconds()},
                            {"sessionId", s.sessionId}, {"lastAssistantUuid", s.lastAssistantUuid},
                            {"costUsd", s.costUsd}, {"entries", std::move(entries)} };
         std::error_code ec;
         std::filesystem::create_directories(std::filesystem::u8path(ChatsDir()), ec);
         // Write-then-rename, so quitting mid-write never leaves half a chat.
         const std::filesystem::path path = ChatPath(s.chatId);
         std::filesystem::path tmp = path;
         tmp += ".tmp";
         {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
               return;
            f << doc.dump(1);
         }
         std::filesystem::rename(tmp, path, ec);
      }

      bool ReadChatFile(const std::filesystem::path& path, json& out)
      {
         std::ifstream f(path, std::ios::binary);
         if (!f)
            return false;
         out = json::parse(f, nullptr, false);
         return !out.is_discarded() && out.is_object();
      }

      std::string Str(const json& j, const char* key)
      {
         auto it = j.find(key);
         return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
      }

      // ---- input box soft wrap ----------------------------------------------
      // ImGui 1.90's multiline input never wraps, so a long question ran off
      // the right edge. The panel wraps it itself: a line that would pass the
      // box's width gets a '\n' the panel inserted and remembers (softBreaks),
      // so it is taken out again before sending and re-flowed whenever the
      // text or the box width changes. Newlines the user typed stay put.

      size_t Utf8Len(unsigned char c)
      {
         return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
      }

      // Display text (with soft breaks) -> the text the user typed.
      // d2l[i] = index in the result of display char i (size display+1).
      std::string Unwrap(const std::string& display, const std::vector<int>& soft, std::vector<int>* d2l)
      {
         std::string out;
         out.reserve(display.size());
         if (d2l)
            d2l->assign(display.size() + 1, 0);
         size_t k = 0;
         for (size_t i = 0; i < display.size(); ++i)
         {
            if (d2l)
               (*d2l)[i] = (int)out.size();
            while (k < soft.size() && soft[k] < (int)i)
               ++k;
            if (k < soft.size() && soft[k] == (int)i && display[i] == '\n')
               continue;
            out += display[i];
         }
         if (d2l)
            (*d2l)[display.size()] = (int)out.size();
         return out;
      }

      // Typed text -> display text wrapped at `width` pixels of the current
      // font. Breaks after a space when there is one on the line, else
      // mid-word. l2d[i] = display index of typed char i (size text+1).
      std::string Wrap(const std::string& text, float width, std::vector<int>& soft, std::vector<int>* l2d)
      {
         soft.clear();
         std::string out;
         out.reserve(text.size() + text.size() / 16);
         if (l2d)
            l2d->assign(text.size() + 1, 0);
         ImFont* font = ImGui::GetFont();
         const float size = ImGui::GetFontSize();
         auto emit = [&](size_t from, size_t to) {
            for (size_t i = from; i < to; ++i)
            {
               if (l2d)
                  (*l2d)[i] = (int)out.size();
               out += text[i];
            }
         };
         size_t p0 = 0;
         while (p0 <= text.size())
         {
            size_t p1 = text.find('\n', p0);
            if (p1 == std::string::npos)
               p1 = text.size();
            size_t start = p0;
            while (width > 0.0f)
            {
               float x = 0.0f;
               size_t e = start, breakAt = std::string::npos;
               while (e < p1)
               {
                  const size_t len = std::min(Utf8Len((unsigned char)text[e]), p1 - e);
                  const float cw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data() + e, text.data() + e + len).x;
                  // Spaces may hang past the edge, so a break never lands before one.
                  if (x + cw > width && e > start && text[e] != ' ')
                     break;
                  x += cw;
                  e += len;
                  if (text[e - 1] == ' ')
                     breakAt = e;
               }
               if (e >= p1)
                  break;
               const size_t cut = (breakAt != std::string::npos && breakAt > start) ? breakAt : e;
               emit(start, cut);
               soft.push_back((int)out.size());
               out += '\n';
               start = cut;
            }
            emit(start, p1);
            if (p1 >= text.size())
               break;
            if (l2d)
               (*l2d)[p1] = (int)out.size();
            out += '\n'; // the user's own
            p0 = p1 + 1;
         }
         if (l2d)
            (*l2d)[text.size()] = (int)out.size();
         return out;
      }

      // Replaces the input text (Edit, Send, New chat). It is wrapped before
      // the next draw; the new ID makes ImGui read it instead of keeping the
      // copy it edits while the box has focus.
      void SetInput(Session& s, const std::string& text)
      {
         const size_t n = std::min(text.size(), sizeof(s.input) - 1);
         memcpy(s.input, text.data(), n);
         s.input[n] = '\0';
         s.softBreaks.clear();
         s.wrappedInput = s.input;
         s.wrappedAt = -1.0f;
         s.inputGen++;
      }

      std::string TypedInput(const Session& s)
      {
         return Unwrap(s.input, s.softBreaks, nullptr);
      }

      // Re-flows `display` (whose soft breaks were `soft` as of `before`),
      // mapping the cursor and selection. Returns false when nothing changed.
      bool Reflow(const std::string& before, const std::string& display, std::vector<int>& soft, float width,
                  bool deleteForward, int* cursor, int* selA, int* selB, std::string& out)
      {
         std::vector<int> carried;
         std::string typed;
         std::vector<int> d2l;
         if (display != before)
         {
            // Soft breaks in the untouched head and tail of the text survive the
            // edit; any '\n' inside the edited span was typed or pasted, so it is hard.
            size_t pre = 0;
            const size_t maxPre = std::min(before.size(), display.size());
            while (pre < maxPre && before[pre] == display[pre])
               ++pre;
            size_t suf = 0;
            while (suf < maxPre - pre && before[before.size() - 1 - suf] == display[display.size() - 1 - suf])
               ++suf;
            const int delta = (int)display.size() - (int)before.size();
            bool removedOnlySoft = display.size() - suf == pre && before.size() - suf > pre;
            for (int pos : soft)
            {
               if (pos < (int)pre)
                  carried.push_back(pos);
               else if (pos >= (int)(before.size() - suf))
                  carried.push_back(pos + delta);
            }
            for (size_t i = pre; removedOnlySoft && i < before.size() - suf; ++i)
               removedOnlySoft = std::find(soft.begin(), soft.end(), (int)i) != soft.end();
            typed = Unwrap(display, carried, &d2l);
            // Backspace (or Delete) on a soft break would just be re-wrapped
            // back in; take the character beside it out instead, as a real
            // soft-wrapping editor does.
            if (removedOnlySoft)
            {
               const int at = d2l[std::min(pre, display.size())];
               int from = deleteForward ? at : at - 1;
               if (from >= 0 && from < (int)typed.size())
               {
                  int len = 1;
                  while (from > 0 && ((unsigned char)typed[(size_t)from] & 0xC0) == 0x80)
                     --from, ++len; // back to the UTF-8 lead byte
                  len = (int)std::min(Utf8Len((unsigned char)typed[(size_t)from]), typed.size() - (size_t)from);
                  typed.erase((size_t)from, (size_t)len);
                  for (int& v : d2l)
                     if (v > from)
                        v = std::max(from, v - len);
               }
            }
         }
         else
         {
            carried = soft;
            typed = Unwrap(display, carried, &d2l);
         }
         std::vector<int> l2d;
         out = Wrap(typed, width, soft, &l2d);
         auto map = [&](int* p) {
            if (!p)
               return;
            const int d = std::clamp(*p, 0, (int)display.size());
            *p = l2d[(size_t)std::clamp(d2l[(size_t)d], 0, (int)typed.size())];
         };
         map(cursor);
         map(selA);
         map(selB);
         return out != display;
      }

      int InputCallback(ImGuiInputTextCallbackData* data)
      {
         Session& s = *(Session*)data->UserData;
         const std::string now(data->Buf, (size_t)data->BufTextLen);
         if (now == s.wrappedInput && s.wrappedAt == s.wrapWidth)
            return 0;
         std::vector<int> soft = s.softBreaks;
         int cursor = data->CursorPos, a = data->SelectionStart, b = data->SelectionEnd;
         std::string out;
         const bool changed = Reflow(s.wrappedInput, now, soft, s.wrapWidth, ImGui::IsKeyDown(ImGuiKey_Delete),
                                     &cursor, &a, &b, out);
         if (changed && (int)out.size() >= data->BufSize)
         {
            // No room for the breaks: keep the text unwrapped rather than cut it.
            s.wrappedInput = now;
            s.softBreaks.clear();
            s.wrappedAt = s.wrapWidth;
            return 0;
         }
         if (changed)
         {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, out.c_str());
         }
         data->CursorPos = cursor;
         data->SelectionStart = a;
         data->SelectionEnd = b;
         s.wrappedInput = out;
         s.softBreaks = soft;
         s.wrappedAt = s.wrapWidth;
         return 0;
      }

      // While the box does not have focus ImGui shows `input` itself, so a
      // width change (panel dragged) or SetInput re-wraps it directly.
      void WrapIdleInput(Session& s)
      {
         if (s.inputActive || s.wrappedAt == s.wrapWidth)
            return;
         std::vector<int> soft = s.softBreaks;
         std::string out;
         Reflow(s.input, s.input, soft, s.wrapWidth, false, nullptr, nullptr, nullptr, out);
         if (out.size() < sizeof(s.input))
         {
            memcpy(s.input, out.c_str(), out.size() + 1);
            s.softBreaks = soft;
         }
         s.wrappedInput = s.input;
         s.wrappedAt = s.wrapWidth;
      }

      // ---- panel layout, remembered in <AppSupport>/assistant/ui.json -------
      void LoadUi(Session& s)
      {
         s.uiLoaded = true;
         json doc;
         if (!ReadChatFile(std::filesystem::u8path(AssistantDir() + "/ui.json"), doc))
            return;
         const auto w = doc.find("panelWidth");
         if (w != doc.end() && w->is_number())
            gAssistantPanelWidth = std::max(kAssistantPanelMinWidth, w->get<float>());
         const auto h = doc.find("inputHeight");
         if (h != doc.end() && h->is_number())
            s.inputHeight = std::max(0.0f, h->get<float>());
      }

      void SaveUi(const Session& s)
      {
         std::error_code ec;
         std::filesystem::create_directories(std::filesystem::u8path(AssistantDir()), ec);
         std::ofstream f(std::filesystem::u8path(AssistantDir() + "/ui.json"), std::ios::binary | std::ios::trunc);
         if (f)
            f << json{ {"panelWidth", gAssistantPanelWidth}, {"inputHeight", s.inputHeight} }.dump(1);
      }

      void ResetChat(Session& s)
      {
         s.StopProcess();
         s.chatId.clear();
         s.log.clear();
         s.sessionId.clear();
         s.lastAssistantUuid.clear();
         s.resumeAt.clear();
         s.lastSent.clear();
         s.editIndex = -1;
         SetInput(s, "");
         s.costUsd = 0.0;
         s.dirty = false;
         s.focusInput = true;
      }

      bool LoadChat(Session& s, const std::string& id)
      {
         json doc;
         if (!ReadChatFile(ChatPath(id), doc))
            return false;
         // Parsed into locals first: a hand-edited or damaged file (a field of
         // the wrong type makes json::value() throw) is skipped, never half-loaded.
         std::vector<Entry> log;
         double cost = 0.0;
         try
         {
            const auto c = doc.find("costUsd");
            cost = (c != doc.end() && c->is_number()) ? c->get<double>() : 0.0;
            auto it = doc.find("entries");
            if (it != doc.end() && it->is_array())
               for (const json& j : *it)
               {
                  if (!j.is_object())
                     continue;
                  const auto kv = j.find("k");
                  const int k = (kv != j.end() && kv->is_number_integer()) ? kv->get<int>() : (int)Entry::Info;
                  if (k < Entry::User || k > Entry::Info)
                     continue;
                  log.push_back({ (Entry::Kind)k, Str(j, "t"), Str(j, "as"), Str(j, "au") });
               }
         }
         catch (const std::exception&)
         {
            return false;
         }
         ResetChat(s);
         s.chatId = id;
         s.sessionId = Str(doc, "sessionId");
         s.lastAssistantUuid = Str(doc, "lastAssistantUuid");
         s.costUsd = cost;
         s.log = std::move(log);
         s.scrollToBottom = true;
         return true;
      }

      std::vector<ChatMeta> ListChats()
      {
         std::vector<ChatMeta> out;
         std::error_code ec;
         for (const auto& de : std::filesystem::directory_iterator(std::filesystem::u8path(ChatsDir()), ec))
         {
            if (de.path().extension() != ".json")
               continue;
            json doc;
            if (!ReadChatFile(de.path(), doc))
               continue;
            const auto u = doc.find("updated");
            out.push_back({ de.path().stem().u8string(), Str(doc, "title"),
                            (u != doc.end() && u->is_number_integer()) ? u->get<long long>() : 0LL });
         }
         std::sort(out.begin(), out.end(), [](const ChatMeta& a, const ChatMeta& b) { return a.updated > b.updated; });
         return out;
      }

      void DeleteChat(const std::string& id)
      {
         std::error_code ec;
         std::filesystem::remove(ChatPath(id), ec);
      }

      std::string FormatWhen(long long unixSeconds)
      {
         const std::time_t t = (std::time_t)unixSeconds;
         std::tm tm{};
#if defined(_WIN32)
         localtime_s(&tm, &t);
#else
         localtime_r(&t, &tm);
#endif
         char buf[32];
         std::strftime(buf, sizeof(buf), "%d %b %H:%M", &tm);
         return buf;
      }

      void ReaderMain(Session* s, Platform::ChildProcess* child)
      {
         std::string pending;
         char buf[16384];
         for (;;)
         {
            const long n = Platform::ChildRead(child, buf, sizeof(buf));
            if (n <= 0)
               break;
            pending.append(buf, (size_t)n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
               std::string line = pending.substr(0, nl);
               pending.erase(0, nl + 1);
               if (!line.empty() && line.back() == '\r')
                  line.pop_back();
               if (line.empty())
                  continue;
               std::lock_guard<std::mutex> lock(s->mutex);
               s->lines.push_back(std::move(line));
            }
         }
         s->readerDone.store(true);
      }

      bool StartProcess(Session& s, std::string& err)
      {
         const std::string dir = AssistantDir();
         std::error_code ec;
         std::filesystem::create_directories(std::filesystem::u8path(dir), ec);

         // The bridge is this very executable; the port is the one this
         // instance listens on, so a second Infinite on another port is
         // never the one being edited.
         const json mcp = { {"mcpServers", { {"infinite", {
            {"command", Platform::ExecutablePath()},
            {"args", json::array({ "--mcp" })},
            {"env", { {"INFINITE_CONTROL_PORT", std::to_string(RemoteControl::Port())} }} } } }} };

         std::vector<std::string> args = {
            "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
            // Only Infinite's tools: no shell, no file access, and no MCP
            // servers from the user's own config.
            "--strict-mcp-config", "--mcp-config", mcp.dump(),
            "--tools", "", "--allowedTools", "mcp__infinite__*",
            // Allowed tools run without asking; anything else is refused
            // rather than waiting on a prompt nobody can answer.
            "--permission-mode", "dontAsk",
            "--append-system-prompt", kSystemPrompt,
         };
         s.resumedStart = !s.sessionId.empty();
         if (s.resumedStart)
         {
            args.push_back("--resume");
            args.push_back(s.sessionId);
            if (!s.resumeAt.empty())
            {
               // Rewind (Edit/Retry): continue from that message on a fork,
               // leaving the original session file as it was.
               args.push_back("--resume-session-at");
               args.push_back(s.resumeAt);
               args.push_back("--fork-session");
            }
         }
         s.resumeAt.clear();
         // Run in a folder of our own, so no project's CLAUDE.md, hooks or
         // .mcp.json from wherever Infinite was launched join the session.
         s.child = Platform::ChildStart(ClaudePath(), args, dir, err);
         if (s.child == nullptr)
            return false;
         s.gotInit = false;
         s.readerDone.store(false);
         s.reader = std::thread(ReaderMain, &s, s.child);
         return true;
      }

      // Starts the process if needed and writes one user message to it.
      void Deliver(Session& s, const std::string& text)
      {
         std::string err;
         if (s.child == nullptr && !StartProcess(s, err))
         {
            s.log.push_back({ Entry::Error, err });
            return;
         }
         const json msg = { {"type", "user"}, {"message", { {"role", "user"}, {"content", text} }} };
         if (!Platform::ChildWrite(s.child, msg.dump() + "\n"))
         {
            s.log.push_back({ Entry::Error, T("Claude Code is not responding. Press Stop and try again.") });
            return;
         }
         s.lastSent = text;
         s.busy = true;
      }

      void Send(Session& s, const std::string& text)
      {
         if (s.chatId.empty())
            s.chatId = NewChatId();
         s.log.push_back({ Entry::User, text, s.sessionId, s.lastAssistantUuid });
         s.scrollToBottom = true;
         s.dirty = true;
         Deliver(s, text);
      }

      // Drops user entry `index` and everything after it, and points the next
      // start at the conversation as it stood just before that entry.
      void RewindTo(Session& s, int index)
      {
         if (index < 0 || index >= (int)s.log.size() || s.log[index].kind != Entry::User)
            return;
         s.StopProcess();
         const Entry& e = s.log[index];
         bool patchEdited = false;
         for (size_t i = (size_t)index + 1; i < s.log.size(); ++i)
            patchEdited = patchEdited || s.log[i].kind == Entry::Tool;
         if (e.anchorUuid.empty())
         {
            s.sessionId.clear(); // first question: start over
            s.resumeAt.clear();
         }
         else
         {
            s.sessionId = e.anchorSession;
            s.resumeAt = e.anchorUuid;
         }
         s.lastAssistantUuid = e.anchorUuid;
         s.log.resize((size_t)index);
         if (patchEdited)
            s.log.push_back({ Entry::Info, T("Earlier answer replaced. The patch changes it made stay; Undo removes them.") });
         s.editIndex = -1;
         s.dirty = true;
      }

      std::string Transcript(const Session& s)
      {
         std::string out;
         for (const Entry& e : s.log)
         {
            if (!out.empty())
               out += e.kind == Entry::Tool ? "\n" : "\n\n";
            switch (e.kind)
            {
            case Entry::User: out += std::string(T("You")) + ": " + e.text; break;
            case Entry::Assistant: out += std::string(T("Assistant")) + ": " + e.text; break;
            case Entry::Tool: out += "  " + e.text; break;
            case Entry::Error:
            case Entry::Info: out += e.text; break;
            }
         }
         return out;
      }

      // One tool call as a short line: "create_node Shape", "connect 1 -> 3".
      std::string ToolLine(const json& block)
      {
         std::string name = block.value("name", "");
         const std::string prefix = "mcp__infinite__";
         if (name.rfind(prefix, 0) == 0)
            name = name.substr(prefix.size());
         const json in = block.value("input", json::object());
         std::string detail;
         if (name == "batch" && in.contains("calls") && in["calls"].is_array())
            detail = std::to_string(in["calls"].size()) + " " + T("changes");
         else if (in.contains("typeName"))
            detail = in["typeName"].get<std::string>();
         else if (in.contains("type") && in["type"].is_string())
            detail = in["type"].get<std::string>();
         else if (in.contains("param"))
            detail = in["param"].dump();
         return detail.empty() ? name : name + "  " + detail;
      }

      // message.content of a stream line, or an empty array when the line has
      // none (indexing a const json with a missing key is undefined).
      json Content(const json& msg)
      {
         const auto m = msg.find("message");
         if (m == msg.end() || !m->is_object())
            return json::array();
         const auto c = m->find("content");
         return (c != m->end() && c->is_array()) ? *c : json::array();
      }

      // json::value() throws on a field of the wrong type; Pump catches, so a
      // line Claude Code changes the shape of is skipped instead of crashing.
      void HandleLine(Session& s, const std::string& line)
      {
         const json msg = json::parse(line, nullptr, false);
         if (msg.is_discarded() || !msg.is_object())
            return;
         const std::string type = msg.value("type", "");
         if (type == "system" && msg.value("subtype", "") == "init")
         {
            s.gotInit = true;
            s.sessionId = msg.value("session_id", s.sessionId);
            s.dirty = true;
            for (const json& srv : msg.value("mcp_servers", json::array()))
               if (srv.value("name", "") == "infinite" && srv.value("status", "") != "connected")
                  s.log.push_back({ Entry::Error, std::string(T("Infinite's tools did not start: ")) +
                                                    srv.value("status", "?") });
         }
         else if (type == "assistant")
         {
            s.lastAssistantUuid = Str(msg, "uuid").empty() ? s.lastAssistantUuid : Str(msg, "uuid");
            for (const json& block : Content(msg))
            {
               if (!block.is_object())
                  continue;
               const std::string kind = block.value("type", "");
               if (kind == "text" && !block.value("text", "").empty())
                  s.log.push_back({ Entry::Assistant, block["text"].get<std::string>() });
               else if (kind == "tool_use")
                  s.log.push_back({ Entry::Tool, ToolLine(block) });
            }
            s.scrollToBottom = true;
            s.dirty = true;
         }
         else if (type == "user")
         {
            // Tool results: only failures are worth a line.
            for (const json& block : Content(msg))
               if (block.is_object() && block.value("type", "") == "tool_result" && block.value("is_error", false))
               {
                  std::string text;
                  const json c = block.contains("content") ? block["content"] : json();
                  if (c.is_string())
                     text = c.get<std::string>();
                  else if (c.is_array() && !c.empty() && c[0].is_object())
                     text = c[0].value("text", "");
                  if (text.size() > 240)
                     text = text.substr(0, 240) + "...";
                  s.log.push_back({ Entry::Tool, "  ! " + text });
                  s.dirty = true;
               }
         }
         else if (type == "result")
         {
            s.busy = false;
            s.sessionId = msg.value("session_id", s.sessionId);
            s.costUsd += msg.value("total_cost_usd", 0.0);
            if (msg.value("is_error", false))
               s.log.push_back({ Entry::Error, msg.value("result", std::string(T("Claude Code reported an error."))) });
            s.scrollToBottom = true;
            s.focusInput = true;
            s.dirty = true;
         }
      }

      void Pump(Session& s)
      {
         if (!s.loaded)
         {
            // Reopen the newest chat from the last run.
            s.loaded = true;
            const std::vector<ChatMeta> chats = ListChats();
            if (!chats.empty())
               LoadChat(s, chats.front().id);
         }

         std::deque<std::string> lines;
         {
            std::lock_guard<std::mutex> lock(s.mutex);
            lines.swap(s.lines);
         }
         for (const std::string& l : lines)
         {
            try
            {
               HandleLine(s, l);
            }
            catch (const std::exception&)
            {
            }
         }
         if (s.child != nullptr && s.readerDone.load())
         {
            // The process ended on its own (crash, logged out, killed outside).
            const bool wasBusy = s.busy;
            const bool resumeFailed = s.resumedStart && !s.gotInit;
            s.StopProcess();
            if (resumeFailed && wasBusy && !s.lastSent.empty())
            {
               // Claude Code no longer has this session (or that fork point):
               // carry on as a new conversation rather than failing forever.
               s.sessionId.clear();
               s.resumeAt.clear();
               s.lastAssistantUuid.clear();
               s.log.push_back({ Entry::Info, T("Claude Code no longer has the earlier part of this chat, so it continues as a new "
                                                "conversation. The messages above are still here, but Claude does not remember them.") });
               Deliver(s, s.lastSent);
            }
            else if (wasBusy)
               s.log.push_back({ Entry::Error, T("Claude Code stopped unexpectedly. Send again to continue.") });
            s.dirty = true;
         }
         if (s.dirty)
            SaveChat(s);
      }

      // What a click on an entry's hover buttons asked for; applied after the
      // log is drawn, since Edit/Retry rewrite the log being iterated.
      struct EntryAction
      {
         enum Kind { None, Edit, Retry } kind = None;
         int index = -1;
      };

      void DrawEntryText(const Entry& e)
      {
         const ImGuiStyle& style = ImGui::GetStyle();
         switch (e.kind)
         {
         case Entry::User:
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_ButtonActive]);
            ImGui::TextWrapped("%s", e.text.c_str());
            ImGui::PopStyleColor();
            break;
         case Entry::Assistant:
            ImGui::TextWrapped("%s", e.text.c_str());
            break;
         case Entry::Tool:
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("  %s", e.text.c_str());
            ImGui::PopStyleColor();
            break;
         case Entry::Error:
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
            ImGui::TextWrapped("%s", e.text.c_str());
            ImGui::PopStyleColor();
            break;
         case Entry::Info:
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", e.text.c_str());
            ImGui::PopStyleColor();
            break;
         }
      }

      // `index` < 0 draws a placeholder line (no hover buttons).
      void DrawEntry(Session& s, const Entry& e, int index, EntryAction& action)
      {
         ImGui::BeginGroup();
         DrawEntryText(e);
         ImGui::EndGroup();
         if (e.kind == Entry::Tool)
            return; // tool lines sit tight together, and have nothing to copy
         const ImVec2 after = ImGui::GetCursorScreenPos();

         // Hover buttons float over the entry's top-right corner, so showing
         // them never moves the text.
         const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
         const ImVec2 rmin = ImGui::GetItemRectMin();
         const ImVec2 rmax(right, ImGui::GetItemRectMax().y);
         // AllowWhenBlockedByActiveItem: pressing one of the buttons below makes
         // it the active item, and without this flag the window stops counting
         // as hovered on that very frame - the buttons vanished under the mouse
         // before the click could complete, so Copy/Edit/Retry never fired.
         const bool hovered = index >= 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                              ImGui::IsMouseHoveringRect(rmin, rmax, false);
         if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
         {
            s.menuIndex = index;
            ImGui::OpenPopup("##assistant_entry_menu");
         }
         if (hovered)
         {
            const ImGuiStyle& style = ImGui::GetStyle();
            const bool justCopied = s.copiedIndex == index && ImGui::GetTime() - s.copiedTime < 1.5;
            const char* copyLabel = justCopied ? T("Copied") : T("Copy");
            const bool isUser = e.kind == Entry::User;
            auto W = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
            float w = W(copyLabel);
            if (isUser)
               w += W(T("Edit")) + W(T("Retry")) + style.ItemSpacing.x * 2.0f;
            const ImVec2 pos(right - w, rmin.y);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - style.ItemSpacing.x, pos.y),
                                                      ImVec2(right, pos.y + ImGui::GetFontSize() + style.FramePadding.y * 2.0f),
                                                      ImGui::GetColorU32(ImGuiCol_PopupBg), style.FrameRounding);
            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID(index);
            if (ImGui::SmallButton(copyLabel))
            {
               ImGui::SetClipboardText(e.text.c_str());
               s.copiedIndex = index;
               s.copiedTime = ImGui::GetTime();
            }
            if (isUser)
            {
               ImGui::BeginDisabled(s.busy);
               ImGui::SameLine();
               if (ImGui::SmallButton(L("Edit")))
                  action = { EntryAction::Edit, index };
               if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                  ImGui::SetTooltip("%s", T("Change this question and ask again. The answers after it are dropped."));
               ImGui::SameLine();
               if (ImGui::SmallButton(L("Retry")))
                  action = { EntryAction::Retry, index };
               if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                  ImGui::SetTooltip("%s", T("Ask this question again. The answers after it are dropped."));
               ImGui::EndDisabled();
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(after);
         }
         ImGui::Spacing();
      }

      // Header button drawn as an icon, sized like a SmallButton.
      bool HeaderIconButton(const char* id, void (*draw)(ImDrawList*, ImVec2, float, ImU32, float), const char* tooltip, bool active = false)
      {
         const float h = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f;
         const bool clicked = ImGui::InvisibleButton(id, ImVec2(h, h));
         const ImVec2 bmin = ImGui::GetItemRectMin();
         const ImVec2 bmax = ImGui::GetItemRectMax();
         ImDrawList* dl = ImGui::GetWindowDrawList();
         const bool hovered = ImGui::IsItemHovered();
         if (hovered || active)
            dl->AddRectFilled(bmin, bmax, ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), ImGui::GetStyle().FrameRounding);
         if (hovered)
            ImGui::SetTooltip("%s", tooltip);
         draw(dl, ImVec2((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f), h * 0.8f, ImGui::GetColorU32(ImGuiCol_Text), 0.0f);
         return clicked;
      }

      void DrawHistoryPopup(Session& s)
      {
         if (!ImGui::BeginPopup("##assistant_history"))
            return;
         ImGui::TextDisabled("%s", T("Chats"));
         ImGui::Separator();
         if (s.history.empty())
            ImGui::TextDisabled("%s", T("No saved chats yet."));
         std::string open, remove, copy;
         const ImGuiStyle& style = ImGui::GetStyle();
         const float deleteW = ImGui::CalcTextSize(T("Delete")).x + style.FramePadding.x * 2.0f;
         const float copyW = ImGui::CalcTextSize(T("Copy")).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
         const float buttonsW = deleteW + copyW;
         const float rowW = std::max(340.0f, ImGui::GetContentRegionAvail().x);
         for (const ChatMeta& c : s.history)
         {
            ImGui::PushID(c.id.c_str());
            const std::string label = (c.title.empty() ? std::string(T("Untitled")) : c.title);
            const bool current = c.id == s.chatId;
            ImGui::BeginDisabled(s.busy && !current);
            if (ImGui::Selectable(label.c_str(), current, ImGuiSelectableFlags_DontClosePopups, ImVec2(rowW - buttonsW - 90.0f, 0.0f)) && !current)
               open = c.id;
            if (s.busy && !current && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
               ImGui::SetTooltip("%s", T("Wait for the current answer, or press Stop, before switching chats."));
            ImGui::EndDisabled();
            ImGui::SameLine(rowW - buttonsW - 80.0f);
            ImGui::TextDisabled("%s", FormatWhen(c.updated).c_str());
            ImGui::SameLine(rowW - buttonsW);
            if (ImGui::SmallButton(L("Copy")))
               copy = c.id;
            if (ImGui::IsItemHovered())
               ImGui::SetTooltip("%s", T("Copy this whole chat as text."));
            ImGui::SameLine(rowW - deleteW);
            if (ImGui::SmallButton(L("Delete")))
               remove = c.id;
            ImGui::PopID();
         }
         if (!copy.empty())
         {
            if (copy == s.chatId)
               ImGui::SetClipboardText(Transcript(s).c_str());
            else
            {
               // Read into a scratch session: never touches the open chat.
               auto scratch = std::make_unique<Session>();
               if (LoadChat(*scratch, copy))
                  ImGui::SetClipboardText(Transcript(*scratch).c_str());
            }
         }
         if (!open.empty())
         {
            SaveChat(s);
            LoadChat(s, open);
            ImGui::CloseCurrentPopup();
         }
         if (!remove.empty())
         {
            if (remove == s.chatId)
               ResetChat(s);
            DeleteChat(remove);
            s.history = ListChats();
         }
         ImGui::EndPopup();
      }

      // Right-click menu of one log entry (opened in DrawEntry).
      void DrawEntryMenu(Session& s, EntryAction& action)
      {
         if (!ImGui::BeginPopup("##assistant_entry_menu"))
            return;
         if (s.menuIndex < 0 || s.menuIndex >= (int)s.log.size())
         {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
         }
         const Entry& e = s.log[(size_t)s.menuIndex];
         if (ImGui::MenuItem(L("Copy")))
         {
            ImGui::SetClipboardText(e.text.c_str());
            s.copiedIndex = s.menuIndex;
            s.copiedTime = ImGui::GetTime();
         }
         if (e.kind == Entry::User)
         {
            if (ImGui::MenuItem(L("Edit"), nullptr, false, !s.busy))
               action = { EntryAction::Edit, s.menuIndex };
            if (ImGui::MenuItem(L("Retry"), nullptr, false, !s.busy))
               action = { EntryAction::Retry, s.menuIndex };
         }
         ImGui::Separator();
         if (ImGui::MenuItem(L("Copy all")))
         {
            ImGui::SetClipboardText(Transcript(s).c_str());
            s.copiedIndex = -1;
            s.copiedTime = ImGui::GetTime();
         }
         ImGui::EndPopup();
      }

      // A thin drag handle; returns the mouse delta while dragged. `vertical`
      // = a column you drag sideways.
      float DragHandle(const char* id, const ImVec2& pos, const ImVec2& size, bool vertical, bool* released)
      {
         const ImVec2 back = ImGui::GetCursorScreenPos();
         // The left-edge handle sits in the window padding, outside the
         // window's normal clip rect, where it could not be hovered.
         ImGui::PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
         ImGui::SetCursorScreenPos(pos);
         ImGui::InvisibleButton(id, size);
         const bool active = ImGui::IsItemActive();
         if (ImGui::IsItemHovered() || active)
         {
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered);
            if (vertical)
               dl->AddLine(ImVec2(pos.x + size.x * 0.5f, pos.y), ImVec2(pos.x + size.x * 0.5f, pos.y + size.y), col, 2.0f);
            else
               dl->AddLine(ImVec2(pos.x, pos.y + size.y * 0.5f), ImVec2(pos.x + size.x, pos.y + size.y * 0.5f), col, 2.0f);
         }
         ImGui::PopClipRect();
         if (released)
            *released = ImGui::IsItemDeactivated();
         ImGui::SetCursorScreenPos(back);
         if (!active)
            return 0.0f;
         return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
      }
   }

   bool AssistantAvailable()
   {
      return !ClaudePath().empty();
   }

   void PumpAssistantHidden()
   {
      // Keeps a running answer flowing into the log (and the saved chat)
      // while the panel is hidden.
      // Never in headless/test runs: those must not read the user's saved chats.
      if (!IsHeadlessProcess() && AssistantAvailable())
         Pump(S());
   }

   void DrawAssistantPanelDocked(const char* id, const ImVec2& size)
   {
      Session& s = S();
      if (!s.uiLoaded && !IsHeadlessProcess())
         LoadUi(s);
      Pump(s);

      PushDockedPanelStyle(/*isChild=*/true);
      ImGui::BeginChild(id, size, ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
      PopDockedPanelStyle();
      const ImVec2 wp = ImGui::GetWindowPos();
      const ImVec2 ws = ImGui::GetWindowSize();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 0.5f, wp.y), ImVec2(wp.x + 0.5f, wp.y + ws.y), PanelSeamColor(), 1.0f);
      {
         // Left edge: drag to make the panel wider or narrower. The layout
         // clamps it (StageLayout) so the canvas always keeps some room.
         bool released = false;
         const float dx = DragHandle("##assistant_width", wp, ImVec2(6.0f, ws.y), /*vertical=*/true, &released);
         if (dx != 0.0f)
            gAssistantPanelWidth = std::max(kAssistantPanelMinWidth, gAssistantPanelWidth - dx);
         if (released)
            SaveUi(s);
      }

      // Header: title, then History / Copy all / Stop / New chat / Hide, right-aligned.
      const ImGuiStyle& style = ImGui::GetStyle();
      const bool allCopied = s.copiedIndex == -1 && ImGui::GetTime() - s.copiedTime < 1.5;
      const char* copyAllLabel = allCopied ? T("Copied") : T("Copy all");
      ImGui::TextUnformatted(T("Assistant"));
      ImGui::SameLine();
      const float iconW = ImGui::GetFontSize() + style.FramePadding.y * 2.0f;
      const float buttonsW = ImGui::CalcTextSize(copyAllLabel).x + ImGui::CalcTextSize(T("Stop")).x +
                             style.FramePadding.x * 4.0f + style.ItemSpacing.x * 4.0f + iconW * 3.0f;
      ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsW));
      const bool historyOpen = ImGui::IsPopupOpen("##assistant_history");
      if (HeaderIconButton("##assistant_historybtn", &Tabler::DrawList, T("Chat history"), historyOpen))
      {
         s.history = ListChats();
         ImGui::OpenPopup("##assistant_history");
      }
      DrawHistoryPopup(s);
      ImGui::SameLine();
      ImGui::BeginDisabled(s.log.empty());
      if (ImGui::SmallButton(copyAllLabel))
      {
         ImGui::SetClipboardText(Transcript(s).c_str());
         s.copiedIndex = -1;
         s.copiedTime = ImGui::GetTime();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!s.busy);
      if (ImGui::SmallButton(L("Stop")))
      {
         s.StopProcess();
         s.log.push_back({ Entry::Info, T("Stopped. Changes made so far stay; undo removes them.") });
         s.dirty = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (HeaderIconButton("##assistant_new", &Tabler::DrawPlus, T("New chat (this one stays in the history)")))
      {
         SaveChat(s);
         ResetChat(s);
      }
      ImGui::SameLine();
      if (HeaderIconButton("##assistant_hide", &Tabler::DrawX, T("Hide the assistant. A running answer keeps going; show it again from the top bar.")))
         gAssistantPanelOpen = false;
      ImGui::Separator();

      const bool listening = RemoteControl::Listening();
      if (s.editIndex >= (int)s.log.size())
         s.editIndex = -1;
      const bool editing = s.editIndex >= 0;
      // The input box is drag-resizable from the handle above it: at least two
      // lines, and never so tall that the log has no room left.
      const float lineH = ImGui::GetTextLineHeight();
      const float defaultInputH = lineH * 4.0f + style.FramePadding.y * 2.0f;
      const float minInputH = lineH * 2.0f + style.FramePadding.y * 2.0f;
      const float maxInputH = std::max(minInputH, ImGui::GetContentRegionAvail().y - lineH * 6.0f);
      const float inputH = std::clamp(s.inputHeight > 0.0f ? s.inputHeight : defaultInputH, minInputH, maxInputH);
      const float handleH = 6.0f;
      const float footerH = inputH + handleH + ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f +
                            (editing ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
      EntryAction action;
      ImGui::BeginChild("##assistant_log", ImVec2(0.0f, -footerH));
      if (!listening)
         DrawEntry(s, { Entry::Error, std::string(T("This Infinite could not open its control port ")) +
                                         std::to_string(RemoteControl::Port()) +
                                         T(" - another Infinite is probably using it. Close the other one and restart this one.") }, -1, action);
      else if (s.log.empty())
         DrawEntry(s, { Entry::Info, T("Ask for something to build or change, e.g. \"add a slowly rotating star "
                                       "and make its size follow the bass\". Changes appear live; undo works as usual. "
                                       "Uses your own Claude Code login.") }, -1, action);
      for (int i = 0; i < (int)s.log.size(); ++i)
         DrawEntry(s, s.log[(size_t)i], i, action);
      DrawEntryMenu(s, action);
      if (s.busy)
      {
         ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
         const int dots = 1 + (int)(ImGui::GetTime() * 2.0) % 3;
         ImGui::Text("%s%.*s", T("Working"), dots, "...");
         ImGui::PopStyleColor();
      }
      if (s.scrollToBottom)
      {
         ImGui::SetScrollHereY(1.0f);
         s.scrollToBottom = false;
      }
      ImGui::EndChild();

      // Drag handle between the log and the input box.
      {
         bool released = false;
         const ImVec2 at = ImGui::GetCursorScreenPos();
         const float dy = DragHandle("##assistant_inputsize", at, ImVec2(ImGui::GetContentRegionAvail().x, handleH),
                                     /*vertical=*/false, &released);
         if (dy != 0.0f)
            s.inputHeight = std::clamp(inputH - dy, minInputH, maxInputH);
         if (released)
            SaveUi(s);
         ImGui::Dummy(ImVec2(0.0f, handleH - style.ItemSpacing.y));
      }

      if (action.kind == EntryAction::Edit && !s.busy)
      {
         SetInput(s, s.log[(size_t)action.index].text);
         s.editIndex = action.index;
         s.focusInput = true;
      }
      else if (action.kind == EntryAction::Retry && listening && !s.busy)
      {
         const std::string text = s.log[(size_t)action.index].text;
         RewindTo(s, action.index);
         SetInput(s, "");
         Send(s, text);
      }

      if (s.editIndex >= 0)
      {
         ImGui::TextDisabled("%s", T("Editing a question - sending replaces it and the answers after it."));
         ImGui::SameLine();
         if (ImGui::SmallButton(L("Cancel")) ||
             (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
         {
            s.editIndex = -1;
            SetInput(s, "");
            s.focusInput = true;
         }
      }

      // Wrap width = the text area inside the box: minus its padding and a
      // scrollbar's width, so a line never runs under the scrollbar.
      s.wrapWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - style.FramePadding.x * 2.0f - style.ScrollbarSize - 2.0f);
      WrapIdleInput(s);

      ImGui::BeginDisabled(!listening || s.busy);
      if (s.focusInput && !s.busy)
      {
         ImGui::SetKeyboardFocusHere();
         s.focusInput = false;
      }
      ImGui::PushID(s.inputGen);
      const bool enter = ImGui::InputTextMultiline("##assistant_input", s.input, sizeof(s.input), ImVec2(-1.0f, inputH),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                                      ImGuiInputTextFlags_CallbackAlways,
                                                   &InputCallback, &s);
      s.inputActive = ImGui::IsItemActive();
      ImGui::PopID();
      const bool click = ImGui::Button(s.editIndex >= 0 ? L("Send edit") : L("Send"));
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("%s", T("Enter sends, Ctrl+Enter new line"));
      if (s.costUsd > 0.0)
      {
         // Claude Code's own estimate; on a subscription it is notional.
         ImGui::SameLine();
         ImGui::TextDisabled("  ~$%.3f", s.costUsd);
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Estimated API cost of this chat, as Claude Code reports it. On a Claude plan it counts against your usage limits instead."));
      }
      std::string text = TypedInput(s);
      while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
         text.pop_back();
      if ((enter || click) && !text.empty() && !s.busy && listening)
      {
         if (s.editIndex >= 0)
            RewindTo(s, s.editIndex);
         Send(s, text);
         SetInput(s, "");
         s.focusInput = true;
      }
      if (s.dirty)
         SaveChat(s);

      ImGui::EndChild();
   }
#else
   bool AssistantAvailable() { return false; }
   void PumpAssistantHidden() {}
   void DrawAssistantPanelDocked(const char*, const ImVec2&) {}
#endif
}
