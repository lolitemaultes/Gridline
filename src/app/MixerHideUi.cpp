// Gridline — hidden, narrow and folded mixer strips in the mixer views (GitHub #58). See MixerHideUi.h.
#include "MixerHideUi.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "../core/I18n.h"
#include "../core/MixerOps.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::mixhideui {

void trackItems(App& a, ObjectId id, const std::vector<ObjectId>& selIds) {
  const Song& s = a.doc.song();
  const int index = s.mixerIndex(id);
  if (index <= 0) return;  // the master always shows
  std::vector<ObjectId> ids;
  for (ObjectId x : selIds)
    if (s.mixerIndex(x) > 0) ids.push_back(x);
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids = {id};
  const bool many = ids.size() > 1;
  const bool narrow = s.mixer[static_cast<size_t>(index)].narrow;
  const bool hide = ImGui::MenuItem(many ? i18n::tr("Hide selected tracks") : i18n::tr("Hide track"));
  devMenuItem("mixmenu hide");
  ui::Hint("Take the strip out of the mixer views (it still plays and routes as before); the toolbar's hidden count brings it back");
  const bool slim = ImGui::MenuItem(i18n::tr("Narrow strip"), nullptr, narrow);
  devMenuItem("mixmenu narrow");
  ui::Hint("A slim strip with only the name, the meter, mute and solo");
  if (hide && mixhide::setHidden(a.doc, ids, true)) {  // the hidden tracks leave the selection
    for (ObjectId x : ids) a.mixerView.sel.erase(x);
    if (std::find(ids.begin(), ids.end(), a.selMixer) != ids.end()) a.selMixer = a.doc.song().masterId();
  } else if (slim) {
    mixhide::setNarrow(a.doc, ids, !narrow);
  }
}

void groupItem(App& a, const std::string& group) {
  const bool folded = mixhide::groupFolded(a.doc.song(), group);
  const bool picked = ImGui::MenuItem(folded ? i18n::tr("Expand group") : i18n::tr("Collapse group"), nullptr, false, !group.empty());
  devMenuItem("mixmenu fold");
  ui::Hint(folded ? "Show the group's tracks again" : "Show the whole group as one slim strip (its tracks keep playing)");
  if (picked) mixhide::setGroupFolded(a.doc, group, !folded);
}

void groupMenu(App& a, const std::string& group) {
  const Song& s = a.doc.song();
  ImGui::TextDisabled("%s", group.c_str());
  ImGui::Separator();
  const bool expand = ImGui::MenuItem(i18n::tr("Expand group"));
  devMenuItem("groupmenu expand");
  const bool muted = mixhide::groupMuted(s, group), soloed = mixhide::groupSoloed(s, group);
  const bool mute = ImGui::MenuItem(muted ? i18n::tr("Unmute group") : i18n::tr("Mute group"));
  const bool solo = ImGui::MenuItem(i18n::tr("Solo group"), nullptr, soloed);
  if (expand) {
    mixhide::setGroupFolded(a.doc, group, false);
    return;
  }
  if (mute) {
    mixhide::setGroupMute(a.doc, group, !muted);
    return;
  }
  if (solo) {
    mixhide::setGroupSolo(a.doc, group, !soloed);
    return;
  }
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Tracks in the group"));
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    if (s.mixer[i].group != group) continue;
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::MenuItem(mixerTrackName(s, i).c_str())) {  // selects it (the strip shows once the group is expanded)
      a.selMixer = s.mixer[i].id;
      a.mixerView.sel = {s.mixer[i].id};
    }
    ImGui::PopID();
  }
}

std::string hiddenLabel(const Song& s) {
  const int n = mixhide::hiddenCount(s);
  if (n <= 0) return {};
  return std::to_string(n) + " " + i18n::tr("hidden");
}

void hiddenMenu(App& a) {
  const Song& s = a.doc.song();
  const std::vector<ObjectId> tracks = mixhide::hiddenTracks(s);
  const std::vector<std::string> groups = mixhide::foldedGroups(s);
  if (!tracks.empty()) ImGui::TextDisabled("%s", i18n::tr("Hidden tracks (click to show)"));
  for (ObjectId id : tracks) {
    const int i = s.mixerIndex(id);
    if (i <= 0) continue;
    ImGui::PushID(i);
    const bool show = ImGui::MenuItem(mixerTrackName(s, static_cast<size_t>(i)).c_str());
    devMenuItem("hidden track " + std::to_string(i));
    ImGui::PopID();
    if (show) {
      mixhide::setHidden(a.doc, {id}, false);
      return;
    }
  }
  if (!groups.empty()) {
    if (!tracks.empty()) ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("Collapsed groups (click to expand)"));
  }
  for (const std::string& g : groups) {
    ImGui::PushID(g.c_str());
    const bool open = ImGui::MenuItem(g.c_str());
    ImGui::PopID();
    if (open) {
      mixhide::setGroupFolded(a.doc, g, false);
      return;
    }
  }
  ImGui::Separator();
  const bool all = ImGui::MenuItem(i18n::tr("Show all"));
  devMenuItem("hidden showall"), devNoteRect("mixshowall");
  if (all) mixhide::showAll(a.doc);
}

void upwardText(ImDrawList* dl, ImFont* f, float fs, ImVec2 o, ImU32 col, const std::string& text, float maxLen) {
  // Drawn flat (ellipsised to maxLen), then its vertices turned a quarter left about `o`. The clip
  // rectangle holds the flat text for the font's own culling and the turned text inside the view.
  const ImVec4 cr = dl->_ClipRectStack.back();
  dl->PushClipRect(ImVec2(std::max(cr.x, o.x), std::max(cr.y, o.y - maxLen)), ImVec2(o.x + maxLen, std::min(cr.w, o.y + fs)), false);
  const int v0 = dl->VtxBuffer.Size;
  theme::textEllipsis(dl, f, fs, o, maxLen, col, text.c_str());
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - o.x, dy = v.pos.y - o.y;
    v.pos = ImVec2(o.x + dy, o.y - dx);
  }
  dl->PopClipRect();
}

namespace {
std::vector<std::pair<size_t, float>> gCols, gColsLast;
int gDevView = -1;
}
int devSmoothView() { const int v = gDevView; gDevView = -1; return v; }
void beginColumns() { gColsLast.swap(gCols), gCols.clear(); }
void noteColumn(size_t index, float x0, float x1) { gCols.push_back({index, x1 - x0}); }

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string verb = args.empty() ? "state" : args[0];
  std::vector<ObjectId> ids;
  const size_t first = verb == "group" ? 2 : 1;
  for (size_t k = first; k < args.size(); ++k) {
    const long i = std::strtol(args[k].c_str(), nullptr, 10);
    if (i >= 0 && static_cast<size_t>(i) < a.doc.song().mixer.size()) ids.push_back(a.doc.song().mixer[static_cast<size_t>(i)].id);
  }
  if (verb == "hide" || verb == "show") mixhide::setHidden(a.doc, ids, verb == "hide");
  else if (verb == "narrow" || verb == "wide") mixhide::setNarrow(a.doc, ids, verb == "narrow");
  else if (verb == "group" && args.size() > 1) mixops::createGroup(a.doc, ids, args[1], false);
  else if ((verb == "fold" || verb == "unfold") && args.size() > 1) mixhide::setGroupFolded(a.doc, args[1], verb == "fold");
  else if (verb == "showall") mixhide::showAll(a.doc);
  else if (verb == "smoothview" && args.size() > 1) gDevView = std::atoi(args[1].c_str());
  if (verb != "state") return;
  const Song& s = a.doc.song();
  std::printf("mixhide frame %d: hidden %d undo %zu\n", frame, mixhide::hiddenCount(s), a.doc.history().position());
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    const MixerTrack& t = s.mixer[i];
    std::printf("  mhtrack %zu hidden %d narrow %d folded %d shown %d group [%s]\n", i, t.hidden ? 1 : 0, t.narrow ? 1 : 0, t.groupFolded ? 1 : 0,
                mixhide::trackHidden(s, i) ? 0 : 1, t.group.c_str());
  }
  const auto& cols = gCols.empty() ? gColsLast : gCols;
  std::printf("  cols");
  for (const auto& col : cols) std::printf(" %zu:%.0f", col.first, static_cast<double>(col.second));
  std::printf("\n");
  std::fflush(stdout);
}

}  // namespace gl::mixhideui
