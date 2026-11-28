// Gridline - the Track Receipt window: the results page (the summary and a card per original
// song), see TrackReceiptUiKit.h.
#include "App.h"
#include "DialogLayout.h"
#include "TrackReceiptUiKit.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

namespace {

void songCard(App& a, SongRow& sr, int idx) {
  State& s = S();
  const auto& P = pal();
  ImGui::PushID(idx);
  ImGui::PushID(sr.key.c_str());
  const float x0 = ImGui::GetCursorPosX();
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(sr.title.c_str());
  ImGui::PopFont();
  {
    char secs[32];
    std::snprintf(secs, sizeof secs, "%s s", secondsText(sr.seconds).c_str());
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    const float w = ImGui::CalcTextSize(secs).x;
    ImGui::SameLine(x0 + kW - w - 16);
    ImGui::TextColored(theme::vec(sr.over ? P.danger : P.text), "%s", secs);
    ImGui::PopFont();
  }
  ImGui::TextColored(theme::vec(P.textDim), "%s", sr.artist.empty() ? i18n::tr("Artist unknown") : sr.artist.c_str());
  {
    const std::string t = tierText(sr);
    const float w = ImGui::CalcTextSize(t.c_str()).x + 16;
    ImGui::SameLine(x0 + kW - w - 16);
    chip(t, sr.over ? P.danger : sr.seconds <= 0 ? P.well : theme::withAlpha(P.accent, 0.3f), sr.over ? P.paper : P.text);
  }
  if (sr.grouped || sr.merged) {
    char b[160];
    std::snprintf(b, sizeof b, sr.merged ? i18n::tr("%d files counted as one song: merged by you") : i18n::tr("%d files counted as one song: the same song name"),
                  static_cast<int>(sr.files.size()));
    ImGui::PushFont(nullptr, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textDim), "%s", b);
    ImGui::PopFont();
  }
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ImGui::PushTextWrapPos(x0 + kW - 140);
  if (sr.grouped || sr.merged) {
    // Every file counted as this song, each on its own line.
    constexpr size_t kShown = 6;
    for (size_t i = 0; i < sr.files.size() && i < kShown; ++i) ImGui::TextColored(theme::vec(P.textFaint), "%s", baseName(sr.files[i]).c_str());
    if (sr.files.size() > kShown) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("and %d more"), static_cast<int>(sr.files.size() - kShown));
      ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
    }
  } else {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", sr.sourceName.c_str());
  }
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  if (sr.checkName) ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("Check artist and title: guessed from the file name."));
  SongFields& f = s.saved.songs[sr.key];
  const float lw = 70, half = (kW - 16) / 2, fw = half - lw - theme::kS2;
  label(i18n::tr("Artist"), x0 + lw);
  if (field("##artist", f.artist, fw, sr.artist.c_str())) commit(a);
  ImGui::SameLine(x0 + half + theme::kS2);
  label(i18n::tr("Title"), x0 + half + theme::kS2 + lw);
  if (field("##title", f.title, fw, sr.title.c_str())) commit(a);
  for (const Timeline& t : sr.timelines) {
    ImGui::Spacing();
    char head[160];
    std::snprintf(head, sizeof head, "%s  %s s %s",
                  t.part > 0 ? (std::string(i18n::tr("Part")) + " " + std::to_string(t.part)).c_str()
                  : t.part < 0 ? i18n::tr("Derived files, part of the original unknown")
                               : i18n::tr("Original"),
                  secondsText(t.seconds).c_str(), i18n::tr("used"));
    ImGui::PushFont(nullptr, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textDim), "%s", head);
    ImGui::PopFont();
    timelineBar(t, kW - 16);
    timelineUses(t, x0);
  }
  ImGui::Spacing();
  // Fields from the license.
  const float cw = (kW - 16 - 2 * theme::kS4) / 3.0f;
  {
    static const char* cats[] = {"", "A", "B", "C"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Category"));
    ImGui::SameLine(x0 + 70);
    ImGui::SetNextItemWidth(cw - 70);
    if (ImGui::BeginCombo("##cat", f.category.empty() ? "-" : f.category.c_str())) {
      for (const char* c : cats)
        if (ImGui::Selectable(*c ? c : "-", f.category == c)) {
          f.category = c;
          commit(a);
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine(x0 + cw + theme::kS4);
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Share %"));
    ImGui::SameLine(x0 + cw + theme::kS4 + 70);
    char autoHint[48];
    if (sr.autoShare >= 0) std::snprintf(autoHint, sizeof autoHint, i18n::tr("%g (auto)"), sr.autoShare);
    else std::snprintf(autoHint, sizeof autoHint, "%s", i18n::tr("auto"));
    if (field("##share", f.share, cw - 70, autoHint)) commit(a);
    ImGui::SameLine(x0 + 2 * (cw + theme::kS4));
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Fee"));
    ImGui::SameLine(x0 + 2 * (cw + theme::kS4) + 70);
    if (field("##fee", f.fee, cw - 70)) commit(a);
  }
  {
    char note[160] = "";
    if (sr.over || sr.seconds <= 0) {
    } else if (f.category.empty()) {
      std::snprintf(note, sizeof note, "%s", i18n::tr("Choose the category on the license and the share is calculated from it and the length."));
    } else if (f.share.empty()) {
      std::snprintf(note, sizeof note, i18n::tr("Calculated: category %s, up to %d s. Type a share if your license says otherwise."), f.category.c_str(), sr.tier);
    } else if (sr.autoShare >= 0 && sr.share != sr.autoShare) {
      std::snprintf(note, sizeof note, i18n::tr("Your typed share is used. Calculated: %g%%. Clear the field to use it."), sr.autoShare);
    }
    if (*note) {
      ImGui::PushFont(nullptr, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", note);
      ImGui::PopFont();
    }
  }
  label(i18n::tr("Rights holders"), x0 + 110);
  if (field("##holders", f.holders, kW - 16 - 110, i18n::tr("Master and publishing owners, from the license"))) commit(a);
  typedParts(a, sr, x0);
  for (const auto& w : sr.warnings) {
    ImGui::PushTextWrapPos(x0 + kW - 16);
    ImGui::TextColored(theme::vec(P.danger), "%s", w.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PopID();
  ImGui::PopID();
}

}  // namespace

void resultsPage(App& a) {
  State& s = S();
  const auto& P = pal();
  Report& r = s.report;
  {
    double secs = 0;
    int songs = 0, over = 0;
    for (const SongRow& sr : r.songs)
      if (sr.seconds > 0) ++songs, secs += sr.seconds, over += sr.over ? 1 : 0;
    char b[200];
    std::snprintf(b, sizeof b, i18n::tr("%d original songs, %s s used in all"), songs, secondsText(secs).c_str());
    ImGui::TextUnformatted(b);
    if (over) {
      ImGui::SameLine(0, theme::kS2);
      std::snprintf(b, sizeof b, i18n::tr("%d over 60 s"), over);
      ImGui::TextColored(theme::vec(P.danger), "%s", b);
    }
    ImGui::SameLine(0, theme::kS2);
    if (r.sharesComplete) std::snprintf(b, sizeof b, i18n::tr("sample shares %s%%, yours %s%%"), secondsText(r.totalShare).c_str(),
                                         secondsText(std::max(0.0, 100 - r.totalShare)).c_str());
    else std::snprintf(b, sizeof b, "%s", i18n::tr("shares to fill in: choose each song's category"));
    ImGui::TextColored(theme::vec(P.textDim), "%s", b);
  }
  for (const auto& w : r.warnings) ImGui::TextColored(theme::vec(P.danger), "%s", w.c_str());
  // K-442: the closing note's two lines stay below it, the footer on screen.
  ImGui::BeginChild("##rres", ImVec2(kW + 16, ui::DialogListHeight(430, 2 * ImGui::GetTextLineHeightWithSpacing())), ImGuiChildFlags_Borders);
  if (r.songs.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No ticked files: go back and tick the licensed samples."));
  int idx = 0;
  for (size_t i = 0; i < r.songs.size(); ++i) {  // (a copy: an edit in the card rebuilds the report)
    SongRow sr = r.songs[i];
    songCard(a, sr, idx++);
  }
  areasSection(a, r);
  notCounted(a, r);
  releaseSection(a);
  ImGui::EndChild();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("A helper, not legal advice. Check the figures against your license on Tracklib."));
  ImGui::PopTextWrapPos();
}

}  // namespace gl::receiptui
