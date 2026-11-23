// Gridline - the Track Receipt window: the files page (every audio file the project uses,
// grouped by song; tick the licensed samples, link a file to a song), see TrackReceiptUiKit.h.
#include <map>
#include <set>
#include "App.h"
#include "TrackReceiptUiKit.h"
#include "Widgets.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

void filesPage(App& a) {
  State& s = S();
  const auto& P = pal();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Every audio file this project uses. Tick the files that are licensed samples: files named like "
                              "Tracklib downloads are ticked for you. Calculate then plays the project through and measures "
                              "which parts of each original are used."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::SetNextItemWidth(kW - ui::ButtonRowWidth(2, 90) - theme::kS2);
  ImGui::InputTextWithHint("##rsearch", i18n::tr("Search files"), s.search, sizeof s.search);
  const std::string q = lowerText(s.search);
  auto visible = [&](const FileRow& f) { return q.empty() || lowerText(f.name).find(q) != std::string::npos || lowerText(f.song).find(q) != std::string::npos; };
  ImGui::SameLine(0, theme::kS2);
  int setAll = -1;
  if (ui::TextButton(i18n::tr("Tick all"), false, 90)) setAll = 1;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Tick none"), false, 90)) setAll = 0;
  if (setAll >= 0) {
    for (const FileRow& f : s.pick.files)
      if (visible(f)) s.saved.files[f.path].ticked = setAll;
    commit(a);
  }
  // Groups by song.
  std::map<std::string, std::vector<const FileRow*>> groups;
  for (const FileRow& f : s.pick.files)
    if (visible(f)) groups[f.song].push_back(&f);
  std::vector<std::string> keys;
  for (const auto& [k, v] : groups) keys.push_back(k);
  std::stable_sort(keys.begin(), keys.end(), [&](const std::string& x, const std::string& y) {
    auto ticked = [&](const std::string& k) {
      for (const FileRow* f : groups[k])
        if (f->ticked) return true;
      return false;
    };
    return ticked(x) > ticked(y);
  });
  std::set<std::string> allSongs;
  for (const FileRow& f : s.pick.files) allSongs.insert(f.song);
  ImGui::BeginChild("##rpick", ImVec2(kW, 360), ImGuiChildFlags_Borders);
  if (s.pick.files.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("This project uses no audio files."));
  std::string linkFile;
  for (const std::string& k : keys) {
    ImGui::PushID(k.c_str());
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::TextUnformatted(songDisplay(k, s.pick).c_str());
    ImGui::PopFont();
    for (const SongRow& sr : s.pick.songs)
      if (sr.key == k && sr.checkName) {
        ImGui::SameLine(0, theme::kS2);
        ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("check artist and title"));
      }
    for (const FileRow* f : groups[k]) {
      ImGui::PushID(f->path.c_str());
      bool on = f->ticked;
      if (ImGui::Checkbox("##tick", &on)) {
        s.saved.files[f->path].ticked = on ? 1 : 0;
        commit(a);
      }
      ImGui::SameLine();
      const float nameW = kW - 260;
      ImGui::TextColored(theme::vec(f->ticked ? P.text : P.textDim), "%s", theme::fitTextMiddle(f->name.c_str(), nameW));
      if (ImGui::IsItemHovered()) ui::Tooltip(f->path.c_str());
      ImGui::SameLine(nameW + 36);
      int nUses = 0;
      for (const FileRef& r : s.refs) nUses += r.path == f->path ? 1 : 0;
      std::string note = f->derived ? std::string(i18n::tr("derived")) : !f->exists ? std::string(i18n::tr("missing")) : "";
      char b[64];
      std::snprintf(b, sizeof b, nUses == 1 ? i18n::tr("%d place") : i18n::tr("%d places"), nUses);
      ImGui::TextColored(theme::vec(!f->exists ? P.danger : P.textFaint), "%s%s%s", b, note.empty() ? "" : ", ", note.c_str());
      ImGui::SameLine(kW - 96);
      if (ui::TextButton(i18n::tr("Song..."), false, 80, i18n::tr("Link this file to an original song"))) {
        linkFile = f->path;
        ImGui::OpenPopup("##link");
      }
      if (ImGui::BeginPopup("##link")) {
        if (ImGui::MenuItem(i18n::tr("Automatic (from the file name)"))) {
          s.saved.files[f->path].song.clear();
          commit(a);
        }
        ImGui::Separator();
        for (const std::string& song : allSongs)
          if (ImGui::MenuItem(songDisplay(song, s.pick).c_str(), nullptr, f->song == song)) {
            s.saved.files[f->path].song = song;
            commit(a);
          }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

}  // namespace gl::receiptui
