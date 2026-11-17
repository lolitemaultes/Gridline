#include "SmoothLibrary.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/LayoutShell.h"
#include "../core/MenuOps.h"
#include "../core/PluginInfo.h"
#include "../core/TrackModes.h"
#include "../host/PluginGroups.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "CollabUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LogicNewTracks.h"
#include "LogicPlugins.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::smoothlib {

using theme::pal;

namespace {

constexpr const char* kRegion = "logic.library";

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// The kind of sound a patch is, by its instrument and its name (as the browser's Sounds list sorts them).
const char* soundKind(const PluginInfo& p, const std::string& preset) {
  const std::string n = lower(preset);
  auto has = [&n](const char* w) { return n.find(w) != std::string::npos; };
  if (p.type == "gl.pads") return "Drum kit";
  if (p.category == "Drums") return "Drums";
  if (p.type == "gl.acid" || has("bass") || has("808") || has("sub ")) return "Bass";
  if (p.type == "gl.keys" || has("piano") || has("organ") || has("keys") || has("rhodes")) return "Keys";
  if (has("bell") || has("mallet") || has("marimba") || has("vibe")) return "Bells and mallets";
  if (p.type == "gl.pluck" || has("pluck") || has("nylon") || has("harp") || has("guitar")) return "Plucked";
  if (has("pad") || has("strings") || has("choir")) return "Pads and strings";
  if (has("lead")) return "Leads";
  if (p.type == "gl.granular") return "Textures";
  return "Synth";
}

bool installedListed(const App& a, const PluginInfo& p) {
  std::string fmt, uid;
  if (host::splitType(p.type, fmt, uid) && !host::formatEnabled(fmt)) return false;
  return host::listedInBrowser(p.type) && collabui::pluginAllowed(a, p.type);
}

int gNav = 1;  // Instruments, as a new project's Library opens
char gFilter[64] = "";
uint64_t gEpoch = ~0ull;
bool gShown = false;
bool gWasActive = false;

bool libraryShown(const App& a) {
  return layout::visible(layout::shell(layout::Preset::Logic), layoutui::regionState(a, layout::Preset::Logic), kRegion);
}

// The track the Library loads onto, and its sound.
int targetTrack(const App& a) { return logicplug::focusTrack(a); }
const Channel* targetSound(const App& a, int track) {
  const Song& s = a.doc.song();
  const Channel* c = logicplug::trackSound(s, track, nullptr);
  if (!c) return nullptr;
  const PluginInfo* p = findPlugin(c->plugin.type);
  if (!p || p->kind != PluginKind::Generator) return nullptr;
  // An audio track's sound (the sample player holding the track's audio regions) is not an instrument to
  // replace. R09-02: told by the track, not by the instrument: an instrument that can also play a sample
  // (Tercet, the New tracks default) on an instrument track is the track's instrument.
  const TrackMode mode = trackmodes::modeOf(s, track);
  if (mode == TrackMode::Audio) return nullptr;
  if (mode != TrackMode::Instrument)
    for (const Clip& k : s.playlist)
      if (k.track == track && k.kind == ClipKind::Audio && k.source == c->id) return nullptr;
  return c;
}

// The Library lists instruments that make sound on their own: sample players are left out (a patch
// without its sample is silent), except Tercet, whose sample oscillator is one of its sources.
bool listedInstrument(const PluginInfo& p) { return !p.usesSample || p.type == "gl.synth"; }


}  // namespace

const std::vector<std::string>& kinds() {
  static const std::vector<std::string> k = {"Bass", "Drum kit", "Drums", "Keys", "Plucked", "Bells and mallets", "Pads and strings", "Leads", "Synth", "Textures"};
  return k;
}

std::vector<Item> items(const App& a, int nav, const std::string& filterIn) {
  std::vector<Item> out;
  const std::string f = lower(trimmed(filterIn));
  auto keep = [&f](const Item& it) { return f.empty() || lower(it.name + " " + it.detail).find(f) != std::string::npos; };
  const bool wantInst = nav == 0 || nav == 1 || nav >= 3, wantFx = nav == 0 || nav == 2;
  const std::string onlyKind = nav >= 3 && nav - 3 < static_cast<int>(kinds().size()) ? kinds()[static_cast<size_t>(nav - 3)] : std::string();
  if (wantInst) {
    for (const PluginInfo& p : pluginCatalog()) {
      if (p.hidden || p.kind != PluginKind::Generator || !listedInstrument(p) || !collabui::pluginAllowed(a, p.type)) continue;
      if (p.presets.empty()) {
        Item it{p.type, -1, p.name, p.name, soundKind(p, p.name), false};
        if ((onlyKind.empty() || it.kind == onlyKind) && keep(it)) out.push_back(it);
      }
      for (size_t i = 0; i < p.presets.size(); ++i) {
        Item it{p.type, static_cast<int>(i), p.presets[i].name, p.name, soundKind(p, p.presets[i].name), false};
        if ((onlyKind.empty() || it.kind == onlyKind) && keep(it)) out.push_back(it);
      }
    }
    if (onlyKind.empty())
      for (const PluginInfo* p : dynamicPlugins()) {
        if (p->kind != PluginKind::Generator || !installedListed(a, *p)) continue;
        Item it{p->type, -1, p->name, p->category.empty() ? std::string(i18n::tr("Installed")) : p->category, "Installed", false};
        if (keep(it)) out.push_back(it);
      }
  }
  if (wantFx) {
    for (const PluginInfo& p : pluginCatalog()) {
      if (p.hidden || p.kind != PluginKind::Effect || !collabui::pluginAllowed(a, p.type)) continue;
      Item it{p.type, -1, p.name, p.category, p.category, true};
      if (keep(it)) out.push_back(it);
    }
    for (const PluginInfo* p : dynamicPlugins()) {
      if (p->kind != PluginKind::Effect || !installedListed(a, *p)) continue;
      Item it{p->type, -1, p->name, p->category.empty() ? std::string(i18n::tr("Installed")) : p->category, "Installed", true};
      if (keep(it)) out.push_back(it);
    }
  }
  // Sorted by name, as the reference's results list (numbers and letters, case ignored).
  std::stable_sort(out.begin(), out.end(), [](const Item& x, const Item& y) {
    if (x.effect != y.effect) return !x.effect;
    return lower(x.name) < lower(y.name);
  });
  // Each patch name once, as the reference lists it: where two instruments ship a patch of the same
  // name, the first (catalogue order) stays; the other is still reached from its instrument's presets.
  out.erase(std::unique(out.begin(), out.end(), [](const Item& x, const Item& y) { return x.effect == y.effect && lower(x.name) == lower(y.name); }),
            out.end());
  return out;
}

bool load(App& a, const Item& it) {
  const PluginInfo* info = findPlugin(it.type);
  if (!info) return false;
  const Song& s0 = a.doc.song();
  const int track = targetTrack(a);
  if (it.effect) {  // into the selected track's first free slot (its strip; the master when it has none)
    const MixerTrack* st = track >= 0 ? logicplug::trackStrip(s0, track) : nullptr;
    if (!st && !s0.mixer.empty()) st = &s0.mixer[0];
    if (!st) return false;
    int free = -1;
    for (int k = 0; k < kEffectSlots && free < 0; ++k)
      if (st->slots[static_cast<size_t>(k)].plugin.type.empty()) free = k;
    if (free < 0) {
      a.notify(i18n::tr("Every effect slot of this track is taken"));
      return false;
    }
    const ObjectId id = st->id;
    a.selMixer = id;
    return a.doc.setSlotPlugin(id, free, it.type);
  }
  PluginState ps = it.preset >= 0 && static_cast<size_t>(it.preset) < info->presets.size() ? presetState(*info, static_cast<size_t>(it.preset))
                                                                                          : defaultPluginState(it.type);
  const std::string name = it.name;
  if (const Channel* c = targetSound(a, track)) {
    // The track's sound takes the patch: its instrument and settings; a name that was the old patch's
    // (or the instrument's own) follows, and so does the track's.
    const ObjectId id = c->id;
    const std::string oldName = c->name;
    const PluginInfo* oldInfo = findPlugin(c->plugin.type);
    bool autoName = channelNameIsAutomatic(*c) || (oldInfo && oldName == oldInfo->name);
    if (oldInfo)
      for (const auto& pr : oldInfo->presets) autoName = autoName || pr.name == oldName;
    const PlaylistTrack& pt = s0.tracks[static_cast<size_t>(track)];
    const bool trackFollows = pt.name.empty() || pt.name == oldName;
    const bool sameType = c->plugin.type == it.type;
    a.doc.beginGroup("load patch");
    if (!sameType) a.doc.setChannelPlugin(id, it.type);
    a.doc.editChannel(id, "load patch", [&ps, &name, autoName](Channel& ch) {
      ch.plugin.params = ps.params;
      ch.plugin.data = ps.data;
      if (!ps.ins.empty()) ch.plugin.ins = ps.ins;
      if (autoName) ch.name = name;
    });
    if (autoName && trackFollows) a.doc.editPlaylistTrack(track, "name track", [&name](PlaylistTrack& x) { x.name = name; });
    a.doc.endGroup();
    if (!sameType) pui::pluginReplaced(PluginTarget{id, -1});
    a.selectChannel(id);
    return true;
  }
  // No instrument on the selected track: a new instrument track for the patch, as the reference does
  // on an empty or audio track (the first unused track, else a new one at the end).
  a.doc.beginGroup("load patch");
  const ObjectId id = a.addInstrument(it.type);
  if (id == kNoId) {
    a.doc.endGroup();
    return false;
  }
  a.doc.editChannel(id, "load patch", [&ps, &name](Channel& ch) {
    ch.plugin.params = ps.params;
    ch.plugin.data = ps.data;
    if (!ps.ins.empty()) ch.plugin.ins = ps.ins;
    ch.name = name;
  });
  const Song& s = a.doc.song();
  auto unused = [&s](int k) {
    bool used = trackmodes::modeOf(s, k) != TrackMode::Unassigned;
    for (const Clip& c : s.playlist) used = used || c.track == k;
    return !used;
  };
  // R09-02: the selected track itself when it is empty (the patch loads onto the selected track)
  int t = track >= 0 && track < static_cast<int>(s.tracks.size()) && unused(track) ? track : -1;
  for (int k = 0; k < static_cast<int>(s.tracks.size()) && t < 0; ++k)
    if (unused(k)) t = k;
  if (t < 0 && ops::insertPlaylistTrack(a.doc, static_cast<int>(s.tracks.size()))) t = static_cast<int>(a.doc.song().tracks.size()) - 1;
  if (t >= 0) {
    trackmodes::makeInstrumentTrack(a.doc, t, id);
    a.doc.editPlaylistTrack(t, "name track", [&name](PlaylistTrack& x) { x.name = name; });
    a.playlist.trackSel = {t};
  }
  a.doc.endGroup();
  a.selectChannel(id);
  return true;
}

void frame(App& a) {
  const bool on = logicplug::active(a) && layoutui::shellBuilt(a, layout::Preset::Logic);
  if (!on) {
    gWasActive = false;
    return;
  }
  const uint64_t ep = a.doc.projectEpoch();
  const bool shown = libraryShown(a);
  if (ep != gEpoch || !gWasActive) {  // a project opened or made, or Smooth just came on: as the project says
    gEpoch = ep;
    gWasActive = true;
    const bool want = a.doc.song().smooth.library != "closed";
    if (want != shown) layoutui::setRegionVisible(a, kRegion, want);
    gShown = libraryShown(a);
    return;
  }
  if (shown != gShown) {  // shown or hidden here (B, the control bar, the View menu): noted in the project
    gShown = shown;
    const char* v = shown ? "open" : "closed";
    if (a.doc.song().smooth.library != v) a.doc.patchSong([v](Song& x) { x.smooth.library = v; });
  }
}

// ------------------------------------------------------------------------ drawing ----

namespace {

// The header: the selected track's sound as a large icon and its patch name (248, 264).
void header(App& a, float w) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const int track = targetTrack(a);
  const Channel* c = targetSound(a, track);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // K-112D (audit H2): the pane's name centred on top, then the sound's picture (about 56 px) and its
  // patch name (248, 264).
  const float h = 104;
  ImGui::Dummy(ImVec2(w, h));
  const float cx = std::round(p.x + w * 0.5f);
  {
    const char* lt = i18n::tr("Library");
    const float lfs = theme::kFontCaption;
    const float lw = a.fontBold->CalcTextSizeA(lfs, 1e9f, 0, lt).x;
    dl->AddText(a.fontBold, lfs, ImVec2(std::round(cx - lw * 0.5f), p.y + 2), P.textDim, lt);
  }
  const int ic = c && c->icon > 0 ? c->icon : trackicons::guess(c ? c->name : std::string(), c ? "Synth" : "Audio");
  const ImVec2 b0(cx - 28, p.y + 22), b1(cx + 28, p.y + 78);
  dl->AddRectFilled(b0, b1, theme::mix(P.surface800, P.text, 0.07f), 10.0f);
  if (ic > 0) trackicons::draw(dl, ic, ImVec2(cx, p.y + 50), 38, c ? theme::mix(theme::content(c->color), P.text, 0.30f) : P.textDim);
  const char* title = c ? c->name.c_str() : track >= 0 && track < static_cast<int>(s.tracks.size()) ? i18n::tr("No instrument") : "";
  const float fs = theme::kFontBody - 1;  // audit X6: a name at 13 px
  const float tw = std::min(a.fontBold->CalcTextSizeA(fs, 1e9f, 0, title).x, w - 12);
  theme::textEllipsis(dl, a.fontBold, fs, ImVec2(std::round(cx - tw * 0.5f), p.y + 84), w - 12, P.text, title);
}

// A navigation row: an icon and a name, the chosen one on a rounded fill.
bool navRow(App& a, const char* id, ui::Icon icon, const char* label, bool on, float w) {
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22;
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on || hov) dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + w, p.y + h - 1), on ? theme::mix(P.surface800, P.text, 0.12f) : theme::mix(P.surface800, P.text, 0.05f), 5.0f);
  const float fs = theme::kFontCaption;
  if (icon != ui::Icon::Count) ui::drawIcon(dl, icon, ImVec2(p.x + 12, p.y + h * 0.5f), 12, on ? P.text : P.textDim);
  const float tx = icon != ui::Icon::Count ? p.x + 24 : p.x + 8;
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(tx, std::round(p.y + (h - fs) * 0.5f) - 1), p.x + w - tx - 4, on ? P.text : P.textDim, label);
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(0, 0));
  return click;
}

void navHeading(App& a, const char* label, float w) {
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, 20));
  const float fs = theme::kFontLabel;
  ImGui::GetWindowDrawList()->AddText(a.fontBold, fs, ImVec2(p.x + 8, std::round(p.y + 20 - fs - 3)), P.textFaint, label);
}

void navLine(float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, 9));
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 6, std::round(p.y + 4)), ImVec2(p.x + w - 6, std::round(p.y + 4)), theme::withAlpha(pal().line, 0.9f));
}

// A row's badge: a small rounded square, its colour and glyph by the kind of sound (bass, keys, pads,
// drums ...), effects and installed plugins their own; muted tones that sit on the graphite.
void kindBadge(ImDrawList* dl, const Item& it, ImVec2 c) {
  struct K {
    const char* kind;
    uint32_t col;
    ui::Icon icon;
  };
  static const K kinds[] = {{"Bass", 0xC9902E, ui::Icon::Wave},          {"Drum kit", 0x4F8FD8, ui::Icon::CatDrums},
                            {"Drums", 0x4F8FD8, ui::Icon::CatDrums},       {"Keys", 0x3FA35B, ui::Icon::Piano},
                            {"Plucked", 0xC9902E, ui::Icon::CatPitch},       {"Bells and mallets", 0x2FA6A0, ui::Icon::Piano},
                            {"Pads and strings", 0x8A67D6, ui::Icon::CatModulation}, {"Leads", 0xD9644A, ui::Icon::CatSynth},
                            {"Synth", 0x6E62D9, ui::Icon::CatSynth},       {"Textures", 0x8A67D6, ui::Icon::CatSpectral},
                            {"Installed", 0x5C6470, ui::Icon::Plug}};
  uint32_t col = 0x3FA35B;
  ui::Icon icon = ui::Icon::Piano;
  if (it.effect) col = 0x5C6470, icon = ui::Icon::CatFilter;
  else
    for (const K& k : kinds)
      if (it.kind == k.kind) col = k.col, icon = k.icon;
  dl->AddRectFilled(ImVec2(c.x - 7, c.y - 7), ImVec2(c.x + 7, c.y + 7), theme::rgb(col), 3.0f);
  ui::drawIcon(dl, icon, c, 9, theme::contrastText(theme::rgb(col)));
}

}  // namespace

void draw(App& a) {
  const auto& P = pal();
  const float w = ImGui::GetContentRegionAvail().x;
  header(a, w);
  // The search field, full width, a rounded dark field as 111 and 252 draw it.
  ImGui::SetNextItemWidth(-1);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.field));
  ImGui::InputTextWithHint("##smlibfind", i18n::tr("Search"), gFilter, sizeof gFilter);
  ImGui::PopStyleColor();
  ui::Hint("Search every patch and effect: type part of a name");
  ImGui::Dummy(ImVec2(0, 2));
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float avH = ImGui::GetContentRegionAvail().y;
  const float footH = 30;
  // Two columns: the navigation, then the results (111, 252); the navigation takes about a third.
  const float navW = std::clamp(std::floor(w * 0.38f), 84.0f, 150.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(o.x + navW + 2, o.y), ImVec2(o.x + navW + 2, o.y + avH - footH), theme::withAlpha(P.line, 0.9f));
  ImGui::BeginChild("##smlibnav", ImVec2(navW, avH - footH), ImGuiChildFlags_None);
  {
    const float nw = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    struct Nav {
      ui::Icon icon;
      const char* name;
    };
    const Nav top[] = {{ui::Icon::ListBullets, i18n::N_("All")}, {ui::Icon::Piano, i18n::N_("Instruments")}, {ui::Icon::CatFilter, i18n::N_("Effects")}};
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      if (navRow(a, "##nav", top[i].icon, i18n::tr(top[i].name), gNav == i, nw)) gNav = i;
      ImGui::PopID();
    }
    navLine(nw);
    navHeading(a, i18n::tr("Sounds"), nw);
    for (size_t k = 0; k < kinds().size(); ++k) {
      ImGui::PushID(static_cast<int>(10 + k));
      if (navRow(a, "##kind", ui::Icon::Count, i18n::tr(kinds()[k].c_str()), gNav == static_cast<int>(3 + k), nw)) gNav = static_cast<int>(3 + k);
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(o.x + navW + 5, o.y));
  // The rows are made again only when the view or the search changes, or once a second (plugins
  // installed or allowed meanwhile): no list is built per frame.
  static std::vector<Item> rows;
  static int rowsNav = -1;
  static std::string rowsFilter;
  static int rowsFrame = -1000;
  const int fr = ImGui::GetFrameCount();
  if (rowsNav != gNav || rowsFilter != gFilter || fr - rowsFrame >= 60 || fr < rowsFrame) {
    rows = items(a, gNav, gFilter);
    rowsNav = gNav;
    rowsFilter = gFilter;
    rowsFrame = fr;
  }
  const float listW = w - navW - 5;
  ImGui::BeginChild("##smlibrows", ImVec2(listW, std::max(40.0f, avH - footH)), ImGuiChildFlags_None);
  {
    const int track = targetTrack(a);
    const Channel* cur = targetSound(a, track);
    const float rw = ImGui::GetContentRegionAvail().x, rh = 22;
    ImDrawList* ld = ImGui::GetWindowDrawList();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()), rh);
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const Item& it = rows[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool click = ImGui::InvisibleButton("##row", ImVec2(rw, rh));
        const bool hov = ImGui::IsItemHovered();
        const bool loaded = !it.effect && cur && cur->plugin.type == it.type && cur->name == it.name;
        // Alternate rows a shade apart, the loaded patch on the selection fill (111, 252).
        if (i % 2 == 1) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.ink, 0.10f));
        if (loaded) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.accent, 0.38f), 4.0f);
        else if (hov) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.text, 0.06f), 4.0f);
        // R09 (visual gap 10): each row's badge by its kind of sound, as 111 tells its kinds apart by the
        // badge's colour and glyph (every patch had the same green one).
        kindBadge(ld, it, ImVec2(p.x + 13, p.y + rh * 0.5f));
        const float fs = theme::kFontCaption, tx = p.x + 26;
        theme::textEllipsis(ld, a.fontRegular, fs, ImVec2(tx, std::round(p.y + (rh - fs) * 0.5f) - 1), rw - (tx - p.x) - 6, P.text, it.name.c_str());
        if (hov) ui::Hint(it.effect ? it.name + " \xC2\xB7 " + it.detail + "   (click: add it to the selected track)"
                                    : it.name + " \xC2\xB7 " + it.detail + "   (click: load it onto the selected track)");
        if (click) load(a, it);
        ImGui::PopID();
      }
    if (rows.empty()) ImGui::TextDisabled("%s", i18n::tr("Nothing has that name"));
  }
  ImGui::EndChild();
  // The footer under the results: how many items (252, 111), then Revert and Save for the selected
  // track's patch (audit H2: the reference's footer buttons). Revert loads the patch's factory
  // settings again; Save writes the sound as a preset file.
  {
    // across the whole pane (the navigation column ends above it), as the reference's footer
    const ImVec2 f(o.x, o.y + avH - footH);
    dl->AddRectFilled(f, ImVec2(f.x + w, f.y + footH), P.surface900);
    dl->AddLine(ImVec2(f.x, f.y), ImVec2(f.x + w, f.y), theme::withAlpha(P.line, 0.9f));
    char b[48];
    std::snprintf(b, sizeof b, i18n::tr("%d items"), static_cast<int>(rows.size()));
    const float fs = theme::kFontLabel;
    dl->AddText(a.fontRegular, fs, ImVec2(f.x + 8, std::round(f.y + (footH - fs) * 0.5f) - 1), P.textFaint, b);
    const int track = targetTrack(a);
    const Channel* cur = targetSound(a, track);
    const Item* factory = nullptr;
    for (const Item& it : rows)
      if (cur && !it.effect && it.preset >= 0 && it.type == cur->plugin.type && it.name == cur->name) factory = &it;
    const float bh = 20, gap = 4, by = std::round(f.y + (footH - bh) * 0.5f);
    const char* labels[2] = {i18n::tr("Revert"), i18n::tr("Save")};
    float bw[2], total = gap;
    for (int k = 0; k < 2; ++k) total += (bw[k] = std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, labels[k]).x) + 16);
    float bx = f.x + w - total - 4;
    for (int k = 0; k < 2; ++k) {
      const bool en = k == 0 ? factory != nullptr : cur != nullptr;
      ImGui::SetCursorScreenPos(ImVec2(bx, by));
      ImGui::PushID(k);
      const bool click = ImGui::InvisibleButton("##libfoot", ImVec2(bw[k], bh)) && en;
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw[k], by + bh), hov && en ? theme::mix(P.surface700, P.text, 0.10f) : P.surface700, 5.0f);
      const float lw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, labels[k]).x;
      dl->AddText(a.fontRegular, fs, ImVec2(std::round(bx + (bw[k] - lw) * 0.5f), std::round(by + (bh - fs) * 0.5f) - 1), en ? P.text : P.textFaint, labels[k]);
      if (hov) ui::Hint(k == 0 ? "Revert: the selected track's patch back to its factory settings" : "Save: the selected track's sound as a preset file");
      if (click && k == 0) load(a, *factory);
      if (click && k == 1) savePresetDialog(a, cur->id, -1);
      bx += bw[k] + gap;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + avH));
  ImGui::Dummy(ImVec2(0, 0));
}

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return false;
  const std::string& op = args[0];
  if (op == "nav" && args.size() > 1) {
    gNav = std::clamp(std::atoi(args[1].c_str()), 0, static_cast<int>(2 + kinds().size()));
  } else if (op == "rows") {
    const std::vector<Item> v = items(a, gNav, gFilter);
    std::printf("smoothlib rows nav %d: %zu\n", gNav, v.size());
    for (size_t i = 0; i < v.size() && i < 400; ++i)
      std::printf("  %zu %s \"%s\" preset %d kind %s\n", i, v[i].type.c_str(), v[i].name.c_str(), v[i].preset, v[i].kind.c_str());
  } else if (op == "load" && args.size() > 1) {
    const std::vector<Item> v = items(a, gNav, gFilter);
    const int i = std::atoi(args[1].c_str());
    const bool ok = i >= 0 && i < static_cast<int>(v.size()) && load(a, v[static_cast<size_t>(i)]);
    std::printf("smoothlib load %d: %s\n", i, ok ? "ok" : "no");
  } else if (op == "newtrack") {  // R09-02 test: New tracks > Instrument, as the dialog's Create does
    newtracks::createDefault(a);
    const Song& s = a.doc.song();
    std::printf("smoothlib newtrack: tracks %zu channels %zu selected %d\n", s.tracks.size(), s.channels.size(), targetTrack(a));
  } else if (op == "tracks") {  // each used track: its mode and its sound
    const Song& s = a.doc.song();
    for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
      const Channel* c = logicplug::trackSound(s, t, nullptr);
      if (trackmodes::modeOf(s, t) == TrackMode::Unassigned && !c) continue;
      std::printf("smoothlib track %d \"%s\" mode %d sound \"%s\" type %s\n", t, playlistTrackName(s, static_cast<size_t>(t)).c_str(),
                  static_cast<int>(trackmodes::modeOf(s, t)), c ? c->name.c_str() : "", c ? c->plugin.type.c_str() : "");
    }
    std::printf("smoothlib tracks: channels %zu\n", s.channels.size());
  } else if (op == "state") {
    const int t = targetTrack(a);
    const Channel* c = targetSound(a, t);
    std::printf("smoothlib state: shown %d saved \"%s\" nav %d track %d sound \"%s\" type %s\n", libraryShown(a) ? 1 : 0,
                a.doc.song().smooth.library.c_str(), gNav, t, c ? c->name.c_str() : "", c ? c->plugin.type.c_str() : "");
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::smoothlib
