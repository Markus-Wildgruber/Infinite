// TintedText: a wrapped paragraph in a text role other than the body colour (a dimmed info line, an error, the
// accent a chat uses for the user's own words). ImGui::TextColored does not wrap; this does, so a surface never
// pushes ImGuiCol_Text itself. Pass a role colour (tok::*, a style colour), never a literal.
#pragma once
#include "imgui.h"

namespace TintedText
{
   inline void Wrapped(const ImVec4& col, const char* text)
   {
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      ImGui::TextWrapped("%s", text);
      ImGui::PopStyleColor();
   }

   // The disabled-text role (ImGuiCol_TextDisabled), wrapped.
   inline void WrappedDim(const char* text)
   {
      Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text);
   }
}
