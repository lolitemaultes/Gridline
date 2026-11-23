// Gridline — "seltmann" pack: the Seltmann tab of Smooth's lower pane (0.7.8 Ribbon). The same
// editor as the instrument window, at the height the pane gives it (Compact, Standard or Full by
// the room). It shows the Seltmann channel being edited (its channel settings stay out of the way
// while the tab shows it), else the one shown last, else the project's first; a row of chips picks
// another when the project has several.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/HostedBodies.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "seltmann_tracker.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann {

bool drawEditor(App& a, const PluginTarget& t, float width);

namespace {
ObjectId gShown = kNoId;  // the channel the tab showed last
}  // namespace

void drawPane(App& a) {
  const Song& s = a.doc.song();
  std::vector<const Channel*> chans;
  for (const Channel& c : s.channels)
    if (c.plugin.type == kType) chans.push_back(&c);
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  if (chans.empty()) {
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y - 40, 60.0f)), i18n::tr("No Seltmann yet"),
                   i18n::tr("Add one to chop a loop, a break or a whole song: it shows here"));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + av.x * 0.5f - 80, p0.y + std::max(av.y - 40, 60.0f)));
    if (ImGui::Button(i18n::tr("Add Seltmann"), ImVec2(160, 0))) {
      gShown = a.addInstrument(kType);
    }
    return;
  }
  const Channel* sel = a.settingsChannel != kNoId ? s.channel(a.settingsChannel) : nullptr;
  ObjectId id = kNoId;
  if (sel && sel->plugin.type == kType) id = sel->id;
  else if (gShown != kNoId && s.channel(gShown)) id = gShown;
  else id = chans.front()->id;
  gShown = id;
  // several Seltmanns: chips to pick one
  if (chans.size() > 1) {
    for (const Channel* c : chans) {
      ImGui::PushID(static_cast<int>(c->id));
      const bool on = c->id == id;
      const ImVec2 sz(ImGui::CalcTextSize(c->name.c_str()).x + 18, 0);
      if (ImGui::Selectable(c->name.c_str(), on, ImGuiSelectableFlags_None, sz)) {
        gShown = c->id;
        if (sel && sel->plugin.type == kType) a.settingsChannel = c->id;  // the channel being edited follows
      }
      ImGui::PopID();
      ImGui::SameLine();
    }
    ImGui::NewLine();
  }
  const PluginTarget t{id, -1};
  hostbody::drawn(hostbody::Body::ChannelSettings, id, -1);
  ImGui::BeginChild("##seltpane", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  drawEditor(a, t, ImGui::GetContentRegionAvail().x);
  ImGui::EndChild();
  // the channel being edited is this one: the tab is its home, its window stays closed
  if (a.settingsChannel == id) hostbody::claim(hostbody::Body::ChannelSettings, "Seltmann");
}

}  // namespace gl::packs::seltmann

namespace gl::packs {
void seltmannPane(App& a) { seltmann::drawPane(a); }
void seltmannChopPage(App& a) { seltmann::tracker::chopPage(a); }
}  // namespace gl::packs
