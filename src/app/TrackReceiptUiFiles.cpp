// Gridline - the Track Receipt window: the files page (every audio file the project uses,
// grouped by song; tick the licensed samples, link a file to a song), see TrackReceiptUiKit.h.
#include <map>
#include <set>
#include "App.h"
#include "DialogLayout.h"
#include "TrackReceiptUiKit.h"
#include "Widgets.h"

namespace gl::receiptui {

using theme::pal;
using namespace gl::receipt;

namespace {

// Why a word of a file name was left out of its song's name (SongIdentity::dropped).
const char* whyText(const std::string& why) {
  if (why == "tempo") return i18n::tr("tempo");
  if (why == "key") return i18n::tr("key");
  if (why == "stem") return i18n::tr("stem or section");
  if (why == "version") return i18n::tr("take or version");
  if (why == "part") return i18n::tr("part number");
  if (why == "note") return i18n::tr("store note");
  if (why == "code") return i18n::tr("pack code");
  return why.c_str();
}

// The tooltip of a group: which song name the files share and which words of each were left out.
std::string groupTip(const std::vector<const FileRow*>& files) {
  std::string t;
  for (const FileRow* f : files) {
    if (!t.empty()) t += "\n\n";
    t += f->name;
    if (f->ownSong) {
      t += "\n" + std::string(i18n::tr("Kept on its own by you"));
      continue;
    }
    if (f->identity.key.empty()) continue;
    char b[256];
    std::snprintf(b, sizeof b, i18n::tr("Song name: %s"), f->identity.title.c_str());
    t += std::string("\n") + b;
    if (f->identity.dropped.empty()) continue;
    std::string left;
    for (const auto& [word, why] : f->identity.dropped) left += (left.empty() ? "" : ", ") + word + " (" + whyText(why) + ")";
    t += "\n" + std::string(i18n::tr("Left out:")) + " " + left;
  }
  return t;
}

}  // namespace

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
  const float btnW = 80, btnX = kW - 96;
  // K-442: shorter on a short screen (200% on a laptop), so Close and Calculate stay on it.
  ImGui::BeginChild("##rpick", ImVec2(kW, ui::DialogListHeight(360)), ImGuiChildFlags_Borders);
  if (s.pick.files.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("This project uses no audio files."));
  std::string linkFile;
  for (const std::string& k : keys) {
    ImGui::PushID(k.c_str());
    const std::vector<const FileRow*>& members = groups[k];
    // How the group came about: by the song name in the files' names, or merged by the user.
    std::set<std::string> byName;
    bool merged = false;
    for (const FileRow* f : members) {
      if (!f->ownSong && !f->identity.key.empty() && f->identity.key == f->autoSong) byName.insert(f->name);
      merged = merged || f->autoSong != k;
    }
    bool check = false;
    for (const SongRow& sr : s.pick.songs) check = check || (sr.key == k && sr.checkName);
    const char* checkText = i18n::tr("check artist and title");
    const float checkW = check ? ImGui::CalcTextSize(checkText).x + theme::kS2 : 0.0f;
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(theme::fitTextMiddle(songDisplay(k, s.pick).c_str(), btnX - x0 - theme::kS4 - checkW));
    ImGui::PopFont();
    if (check) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.danger), "%s", checkText);
    }
    ImGui::SameLine(btnX);
    if (ui::TextButton(i18n::tr("Group..."), false, btnW, i18n::tr("Merge this song with another one, or keep its files apart")))
      ImGui::OpenPopup("##group");
    if (ImGui::BeginPopup("##group")) {
      if (ImGui::BeginMenu(i18n::tr("Count with"))) {
        for (const std::string& song : allSongs) {
          if (song == k) continue;
          if (ImGui::MenuItem(songDisplay(song, s.pick).c_str())) {
            mergeSong(s.saved, k, song);
            commit(a);
          }
        }
        ImGui::EndMenu();
      }
      bool mergedIn = false;
      for (const auto& [from, into] : s.saved.merges) mergedIn = mergedIn || into == k;
      if (ImGui::MenuItem(i18n::tr("Undo merges into this song"), nullptr, false, mergedIn)) {
        unmergeInto(s.saved, k);
        commit(a);
      }
      if (ImGui::MenuItem(i18n::tr("Keep these files apart"), nullptr, false, members.size() >= 2)) {
        std::vector<std::string> paths;
        for (const FileRow* f : members) paths.push_back(f->path);
        keepApart(s.saved, k, paths);
        commit(a);
      }
      ImGui::EndPopup();
    }
    if (members.size() >= 2 && (byName.size() >= 2 || merged)) {
      char b[160];
      if (merged) std::snprintf(b, sizeof b, i18n::tr("%d files, merged by you: counted as one song"), static_cast<int>(members.size()));
      else std::snprintf(b, sizeof b, i18n::tr("%d files with the same song name: counted as one song"), static_cast<int>(members.size()));
      ImGui::PushFont(nullptr, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
      ImGui::PopFont();
      if (ImGui::IsItemHovered()) ui::Tooltip(groupTip(members).c_str());
    }
    for (const FileRow* f : members) {
      ImGui::PushID(f->path.c_str());
      bool on = f->ticked;
      if (ImGui::Checkbox("##tick", &on)) {
        s.saved.files[f->path].ticked = on ? 1 : 0;
        commit(a);
      }
      ImGui::SameLine();
      const float nameW = kW - 260;
      // (ends 8 px before the places column)
      const float fitW = nameW + 36 - ImGui::GetCursorPosX() - theme::kS2;
      ImGui::TextColored(theme::vec(f->ticked ? P.text : P.textDim), "%s", theme::fitTextMiddle(f->name.c_str(), fitW));
      if (ImGui::IsItemHovered()) {
        const std::string tip = f->identity.key.empty() ? f->path : f->path + "\n\n" + groupTip({f});
        ui::Tooltip(tip.c_str());
      }
      ImGui::SameLine(nameW + 36);
      int nUses = 0;
      for (const FileRef& r : s.refs) nUses += r.path == f->path ? 1 : 0;
      std::string note = f->derived ? std::string(i18n::tr("derived")) : !f->exists ? std::string(i18n::tr("missing")) : "";
      char b[64];
      std::snprintf(b, sizeof b, nUses == 1 ? i18n::tr("%d place") : i18n::tr("%d places"), nUses);
      ImGui::TextColored(theme::vec(!f->exists ? P.danger : P.textFaint), "%s%s%s", b, note.empty() ? "" : ", ", note.c_str());
      ImGui::SameLine(btnX);
      if (ui::TextButton(i18n::tr("Song..."), false, btnW, i18n::tr("Link this file to an original song"))) {
        linkFile = f->path;
        ImGui::OpenPopup("##link");
      }
      if (ImGui::BeginPopup("##link")) {
        if (ImGui::MenuItem(i18n::tr("Automatic (from the file name)"))) {
          s.saved.files[f->path].song.clear();
          commit(a);
        }
        if (ImGui::MenuItem(i18n::tr("On its own"), nullptr, f->ownSong)) {
          s.saved.files[f->path].song = ownSongKey(f->path);
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
