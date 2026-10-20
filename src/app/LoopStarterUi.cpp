// Gridline — loop starter window (GEN-01, see J59Ui.h and core/LoopStarter.h).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/ChordTools.h"
#include "../core/I18n.h"
#include "../core/LoopStarter.h"
#include "../core/NoteTools.h"
#include "App.h"
#include "DialogLayout.h"
#include "J59Ui.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::j59 {

using theme::pal;
namespace ls = gl::loopstarter;

namespace {

// Text the core hands over (style names and blurbs, moods, forms, parts),
// listed here so the catalogue extractor finds it; shown through tr().
[[maybe_unused]] const char* const kCoreText[] = {
    i18n::N_("House"), i18n::N_("Techno"), i18n::N_("Hip hop"), i18n::N_("Trap"), i18n::N_("Lo-fi"), i18n::N_("Drum and bass"),
    i18n::N_("Pop"), i18n::N_("Ambient"),
    i18n::N_("Four on the floor, off-beat hats, stabbed chords and a plucked arpeggio."),
    i18n::N_("Driving kick, rolling bass, sparse stabs and a hypnotic sequence."),
    i18n::N_("Swung boom-bap drums, round bass, soft keys and a short motif."),
    i18n::N_("Half-time snare, hat rolls, long sub bass and bell melodies."),
    i18n::N_("Lazy swing, dusty drums, seventh-chord keys and a gentle pluck."),
    i18n::N_("Two-step breaks at full speed under a saw bass and wide pads."),
    i18n::N_("Straight beat, pulsing piano chords, eighth-note bass and a hook."),
    i18n::N_("Slow pads, drone bass, soft percussion and a floating bell arp."),
    i18n::N_("Dark"), i18n::N_("Warm"), i18n::N_("Bright"), i18n::N_("Dreamy"),
    i18n::N_("4-bar loop"), i18n::N_("8-bar loop"), i18n::N_("Short arrangement"),
    i18n::N_("Drums"), i18n::N_("Bass"), i18n::N_("Chords"), i18n::N_("Lead"),
};

struct State {
  bool open = false, init = false;
  ls::Settings set;
  bool applied = false;   // the current project was made here
  uint64_t revision = 0;  // document revision right after the last apply
  uint32_t entropy = 1;
  // Preview of the parts (generated, not rendered) for the strips.
  ls::Settings cached;
  bool haveCache = false;
  Song preview;
} L;

void formLabel(const char* label, float x = 112.0f) { ui::FormLabelFit(i18n::tr(label), x, pal().textDim); }

bool live(const App& a) { return L.applied && a.doc.revision() == L.revision; }

const Song& previewSong() {
  if (!L.haveCache || L.cached != L.set) {
    L.preview = ls::generate(L.set);
    L.cached = L.set;
    L.haveCache = true;
  }
  return L.preview;
}

// Puts the generated project into the document as one undo step.
void applyTo(App& a, const std::string& label) {
  const Song s = ls::generate(L.set);
  a.doc.editSong(label, [&s](Song& x) { x = s; });
  const Song& song = a.doc.song();
  a.curPattern = song.patterns.empty() ? kNoId : song.patterns[0].id;
  a.selChannel = song.channels.empty() ? kNoId : song.channels[0].id;
  a.selChannels = {a.selChannel};
  a.selMixer = song.masterId();
  a.piano.selPattern = kNoId;
  a.piano.selected.clear();
  a.playlist.selected.clear();
  a.engine.setMode(PlayMode::Song);
  L.applied = true;
  L.revision = a.doc.revision();
}

void createProject(App& a) {
  a.guardUnsaved([&a]() {
    Song empty = makeEmptySong();
    a.applyProjectDefaults(empty);
    a.startProject(std::move(empty));
    applyTo(a, "loop starter");
    a.notify(ls::describe(L.set));
  });
}

std::string soundText(const ls::Sound& s) {
  const PluginInfo* p = findPlugin(s.type);
  std::string t = p ? p->name : s.type;
  if (!s.preset.empty()) t += " · " + s.preset;
  return t;
}

// Small strip of a part's notes / steps (the pattern's first bars).
void partStrip(const Song& song, int part, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
  dl->AddRectFilled(a, b, P.well, 2.0f);
  ImGui::Dummy(size);
  static const char* const pats[] = {"Drums", "Bass", "Chords", "Lead"};
  const Pattern* pat = nullptr;
  for (const auto& p : song.patterns)
    if (p.name == pats[part]) pat = &p;
  if (!pat) return;
  const Tick len = song.patternLength(*pat);
  if (len <= 0) return;
  const float sx = size.x / static_cast<float>(len);
  const Tick step = song.ticksPerStep(*pat);
  const ImU32 col = theme::rgb(part == 0 ? 0xD8775A : part == 1 ? 0xA56CD8 : part == 2 ? 0x7A6FE0 : 0x4FB8B0);
  if (part == 0) {  // three lanes: kick, backbeat, hats
    for (const auto& c : song.channels) {
      int l = c.name == "Kick" ? 2 : (c.name == "Snare" || c.name == "Clap") ? 1 : c.name == "Hat" || c.name == "Open hat" ? 0 : -1;
      if (l < 0) continue;
      const float y0 = a.y + 2 + l * (size.y - 4) / 3.0f, y1 = y0 + (size.y - 4) / 3.0f - 1;
      if (auto it = pat->steps.find(c.id); it != pat->steps.end())
        for (size_t i = 0; i < it->second.size(); ++i)
          if (it->second[i]) {
            const float x = a.x + sx * static_cast<float>(static_cast<Tick>(i) * step);
            dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + std::max(1.5f, sx * static_cast<float>(step) - 1), y1),
                              theme::withAlpha(col, 0.35f + 0.65f * it->second[i] / 127.0f));
          }
      if (auto it = pat->notes.find(c.id); it != pat->notes.end())
        for (const Note& n : it->second) {
          const float x = a.x + sx * static_cast<float>(n.start);
          dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + std::max(1.0f, sx * static_cast<float>(n.length) - 1), y1),
                            theme::withAlpha(col, 0.35f + 0.65f * n.velocity / 127.0f));
        }
    }
    return;
  }
  int lo = 127, hi = 0;
  for (const auto& [id, ns] : pat->notes)
    for (const Note& n : ns) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  if (lo > hi) return;
  const float rows = static_cast<float>(hi - lo + 1), rh = std::max(1.5f, (size.y - 4) / std::max(rows, 6.0f));
  for (const auto& [id, ns] : pat->notes)
    for (const Note& n : ns) {
      const float x = a.x + sx * static_cast<float>(n.start);
      const float y = b.y - 2 - (static_cast<float>(n.pitch - lo) + 1) * (size.y - 4) / rows;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(1.5f, sx * static_cast<float>(n.length) - 1), y + rh), col, 1.0f);
    }
}

bool combo(const char* id, int* idx, const std::vector<std::string>& items, float w) {
  bool changed = false;
  if (ui::BeginCombo(id, items[static_cast<size_t>(std::clamp(*idx, 0, static_cast<int>(items.size()) - 1))].c_str(), w)) {
    for (int k = 0; k < static_cast<int>(items.size()); ++k)
      if (ImGui::Selectable(items[static_cast<size_t>(k)].c_str(), k == *idx)) *idx = k, changed = true;
    ImGui::EndCombo();
  }
  return changed;
}

void window(App& a) {
  if (!L.open) return;
  const auto& P = pal();
  ui::PlaceNextDialog(ImVec2(820, 600), ImGuiCond_Appearing, ImVec2(720, 560));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const std::string title = std::string(i18n::tr("Loop starter")) + "###j59loopstarter";
  bool vis = ImGui::Begin(title.c_str(), &L.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  ls::Settings& s = L.set;
  const ls::Settings before = s;
  std::string undoLabel = "loop starter";
  bool commit = false;  // a finished change: re-applied at once while the project is live

  const float footer = ui::DialogFooterHeight();
  const float h = ImGui::GetContentRegionAvail().y - footer;
  // ---- styles
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##styles", ImVec2(190, h), ImGuiChildFlags_Borders);
  ui::Label("STYLE");
  for (int i = 0; i < static_cast<int>(ls::styles().size()); ++i) {
    const auto& st = ls::styles()[static_cast<size_t>(i)];
    std::string l = std::string(i18n::tr(st.name)) + "##style" + std::to_string(i);
    if (ImGui::Selectable(l.c_str(), s.style == i, 0, ImVec2(0, 22))) {
      const ls::Settings d = ls::styleDefaults(i);
      s.style = i, s.tempo = d.tempo, s.key = d.key, s.scale = d.scale;
      commit = true, undoLabel = "loop starter: style";
    }
    ui::Hint(st.blurb);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine();

  // ---- settings and parts
  ImGui::BeginChild("##main", ImVec2(0, h));
  const ls::StyleInfo& st = ls::styles()[static_cast<size_t>(std::clamp(s.style, 0, static_cast<int>(ls::styles().size()) - 1))];
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(i18n::tr(st.name));
  ImGui::PopFont();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(st.blurb));
  ImGui::Dummy(ImVec2(0, theme::kS1));

  ui::BeginSection("SONG");
  {
    formLabel("Tempo (BPM)");
    double tempo = ls::effectiveTempo(s);
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputDouble("##tempo", &tempo, 1, 5, "%.1f")) s.tempo = std::clamp(tempo, 10.0, 522.0);
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true, undoLabel = "loop starter: tempo";
    ui::FocusRing();
    ImGui::SameLine();
    if (ui::TextButton("Style tempo", false, 0, "The style's usual tempo")) s.tempo = st.tempoDefault, commit = true;
    ImGui::SameLine();
    ImGui::TextColored(theme::vec(P.textFaint), "%.0f-%.0f", st.tempoMin, st.tempoMax);

    formLabel("Key");
    std::vector<std::string> keys;
    for (int k = 0; k < 12; ++k) keys.push_back(chord::noteName(k));
    if (combo("##key", &s.key, keys, 70)) commit = true, undoLabel = "loop starter: key";
    ImGui::SameLine();
    std::vector<std::string> scales;
    int si = 0;
    for (size_t k = 0; k < ls::scaleChoices().size(); ++k) {
      scales.push_back(i18n::tr(tools::scales()[static_cast<size_t>(ls::scaleChoices()[k])].name));
      if (ls::scaleChoices()[k] == s.scale) si = static_cast<int>(k);
    }
    if (combo("##scale", &si, scales, 170)) s.scale = ls::scaleChoices()[static_cast<size_t>(si)], commit = true, undoLabel = "loop starter: scale";

    formLabel("Mood");
    static const char* const moods[] = {i18n::N_("Dark"), i18n::N_("Warm"), i18n::N_("Bright"), i18n::N_("Dreamy")};
    static const char* const moodHints[] = {i18n::N_("Lower lines, filtered chords and lead"), i18n::N_("The style as it is"),
                                            i18n::N_("Higher lead, lighter reverb"), i18n::N_("Seventh chords, held pads, more reverb and delay")};
    int mood = static_cast<int>(s.mood);
    if (ui::Segmented("mood", moods, 4, &mood, 78, 0, moodHints)) s.mood = static_cast<ls::Mood>(mood), commit = true, undoLabel = "loop starter: mood";

    formLabel("Energy");
    float energy = static_cast<float>(s.energy * 100.0);
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("##energy", &energy, 0.0f, 100.0f, "%.0f %%")) s.energy = energy / 100.0;
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true, undoLabel = "loop starter: energy";
    ui::Hint("Density of the drums, bass and lead: ghost notes, fills, hat rolls");

    formLabel("Length");
    static const char* const forms[] = {i18n::N_("4-bar loop"), i18n::N_("8-bar loop"), i18n::N_("Short arrangement")};
    int form = static_cast<int>(s.form);
    if (ui::Segmented("form", forms, 3, &form, 112)) s.form = static_cast<ls::Form>(form), commit = true, undoLabel = "loop starter: length";
  }
  ui::EndSection();

  ui::BeginSection("PARTS");
  {
    const Song& pv = previewSong();
    for (int p = 0; p < ls::kPartCount; ++p) {
      ImGui::PushID(p);
      bool locked = s.locked[static_cast<size_t>(p)];
      if (ui::ToggleIcon("##lock", ui::Icon::Lock, locked, 24, "Lock: re-roll all leaves this part alone"))
        s.locked[static_cast<size_t>(p)] = !locked;
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(a.fontBold, theme::kFontBody);
      ImGui::TextUnformatted(i18n::tr(ls::partName(p)));
      ImGui::PopFont();
      ImGui::SameLine(96);
      partStrip(pv, p, ImVec2(200, 24));
      ImGui::SameLine();
      ImGui::BeginGroup();
      std::string snd;
      const auto sounds = ls::chosenSounds(s, p);
      if (p == ls::kDrums) {
        for (size_t k = 0; k < sounds.size() && k < 3; ++k) snd += (k ? ", " : "") + (sounds[k].preset.empty() ? soundText(sounds[k]) : sounds[k].preset);
        snd += ", …";
      } else if (!sounds.empty()) {
        snd = soundText(sounds[0]);
      }
      ui::TextFit(snd.c_str(), ImGui::GetContentRegionAvail().x - 100, P.text);
      ImGui::PushFont(a.fontMono, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "#%08X", s.seeds[static_cast<size_t>(p)]);
      ImGui::PopFont();
      ImGui::EndGroup();
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 84);
      ImGui::BeginDisabled(locked);
      if (ui::TextButton("Re-roll", false, 84, "New notes (and maybe a new sound) for this part")) {
        ls::reroll(s, p, ++L.entropy * 2654435761u + static_cast<uint32_t>(a.now * 1000.0));
        commit = true;
        undoLabel = std::string("loop starter: re-roll ") + ls::partName(p);
        for (auto& c : undoLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  ui::EndSection();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     i18n::tr("Gridline instruments only. Each part has its own seed: the same settings build the same project. Bass and lead follow the chords."));
  ImGui::PopTextWrapPos();
  ImGui::EndChild();

  // ---- footer
  ui::DialogFooter(ImGui::GetContentRegionAvail().x);
  ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
  if (ui::TextButton("Re-roll all", false, 96, "New seeds for every unlocked part")) {
    ls::reroll(s, -1, ++L.entropy * 2246822519u + static_cast<uint32_t>(a.now * 1000.0));
    commit = true, undoLabel = "loop starter: re-roll";
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!L.applied);
  const bool playing = a.engine.playing();
  if (ui::TextButton(playing ? "Stop" : "Play", playing, 72, "Hear the project the loop starter made")) {
    if (playing) a.engine.stop();
    else a.engine.setMode(PlayMode::Song), a.engine.play();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const float bw = 96;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui::ButtonRowWidth(3, bw));
  if (ui::TextButton("Close", false, bw) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
    L.open = false;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Apply", false, bw, "Replace the current project with this one (undo brings it back)")) applyTo(a, undoLabel);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Create", true, bw, "A new project from these settings")) createProject(a);
  ImGui::End();
  // Finished changes re-apply while the project made here is untouched.
  if (commit && s != before && live(a)) applyTo(a, undoLabel);
}

}  // namespace

void openLoopStarter(App& a) {
  (void)a;
  if (!L.init) {
    L.set = ls::styleDefaults(0, static_cast<uint32_t>(std::time(nullptr)));
    L.init = true;
  }
  L.open = true;
}

void createFromLoopStarter(App& a, int style) {
  openLoopStarter(a);
  const ls::Settings d = ls::styleDefaults(style, 2026);
  L.set = d;
  createProject(a);
}

void drawLoopStarter(App& a) { window(a); }

void drawDialogs(App& a) {
  drawLoopStarter(a);
  drawRenderSampler(a);
}

}  // namespace gl::j59
