// Gridline — "synths" pack, J-33 group "pixel": Pixel Tone editor
// (gl.pixeltone): the picture (red = left, green = right, blue = pitch bend),
// span / length wheels, scale selector, switches and the row limit.
// J-184: pictures from image files: "Load picture..." (file dialog), files
// dropped on the picture (from the desktop or the browser; the plugin window
// and a Pixel Tone channel row take them too, BrowserDrop.cpp), decoded on a
// worker thread and applied on the UI thread as one undo step; reverse /
// flip / invert / reset, the file name and sizes under the picture.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "../../../third_party/imgui/imgui_internal.h"
#include "../../app/BrowserUi.h"
#include "../../app/DevSignal.h"
#include "../../core/I18n.h"
#include "../../core/Platform.h"
#include "../flc_kit.h"
#include "pixel.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace pixel;

std::string pixelToneDefaultData();  // pixel_info.cpp
bool loadPicture(App& a, const PluginTarget& t, const std::string& path);

namespace {

const skin::Skin& pixelSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x26302E, 0x171E1C, 0x35423F, 0x6FD3C0, 0x40504C, 0xDDEBE8);
    s.select = skin::hex(0x6FD3C0);
    s.accent2 = skin::hex(0x6FD3C0);
    return s;
  }();
  return k;
}

// ---- background picture loads (J-184)
struct LoadJob {
  PluginTarget t;
  std::string path;
  int maxRows = 23;
  uint64_t serial = 0;
  std::atomic<int> state{0};  // 0 decoding, 1 done, 2 failed
  Picture pic;
  std::string err;
};

std::vector<std::shared_ptr<LoadJob>>& jobs() {
  static std::vector<std::shared_ptr<LoadJob>> j;
  return j;
}

// The newest load per plugin (PluginTarget::key): only that one is applied.
std::map<uint64_t, uint64_t>& latestLoad() {
  static std::map<uint64_t, uint64_t> m;
  return m;
}

struct Status {
  std::string text;
  bool error = false;
  double until = 0;
};
std::map<uint64_t, Status>& statuses() {
  static std::map<uint64_t, Status> m;
  return m;
}

const LoadJob* pendingFor(const PluginTarget& t) {
  for (auto it = jobs().rbegin(); it != jobs().rend(); ++it)
    if ((*it)->t.owner == t.owner && (*it)->t.slot == t.slot) return it->get();
  return nullptr;
}

int maxRowsOf(const PluginState& st) {
  return std::clamp(static_cast<int>(std::lround(st.params.size() > PxMaxHeight ? st.params[PxMaxHeight] : 23.0f)), 3, kMaxRows);
}

void runJob(const std::shared_ptr<LoadJob>& j) {
  const bool ok = loadPictureFile(j->path, j->maxRows, j->pic, &j->err);
  j->state.store(ok ? 1 : 2, std::memory_order_release);
}

bool trace() {
  static const bool on = std::getenv("GRIDLINE_PIXEL_TRACE") != nullptr;
  return on;
}

// A picture edit (reverse / flip / invert) as one undo step.
void editPicture(App& a, const PluginTarget& t, const PluginState& st, void (*fn)(Picture&), const char* label) {
  Picture p;
  if (!parsePicture(st.data.empty() ? pixelToneDefaultData() : st.data, p) || p.w < 1 || p.h < 1) return;
  fn(p);
  PluginState ns = st;
  ns.data = formatPicture(p);
  if (!(ns == st)) setPluginState(a, t, ns, label);
}

std::string& lastDir() {
  static std::string d;
  return d;
}

void openPictureDialog(App& a, const PluginTarget& t) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = i18n::tr("Load picture");
  const char* h = gl::platform::homeEnv();
  std::error_code ec;
  a.fileDialog.dir = !lastDir().empty() && std::filesystem::is_directory(std::filesystem::u8path(lastDir()), ec) ? lastDir() : (h ? h : ".");
  a.fileDialog.exts = pictureExtensions();
  const PluginTarget tt = t;
  a.fileDialog.onOk = [&a, tt](const std::string& path) {
    lastDir() = std::filesystem::u8path(path).parent_path().u8string();
    loadPicture(a, tt, path);
  };
}

}  // namespace

bool loadPicture(App& a, const PluginTarget& t, const std::string& path) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != "gl.pixeltone" || path.empty()) return false;
  static uint64_t serial = 0;
  auto j = std::make_shared<LoadJob>();
  j->t = t, j->path = path, j->maxRows = maxRowsOf(*st), j->serial = ++serial;
  jobs().push_back(j);
  latestLoad()[t.key()] = j->serial;
  statuses().erase(t.key());
  try {
    std::thread([j] { runJob(j); }).detach();
  } catch (...) {  // no thread available: decode here
    runJob(j);
  }
  return true;
}

void tickPictureLoads(App& a) {
  auto& v = jobs();
  for (size_t i = 0; i < v.size();) {
    const std::shared_ptr<LoadJob> j = v[i];
    const int state = j->state.load(std::memory_order_acquire);
    if (state == 0) {
      ++i;
      continue;
    }
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    if (latestLoad()[j->t.key()] != j->serial) continue;  // a newer load for the same plugin replaces this one
    latestLoad().erase(j->t.key());
    devsig::reach("pixeltone-picture");  // scripted runs: "waitfor pixeltone-picture <s>"
    const std::string name = std::filesystem::u8path(j->path).filename().u8string();
    if (state == 2) {
      statuses()[j->t.key()] = {std::string(i18n::tr("Could not load ")) + name + ": " + j->err, true, a.now + 8};
      a.notify(std::string(i18n::tr("Could not load the picture ")) + name + ": " + j->err, true);
      if (trace()) std::printf("pixel-ui: load-failed %s (%s)\n", name.c_str(), j->err.c_str());
      continue;
    }
    const PluginState* st = j->t.state(a.doc.song());
    if (!st || st->type != "gl.pixeltone") continue;  // the plugin went away meanwhile
    PluginState ns = *st;
    ns.data = formatPicture(j->pic);
    setPluginState(a, j->t, ns, "load picture");
    statuses()[j->t.key()] = {std::string(i18n::tr("Loaded ")) + name, false, a.now + 4};
    if (trace()) std::printf("pixel-ui: loaded %s %dx%d from %dx%d\n", name.c_str(), j->pic.w, j->pic.h, j->pic.srcW, j->pic.srcH);
  }
  if (trace()) std::fflush(stdout);
}

void drawPixelToneEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = pixelSkin();
  constexpr float kW = 620, kH = 270;
  ImGui::PushID("gl.pixeltone");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  if (!pui::titleWordmark(dl, "gl.pixeltone", F.p(12, 10), F.font(14)))  // J-136: the wordmark
    skin::text(dl, F.p(12, 10), K.accent2C(), "PIXEL TONE", F.font(14));
  std::string traceLine;
  auto traceRect = [&](const char* name, ImVec2 p, ImVec2 q) {
    if (!trace()) return;
    char b[96];
    std::snprintf(b, sizeof b, " %s=%.0f,%.0f,%.0f,%.0f", name, static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(q.x),
                  static_cast<double>(q.y));
    traceLine += b;
  };

  // Picture tools (J-184).
  {
    struct B {
      const char* id;
      float x, w;
      const char* text;
      const char* hint;
    };
    static const B bs[] = {
        {"##load", 318, 104, "LOAD PICTURE...", i18n::N_("Load a picture: PNG, JPEG, BMP, GIF, TGA, PSD or HDR (or drop one on the picture)")},
        {"##rev", 426, 52, "REVERSE", i18n::N_("Play the picture right to left (mirror it)")},
        {"##flip", 482, 36, "FLIP", i18n::N_("Turn the picture upside down (high rows become low)")},
        {"##inv", 522, 44, "INVERT", i18n::N_("Colour negative: dark becomes loud, bright becomes quiet")},
        {"##reset", 570, 42, "RESET", i18n::N_("Back to the built-in picture")},
    };
    for (const B& b : bs) {
      const bool hit = flk::btn(c, b.id, b.x, 8, b.w, 16, b.text, false);
      if (ImGui::IsItemHovered()) a.setHint(i18n::tr(b.hint));
      traceRect(b.id + 2, F.p(b.x, 8), F.p(b.x + b.w, 24));
      if (!hit) continue;
      const std::string id = b.id;
      if (id == "##load") openPictureDialog(a, t);
      else if (id == "##rev") editPicture(a, t, st, flipColumns, "reverse picture");
      else if (id == "##flip") editPicture(a, t, st, flipRows, "flip picture");
      else if (id == "##inv") editPicture(a, t, st, invertColours, "invert picture");
      else if (id == "##reset" && !st.data.empty()) {
        PluginState ns = st;
        ns.data.clear();
        setPluginState(a, t, ns, "reset picture");
      }
    }
  }

  // Picture.
  static thread_local std::string cacheKey;
  static thread_local Picture pic;
  const std::string& data = st.data.empty() ? pixelToneDefaultData() : st.data;
  if (data != cacheKey) {
    cacheKey = data;
    if (!parsePicture(data, pic)) pic = Picture{};
  }
  const float x0 = 8, y0 = 32, pw = 604, ph = 120;
  ImVec2 p0 = F.p(x0, y0), p1 = F.p(x0 + pw, y0 + ph);
  skin::graphWell(dl, p0, p1, K);
  traceRect("well", p0, p1);
  if (pic.w > 0 && pic.h > 0) {
    // At most one cell per screen pixel row and two per column (nearest pixel).
    const int cols = std::max(1, std::min({pic.w, 302, static_cast<int>((p1.x - p0.x) * 0.5f)}));
    const int rows = std::max(1, std::min(pic.h, static_cast<int>(p1.y - p0.y)));
    const float cw = (p1.x - p0.x) / static_cast<float>(cols), rh = (p1.y - p0.y) / static_cast<float>(rows);
    for (int x = 0; x < cols; ++x) {
      const int sx = x * pic.w / cols;
      for (int y = 0; y < rows; ++y) {
        const int sy = y * pic.h / rows;
        const uint8_t* px = &pic.rgb[(static_cast<size_t>(sy) * static_cast<size_t>(pic.w) + static_cast<size_t>(sx)) * 3];
        if (px[0] == 0 && px[1] == 0 && px[2] == 0) continue;
        dl->AddRectFilled(ImVec2(p0.x + cw * x, p0.y + rh * y), ImVec2(p0.x + cw * (x + 1), p0.y + rh * (y + 1)),
                          IM_COL32(px[0], px[1], px[2], 255));
      }
    }
  }
  // Drops on the picture (desktop files and browser items): the plan of the
  // plugin window target (a picture loads; anything else says why not).
  bool dropHover = false;
  {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    const bool fileDrag = pl && (pl->IsDataType("GL_FILE") || pl->IsDataType("GL_ITEM") || pl->IsDataType("GL_FILES"));    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##picture", ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)));
    if (ImGui::IsItemHovered() && !fileDrag)
      a.setHint(i18n::tr("Rows are sine partials (top = high), columns are time: red = left level, green = right level, blue bends the pitch. Drop a picture here to load it"));
    // (a custom target over the picture: smaller than the plugin window's, so it wins the drop)
    if (fileDrag && ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##picturedrop"))) {
      browserui::DropPlace place;
      place.owner = t.owner;
      place.pluginSlot = t.slot;
      browserui::DropFeedback fb;
      fb.highlight = false;
      browserui::acceptFileDrop(a, browse::DropTarget::PluginWindow, place, &fb);
      ImGui::EndDragDropTarget();
      dropHover = fb.hovering;
    }
    const ImU32 acc = K.select;
    if (dropHover) {
      dl->AddRectFilled(p0, p1, theme::withAlpha(acc, 0.16f), 3 * F.s);
      dl->AddRect(p0, p1, acc, 3 * F.s, 0, 2.0f);
      const char* msg = i18n::tr("Drop to load this picture");
      ImGui::PushFont(a.fontBold, F.font(13));
      const ImVec2 ts = ImGui::CalcTextSize(msg);
      const ImVec2 c0((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
      dl->AddRectFilled(ImVec2(c0.x - 10, c0.y - 6), ImVec2(c0.x + ts.x + 10, c0.y + ts.y + 6), IM_COL32(10, 16, 15, 225), 4 * F.s);
      dl->AddText(ImVec2(std::round(c0.x), std::round(c0.y)), K.text, msg);
      ImGui::PopFont();
    } else if (fileDrag) {
      dl->AddRect(p0, p1, theme::withAlpha(acc, 0.55f), 3 * F.s, 0, 1.0f);
    }
  }
  const LoadJob* pending = pendingFor(t);
  if (pending) {
    const char* msg = i18n::tr("Loading picture...");
    ImGui::PushFont(a.fontBold, F.font(12));
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    const ImVec2 c0((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
    dl->AddRectFilled(ImVec2(c0.x - 10, c0.y - 6), ImVec2(c0.x + ts.x + 10, c0.y + ts.y + 6), IM_COL32(10, 16, 15, 225), 4 * F.s);
    dl->AddText(ImVec2(std::round(c0.x), std::round(c0.y)), K.text, msg);
    ImGui::PopFont();
  }

  // The picture's source and size (or the last load's outcome).
  {
    char size[96];
    if (pic.srcW > 0 && pic.srcH > 0 && (pic.srcW != pic.w || pic.srcH != pic.h))
      std::snprintf(size, sizeof size, "%d x %d  (%s %d x %d)", pic.w, pic.h, i18n::tr("from"), pic.srcW, pic.srcH);
    else
      std::snprintf(size, sizeof size, "%d x %d", pic.w, pic.h);
    std::string left = pic.name.empty() ? std::string(i18n::tr("Built-in picture")) : pic.name;
    ImU32 leftCol = K.textDim;
    auto it = statuses().find(t.key());
    if (it != statuses().end() && it->second.until < a.now) statuses().erase(it), it = statuses().end();
    if (it != statuses().end() && it->second.error) left = it->second.text, leftCol = skin::hex(0xFF7A6B);
    const float ty = 155, tyH = 12;
    ImGui::PushFont(a.fontMono, F.font(8.5f));
    const float sizeW = ImGui::CalcTextSize(size).x;
    const float room = std::max(10.0f, (pw - 8) * F.s - sizeW - 16 * F.s);
    const std::string fitted = theme::fitText(left.c_str(), room);
    ImGui::PopFont();
    skin::textAt(dl, F.p(x0 + 2, ty), F.p(x0 + pw - 2, ty + tyH), leftCol, fitted.c_str(), F.font(8.5f), -1, true);
    skin::textAt(dl, F.p(x0 + 2, ty), F.p(x0 + pw - 2, ty + tyH), K.textDim, size, F.font(8.5f), 1, true);
    if (trace()) {
      char b[160];
      std::snprintf(b, sizeof b, " pic=%dx%d src=%dx%d hover=%d loading=%d err=%d", pic.w, pic.h, pic.srcW, pic.srcH, dropHover ? 1 : 0, pending ? 1 : 0,
                    it != statuses().end() && it->second.error ? 1 : 0);
      traceLine += b;
      traceLine += " name=" + (pic.name.empty() ? std::string("-") : pic.name);
    }
  }

  flk::section(c, 4, 172, 612, 92, "");
  flk::wheel(c, PxRange, 14, 182, 34, "Span");
  flk::wheel(c, PxLength, 62, 182, 34, "Length");
  {
    char b[32];
    const int n = columnSamples44(ival(st, PxLength));
    std::snprintf(b, sizeof b, "%.1f ms / col", n / 44.1);
    flk::lcd(c, 110, 186, 84, 16, b);
  }
  static const char* scales[3] = {"LOG", "LINEAR", "HARMONICS"};
  flk::sw(c, PxScale, 206, 182, 120, 54, scales, 3);
  flk::chk(c, PxUseBlue, 340, 184, 110, 14, "BLUE = PITCH");
  flk::chk(c, PxGrainy, 340, 204, 110, 14, "GRAINY");
  flk::chk(c, PxLoop, 460, 184, 80, 14, "LOOP");
  flk::chk(c, PxWiden, 460, 204, 80, 14, "WIDEN");
  flk::wheel(c, PxMaxHeight, 556, 182, 34, "Max rows");
  if (trace()) {
    std::printf("pixel-ui:%s\n", traceLine.c_str());
    std::fflush(stdout);
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
