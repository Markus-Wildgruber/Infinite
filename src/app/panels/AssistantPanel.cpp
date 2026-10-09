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
#include "app/AppShared.h"

#include "core/RemoteControl.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
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
      };

      struct Session
      {
         Platform::ChildProcess* child = nullptr;
         std::thread reader;
         std::mutex mutex;
         std::deque<std::string> lines; // complete stdout lines, filled by `reader`
         std::atomic<bool> readerDone{ false };

         std::string sessionId; // for --resume after Stop / a crash
         bool busy = false;     // a turn is running
         double costUsd = 0.0;
         std::vector<Entry> log;
         char input[4096] = {};
         bool scrollToBottom = false;
         bool focusInput = true;

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
         const std::string dir = AppPaths::AppSupportDir() + "/assistant";
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
         if (!s.sessionId.empty())
         {
            args.push_back("--resume");
            args.push_back(s.sessionId);
         }
         // Run in a folder of our own, so no project's CLAUDE.md, hooks or
         // .mcp.json from wherever Infinite was launched join the session.
         s.child = Platform::ChildStart(ClaudePath(), args, dir, err);
         if (s.child == nullptr)
            return false;
         s.readerDone.store(false);
         s.reader = std::thread(ReaderMain, &s, s.child);
         return true;
      }

      void Send(Session& s, const std::string& text)
      {
         s.log.push_back({ Entry::User, text });
         s.scrollToBottom = true;
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
         s.busy = true;
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

      void HandleLine(Session& s, const std::string& line)
      {
         const json msg = json::parse(line, nullptr, false);
         if (msg.is_discarded() || !msg.is_object())
            return;
         const std::string type = msg.value("type", "");
         if (type == "system" && msg.value("subtype", "") == "init")
         {
            s.sessionId = msg.value("session_id", s.sessionId);
            for (const json& srv : msg.value("mcp_servers", json::array()))
               if (srv.value("name", "") == "infinite" && srv.value("status", "") != "connected")
                  s.log.push_back({ Entry::Error, std::string(T("Infinite's tools did not start: ")) +
                                                    srv.value("status", "?") });
         }
         else if (type == "assistant")
         {
            for (const json& block : msg["message"].value("content", json::array()))
            {
               const std::string kind = block.value("type", "");
               if (kind == "text" && !block.value("text", "").empty())
                  s.log.push_back({ Entry::Assistant, block["text"].get<std::string>() });
               else if (kind == "tool_use")
                  s.log.push_back({ Entry::Tool, ToolLine(block) });
            }
            s.scrollToBottom = true;
         }
         else if (type == "user")
         {
            // Tool results: only failures are worth a line.
            for (const json& block : msg["message"].value("content", json::array()))
               if (block.is_object() && block.value("type", "") == "tool_result" && block.value("is_error", false))
               {
                  std::string text;
                  const json& c = block["content"];
                  if (c.is_string())
                     text = c.get<std::string>();
                  else if (c.is_array() && !c.empty() && c[0].is_object())
                     text = c[0].value("text", "");
                  if (text.size() > 240)
                     text = text.substr(0, 240) + "...";
                  s.log.push_back({ Entry::Tool, "  ! " + text });
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
         }
      }

      void Pump(Session& s)
      {
         std::deque<std::string> lines;
         {
            std::lock_guard<std::mutex> lock(s.mutex);
            lines.swap(s.lines);
         }
         for (const std::string& l : lines)
            HandleLine(s, l);
         if (s.child != nullptr && s.readerDone.load())
         {
            // The process ended on its own (crash, logged out, killed outside).
            const bool wasBusy = s.busy;
            s.StopProcess();
            if (wasBusy)
               s.log.push_back({ Entry::Error, T("Claude Code stopped unexpectedly. Send again to continue.") });
         }
      }

      void DrawEntry(const Entry& e)
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
            return; // tool lines sit tight together
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
         ImGui::Spacing();
      }
   }

   bool AssistantAvailable()
   {
      return !ClaudePath().empty();
   }

   void DrawAssistantPanelDocked(const char* id, const ImVec2& size)
   {
      Session& s = S();
      Pump(s);

      PushDockedPanelStyle(/*isChild=*/true);
      ImGui::BeginChild(id, size, ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysUseWindowPadding);
      PopDockedPanelStyle();
      {
         const ImVec2 wp = ImGui::GetWindowPos();
         const ImVec2 ws = ImGui::GetWindowSize();
         ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 0.5f, wp.y), ImVec2(wp.x + 0.5f, wp.y + ws.y), PanelSeamColor(), 1.0f);
      }

      ImGui::TextUnformatted(T("Assistant"));
      ImGui::SameLine();
      const float buttonsW = ImGui::CalcTextSize(T("New chat")).x + ImGui::CalcTextSize(T("Stop")).x +
                             ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
      ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsW));
      ImGui::BeginDisabled(!s.busy);
      if (ImGui::SmallButton(L("Stop")))
      {
         s.StopProcess();
         s.log.push_back({ Entry::Info, T("Stopped. Changes made so far stay; undo removes them.") });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::SmallButton(L("New chat")))
      {
         s.StopProcess();
         s.log.clear();
         s.sessionId.clear();
         s.costUsd = 0.0;
         s.focusInput = true;
      }
      ImGui::Separator();

      const bool listening = RemoteControl::Listening();
      const float inputH = ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
      const float footerH = inputH + ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
      ImGui::BeginChild("##assistant_log", ImVec2(0.0f, -footerH));
      if (!listening)
         DrawEntry({ Entry::Error, std::string(T("This Infinite could not open its control port ")) +
                                      std::to_string(RemoteControl::Port()) +
                                      T(" - another Infinite is probably using it. Close the other one and restart this one.") });
      else if (s.log.empty())
         DrawEntry({ Entry::Info, T("Ask for something to build or change, e.g. \"add a slowly rotating star "
                                    "and make its size follow the bass\". Changes appear live; undo works as usual. "
                                    "Uses your own Claude Code login.") });
      for (const Entry& e : s.log)
         DrawEntry(e);
      if (s.busy)
      {
         ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
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

      ImGui::BeginDisabled(!listening || s.busy);
      if (s.focusInput && !s.busy)
      {
         ImGui::SetKeyboardFocusHere();
         s.focusInput = false;
      }
      const bool enter = ImGui::InputTextMultiline("##assistant_input", s.input, sizeof(s.input), ImVec2(-1.0f, inputH),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
      const bool click = ImGui::Button(L("Send"));
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
      std::string text = s.input;
      while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
         text.pop_back();
      if ((enter || click) && !text.empty() && !s.busy && listening)
      {
         Send(s, text);
         s.input[0] = '\0';
      }

      ImGui::EndChild();
   }
#else
   bool AssistantAvailable() { return false; }
   void DrawAssistantPanelDocked(const char*, const ImVec2&) {}
#endif
}
