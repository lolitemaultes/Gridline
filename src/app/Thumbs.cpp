// Gridline — plugin editor thumbnails for the plugin picker. A child process
// (--make-thumbs, off-screen) opens every built-in instrument / effect on the
// default project, draws its editor once in a bare window at the top-left,
// reads those pixels back and stores a scaled PNG in <config>/thumbs/. The
// picker loads them lazily as textures.
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>
#include "../core/Png.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "BrowserUi.h"
#include "LayoutAudit.h"
#include "PluginUI.h"
#include "Theme.h"
#include "../core/Platform.h"
#include "../packs/modulation/modulation_ui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::browserui {

namespace fs = std::filesystem;

namespace {

std::string readText(const char* path) {  // a whole small text file ("" when missing)
  std::string s;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}

std::string safeName(const std::string& type) {
  std::string s;
  for (char c : type) s += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_';
  return s;
}

struct Tex {
  GLuint id = 0;
  ImVec2 size;
  bool tried = false;
};
std::map<std::string, Tex> textures;
std::atomic<uint64_t> texGeneration{0};
uint64_t seenGeneration = 0;

std::atomic<bool> runActive{false};

// ---- --make-thumbs mode ----
struct ThumbRun {
  bool on = false;
  std::vector<std::string> types;
  size_t idx = 0;
  int phase = 0;
  PluginTarget target;
  ObjectId channel = kNoId;
  ImVec2 pos, size;  // editor window rect (logical px)
  bool drawn = false;
  bool setup = false;  // the current plugin's channel / slot exists
  int written = 0;
  // Q-7 --layout-audit: every editor at three widths, issues to a TSV and contact sheets.
  std::string auditDir;
  int factor = 0;  // index into kAuditFactors
  audit::Report report;
  std::vector<png::Image> cells;
  std::string tsv;
  int editorsWithIssues = 0, issueCount = 0;
  // J-128 alignment findings (AlignCheck.h): alignment.tsv, per width.
  std::string alignTsv;
  int alignCount[3] = {0, 0, 0}, alignEditors = 0;
  std::vector<align::Count> alignCounts;
  // J-92: parameter bindings (reads / bound controls per parameter) at width x1,
  // over the editor as drawn and with every choice / switch value in turn
  // (pages, modes).
  bool noteFx = false;
  audit::Bindings bind;
  std::string bindTsv;
  // --binding-crawl: after the three widths, every item of the editor is
  // clicked once (pages, tabs, selectors) while the bindings keep counting.
  bool crawl = false, crawling = false, crawled = false, collect = false;
  std::vector<ImVec2> clicks;
  std::vector<ImGuiID> clickIds;
  size_t crawlIdx = 0;
  int crawlStep = 0;
};
constexpr float kAuditFactors[3] = {1.0f, 1.5f, 2.0f};
ThumbRun run;

}  // namespace

std::string thumbPath(App& a, const std::string& type) {
  return (fs::path(a.configDir()) / "thumbs" / (safeName(type) + ".png")).string();
}

ImTextureID pluginThumb(App& a, const std::string& type, ImVec2* size) {
  if (texGeneration.load() != seenGeneration) {  // a thumbnail run finished: reload
    seenGeneration = texGeneration.load();
    for (auto& [t, x] : textures)
      if (!x.id) x.tried = false;
  }
  Tex& t = textures[type];
  if (!t.tried) {
    t.tried = true;
    png::Image img;
    if (png::readFile(thumbPath(a, type), img)) {
      std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4, 255);
      for (size_t i = 0, n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height); i < n; ++i)
        for (size_t c = 0; c < static_cast<size_t>(img.channels) && c < 4; ++c) rgba[i * 4 + c] = img.pixels[i * static_cast<size_t>(img.channels) + c];
      GLint last = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
      glGenTextures(1, &t.id);
      glBindTexture(GL_TEXTURE_2D, t.id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
      t.size = ImVec2(static_cast<float>(img.width), static_cast<float>(img.height));
    }
  }
  if (size) *size = t.size;
  return t.id ? static_cast<ImTextureID>(t.id) : ImTextureID_Invalid;
}

void startThumbnailRun(App& a) {
  if (runActive.exchange(true)) return;
  const std::string exe = host::selfExecutablePath();
  const fs::path dir = fs::path(a.configDir()) / "thumbs";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string frame = (dir / ".frame.ppm").string();
  const std::vector<std::string> args = {exe, "--make-thumbs", "--size", "1400x1000", "--screenshot", frame, "--frames", "100000"};
  a.notify("Rendering plugin thumbnails in the background");
  std::thread([exe, args]() {
    platform::Process p;
    if (p.start(exe, args)) p.wait();
    ++texGeneration;
    runActive = false;
  }).detach();
}

bool thumbnailRunActive() { return runActive.load(); }

// ------------------------------------------------------------ thumb mode

void beginThumbMode(App& a, const std::string& onlyType) {
  run = ThumbRun{};
  run.on = true;
  for (const auto& p : pluginCatalog())
    if (p.kind != PluginKind::NoteFx && (onlyType.empty() || p.type == onlyType)) run.types.push_back(p.type);
  std::error_code ec;
  fs::create_directories(fs::path(a.configDir()) / "thumbs", ec);
  a.showBrowser = a.showRack = a.showPiano = a.showPlaylist = a.showMixer = false;
}

void beginAuditMode(App& a, const std::string& dir) {
  beginThumbMode(a);
  for (const auto& p : pluginCatalog())  // J-92: note processors too (binding audit)
    if (p.kind == PluginKind::NoteFx) run.types.push_back(p.type);
  if (const char* only = std::getenv("GRIDLINE_AUDIT_ONLY")) {  // J-128 dev aid: audit only these types (comma separated)
    const std::string list = std::string(",") + only + ",";
    run.types.erase(std::remove_if(run.types.begin(), run.types.end(), [&](const std::string& t) { return list.find("," + t + ",") == std::string::npos; }),
                    run.types.end());
  }
  run.bindTsv = "type\tindex\tparam\treads\tcontrols\tstate_reads\n";
  run.auditDir = dir;
  std::error_code ec;
  fs::create_directories(dir, ec);
  run.tsv = "type\twidth_factor\tissues\tcontrol\tframe\tgap_px\tside\n";
  run.alignTsv = "type\twidth_factor\trule\toff_px\tat\tdetail\n";
}

bool thumbMode() { return run.on; }

void setBindingCrawl(bool on) { run.crawl = on; }

namespace {
// Item hook (ImGuiItemHook.h): items of the editor window, collected on its first draw.
void onItem(ImGuiID id, ImVec2 a, ImVec2 b) {
  audit::item(id, a, b);  // J-128: alignment check (no-op unless a collection is open)
  if (!run.collect) return;
  ImGuiContext* g = ImGui::GetCurrentContext();
  ImGuiWindow* w = g ? g->CurrentWindow : nullptr;
  if (!w || !w->RootWindow || std::strcmp(w->RootWindow->Name, "##thumbgen") != 0 || id == w->ID) return;
  if (b.x - a.x < 3 || b.y - a.y < 3) return;
  if (std::find(run.clickIds.begin(), run.clickIds.end(), id) != run.clickIds.end()) return;
  run.clickIds.push_back(id);
  run.clicks.push_back(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f));
}

// One step of the crawl per frame: press + release queued (ImGui trickles them
// over two frames), then two frames for the editor to show what the click opened.
void crawlStep(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  if (run.crawlIdx >= run.clicks.size()) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    run.crawled = true;
    return;
  }
  if (run.crawlStep == 0) {
    io.AddMousePosEvent(run.clicks[run.crawlIdx].x, run.clicks[run.crawlIdx].y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  }
  if (++run.crawlStep >= 4) {
    a.fileDialog.open = false;  // "Load…" buttons: no dialogs in an audit run
    run.crawlStep = 0;
    ++run.crawlIdx;
  }
}

// The editor of the run's current plugin.
void drawBody(App& a, float width) {
  if (run.noteFx) packs::modulation::drawChannelNoteFx(a, run.channel, width);
  else drawPluginBody(a, run.target, width);
}

ParamId runParam(int k) { return run.noteFx ? noteFxParam(run.channel, 0, k) : run.target.pid(k); }

void mergeBindings(const audit::Bindings& b) {
  for (const auto& [k, n] : b.reads) run.bind.reads[k] += n;
  for (const auto& [k, n] : b.controls) run.bind.controls[k] = std::max(run.bind.controls[k], n);
  for (const auto& [k, n] : b.stateReads) run.bind.stateReads[k] += n;
}

// J-92: the editor once more for every value of each choice / switch
// parameter (pages, modes, sections shown on demand), off-screen.
void drawVariants(App& a, const PluginInfo& info, float width) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x + 50, vp->Pos.y));
  ImGui::SetNextWindowSize(ImVec2(width + 16, vp->Size.y));
  if (ImGui::Begin("##bindvariants", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
    int drawn = 0;
    for (size_t k = 0; k < info.params.size() && drawn < 1500; ++k) {
      const ParamDesc& d = info.params[k];
      if (d.kind == ParamKind::Continuous) continue;
      const ParamId pid = runParam(static_cast<int>(k));
      const auto cur = getParam(a.doc.song(), pid);
      if (!cur) continue;
      const int count = d.kind == ParamKind::Toggle ? 2 : static_cast<int>(std::min<size_t>(d.choices.size(), 16));
      for (int v = 0; v < count; ++v) {
        if (v == static_cast<int>(*cur + 0.5)) continue;
        a.doc.setParam(pid, v, false);
        ImGui::PushID(static_cast<int>(k * 64 + static_cast<size_t>(v)));
        ImGui::BeginChild("##v", ImVec2(width + 8, vp->Size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        audit::bindBegin();
        drawBody(a, width);
        mergeBindings(audit::bindEnd());
        ImGui::EndChild();
        ImGui::PopID();
        ++drawn;
      }
      a.doc.setParam(pid, *cur, false);
    }
  }
  ImGui::End();
}

void writeBindings(const std::string& type) {
  const PluginInfo* info = findPlugin(type);
  if (!info) return;
  for (size_t k = 0; k < info->params.size(); ++k) {
    const uint64_t key = runParam(static_cast<int>(k)).packed();
    const auto r = run.bind.reads.find(key);
    const auto c = run.bind.controls.find(key);
    std::string name = info->params[k].name;
    for (char& ch : name)
      if (ch == '\t') ch = ' ';
    run.bindTsv += type + "\t" + std::to_string(k) + "\t" + name + "\t" + std::to_string(r == run.bind.reads.end() ? 0 : r->second) + "\t" +
                   std::to_string(c == run.bind.controls.end() ? 0 : c->second) + "\t" +
                   std::to_string(run.bind.stateReads.count(static_cast<int>(k)) ? run.bind.stateReads[static_cast<int>(k)] : 0) + "\n";
  }
  run.bind = audit::Bindings{};
}
}  // namespace

void thumbDraw(App& a) {
  run.drawn = false;
  if (!run.on || run.idx >= run.types.size()) return;
  const std::string& type = run.types[run.idx];
  const PluginInfo* info = findPlugin(type);
  if (!info) return;
  if (run.crawling && !run.crawled) crawlStep(a);
  if (run.phase == 0) {
    run.clicks.clear(), run.clickIds.clear();
    run.setup = false;
    a.settingsChannel = kNoId;
    a.fxEditor = {kNoId, -1};
    run.noteFx = info->kind == PluginKind::NoteFx;
    if (run.noteFx) {
      run.channel = a.addInstrument("gl.synth");
      run.target = PluginTarget{run.channel, -1};
      a.settingsChannel = kNoId;
      run.setup = run.channel != kNoId && a.doc.addNoteFx(run.channel, type);
    } else if (info->kind == PluginKind::Generator) {
      run.channel = a.addInstrument(type);
      run.target = PluginTarget{run.channel, -1};
      a.settingsChannel = kNoId;
      run.setup = run.channel != kNoId;
    } else {
      const Song& s = a.doc.song();
      if (s.mixer.size() < 2) return;
      a.doc.setSlotPlugin(s.mixer[1].id, 0, type);
      run.target = PluginTarget{s.mixer[1].id, 0};
      run.channel = kNoId;
      run.setup = true;
    }
    a.syncEngine();
  }
  const bool auditing = !run.auditDir.empty();
  const float factor = auditing ? kAuditFactors[run.factor] : 1.0f;
  const float width = (info->editorWidth > 0 ? info->editorWidth : (type == "gl.eq" ? 780.0f : 600.0f)) * factor;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  if (!run.setup) return;
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width + 16, 0), ImVec2(width + 16, vp->Size.y));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  if (ImGui::Begin("##thumbgen", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking)) {
    if (auditing) {
      if (!run.noteFx) audit::begin();  // layout check: plugin editors only
      if (run.factor == 0) audit::bindBegin();
      run.collect = run.crawl && !run.crawling && run.factor == 0 && run.phase == 1;
      imhook::setItemHook(&onItem);  // crawl clicks (run.collect) + J-128 alignment items
      drawBody(a, width);
      imhook::setItemHook(nullptr), run.collect = false;
      if (!run.noteFx) audit::collectText(ImGui::GetCurrentWindow());  // J-128: text runs for the alignment check
      if (run.factor == 0) mergeBindings(audit::bindEnd());
      run.report = run.noteFx ? audit::Report{} : audit::end();
      if (!run.report.issues.empty() && !std::getenv("GRIDLINE_AUDIT_NOMARK")) {  // outline what is too close to its panel's edge (contact sheet)
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        for (const auto& is : run.report.issues) {
          const inset::Rect& f = run.report.frames[is.frame];
          const inset::Rect& c = run.report.controls[is.control];
          fg->AddRect(ImVec2(f.x0, f.y0), ImVec2(f.x1, f.y1), IM_COL32(255, 160, 0, 140), 0, 0, 1.0f);
          fg->AddRect(ImVec2(c.x0 - 1, c.y0 - 1), ImVec2(c.x1 + 1, c.y1 + 1), IM_COL32(255, 40, 40, 255), 0, 0, 2.0f);
        }
      }
      if (!run.report.findings.empty() && !std::getenv("GRIDLINE_AUDIT_NOMARK")) {  // J-128: alignment findings in magenta
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        for (const auto& f : run.report.findings) {
          fg->AddRect(ImVec2(f.a.x0 - 1, f.a.y0 - 1), ImVec2(f.a.x1 + 1, f.a.y1 + 1), IM_COL32(255, 0, 220, 230), 0, 0, 1.5f);
          if (f.b.w() > 0) fg->AddRect(ImVec2(f.b.x0 - 1, f.b.y0 - 1), ImVec2(f.b.x1 + 1, f.b.y1 + 1), IM_COL32(0, 220, 255, 200), 0, 0, 1.0f);
        }
      }
    } else {
      drawPluginBody(a, run.target, width);
    }
    run.pos = ImGui::GetWindowPos();
    run.size = ImGui::GetWindowSize();
    run.drawn = true;
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  if (auditing && run.factor == 0 && run.phase == 2) drawVariants(a, *info, width);
}

namespace {
int& auditIssues() {
  static int n = 0;
  return n;
}
constexpr int kCellW = 420, kCellMaxH = 360, kSheetCols = 4, kSheetRows = 6, kGap = 6;

// The editor window's pixels (rows top to bottom).
bool grab(int fbW, int fbH, png::Image& img) {
  const ImVec2 sc = ImGui::GetIO().DisplayFramebufferScale;
  const int x0 = static_cast<int>(run.pos.x * sc.x), y0 = static_cast<int>(run.pos.y * sc.y);
  const int w = std::min(static_cast<int>(run.size.x * sc.x), fbW - x0), h = std::min(static_cast<int>(run.size.y * sc.y), fbH - y0);
  if (!run.drawn || w <= 8 || h <= 8) return false;
  img.width = w;
  img.height = h;
  img.channels = 3;
  img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  std::vector<uint8_t> raw(img.pixels.size());
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(x0, fbH - y0 - h, w, h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
  for (int y = 0; y < h; ++y)
    std::copy_n(raw.data() + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 3, static_cast<size_t>(w) * 3,
                img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3);
  return true;
}

// Contact sheets (kSheetCols x kSheetRows editors a page) and the TSV.
void finishAudit() {
  const fs::path dir(run.auditDir);
  const size_t perSheet = static_cast<size_t>(kSheetCols * kSheetRows);
  for (size_t first = 0, page = 1; first < run.cells.size(); first += perSheet, ++page) {
    const size_t n = std::min(perSheet, run.cells.size() - first);
    const int rows = static_cast<int>((n + kSheetCols - 1) / kSheetCols);
    png::Image sheet;
    sheet.channels = 3;
    sheet.width = kSheetCols * (kCellW + kGap) + kGap;
    sheet.height = rows * (kCellMaxH + kGap) + kGap;
    sheet.pixels.assign(static_cast<size_t>(sheet.width) * static_cast<size_t>(sheet.height) * 3, 24);
    for (size_t i = 0; i < n; ++i) {
      const png::Image& c = run.cells[first + i];
      const int cx = kGap + static_cast<int>(i % kSheetCols) * (kCellW + kGap), cy = kGap + static_cast<int>(i / kSheetCols) * (kCellMaxH + kGap);
      for (int y = 0; y < std::min(c.height, kCellMaxH); ++y)
        std::copy_n(c.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(c.width) * 3, static_cast<size_t>(std::min(c.width, kCellW)) * 3,
                    sheet.pixels.data() + (static_cast<size_t>(cy + y) * static_cast<size_t>(sheet.width) + static_cast<size_t>(cx)) * 3);
    }
    char name[32];
    std::snprintf(name, sizeof name, "sheet_%02zu.png", page);
    png::writeFile((dir / name).string(), sheet);
  }
  if (FILE* f = std::fopen((dir / "layout_audit.tsv").string().c_str(), "wb")) {
    std::fwrite(run.tsv.data(), 1, run.tsv.size(), f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((dir / "alignment.tsv").string().c_str(), "wb")) {  // J-128
    std::fwrite(run.alignTsv.data(), 1, run.alignTsv.size(), f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((dir / "param_bindings.tsv").string().c_str(), "wb")) {  // J-92
    std::fwrite(run.bindTsv.data(), 1, run.bindTsv.size(), f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((dir / "alignment_counts.tsv").string().c_str(), "wb")) {  // J-128: a baseline candidate
    const std::string s = align::countsTsv(run.alignCounts);
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
  auditIssues() = run.issueCount;
  std::fprintf(stderr, "layout audit: %zu editors, %d with issues at width x1, %d issues in all\n", run.types.size(), run.editorsWithIssues,
               run.issueCount);
  std::fprintf(stderr, "alignment audit: %d editors with findings at width x1; findings x1 %d, x1.5 %d, x2 %d\n", run.alignEditors, run.alignCount[0],
               run.alignCount[1], run.alignCount[2]);
  // J-128 regression gate: GRIDLINE_ALIGN_BASELINE / GRIDLINE_ALIGN_ALLOW (AlignCheck.h gate()).
  if (const char* base = std::getenv("GRIDLINE_ALIGN_BASELINE"); base && !fs::exists(fs::path(base))) {
    std::fprintf(stderr, "alignment gate: no baseline at %s (skipped)\n", base);
  } else if (base) {
    const char* allow = std::getenv("GRIDLINE_ALIGN_ALLOW");
    const auto bad = align::gate(run.alignCounts, readText(base), allow ? readText(allow) : std::string());
    for (const std::string& b : bad) std::fprintf(stderr, "alignment regression: %s\n", b.c_str());
    std::fprintf(stderr, "alignment gate: %zu regressions (see alignment.tsv)\n", bad.size());
    auditIssues() += static_cast<int>(bad.size());
  }
}
}  // namespace

namespace {
void teardown(App& a) {
  if (run.setup && run.channel != kNoId) a.doc.removeChannel(run.channel);
  else if (run.setup) a.doc.setSlotPlugin(run.target.owner, run.target.slot, "");
  run.setup = false;
  a.syncEngine();
  run.phase = 0;
  ++run.idx;
}
}  // namespace

bool thumbCapture(App& a, int fbW, int fbH) {
  if (!run.on) return true;
  if (run.crawling) {  // --binding-crawl (see crawlStep)
    if (!run.crawled) return true;
    run.crawling = false;
    writeBindings(run.types[run.idx]);
    teardown(a);
    return true;
  }
  if (run.idx >= run.types.size()) {
    if (!run.auditDir.empty()) finishAudit();
    else std::fprintf(stderr, "thumbnails: %d written\n", run.written);
    return false;
  }
  if (run.phase >= 4 && !run.auditDir.empty()) {
    const std::string& type = run.types[run.idx];
    const audit::Report& r = run.report;
    char line[512];
    for (const auto& is : r.issues) {
      static const char* side[4] = {"left", "top", "right", "bottom"};
      const inset::Rect& c = r.controls[is.control];
      const inset::Rect& f = r.frames[is.frame];
      std::snprintf(line, sizeof line, "%s\t%.1f\t%zu\t%s@%.0f,%.0f %.0fx%.0f\t%s@%.0f,%.0f %.0fx%.0f\t%.1f\t%s\n", type.c_str(),
                    static_cast<double>(kAuditFactors[run.factor]), r.issues.size(), r.controlWhat[is.control].c_str(),
                    static_cast<double>(c.x0 - run.pos.x), static_cast<double>(c.y0 - run.pos.y), static_cast<double>(c.w()), static_cast<double>(c.h()),
                    r.frameWhat[is.frame].c_str(), static_cast<double>(f.x0 - run.pos.x), static_cast<double>(f.y0 - run.pos.y),
                    static_cast<double>(f.w()), static_cast<double>(f.h()), static_cast<double>(is.gap), side[is.side]);
      run.tsv += line;
    }
    run.issueCount += static_cast<int>(r.issues.size());
    for (const auto& f : r.findings) {  // J-128
      char ab[96];
      std::snprintf(ab, sizeof ab, "%.1f,%.1f %.1fx%.1f", static_cast<double>(f.a.x0 - run.pos.x), static_cast<double>(f.a.y0 - run.pos.y),
                    static_cast<double>(f.a.w()), static_cast<double>(f.a.h()));
      char off[24];
      std::snprintf(off, sizeof off, "%.2f", static_cast<double>(f.off));
      run.alignTsv += type + "\t" + std::to_string(kAuditFactors[run.factor]).substr(0, 3) + "\t" + f.rule + "\t" + off + "\t" + ab + "\t" + f.detail + "\n";
    }
    align::tally(run.alignCounts, type, r.findings);  // the regression gate's counts (all widths)
    run.alignCount[run.factor] += static_cast<int>(r.findings.size());
    if (run.factor == 0 && !r.findings.empty()) ++run.alignEditors;
    if (run.factor == 0 && !run.crawl) writeBindings(type);
    if (run.factor == 0) {
      if (!r.issues.empty()) ++run.editorsWithIssues;
      png::Image img;
      if (grab(fbW, fbH, img)) {
        run.cells.push_back(png::resize(img, kCellW, std::max(1, img.height * kCellW / img.width)));
        if (std::getenv("GRIDLINE_AUDIT_FULL")) {  // J-128 dev aid: each editor at full size (review sheets)
          std::error_code ec;
          fs::create_directories(fs::path(run.auditDir) / "full", ec);
          png::writeFile((fs::path(run.auditDir) / "full" / (safeName(type) + ".png")).string(), img);
        }
      }
    }
    if (++run.factor < 3) {
      run.phase = 1;  // same plugin, next width
      return true;
    }
    run.factor = 0;
    if (run.crawl && run.setup) {  // the widths are done: click through the editor (x1)
      run.crawling = true, run.crawled = false, run.crawlIdx = 0, run.crawlStep = 0;
      return true;
    }
    if (run.crawl) writeBindings(type);
    teardown(a);
    return true;
  }
  if (run.phase >= 4) {
    const ImVec2 sc = ImGui::GetIO().DisplayFramebufferScale;
    int x0 = static_cast<int>(run.pos.x * sc.x), y0 = static_cast<int>(run.pos.y * sc.y);
    int w = std::min(static_cast<int>(run.size.x * sc.x), fbW - x0), h = std::min(static_cast<int>(run.size.y * sc.y), fbH - y0);
    if (run.drawn && w > 8 && h > 8) {
      png::Image img;
      img.width = w;
      img.height = h;
      img.channels = 3;
      img.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
      std::vector<uint8_t> raw(img.pixels.size());
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(x0, fbH - y0 - h, w, h, GL_RGB, GL_UNSIGNED_BYTE, raw.data());
      for (int y = 0; y < h; ++y)
        std::copy_n(raw.data() + static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 3, static_cast<size_t>(w) * 3,
                    img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3);
      // 320 px wide, aspect kept (tall editors are cut at 3:2).
      const int tw = 320;
      int th = std::max(1, h * tw / w);
      png::Image small = png::resize(img, tw, th);
      if (small.height > 260) {
        small.height = 260;
        small.pixels.resize(static_cast<size_t>(small.width) * static_cast<size_t>(small.height) * 3);
      }
      if (png::writeFile(thumbPath(a, run.types[run.idx]), small)) ++run.written;
    }
    // Tear down: remove the channel / clear the slot, next plugin.
    if (run.setup && run.channel != kNoId) a.doc.removeChannel(run.channel);
    else if (run.setup) a.doc.setSlotPlugin(run.target.owner, run.target.slot, "");
    run.setup = false;
    a.syncEngine();
    run.phase = 0;
    ++run.idx;
    return true;
  }
  ++run.phase;
  return true;
}

int auditIssueCount() { return auditIssues(); }

}  // namespace gl::browserui
