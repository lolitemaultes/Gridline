// Gridline - the Track Receipt window: the uses inside a song card, the parts typed by hand
// for uses Gridline cannot measure, and the uses not counted (see TrackReceiptUiKit.h).
#include "App.h"
#include "TrackReceiptUiKit.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

// The parts of one timeline, each with where it is used.
void timelineUses(const Timeline& t, float x0) {
  const auto& P = pal();
  for (const RangeFrom& rf : t.parts) {
    ImGui::TextColored(theme::vec(P.accent), "%s - %s", timeText(rf.r.a).c_str(), timeText(rf.r.b).c_str());
    ImGui::SameLine(x0 + 130);
    ImGui::TextColored(theme::vec(P.textDim), "%s s", secondsText(rf.r.b - rf.r.a).c_str());
    ImGui::SameLine(x0 + 190);
    std::string from;
    for (const auto& fr : rf.from) from += (from.empty() ? "" : "; ") + fr;
    if (rf.estimated) from += std::string("  ") + i18n::tr("(not measured: whole file, type the parts below)");
    ImGui::PushTextWrapPos(x0 + kW - 16);
    ImGui::TextColored(theme::vec(rf.estimated ? P.danger : P.textFaint), "%s", from.c_str());
    ImGui::PopTextWrapPos();
  }
}

// Parts typed by the user for uses Gridline cannot measure.
void typedParts(App& a, const SongRow& sr, float x0) {
  State& s = S();
  const auto& P = pal();
  for (const std::string& path : sr.files) {
    const FileRow* row = nullptr;
    for (const FileRow& fr : s.report.files)
      if (fr.path == path) row = &fr;
    if (!row || !row->estimated) continue;
    ImGui::PushID(path.c_str());
    FileFields& ff = s.saved.files[path];
    ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitTextMiddle(row->name.c_str(), kW - 16));
    label(i18n::tr("Parts used"), x0 + 110);
    Ranges tmp;
    const bool ok = ff.ranges.empty() || parseRanges(ff.ranges, tmp);
    if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.danger));
    if (field("##ranges", ff.ranges, kW - 16 - 110, i18n::tr("0:01.0-0:03.0, 0:10-0:12 (empty: the whole file)"))) commit(a);
    if (!ok) ImGui::PopStyleColor();
    ImGui::PopID();
  }
}

// Not counted (muted or silent).
void notCounted(App& a, const Report& r) {
  const auto& P = pal();
  bool anyMuted = false;
  for (const FileRow& f : r.files) anyMuted = anyMuted || !f.muted.empty();
  if (!anyMuted) return;
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Not counted (muted or silent)"));
  ImGui::PopFont();
  for (const FileRow& f : r.files)
    for (const Use& u : f.muted) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW - 16);
      ImGui::TextColored(theme::vec(P.textFaint), "%s: %s, %s", f.name.c_str(), u.where.c_str(), u.note.c_str());
      ImGui::PopTextWrapPos();
    }
  ImGui::Spacing();
  ImGui::Separator();
}

}  // namespace gl::receiptui
