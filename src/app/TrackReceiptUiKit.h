// Gridline - the Track Receipt window's shared parts (see TrackReceiptUi.h). The shell
// (TrackReceiptUi.cpp) owns the window, the steps, the measuring job and the footer; each page
// has its own file:
//   TrackReceiptUiFiles.cpp    the files page (tick the licensed samples, link files to songs)
//   TrackReceiptUiResults.cpp  the results summary and the song cards
//   TrackReceiptUiUses.cpp     the uses inside a song card, parts typed by hand, uses not counted
//   TrackReceiptUiRelease.cpp  the release and the writers
// The pieces they share (the window state, a text field, labels, chips, the timeline bar) are here.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../core/I18n.h"
#include "../core/TrackReceipt.h"
#include "../engine/TrackReceiptPass.h"
#include "Theme.h"

namespace gl {

class App;

namespace receiptui {

constexpr float kW = 720.0f;  // content width

struct State {
  bool open = false, focus = false;
  int page = 0;  // 0 picker, 1 measuring, 2 results
  char search[128] = {};
  receipt::Measurement scanned, measured;
  receipt::Report pick, report;
  receipt::Saved saved;
  std::vector<receipt::FileRef> refs;
  std::string lastFile;
  double demoProgress = -1;  // screenshots: a frozen progress page
};
State& S();  // (TrackReceiptUi.cpp)

// The typed fields changed: saved in the project (one undo step) and the reports rebuilt.
void commit(App& a);

// ---- pages

void filesPage(App& a);                                          // TrackReceiptUiFiles.cpp
void resultsPage(App& a);                                        // TrackReceiptUiResults.cpp
void timelineUses(const receipt::Timeline& t, float x0);         // TrackReceiptUiUses.cpp
void typedParts(App& a, const receipt::SongRow& sr, float x0);   // TrackReceiptUiUses.cpp
void notCounted(App& a, const receipt::Report& r);               // TrackReceiptUiUses.cpp
void releaseSection(App& a);                                     // TrackReceiptUiRelease.cpp

// ---- the kit

namespace kit {

inline int resizeCb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(d->UserData);
    str->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = str->data();
  }
  return 0;
}

}  // namespace kit

// A text field bound to a std::string; true when an edit is finished (focus left / Enter).
inline bool field(const char* id, std::string& v, float w, const char* hint = "") {
  ImGui::SetNextItemWidth(w);
  if (v.capacity() < 64) v.reserve(64);
  ImGui::InputTextWithHint(id, hint, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, kit::resizeCb, &v);
  return ImGui::IsItemDeactivatedAfterEdit();
}

inline void label(const char* t, float w) {
  const auto& P = theme::pal();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", t);
  ImGui::SameLine(w);
}

// The used parts of one original on a bar (seconds 0..length).
inline void timelineBar(const receipt::Timeline& t, float w) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 10.0f;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.well, h * 0.5f);
  const double len = std::max(t.length, 0.001);
  for (const receipt::Range& r : t.used) {
    const float a = p.x + w * static_cast<float>(std::clamp(r.a / len, 0.0, 1.0));
    const float b = p.x + w * static_cast<float>(std::clamp(r.b / len, 0.0, 1.0));
    dl->AddRectFilled(ImVec2(a, p.y), ImVec2(std::max(a + 1.5f, b), p.y + h), P.accent);
  }
  ImGui::Dummy(ImVec2(w, h));
  ImGui::PushFont(nullptr, theme::kFontLabel);
  ImGui::TextColored(theme::vec(P.textFaint), "0:00");
  const std::string end = receipt::timeText(t.length);
  ImGui::SameLine(w - ImGui::CalcTextSize(end.c_str()).x + ImGui::GetStyle().WindowPadding.x * 0.0f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", end.c_str());
  ImGui::PopFont();
}

inline std::string tierText(const receipt::SongRow& s) {
  if (s.seconds <= 0) return i18n::tr("Not played");
  if (s.over) return i18n::tr("Over 60 s: not licensable");
  char b[64];
  std::snprintf(b, sizeof b, i18n::tr("Up to %d s"), s.tier);
  return b;
}

inline void chip(const std::string& t, ImU32 bg, ImU32 fg) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz(ts.x + 16, ts.y + 4);
  dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, sz.y * 0.5f);
  dl->AddText(ImVec2(p.x + 8, p.y + 2), fg, t.c_str());
  ImGui::Dummy(sz);
}

inline std::string songDisplay(const std::string& key, const receipt::Report& r) {
  for (const receipt::SongRow& s : r.songs)
    if (s.key == key) return s.artist.empty() ? s.title : s.artist + " - " + s.title;
  return key;
}

}  // namespace receiptui

}  // namespace gl
