// Gridline - drum kits loaded as drum pad channels (DrumKitUi.h).
#include "DrumKitUi.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../core/HydrogenKit.h"
#include "../core/I18n.h"
#include "../core/ProjectArchive.h"
#include "../engine/Slicing.h"
#include "App.h"
#include "BrowserUi.h"
#include "CacheUi.h"
#include "DialogLayout.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::drumkitui {

namespace fs = std::filesystem;
namespace h2 = gl::hydrogen;
using theme::pal;

namespace {

// Reads a kit from a file or folder; a packed kit is unpacked into the local cache first (an
// unchanged one finds its earlier copy). Blocks: runs on a worker.
bool readAny(const std::string& path, h2::Kit& kit, archive::CopyHold& hold, std::string* err) {
  if (!h2::isKitArchive(path)) return h2::readKit(path, kit, err);
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) {
    if (err) *err = "cannot read the file";  // (a worker: no translation here)
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  archive::MakeRequest mr;
  mr.sourcePath = path;
  mr.size = bytes.size();
  mr.write = [&bytes](const std::string& folder, std::vector<std::string>& names, std::string* we) { return h2::unpackKit(bytes, folder, names, we); };
  archive::UnpackResult ur;
  if (!archive::makeCopy(mr, ur, err)) return false;
  hold = ur.use;
  return h2::readKit(ur.folder, kit, err);
}

// A layer to preview per instrument: the loudest of its first component.
std::string previewOf(const h2::Kit& kit, const h2::Instrument& in) {
  for (const auto& c : in.components) {
    const h2::Layer* best = nullptr;
    for (const auto& l : c.layers)
      if (!l.file.empty() && !l.muted && (!best || l.max > best->max)) best = &l;
    if (best) return h2::samplePath(kit, best->file);
  }
  return {};
}

// The kit's info is often a little HTML: its text, with blank runs folded.
std::string plainText(const std::string& in) {
  std::string out;
  bool tag = false, space = false;
  for (char c : in) {
    if (c == '<') {
      tag = true;
      space = true;
      continue;
    }
    if (tag) {
      tag = c != '>';
      continue;
    }
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  if (out.size() > 400) out = out.substr(0, 397) + "...";
  return out;
}

// ---------------------------------------------------------------- loading

struct LoadJob {
  std::string path;
  ObjectId replace = kNoId;
  int insertAt = -1;
  std::atomic<bool> done{false};
  bool ok = false;
  std::string error;
  h2::Kit kit;
  h2::PadsImport pads;
  archive::CopyHold hold;
};
std::shared_ptr<LoadJob>& job() {
  static auto* j = new std::shared_ptr<LoadJob>;
  return *j;
}

struct Report {
  bool open = false, popup = false, closeReq = false;
  std::string kitName, info, error;
  std::vector<std::pair<std::string, std::string>> credits;
  std::vector<std::string> notes;
  int instruments = 0, layers = 0, channels = 0;
};
Report& report() {
  static auto* r = new Report;
  return *r;
}

std::string fmt1(const char* f, const std::string& a) {
  const std::string t = i18n::tr(f);
  const size_t at = t.find("%s");
  return at == std::string::npos ? t + " " + a : t.substr(0, at) + a + t.substr(at + 2);
}

void apply(App& a, LoadJob& j) {
  if (!j.ok) {
    a.notify(std::string(i18n::tr("Cannot load the drum kit")) + ": " + j.error, true);
    return;
  }
  const h2::PadsImport& p = j.pads;
  const Song& s = a.doc.song();
  ObjectId first = kNoId;
  a.doc.beginGroup("load drum kit");
  size_t k = 0;
  if (j.replace != kNoId && s.channel(j.replace) && !p.channels.empty()) {
    const ObjectId ch = j.replace;
    bool ok = s.channel(ch)->plugin.type == "gl.pads" || a.doc.setChannelPlugin(ch, "gl.pads");
    if (ok)
      a.doc.editChannel(ch, "", [&](Channel& c) {
        const std::string ins = c.plugin.ins;
        c.plugin = p.channels[0];
        c.plugin.ins = ins;
        c.name = p.names[0];
      });
    first = ch;
    k = 1;
  }
  int at = j.insertAt;
  for (; k < p.channels.size(); ++k) {
    const ObjectId id = a.addInstrument("gl.pads");
    if (id == kNoId) break;
    if (at >= 0) a.doc.moveChannel(id, std::min(at++, static_cast<int>(a.doc.song().channels.size()) - 1));
    const PluginState st = p.channels[k];
    const std::string name = p.names[k];
    a.doc.editChannel(id, "", [&](Channel& c) {
      c.plugin = st;
      c.name = name;
    });
    if (first == kNoId) first = id;
  }
  a.doc.endGroup();
  if (first == kNoId) {
    a.notify(i18n::tr("Cannot load the drum kit"), true);
    return;
  }
  cacheui::projectUsesMore(a);  // a packed kit's samples live in the cache: kept while the project uses them
  a.selectChannel(first);
  a.showNewInstrument(first);
  a.notify(std::string(i18n::tr("Loaded the drum kit ")) + p.names[0]);
  Report& r = report();
  r = Report{};
  r.open = r.popup = true;
  r.kitName = j.kit.name.empty() ? fs::u8path(j.path).stem().u8string() : j.kit.name;
  r.info = plainText(j.kit.info);
  for (const auto& [label, value] : h2::creditLines(j.kit)) r.credits.emplace_back(i18n::tr(label), value);
  for (const auto& n : p.notes) r.notes.push_back(fmt1(n.text, n.arg));
  r.instruments = p.instruments, r.layers = p.layers, r.channels = static_cast<int>(p.channels.size());
}

void drawReport(App& a) {
  Report& r = report();
  if (!r.open) return;
  if (r.popup) {
    ImGui::OpenPopup("##drumkitreport");
    r.popup = false;
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
  const bool open = ImGui::BeginPopupModal("##drumkitreport", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) {
    r.open = false;
    return;
  }
  const auto& P = pal();
  constexpr float kW = 380.0f, kButtonW = 112.0f, kLabelW = 96.0f;
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Drum kit loaded"));
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(theme::fitText(r.kitName.c_str(), kW));
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(kW, theme::kS1));
  for (const auto& [label, value] : r.credits) {
    const float x = ImGui::GetCursorPosX();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(label.c_str(), kLabelW - theme::kS2));
    ImGui::SameLine(x + kLabelW);
    ImGui::PushTextWrapPos(x + kW);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
  }
  {
    char line[160];
    std::snprintf(line, sizeof line, i18n::tr("%d instruments, %d sample layers, on %d drum pad channels"), r.instruments, r.layers, r.channels);
    if (r.channels == 1) std::snprintf(line, sizeof line, i18n::tr("%d instruments, %d sample layers, on one drum pad channel"), r.instruments, r.layers);
    const float x = ImGui::GetCursorPosX();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Loaded"));
    ImGui::SameLine(x + kLabelW);
    ImGui::PushTextWrapPos(x + kW);
    ImGui::TextUnformatted(line);
    ImGui::PopTextWrapPos();
  }
  if (!r.info.empty()) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
    ImGui::TextColored(theme::vec(P.textDim), "%s", r.info.c_str());
    ImGui::PopTextWrapPos();
  }
  if (!r.notes.empty()) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
    for (const auto& n : r.notes) ImGui::TextColored(theme::vec(P.hot), "%s", n.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kW - ui::ButtonRowWidth(1, kButtonW));
  if (ui::PrimaryButton("OK", kButtonW) || keys::escapePressed() || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || r.closeReq) {
    r.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------- browser summaries

struct SummaryJob {
  std::atomic<bool> done{false};
  Summary s;
  archive::CopyHold hold;  // a packed kit's copy stays while it is previewed
};
std::map<std::string, std::shared_ptr<SummaryJob>>& summaries() {
  static auto* m = new std::map<std::string, std::shared_ptr<SummaryJob>>;
  return *m;
}

}  // namespace

void load(App& a, const std::string& path, ObjectId replace, int insertAt) {
  if (loading()) {
    a.notify(i18n::tr("A drum kit is still loading"), true);
    return;
  }
  auto j = std::make_shared<LoadJob>();
  j->path = path, j->replace = replace, j->insertAt = insertAt;
  job() = j;
  a.notify(std::string(i18n::tr("Reading the drum kit ")) + fs::u8path(path).filename().u8string());
  std::thread([j] {
    std::string err;
    j->ok = readAny(j->path, j->kit, j->hold, &err);
    if (j->ok) j->pads = h2::kitToPads(j->kit);
    if (j->ok && j->pads.channels.empty()) j->ok = false, err = "the kit has no instruments";
    j->error = err;
    j->done.store(true, std::memory_order_release);
  }).detach();
}

bool loading() { return job() && !job()->done.load(std::memory_order_acquire); }

void draw(App& a) {
  if (job() && job()->done.load(std::memory_order_acquire)) {
    std::shared_ptr<LoadJob> j = std::move(job());
    job().reset();
    apply(a, *j);
  }
  drawReport(a);
}

const Summary* summary(const std::string& path) {
  auto& m = summaries();
  auto it = m.find(path);
  if (it == m.end()) {
    if (m.size() > 8) {  // keep the newest few (their copies are released with them)
      for (auto e = m.begin(); e != m.end();)
        e = e->second->done.load(std::memory_order_acquire) ? m.erase(e) : std::next(e);
    }
    auto j = std::make_shared<SummaryJob>();
    it = m.emplace(path, j).first;
    std::thread([j, path] {
      h2::Kit kit;
      std::string err;
      Summary& s = j->s;
      s.ok = readAny(path, kit, j->hold, &err);
      s.error = err;
      if (s.ok) {
        s.name = kit.name, s.author = kit.author, s.license = kit.license;
        for (const auto& in : kit.instruments) {
          s.instruments.push_back(in.name);
          s.previews.push_back(previewOf(kit, in));
          for (const auto& c : in.components) s.layers += static_cast<int>(c.layers.size());
        }
      }
      j->done.store(true, std::memory_order_release);
    }).detach();
  }
  return it->second->done.load(std::memory_order_acquire) ? &it->second->s : nullptr;
}

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.size() >= 2 && args[0] == "load") {
    load(a, args[1]);
    return true;
  }
  if (!args.empty() && args[0] == "report") {  // the report of the last load, and the channels it made
    const Report& r = report();
    std::printf("drumkit report: open %d name \"%s\" instruments %d layers %d channels %d notes %zu loading %d\n", r.open ? 1 : 0, r.kitName.c_str(),
                r.instruments, r.layers, r.channels, r.notes.size(), loading() ? 1 : 0);
    for (const auto& [l, v] : r.credits) std::printf("  credit %s: %s\n", l.c_str(), v.c_str());
    for (const auto& n : r.notes) std::printf("  note %s\n", n.c_str());
    for (const auto& c : a.doc.song().channels)
      if (c.plugin.type == "gl.pads")
        std::printf("  channel \"%s\" data %zu bytes pad1 %s\n", c.name.c_str(), c.plugin.data.size(), padSamplePath(c.plugin.data, 0, 0).c_str());
    std::fflush(stdout);
    return true;
  }
  if (args.size() >= 2 && args[0] == "summary") {  // what the browser shows of a kit
    const Summary* s = summary(args[1]);
    std::printf("drumkit summary: %s", !s ? "reading\n" : s->ok ? "ok" : ("error " + s->error + "\n").c_str());
    if (s && s->ok) {
      int previews = 0;
      for (const auto& p : s->previews) previews += !p.empty() && fs::is_regular_file(fs::u8path(p)) ? 1 : 0;
      std::printf(" name \"%s\" license \"%s\" instruments %zu previews %d layers %d\n", s->name.c_str(), s->license.c_str(), s->instruments.size(), previews,
                  s->layers);
    }
    std::fflush(stdout);
    return true;
  }
  if (!args.empty() && args[0] == "close") {
    report().closeReq = true;
    return true;
  }
  return false;
}

}  // namespace gl::drumkitui
