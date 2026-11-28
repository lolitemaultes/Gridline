// Gridline - the Track Receipt window: what each marker area of the arrangement uses of each
// original, counted as if the area were a song of its own (see TrackReceiptUiKit.h).
#include "App.h"
#include "TrackReceiptUiKit.h"
#include "Widgets.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

namespace {

std::string areaTitle(const Area& ar, size_t k) {
  if (!ar.name.empty()) return ar.name;
  if (ar.beforeFirst) return i18n::tr("Before the first marker");
  char b[64];
  std::snprintf(b, sizeof b, i18n::tr("Area %d"), static_cast<int>(k + 1));
  return b;
}

std::string areaPlace(const Area& ar) {
  char b[96];
  if (ar.bar1 > ar.bar0) std::snprintf(b, sizeof b, i18n::tr("bars %d-%d"), ar.bar0, ar.bar1);
  else std::snprintf(b, sizeof b, i18n::tr("bar %d"), ar.bar0);
  return std::string(b) + "  ·  " + timeText(ar.a) + " - " + timeText(ar.b);
}

// Text right-aligned so that it ends at `right` (window x).
void rightText(float right, ImU32 col, const char* t) {
  ImGui::SameLine(right - ImGui::CalcTextSize(t).x);
  ImGui::TextColored(theme::vec(col), "%s", t);
}

}  // namespace

void areasSection(App& a, const Report& r) {
  if (r.areas.empty()) return;
  const auto& P = pal();
  const float x0 = ImGui::GetCursorPosX(), w = kW - 16;
  // Columns: the song, the seconds of the original (right-aligned), the tier, the time it sounds.
  const float cSecs = x0 + w - 250, cTier = x0 + w - 230, cPlay = x0 + w;
  if (S().demoScrollAreas > 0) --S().demoScrollAreas, ImGui::SetScrollHereY(0.0f);  // (screenshots)
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(i18n::tr("By marker area"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(x0 + w);
  ImGui::PushFont(nullptr, theme::kFontLabel);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Each area from one marker to the next, counted as if it were a song of its own: a sample used in two "
                              "areas counts in each."));
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  for (size_t k = 0; k < r.areas.size(); ++k) {
    const AreaRow& ar = r.areas[k];
    ImGui::PushID(static_cast<int>(k));
    ImGui::Dummy(ImVec2(w, theme::kS2));
    // The area: its name, where it is.
    const std::string place = areaPlace(ar.area);
    ImGui::PushFont(nullptr, theme::kFontLabel);
    const float placeW = ImGui::CalcTextSize(place.c_str()).x;
    ImGui::PopFont();
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetTextLineHeight();
      dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 3, p.y + h - 2), P.accent, 1.5f);
      ImGui::SetCursorPosX(x0 + theme::kS3);
      const float lineY = ImGui::GetCursorPosY();
      ImGui::PushFont(a.fontBold, 0.0f);
      ImGui::TextUnformatted(theme::fitTextMiddle(areaTitle(ar.area, k).c_str(), w - theme::kS3 - placeW - theme::kS4));
      ImGui::PopFont();
      ImGui::PushFont(nullptr, theme::kFontLabel);
      // (on the name's baseline)
      ImGui::SameLine(x0 + w - placeW);
      ImGui::SetCursorPosY(lineY + h - ImGui::GetTextLineHeight() - 1.0f);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", place.c_str());
      ImGui::PopFont();
    }
    if (ar.songs.empty()) {
      ImGui::SetCursorPosX(x0 + theme::kS3);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No licensed sample is used here."));
      ImGui::PopID();
      continue;
    }
    // Column heads.
    ImGui::PushFont(nullptr, theme::kFontLabel);
    ImGui::SetCursorPosX(x0 + theme::kS3);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Original song"));
    rightText(cSecs, P.textFaint, i18n::tr("Seconds used"));
    ImGui::SameLine(cTier);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Tier"));
    rightText(cPlay, P.textFaint, i18n::tr("Sounds for"));
    ImGui::PopFont();
    for (const AreaSong& as : ar.songs) {
      const std::string title = as.artist.empty() ? as.title : as.artist + " - " + as.title;
      ImGui::SetCursorPosX(x0 + theme::kS3);
      const float rowY = ImGui::GetCursorPosY();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(theme::fitTextMiddle(title.c_str(), cSecs - x0 - theme::kS3 - 90));
      char secs[32];
      std::snprintf(secs, sizeof secs, "%s s", secondsText(as.seconds).c_str());
      rightText(cSecs, as.over ? P.danger : P.text, secs);
      ImGui::SameLine(cTier);
      ImGui::SetCursorPosY(rowY + (ImGui::GetFrameHeight() - (ImGui::GetTextLineHeight() + 4.0f)) * 0.5f);  // the chip centred on the row
      std::string tier;
      if (as.over) {
        tier = i18n::tr("Over 60 s");
      } else {
        char b[48];
        std::snprintf(b, sizeof b, i18n::tr("Up to %d s"), as.tier);
        tier = b;
      }
      chip(tier, as.over ? P.danger : theme::withAlpha(P.accent, 0.3f), as.over ? P.paper : P.text);
      const std::string play = as.playSeconds > 0 ? timeText(as.playSeconds) : std::string("-");
      rightText(cPlay, P.textDim, play.c_str());
      // Its files, each with the seconds of the file used here.
      ImGui::PushFont(a.fontMono, theme::kFontLabel);
      for (const AreaFile& f : as.files) {
        ImGui::SetCursorPosX(x0 + theme::kS3 + theme::kS4);
        ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitTextMiddle(f.name.c_str(), cSecs - x0 - theme::kS3 - theme::kS4 - 90));
        if (ImGui::IsItemHovered()) ui::Tooltip(f.path.c_str());
        std::snprintf(secs, sizeof secs, "%s s", secondsText(f.seconds).c_str());
        rightText(cSecs, P.textFaint, secs);
        const std::string fp = f.playSeconds > 0 ? timeText(f.playSeconds) : std::string("-");
        rightText(cPlay, P.textFaint, fp.c_str());
      }
      ImGui::PopFont();
      if (as.rough || as.estimated) {
        ImGui::PushFont(nullptr, theme::kFontLabel);
        ImGui::SetCursorPosX(x0 + theme::kS3 + theme::kS4);
        ImGui::PushTextWrapPos(x0 + w);
        std::string note;
        if (as.rough) note = i18n::tr("Some of it may play in more than one area: counted in each.");
        if (as.estimated) note += std::string(note.empty() ? "" : " ") + i18n::tr("Not measured: the whole file is counted.");
        ImGui::TextColored(theme::vec(P.danger), "%s", note.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
      }
    }
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
}

}  // namespace gl::receiptui
