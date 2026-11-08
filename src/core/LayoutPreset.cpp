// Gridline - window layout presets (K-79). See LayoutPreset.h.
#include "LayoutPreset.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "I18n.h"
#include "Settings.h"

namespace gl::layout {

namespace {
std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

const char* id(Preset p) {
  switch (p) {
    case Preset::Live: return "live";
    case Preset::Logic: return "logic";
    default: return "studio";
  }
}

Preset fromId(const std::string& text) {
  const std::string t = lower(text);
  if (t == "live" || t == "performance") return Preset::Live;
  if (t == "logic" || t == "tracks") return Preset::Logic;
  return Preset::Studio;
}

// The display names are not confirmed yet: change them here only.
const char* displayName(Preset p) {
  switch (p) {
    case Preset::Live: return i18n::N_("Live");
    case Preset::Logic: return i18n::N_("Logic");
    default: return i18n::N_("Studio");
  }
}

const char* blurb(Preset p) {
  switch (p) {
    case Preset::Live: return i18n::N_("Playlist or pads above the mixer, editors along the bottom");
    case Preset::Logic: return i18n::N_("Tracks in the centre, one shared bottom pane, history on the right");
    default: return i18n::N_("Channel rack floating over the playlist, mixer on the right");
  }
}

bool isPreview(Preset p) { return p != Preset::Studio; }
const char* previewTag() { return i18n::N_("Preview"); }

std::string iniFileName(Preset p) { return p == Preset::Studio ? std::string("layout.ini") : std::string("layout-") + id(p) + ".ini"; }

std::string versionKey(Preset p) { return p == Preset::Studio ? std::string("ui.layoutVersion") : std::string("ui.layoutVersion.") + id(p); }

int defaultVersion(Preset p) { return p == Preset::Studio ? workspace::kLayoutVersion : 1; }

std::string bindingsKey(Preset p) { return p == Preset::Studio ? std::string("keys.bindings") : std::string("keys.bindings.") + id(p); }

Linked linkedFromSettings(const Settings& s) {
  Linked l;
  l.keys = s.getBool("ui.layoutLinkKeys");
  l.menus = s.getBool("ui.layoutLinkMenus");
  return l;
}

Preset current(const Settings& s) { return fromId(s.getString("ui.layout")); }
Preset keyProfile(const Settings& s) { return fromId(s.getString("keys.profile")); }
Preset menuStyle(const Settings& s) { return fromId(s.getString("ui.menuStyle")); }

void choose(Settings& s, Preset p, const Linked& linked) {
  s.set("ui.layout", id(p));
  s.set("ui.layoutLinkKeys", linked.keys);
  s.set("ui.layoutLinkMenus", linked.menus);
  if (linked.keys) s.set("keys.profile", id(p));
  if (linked.menus) s.set("ui.menuStyle", id(p));
}

bool normalize(Settings& s) {
  bool changed = false;
  for (const char* key : {"ui.layout", "keys.profile", "ui.menuStyle"}) {
    const std::string v = s.getString(key);
    const std::string want = id(fromId(v));
    if (v != want) changed |= s.set(key, want);
  }
  return changed;
}

LiveLayout liveLayout(float w, float h) {
  LiveLayout L;
  const float W = std::max(w, 400.0f), H = std::max(h, 300.0f);
  L.area = {0, 0, W, H};
  // The bottom panel (editors, channel settings) across the full width.
  const float bottomH = std::floor(std::min(std::clamp(H * 0.30f, 200.0f, 420.0f), H * 0.40f));
  const float topH = H - bottomH;
  L.bottom = {0, topH, W, bottomH};
  L.bottomFrac = bottomH / H;
  // The browser as a sidebar left of the main view and the mixer.
  const float browserW = std::floor(std::min(std::clamp(W * 0.21f, 240.0f, 340.0f), W * 0.35f));
  L.browser = {0, 0, browserW, topH};
  L.browserFrac = browserW / W;
  // The mixer under the main view; the main view keeps at least 40 % of the top.
  const float restW = W - browserW;
  const float mixerH = std::floor(std::min(std::clamp(topH * 0.42f, 190.0f, 360.0f), topH * 0.60f));
  L.mixer = {browserW, topH - mixerH, restW, mixerH};
  L.main = {browserW, 0, restW, topH - mixerH};
  L.mixerFrac = mixerH / topH;
  return L;
}

LogicLayout logicLayout(float w, float h) {
  LogicLayout L;
  const float W = std::max(w, 400.0f), H = std::max(h, 300.0f);
  L.area = {0, 0, W, H};
  const float browserW = std::floor(std::min(std::clamp(W * 0.17f, 240.0f, 320.0f), W * 0.25f));
  L.browser = {0, 0, browserW, H};
  L.browserFrac = browserW / W;
  const float restW = W - browserW;
  const float rightW = std::floor(std::min(std::clamp(W * 0.20f, 280.0f, 340.0f), restW * 0.30f));
  L.right = {W - rightW, 0, rightW, H};
  L.rightFrac = rightW / restW;
  const float centreW = restW - rightW;
  const float bottomH = std::floor(std::clamp(H * 0.40f, std::min(220.0f, H * 0.5f), H * 0.55f));
  L.bottom = {browserW, H - bottomH, centreW, bottomH};
  L.main = {browserW, 0, centreW, H - bottomH};
  L.bottomFrac = bottomH / H;
  return L;
}

}  // namespace gl::layout
