// Gridline: the clip menus' render rows (K-250, K-251) and Send to Loupe (K-252). See ClipRenderUi.h.
#include "ClipRenderUi.h"
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../engine/ClipRender.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ExportUi.h"
#include "HostedUi.h"
#include "LayoutShellUi.h"
#include "LogicPlugins.h"
#include "Widgets.h"

namespace gl::cliprenderui {

std::set<ObjectId> targets(const App& a, ObjectId clip) {
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(clip)) sel = {clip};
  return sel;
}

void renderInPlace(App& a, const std::set<ObjectId>& clips) {
  a.later([&a, clips] {
    const std::vector<ObjectId> ids = renderableClips(a.doc.song(), clips);
    if (ids.empty()) {
      a.notify("Nothing to render", true);
      return;
    }
    captureHostedStates(a);  // clips through a hosted plugin: its current state
    const std::string dir = renderedDir(a);
    const int sr = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
    std::vector<ObjectId> made;
    a.doc.editSong(ids.size() == 1 ? "render clip to audio" : "render clips to audio", [&](Song& x) {
      made = renderClipsInPlace(x, std::set<ObjectId>(ids.begin(), ids.end()), dir, sr);
    });
    if (made.empty()) {
      a.notify("Nothing to render", true);
      return;
    }
    a.playlist.selected = std::set<ObjectId>(made.begin(), made.end());
    if (made.size() > 1) a.notify(i18n::plural(static_cast<long>(made.size()), "%d clip rendered to audio", "%d clips rendered to audio, each on its own"));
  });
}

bool menuItems(App& a, ObjectId clip) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c || c->kind == ClipKind::Automation) return false;
  const std::set<ObjectId> sel = targets(a, clip);
  const size_t n = renderableClips(a.doc.song(), sel).size();
  const bool many = n > 1;
  if (ImGui::MenuItem(many ? i18n::tr("Render each clip to audio") : i18n::tr("Render clip to audio"))) {
    renderInPlace(a, sel);
    return true;
  }
  ui::Hint(many ? "Every selected clip becomes an audio clip of its own sound, in its place"
                : "The clip becomes an audio clip of its sound, in its place (its channel's effects included)");
  if (ImGui::MenuItem(many ? i18n::tr("Render each clip to a file...") : i18n::tr("Render clip to file..."))) {
    if (many) exportui::exportClipsEach(a, sel);
    else exportui::exportClipAudio(a, clip);
    return true;
  }
  ui::Hint(many ? "One WAV file for every selected clip, in a folder you choose" : "The clip's sound as a WAV file");
  return false;
}

bool sendToLoupe(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c && c->kind == ClipKind::Audio ? s.channel(c->source) : nullptr;
  if (!ch || ch->plugin.data.empty()) return false;
  const std::string path = ch->plugin.data;
  const ObjectId id = ch->id;
  if (logicplug::active(a)) {  // the Smooth layout edits audio in its lower pane (in place of the piano roll)
    a.settings.set("ui.loupePinned", true);
    layoutui::setRegionVisible(a, "logic.lower", true);
    a.showPiano = a.showRack = a.showMixer = false;
  }
  packs::openAudioEditor(a, path, id, clip);
  return true;
}

bool sendToLoupeItem(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c && c->kind == ClipKind::Audio ? s.channel(c->source) : nullptr;
  if (!ch) return false;
  const bool chosen = ImGui::MenuItem(i18n::tr("Send to Loupe"), nullptr, false, !ch->plugin.data.empty());
  ui::Hint("Opens the clip's audio in Loupe: every edit there comes back to the clip");
  return chosen && sendToLoupe(a, clip);
}

}  // namespace gl::cliprenderui
