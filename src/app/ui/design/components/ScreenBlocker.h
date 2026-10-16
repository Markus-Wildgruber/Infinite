// ScreenBlocker: the dim + click-catcher laid over the whole window while a job drives the graph synchronously
// from the main loop (offline render, render to track / file, note capture). Editing underneath would edit what is
// being rendered, so one full-screen window soaks up every click; the progress window is drawn after it, on top.
// The scrim is the same 35% black the offline-render blockers use.
#pragma once
#include <string>
#include "imgui.h"
#include "app/ui/design/TokenColors.h"

namespace ScreenBlocker
{
   // `id` is the window name ("##RenderToFileBlocker"); the catcher takes the same id with "Catch" appended.
   inline void Draw(const char* id)
   {
      const ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
      ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, tok::V4(tok::palf::v_0_0_0_350));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::Begin(id, nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::InvisibleButton((std::string(id) + "Catch").c_str(), io.DisplaySize);
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();
   }
}
