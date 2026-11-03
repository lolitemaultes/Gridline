// Gridline: seeded stress run of the running app. See DevStress.h.
#include "DevStress.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "../../third_party/imgui/imgui.h"
#include "../core/PluginInfo.h"
#include "../core/Project.h"
#include "App.h"
#include "ExportUi.h"
#include "MenuParityUi.h"

namespace gl::devstress {

namespace {

struct Run {
  bool on = false;
  std::mt19937 rng;
  int frames = 0, perFrame = 1, done = 0, step = 0;
  std::string folder, groups = "mceptrui", project;
  std::vector<std::string> gens, fxs;
  bool mouseDown = false;
  int mouseButton = 0;
  long exports = 0;
} R;

int pick(int n) { return n <= 0 ? 0 : static_cast<int>(R.rng() % static_cast<unsigned>(n)); }
bool chance(int percent) { return pick(100) < percent; }

template <class V>
auto* pickFrom(V& v) {
  return v.empty() ? nullptr : &v[static_cast<size_t>(pick(static_cast<int>(v.size())))];
}

void say(const std::string& what, long long x = 0, long long y = 0) {
  std::printf("stress %d: %s %lld %lld\n", R.step, what.c_str(), x, y);
  std::fflush(stdout);
}

void modelOp(App& a) {
  const Song& s = a.doc.song();
  switch (pick(6)) {
    case 0:
      if (auto* c = pickFrom(s.channels)) {
        say("rename channel", static_cast<long long>(c->id));
        a.doc.renameChannel(c->id, "ch " + std::to_string(pick(1000)));
      }
      break;
    case 1:
      if (auto* c = pickFrom(s.channels)) {
        const ObjectId id = c->id;
        const int k = pick(64);
        const float v = static_cast<float>(pick(1001)) / 1000.0f;
        say("channel param", static_cast<long long>(id), k);
        a.doc.editChannel(id, "knob", [&](Channel& x) {
          if (!x.plugin.params.empty()) x.plugin.params[static_cast<size_t>(k) % x.plugin.params.size()] = v;
        });
      }
      break;
    case 2: {
      const int bpm = 40 + pick(200);
      say("tempo", bpm);
      a.doc.editSong("tempo", [&](Song& x) { x.tempo = bpm; });
      break;
    }
    case 3: {
      const int n = 1 + pick(12);
      const int d = 1 << (1 + pick(3));
      say("time signature", n, d);
      a.doc.setTimeSignature(n, d);
      break;
    }
    case 4:
      if (s.mixer.size() > 1) {
        auto* m = pickFrom(s.mixer);
        const float v = static_cast<float>(pick(1001)) / 1000.0f;
        say("mixer volume", static_cast<long long>(m->id));
        a.doc.editMixerTrack(m->id, "volume", [&](MixerTrack& x) { x.volume = v; });
      }
      break;
    default: {
      const int steps = 1 + pick(5);
      const bool undo = chance(55);
      say(undo ? "undo" : "redo", steps);
      for (int k = 0; k < steps; ++k) (void)(undo ? a.doc.undo() : a.doc.redo());
      break;
    }
  }
}

void channelOp(App& a) {
  const Song& s = a.doc.song();
  switch (pick(5)) {
    case 0:
      if (auto* t = pickFrom(R.gens); t && s.channels.size() < 40) {
        say("add channel " + *t);
        a.addInstrument(*t);
      }
      break;
    case 1:
      if (auto* c = pickFrom(s.channels)) {
        say("remove channel", static_cast<long long>(c->id));
        a.doc.removeChannel(c->id);
      }
      break;
    case 2:
      if (auto* c = pickFrom(s.channels); c && s.channels.size() < 40) {
        say("clone channel", static_cast<long long>(c->id));
        a.doc.cloneChannel(c->id);
      }
      break;
    case 3:
      if (auto* c = pickFrom(s.channels)) {
        if (auto* t = pickFrom(R.gens)) {
          say("replace instrument " + *t, static_cast<long long>(c->id));
          a.doc.setChannelPlugin(c->id, *t);
        }
      }
      break;
    default:
      if (auto* c = pickFrom(s.channels)) {
        say("select channel", static_cast<long long>(c->id));
        a.selectChannel(c->id, chance(30));
      }
      break;
  }
}

void effectOp(App& a) {
  const Song& s = a.doc.song();
  switch (pick(5)) {
    case 0:
      if (s.mixer.size() < 30) {
        say("add mixer track");
        a.doc.addMixerTrack();
      }
      break;
    case 1:
      if (s.mixer.size() > 1) {
        const auto& m = s.mixer[static_cast<size_t>(1 + pick(static_cast<int>(s.mixer.size()) - 1))];
        say("remove mixer track", static_cast<long long>(m.id));
        a.doc.removeMixerTrack(m.id);
      }
      break;
    case 2:
    case 3:
      if (auto* m = pickFrom(s.mixer)) {
        const int slot = pick(10);
        const bool clear = chance(30);
        if (auto* t = pickFrom(R.fxs)) {
          say(clear ? std::string("clear slot") : "insert " + *t, static_cast<long long>(m->id), slot);
          a.doc.setSlotPlugin(m->id, slot, clear ? std::string() : *t);
        }
      }
      break;
    default:
      if (s.mixer.size() > 1) {
        auto* from = pickFrom(s.mixer);
        auto* to = pickFrom(s.mixer);
        say("send", static_cast<long long>(from->id), static_cast<long long>(to->id));
        a.doc.setSend(from->id, to->id, static_cast<float>(pick(100)) / 100.0f);
      }
      break;
  }
}

void patternOp(App& a) {
  const Song& s = a.doc.song();
  switch (pick(8)) {
    case 0:
      if (s.patterns.size() < 40) {
        say("add pattern");
        a.selectPattern(a.doc.addPattern("p" + std::to_string(pick(1000)), 1 + pick(4)));
      }
      break;
    case 1:
      if (s.patterns.size() > 1) {
        auto* p = pickFrom(s.patterns);
        say("remove pattern", static_cast<long long>(p->id));
        a.doc.removePattern(p->id);
      }
      break;
    case 2:
      if (auto* p = pickFrom(s.patterns); p && s.patterns.size() < 40) {
        say("clone pattern", static_cast<long long>(p->id));
        a.doc.clonePattern(p->id);
      }
      break;
    case 3:
      if (auto* p = pickFrom(s.patterns)) {
        if (auto* c = pickFrom(s.channels)) {
          std::vector<Note> ns;
          const int n = pick(64);
          for (int k = 0; k < n; ++k) ns.emplace_back(static_cast<Tick>(pick(3840)), static_cast<Tick>(1 + pick(480)), 24 + pick(84), 20 + pick(100));
          say("set notes", static_cast<long long>(p->id), n);
          a.doc.setNotes(p->id, c->id, std::move(ns));
        }
      }
      break;
    case 4:
      if (auto* p = pickFrom(s.patterns)) {
        if (auto* c = pickFrom(s.channels)) {
          const int st = pick(64);
          say("step", static_cast<long long>(p->id), st);
          a.doc.setStep(p->id, c->id, st, static_cast<uint8_t>(pick(128)));
        }
      }
      break;
    case 5:
      if (auto* p = pickFrom(s.patterns); p && s.playlist.size() < 200) {
        say("add clip", static_cast<long long>(p->id));
        a.doc.addClip(p->id, pick(40), static_cast<Tick>(pick(64)) * s.ppq);
      }
      break;
    case 6:
      if (auto* c = pickFrom(s.playlist)) {
        say("remove clip", static_cast<long long>(c->id));
        a.doc.removeClip(c->id);
      }
      break;
    default:
      if (auto* p = pickFrom(s.patterns)) {
        say("select pattern", static_cast<long long>(p->id));
        a.selectPattern(p->id);
      }
      break;
  }
}

void transportOp(App& a) {
  switch (pick(6)) {
    case 0:
    case 1:
      say("play");
      menuparity::playPressed(a);
      break;
    case 2:
      say("stop");
      menuparity::stopPressed(a);
      break;
    case 3:
      say("start song");
      a.startPlayback();
      break;
    case 4:
      say("seek");
      a.engine.setPosition(static_cast<double>(pick(64)) * a.doc.song().ppq);
      break;
    default:
      say("note on", 60);
      if (auto* c = pickFrom(a.doc.song().channels)) a.engine.noteOn(c->id, 36 + pick(60), 0.8f);
      a.engine.releaseLiveNotes();
      break;
  }
}

void renderOp(App& a, bool finish) {
  switch (finish ? 0 : pick(4)) {
    case 0:
    case 1: {
      exportui::DevRender r;
      r.formats = 1 + pick(31);
      r.scope = pick(2);
      r.dest = chance(80) ? 0 : 1 + pick(2);
      r.split = pick(4);
      r.sampleRate = pick(7);
      if (finish) {  // short ones, so that many reach the writers and encoders during a run
        r.scope = 1;
        r.split = 0;
        r.sampleRate = pick(2);
      }
      r.folder = R.folder + "/out" + std::to_string(R.exports++ % 4);
      say("render", r.formats, r.scope * 100 + r.dest * 10 + r.split);
      exportui::devStart(a, r);
      break;
    }
    case 2:
      say("render cancel");
      exportui::devCancel();
      break;
    default:
      say("render dialog");
      exportui::openRender(a, static_cast<exportui::Preset>(pick(8)));
      break;
  }
}

void uiOp(App& a) {
  const Song& s = a.doc.song();
  switch (pick(8)) {
    case 0:
      if (auto* c = pickFrom(s.channels)) {
        say("open instrument editor", static_cast<long long>(c->id));
        a.settingsChannel = c->id;
      }
      break;
    case 1:
      say("close instrument editor");
      a.settingsChannel = kNoId;
      break;
    case 2:
      if (auto* m = pickFrom(s.mixer)) {
        const int slot = pick(10);
        say("open effect editor", static_cast<long long>(m->id), slot);
        a.fxEditor = {m->id, slot};
      }
      break;
    case 3:
      say("close effect editor");
      a.fxEditor = {kNoId, -1};
      break;
    case 4:
      if (auto* c = pickFrom(s.channels)) {
        say("open piano roll", static_cast<long long>(c->id));
        a.openPianoRoll(c->id);
      }
      break;
    case 5: {
      const int k = pick(5);
      say("toggle panel", k);
      bool* panels[] = {&a.showRack, &a.showPiano, &a.showPlaylist, &a.showMixer, &a.showHistory};
      *panels[k] = !*panels[k];
      break;
    }
    case 6: {
      const int k = pick(3);
      say("toggle window", k);
      bool* w[] = {&a.showPerformance, &a.showMaster, &a.showInfo};
      *w[k] = !*w[k];
      break;
    }
    default:
      say("open mixer");
      a.showMixer = true;
      break;
  }
}

// Random pointer and key input over the panels; never the top bar's menus or the browser.
void inputOp() {
  ImGuiIO& io = ImGui::GetIO();
  if (R.mouseDown) {
    io.AddMousePosEvent(static_cast<float>(40 + pick(1360)), static_cast<float>(70 + pick(780)));
    io.AddMouseButtonEvent(R.mouseButton, false);
    R.mouseDown = false;
    say("mouse up");
    return;
  }
  switch (pick(6)) {
    case 0:
    case 1:
    case 2: {
      const float x = static_cast<float>(40 + pick(1360)), y = static_cast<float>(70 + pick(780));
      R.mouseButton = chance(70) ? 0 : 1;
      io.AddMousePosEvent(x, y);
      io.AddMouseButtonEvent(R.mouseButton, true);
      R.mouseDown = true;
      say("mouse down", static_cast<long long>(x), static_cast<long long>(y));
      break;
    }
    case 3:
      say("wheel");
      io.AddMouseWheelEvent(0, static_cast<float>(pick(7) - 3));
      break;
    case 4: {
      static const ImGuiKey keys[] = {ImGuiKey_Delete, ImGuiKey_Escape, ImGuiKey_Enter, ImGuiKey_Space, ImGuiKey_LeftArrow,
                                      ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab};
      const ImGuiKey k = keys[pick(9)];
      say("key", static_cast<long long>(k));
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
      break;
    }
    default: {
      const ImGuiKey k = static_cast<ImGuiKey>(ImGuiKey_A + pick(26));
      say("letter", static_cast<long long>(k));
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
      break;
    }
  }
}

}  // namespace

void start(const std::vector<std::string>& args) {
  if (args.size() < 4) return;
  R.rng.seed(static_cast<unsigned>(std::strtoul(args[0].c_str(), nullptr, 10)));
  R.frames = std::atoi(args[1].c_str());
  R.perFrame = std::max(1, std::atoi(args[2].c_str()));
  R.folder = args[3];
  if (args.size() > 4) R.groups = args[4];
  if (args.size() > 5) R.project = args[5];
  R.gens.clear();
  R.fxs.clear();
  for (const auto& p : pluginCatalog()) {
    if (p.hidden || isHostedType(p.type)) continue;
    if (p.kind == PluginKind::Generator) R.gens.push_back(p.type);
    else if (p.kind == PluginKind::Effect) R.fxs.push_back(p.type);
  }
  R.done = 0;
  R.on = true;
}

bool active() { return R.on; }

void frame(App& a) {
  if (!R.on) return;
  // Dialogs that reach the disk or the network stay out of the run.
  a.fileDialog.open = false;
  a.showSettings = a.showCollab = a.showJoin = false;
  if (R.done++ >= R.frames) {
    R.on = false;
    exportui::devCancel();
    std::printf("stress done: %d frames, %d actions; %s\n", R.frames, R.step, exportui::devState().c_str());
    std::fflush(stdout);
    a.doc.markSaved();
    a.quitRequested = true;
    return;
  }
  for (int k = 0; k < R.perFrame; ++k) {
    ++R.step;
    const char g = R.groups[static_cast<size_t>(pick(static_cast<int>(R.groups.size())))];
    switch (g) {
      case 'm': modelOp(a); break;
      case 'c': channelOp(a); break;
      case 'e': effectOp(a); break;
      case 'p': patternOp(a); break;
      case 't': transportOp(a); break;
      case 'r': renderOp(a, false); break;
      case 'x': renderOp(a, true); break;
      case 'u': uiOp(a); break;
      case 'i': inputOp(); break;
      case 'l':
        if (!R.project.empty() && chance(10)) {
          say("reopen project");
          a.doc.markSaved();
          a.openProject(R.project);
        }
        break;
      default: break;
    }
    a.syncEngine();
  }
}

}  // namespace gl::devstress
