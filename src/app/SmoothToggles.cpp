// Gridline — the Tracks layout's (Smooth's) check boxes and radio buttons, see SmoothToggles.h.
#include "SmoothToggles.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "Theme.h"

namespace gl::smoothtoggles {

ImU32 accent() { return theme::rgb(0x2F7EDB); }

void apply(ImGuiStyle& st, Saved& saved) {
  // Checkpoint 38: a ticked box is filled with the layout's blue and ticked in white, as every reference
  // window draws its check boxes (023, 100, 132); it was an orange tick on a faintly tinted field.
  saved.mark = st.Colors[ImGuiCol_CheckMark], saved.selected = st.Colors[ImGuiCol_CheckboxSelectedBg];
  saved.bar = st.Colors[ImGuiCol_PlotHistogram];
  st.Colors[ImGuiCol_CheckMark] = theme::vec(theme::contrastText(accent()));
  st.Colors[ImGuiCol_CheckboxSelectedBg] = theme::vec(accent());
  GImGuiGridlineCheckRounding = 4.0f;  // rounded squares (023, 100, 132), not the pill-round frames
  // A selected radio the same way: the blue disc with a white dot. (ImGui's own radio draws a CheckMark
  // dot on the field, which with the white mark was white on white in the light theme.)
  GImGuiGridlineRadioDot = 0.38f;
  // A progress bar fills with the same blue (it was the theme's text colour: near black on the light theme).
  st.Colors[ImGuiCol_PlotHistogram] = theme::vec(accent());
}

void restore(ImGuiStyle& st, const Saved& saved) {
  st.Colors[ImGuiCol_CheckMark] = saved.mark, st.Colors[ImGuiCol_CheckboxSelectedBg] = saved.selected;
  st.Colors[ImGuiCol_PlotHistogram] = saved.bar;
  GImGuiGridlineCheckRounding = -1.0f;
  GImGuiGridlineRadioDot = -1.0f;
}

}  // namespace gl::smoothtoggles
