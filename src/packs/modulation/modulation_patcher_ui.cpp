// Gridline - Junction editor (K-7): the Map page (modules, cables, zoom and pan, minimap), the
// Panel page (knobs with names, what they drive, snapshots) and the module inspector. Every
// edit is one undo step; edits made while drawing are stored at the end of the frame.
//
// Mouse on the map: drag a module to move it, drag from a port to wire it (a parameter input
// asks which parameter), drag on the empty map to select, middle button or Alt to pan, wheel
// to zoom, double click a module for its interface, double click the map to add a module,
// right click anything for its menu. Delete removes the selection.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include "../../app/HostedUi.h"
#include "../../app/PluginFavsUi.h"
#include "../../app/Windows.h"
#include "../../core/I18n.h"
#include "../../host/HostedPlugins.h"
#include "../../host/PluginScan.h"
#include "junction.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_skin.h"
#include "modulation_widgets.h"
#include "patchv2.h"

namespace gl::packs::modulation {

using theme::pal;

namespace {

constexpr float kNodeW = 156, kHostW = 104, kHead = 26, kRow = 15, kPad = 7, kPortR = 4.2f;
constexpr float kMinZoom = 0.3f, kMaxZoom = 2.0f;
constexpr float kCanvasH = 430;

// Module colours the user can pick (0 keeps the role colour of its kind).
constexpr uint32_t kSwatches[] = {0, 0xE5604F, 0xE8A23C, 0xD9CF4A, 0x62C76A, 0x45BDB0, 0x4E9CF0, 0x8F7BEF, 0xD86BC4};
constexpr int kSwatchCount = static_cast<int>(sizeof kSwatches / sizeof kSwatches[0]);

ImU32 kindColor(JKind k) {
  switch (k) {
    case JKind::Audio: return theme::litRole(theme::role::kMenu);
    case JKind::Notes: return theme::litRole(theme::role::kLoop);
    default: return theme::litRole(theme::role::kGraph);
  }
}

struct Port {
  JPort p;
  float y = 0;   // from the module's top
};
struct Item {
  uint32_t id = 0;
  float x = 0, y = 0, w = kNodeW, h = 40;
  std::string title, sub;
  ImU32 tint = 0;
  bool host = false;
  std::vector<Port> ports;
  const Port* find(JKind k, bool out, int index) const {
    const Port* any = nullptr;
    for (const Port& q : ports)
      if (q.p.kind == k && q.p.out == out) {
        if (q.p.index == index) return &q;
        if (q.p.index == -1 || !any) any = &q;
      }
    return any;
  }
};

enum class Drag { None, Pan, Move, Box, Cable, Mini };
enum class Pending { None, Graph, Node, Move };

struct View {
  std::string cached;  // the data the graph below was read from
  JGraph g;
  std::vector<Item> items;
  std::vector<char> feedback;
  float panX = 0, panY = 0, zoom = 1, zoomTo = 1;
  ImVec2 zoomAt{0, 0};
  bool fitted = false;
  std::set<uint32_t> sel;
  bool showAudio = true, showNotes = true, showParams = true, activity = true, labels = true, latency = false, perf = false, minimap = true;
  int miniSize = 1, miniCorner = 2;
  int page = 0;
  // Gesture.
  Drag drag = Drag::None;
  ImVec2 dragStart{0, 0};
  uint32_t cableNode = 0;
  JPort cablePort;
  // Popups.
  uint32_t menuNode = 0;
  int menuLink = -1;
  ImVec2 addAt{0, 0};
  bool addConnects = false;
  JLink pendingLink;
  bool paramSrcOpen = false;
  char renameBuf[64] = {};
  // End of frame.
  Pending pending = Pending::None;
  uint32_t pendingNode = 0;
  std::string pendingLabel;
  uint64_t pendingKey = 0;
  int panelEditing = -1;
  char panelBuf[32] = {};
  char snapBuf[32] = {};
  std::map<uint32_t, float> levels, loads;
  // Dev runs (--dev-input "junction ...").
  int devPatch = -1, devStress = 0;
  std::string devAdd;
  ObjectId devOwner = kNoId;
  int devSlot = -1;
  float devBodyMs = 0, devBodyWorstMs = 0;
  uint32_t devLastAdded = 0;
  std::map<int, ImVec2> devKnobs;  // the inspector's knobs on screen (dump)
  ImVec2 origin{0, 0};
};

View* gLastView = nullptr;

View& viewOf(uint64_t key) {
  static std::map<uint64_t, View> views;
  return views[key];
}

std::string nodeTitle(const JNode& n) {
  if (!n.name.empty()) return n.name;
  const PluginInfo* info = findPlugin(n.plugin.type);
  return info ? info->name : (n.flName.empty() ? n.plugin.type : n.flName);
}

ImU32 nodeTint(const JNode& n) {
  if (n.color > 0 && n.color < kSwatchCount) return theme::litRole(kSwatches[n.color]);
  const PluginInfo* info = findPlugin(n.plugin.type);
  if (info && !info->outputs.empty()) return theme::litRole(theme::role::kGraph);
  if (!info) return theme::litRole(0x9AA5AC);
  if (info->kind == PluginKind::Generator) return theme::litRole(theme::role::kLoop);
  if (info->kind == PluginKind::NoteFx) return theme::litRole(theme::role::kStamp);
  return theme::litRole(theme::role::kMenu);
}

Item itemFor(const std::vector<JPort>& ports, float w) {
  Item it;
  it.w = w;
  float l = kHead + kPad, r = kHead + kPad;
  for (const JPort& p : ports) {
    Port q;
    q.p = p;
    float& at = p.out ? r : l;
    q.y = at + kRow * 0.5f;
    at += kRow;
    it.ports.push_back(std::move(q));
  }
  it.h = std::max(std::max(l, r) + kPad - 2, kHead + 22);
  return it;
}

void rebuild(View& v, const PluginState& st) {
  const bool fx = st.type == "gl.patcherfx";
  v.items.clear();
  Item in = itemFor(jHostPorts(true, fx), kHostW);
  in.id = kJInputs, in.x = v.g.inX, in.y = v.g.inY, in.host = true;
  in.title = fx ? i18n::tr("Input") : i18n::tr("Notes");
  in.tint = theme::litRole(theme::role::kPlay);
  v.items.push_back(std::move(in));
  Item out = itemFor(jHostPorts(false, fx), kHostW);
  out.id = kJOutputs, out.x = v.g.outX, out.y = v.g.outY, out.host = true;
  out.title = i18n::tr("Output");
  out.tint = theme::litRole(theme::role::kPlay);
  v.items.push_back(std::move(out));
  for (const JNode& n : v.g.nodes) {
    Item it = itemFor(jNodePorts(n), kNodeW);
    it.id = n.id, it.x = n.x, it.y = n.y;
    it.title = nodeTitle(n);
    const PluginInfo* info = findPlugin(n.plugin.type);
    it.sub = !n.name.empty() && info ? info->name : "";
    it.tint = nodeTint(n);
    v.items.push_back(std::move(it));
  }
  v.feedback = jFeedbackLinks(v.g);
}

Item* itemOf(View& v, uint32_t id) {
  for (Item& it : v.items)
    if (it.id == id) return &it;
  return nullptr;
}

void bounds(const View& v, float& x0, float& y0, float& x1, float& y1) {
  x0 = y0 = 1e9f, x1 = y1 = -1e9f;
  for (const Item& it : v.items) {
    x0 = std::min(x0, it.x), y0 = std::min(y0, it.y);
    x1 = std::max(x1, it.x + it.w), y1 = std::max(y1, it.y + it.h);
  }
}

void fitView(View& v, float W, float H) {
  float x0, y0, x1, y1;
  bounds(v, x0, y0, x1, y1);
  if (x1 < x0) return;
  const float z = std::clamp(std::min((W - 60) / std::max(1.0f, x1 - x0), (H - 60) / std::max(1.0f, y1 - y0)), kMinZoom, 1.1f);
  v.zoom = v.zoomTo = z;
  v.panX = (W - (x1 - x0) * z) * 0.5f - x0 * z;
  v.panY = (H - (y1 - y0) * z) * 0.5f - y0 * z;
}

void edit(View& v, Pending kind, const char* label, uint64_t key = 0, uint32_t node = 0) {
  if (v.pending == Pending::Graph) return;  // (already the widest kind)
  v.pending = kind;
  v.pendingLabel = label;
  v.pendingKey = key;
  v.pendingNode = node;
}

// Stores the frame's edit. Plain module edits and moves of FL patches stay in the FL data
// (exact export); anything that changes the wiring makes the state a version 3 graph.
void applyPending(EditTarget& e, View& v) {
  if (v.pending == Pending::None || !e.st) return;
  PluginState st = *e.st;
  const Pending kind = v.pending;
  v.pending = Pending::None;
  if (kind != Pending::Graph && isPatchV2(st.data)) {
    PatchV2 g;
    const JNode* n = v.g.node(v.pendingNode);
    if (parsePatchV2(st.data, g) && n && n->id >= kJFirstId && n->id - kJFirstId < g.nodes.size()) {
      if (kind == Pending::Node) {
        g.nodes[n->id - kJFirstId].plugin = n->plugin;
      } else {  // the layout record of this module
        const uint64_t flId = g.nodes[n->id - kJFirstId].id;
        for (std::string& item : g.items) {
          if (item.empty() || item[0] == '#') continue;
          std::vector<uint8_t> b = patchUnhex(item);
          // container { chunk 1600 { u64 id, f32 x, f32 y, … } }
          if (b.size() < 12 + 12 + 16) continue;
          uint32_t cid = 0, kid = 0;
          uint64_t id = 0;
          std::memcpy(&cid, b.data(), 4);
          std::memcpy(&kid, b.data() + 12, 4);
          std::memcpy(&id, b.data() + 24, 8);
          if (cid != 0 || kid != 1600 || id != flId) continue;
          std::memcpy(b.data() + 32, &n->x, 4);
          std::memcpy(b.data() + 36, &n->y, 4);
          item = patchHex(b);
        }
      }
      st.data = serializePatchV2(g);
      v.cached = st.data;
      e.set(st, v.pendingLabel, v.pendingKey);
      return;
    }
  }
  if (kind == Pending::Node && !isJunction3(st.data) && !isPatchV2(st.data)) {  // a chain: stay a chain
    PatchGraph g = patchGraphOf(st);
    const JNode* n = v.g.node(v.pendingNode);
    if (n && n->id >= kJFirstId && n->id - kJFirstId < g.nodes.size()) {
      g.nodes[n->id - kJFirstId].plugin = n->plugin;
      st.data = serializePatchGraph(g);
      v.cached = st.data;
      e.set(st, v.pendingLabel, v.pendingKey);
      return;
    }
  }
  st.data = serializeJunction(v.g);
  v.cached = st.data;
  e.set(st, v.pendingLabel, v.pendingKey);
}

// ------------------------------------------------------------------ drawing
struct Xf {
  ImVec2 o;
  float panX, panY, z;
  ImVec2 pt(float x, float y) const { return ImVec2(o.x + panX + x * z, o.y + panY + y * z); }
  ImVec2 inv(ImVec2 s) const { return ImVec2((s.x - o.x - panX) / z, (s.y - o.y - panY) / z); }
};

ImVec2 portAt(const Xf& X, const Item& it, const Port& q) { return X.pt(q.p.out ? it.x + it.w : it.x, it.y + q.y); }

void bezierPoints(ImVec2 a, ImVec2 b, float z, ImVec2& c1, ImVec2& c2) {
  if (b.x < a.x + 12.0f * z) {  // runs backwards: a loop under the modules between its ends
    const float back = std::max(70.0f * z, (a.x - b.x) * 0.45f), under = 90.0f * z;
    c1 = ImVec2(a.x + back, a.y + under);
    c2 = ImVec2(b.x - back, b.y + under);
    return;
  }
  const float dx = std::max(28.0f * z, std::fabs(b.x - a.x) * 0.5f);
  c1 = ImVec2(a.x + dx, a.y);
  c2 = ImVec2(b.x - dx, b.y);
}

ImVec2 bezierAt(ImVec2 a, ImVec2 c1, ImVec2 c2, ImVec2 b, float t) {
  const float u = 1 - t;
  return ImVec2(u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y);
}

bool nearCable(ImVec2 a, ImVec2 c1, ImVec2 c2, ImVec2 b, ImVec2 m, float r) {
  ImVec2 last = a;
  for (int k = 1; k <= 24; ++k) {
    const ImVec2 p = bezierAt(a, c1, c2, b, static_cast<float>(k) / 24.0f);
    const float vx = p.x - last.x, vy = p.y - last.y;
    const float len = vx * vx + vy * vy;
    const float t = len > 0 ? std::clamp(((m.x - last.x) * vx + (m.y - last.y) * vy) / len, 0.0f, 1.0f) : 0;
    const float dx = last.x + vx * t - m.x, dy = last.y + vy * t - m.y;
    if (dx * dx + dy * dy < r * r) return true;
    last = p;
  }
  return false;
}

void drawCable(ImDrawList* dl, ImVec2 a, ImVec2 b, float z, ImU32 col, float th, bool dashed, bool glow) {
  ImVec2 c1, c2;
  bezierPoints(a, b, z, c1, c2);
  if (glow) dl->AddBezierCubic(a, c1, c2, b, theme::withAlpha(col, 0.25f), th + 4.0f * std::min(1.0f, z + 0.3f), 0);
  if (!dashed) {
    dl->AddBezierCubic(a, c1, c2, b, col, th, 0);
    return;
  }
  ImVec2 last = a;
  for (int k = 1; k <= 28; ++k) {
    const ImVec2 p = bezierAt(a, c1, c2, b, static_cast<float>(k) / 28.0f);
    if (k % 2) dl->AddLine(last, p, col, th);
    last = p;
  }
}

std::string paramName(const JGraph& g, const JLink& l) {
  if (l.kind != JKind::Param) return "";
  const JNode* d = g.node(l.dst);
  const PluginInfo* info = d ? findPlugin(d->plugin.type) : nullptr;
  std::string to = info && l.dp >= 0 && static_cast<size_t>(l.dp) < info->params.size() ? info->params[static_cast<size_t>(l.dp)].name : std::string("?");
  if (l.src == kJInputs) return jControlName(g, l.sp) + " > " + to;
  const JNode* s = g.node(l.src);
  const PluginInfo* si = s ? findPlugin(s->plugin.type) : nullptr;
  return (si && l.sp >= 0 && static_cast<size_t>(l.sp) < si->outputs.size() ? si->outputs[static_cast<size_t>(l.sp)] : std::string("Out")) + " > " + to;
}

bool visible(const View& v, JKind k) { return k == JKind::Audio ? v.showAudio : k == JKind::Notes ? v.showNotes : v.showParams; }

// The menu of parameters of a module; `pick` gets the parameter index.
void paramMenu(const JNode& n, const std::function<void(int)>& pick) {
  const PluginInfo* info = findPlugin(n.plugin.type);
  if (!info) {
    ImGui::TextDisabled("%s", i18n::tr("No parameters"));
    return;
  }
  size_t shown = 0;
  for (size_t k = 0; k < info->params.size() && shown < 400; ++k) {
    if (info->params[k].name.empty()) continue;
    ++shown;
    ImGui::PushID(static_cast<int>(k));
    if (ImGui::MenuItem(info->params[k].name.c_str())) pick(static_cast<int>(k));
    ImGui::PopID();
  }
}

// Adds a plugin; connects it to the cable being dragged when there is one.
void addPlugin(EditTarget& e, View& v, const std::string& type, ImVec2 at, bool connect) {
  if (type == "gl.patcher" || type == "gl.patcherfx") {
    e.a->notify(i18n::tr("A Junction cannot run inside a Junction."), true);
    return;
  }
  PluginState st = defaultPluginState(type);
  if (st.empty()) {
    st.type = type;
  }
  const uint32_t id = jAddNode(v.g, st, at.x, at.y);
  if (!id) {
    e.a->notify(i18n::tr("This Junction is full."), true);
    return;
  }
  v.sel = {id};
  if (connect) {
    const JNode* n = v.g.node(id);
    const std::vector<JPort> ports = jNodePorts(*n);
    const bool fromOut = v.cablePort.out;
    for (const JPort& p : ports)
      if (p.kind == v.cablePort.kind && p.out != fromOut && p.index >= 0) {
        JLink l;
        l.kind = p.kind;
        if (fromOut) {
          l.src = v.cableNode, l.sp = v.cablePort.index, l.dst = id, l.dp = p.index;
        } else {
          l.src = id, l.sp = p.index, l.dst = v.cableNode, l.dp = v.cablePort.index;
        }
        jConnect(v.g, l, e.st->type == "gl.patcherfx");
        break;
      }
  }
  edit(v, Pending::Graph, "add module");
}

void addMenuBody(EditTarget& e, View& v) {
  const bool fx = e.st->type == "gl.patcherfx";
  auto pick = [&](const std::string& type) { addPlugin(e, v, type, v.addAt, v.addConnects); };
  if (!fx && ImGui::BeginMenu(i18n::tr("Instrument"))) {
    favui::pluginMenu(PluginKind::Generator, pick, "");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Effect"))) {
    favui::pluginMenu(PluginKind::Effect, pick, "");
    ImGui::EndMenu();
  }
  if (!fx && ImGui::BeginMenu(i18n::tr("Note processor"))) {
    for (const auto& p : pluginCatalog())
      if (p.kind == PluginKind::NoteFx && !p.hidden && ImGui::MenuItem(p.name.c_str())) pick(p.type);
    ImGui::EndMenu();
  }
}

// One LED filter of the toolbar.
void filterToggle(const char* label, bool& on, ImU32 col, const char* hint) {
  ImGui::PushID(label);
  const float tw = ImGui::CalcTextSize(label).x;
  const ImVec2 size(tw + 22, ImGui::GetFrameHeight());
  ImVec2 p = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##t", size)) on = !on;
  const bool hov = ImGui::IsItemHovered();
  if (hov && hint) ui::Hint(hint);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + 8, p.y + size.y * 0.5f);
  if (on) dl->AddCircleFilled(c, 6.5f, theme::withAlpha(col, 0.25f), 16);
  dl->AddCircleFilled(c, 3.6f, on ? col : theme::withAlpha(pal().textFaint, 0.6f), 16);
  dl->AddText(ImVec2(p.x + 18, p.y + (size.y - ImGui::GetFontSize()) * 0.5f), on || hov ? pal().text : pal().textDim, label);
  ImGui::PopID();
}

// ------------------------------------------------------------------- the map
void drawMap(EditTarget& e, View& v, const skin::Skin& K, float W, float H) {
  const auto& P = pal();
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + W, p0.y + H);
  v.origin = p0;
  const bool fx = e.st->type == "gl.patcherfx";
  ImGui::InvisibleButton("##jmap", ImVec2(W, H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(p0, p1, true);
  dl->AddRectFilled(p0, p1, K.graphBg, 5);
  // Smooth zoom about the pointer.
  if (hovered && io.MouseWheel != 0) {
    v.zoomTo = std::clamp(v.zoomTo * std::pow(1.12f, io.MouseWheel), kMinZoom, kMaxZoom);
    v.zoomAt = ImVec2(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
  }
  if (std::fabs(v.zoom - v.zoomTo) > 1e-4f) {
    const float nz = v.zoom + (v.zoomTo - v.zoom) * (1.0f - std::exp(-io.DeltaTime * 22.0f));
    const float z = std::fabs(nz - v.zoomTo) < 1e-3f ? v.zoomTo : nz;
    v.panX = v.zoomAt.x - (v.zoomAt.x - v.panX) * (z / v.zoom);
    v.panY = v.zoomAt.y - (v.zoomAt.y - v.panY) * (z / v.zoom);
    v.zoom = z;
  }
  const Xf X{p0, v.panX, v.panY, v.zoom};
  // Dot grid.
  {
    const float step = (v.zoom < 0.55f ? 48.0f : 24.0f) * v.zoom;
    const float sx = std::fmod(v.panX, step), sy = std::fmod(v.panY, step);
    const ImU32 dot = theme::withAlpha(K.graphGridStrong, 0.85f);
    for (float y = sy; y < H; y += step)
      for (float x = sx; x < W; x += step) dl->AddRectFilled(ImVec2(p0.x + x, p0.y + y), ImVec2(p0.x + x + 1.2f, p0.y + y + 1.2f), dot);
  }
  const ImVec2 m = io.MousePos;
  const ImVec2 mc = X.inv(m);
  // What is under the pointer: ports first, then modules, then cables.
  uint32_t hoverNode = 0;
  const Port* hoverPort = nullptr;
  uint32_t hoverPortNode = 0;
  const float portReach = std::max(7.0f, 9.0f * v.zoom);
  if (hovered) {
    for (auto it = v.items.rbegin(); it != v.items.rend() && !hoverPort; ++it)
      for (const Port& q : it->ports) {
        const ImVec2 at = portAt(X, *it, q);
        const float dx = at.x - m.x, dy = at.y - m.y;
        if (!visible(v, q.p.kind) || dx * dx + dy * dy > portReach * portReach) continue;
        hoverPort = &q;
        hoverPortNode = it->id;
        break;
      }
    for (auto it = v.items.rbegin(); it != v.items.rend(); ++it)
      if (mc.x >= it->x && mc.x < it->x + it->w && mc.y >= it->y && mc.y < it->y + it->h) {
        hoverNode = it->id;
        break;
      }
  }
  int hoverLink = -1;
  // Moving modules: their position follows the pointer until release.
  ImVec2 moveBy(0, 0);
  if (v.drag == Drag::Move) moveBy = ImVec2((m.x - v.dragStart.x) / v.zoom, (m.y - v.dragStart.y) / v.zoom);
  auto shifted = [&](const Item& it) {
    Item c = it;
    if (v.drag == Drag::Move && v.sel.count(it.id)) c.x += moveBy.x, c.y += moveBy.y;
    return c;
  };
  // ---- cables
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t k = 0; k < v.g.links.size(); ++k) {
      const JLink& l = v.g.links[k];
      if (!visible(v, l.kind)) continue;
      const Item* a = itemOf(v, l.src);
      const Item* b = itemOf(v, l.dst);
      if (!a || !b) continue;
      const Port* qa = a->find(l.kind, true, l.src == kJInputs && l.kind == JKind::Param ? -1 : l.sp);
      const Port* qb = b->find(l.kind, false, l.kind == JKind::Param ? -1 : l.dp);
      if (!qa || !qb) continue;
      const Item ia = shifted(*a), ib = shifted(*b);
      const ImVec2 pa = portAt(X, ia, *qa), pb = portAt(X, ib, *qb);
      ImVec2 c1, c2;
      bezierPoints(pa, pb, v.zoom, c1, c2);
      if (pass == 0) {
        if (hovered && !hoverPort && !hoverNode && hoverLink < 0 && v.drag == Drag::None && nearCable(pa, c1, c2, pb, m, 6.0f)) hoverLink = static_cast<int>(k);
        continue;
      }
      const bool hot = static_cast<int>(k) == hoverLink || v.sel.count(l.src) || v.sel.count(l.dst);
      ImU32 col = kindColor(l.kind);
      const bool fb = k < v.feedback.size() && v.feedback[k];
      float th = l.kind == JKind::Param ? 1.25f : 2.0f;
      if (l.kind == JKind::Param && !hot) col = theme::withAlpha(col, 0.55f);
      if (l.kind == JKind::Audio && l.gain <= 0.0f) col = theme::withAlpha(col, 0.3f);
      th *= std::clamp(v.zoom, 0.6f, 1.4f);
      drawCable(dl, pa, pb, v.zoom, col, th, fb, hot);
      const bool roomy = fb || std::fabs(pb.x - pa.x) + std::fabs(pb.y - pa.y) > 92.0f * v.zoom;
      if (l.kind == JKind::Audio && v.labels && (l.gain != 1.0f || fb) && v.zoom > 0.55f && roomy) {
        const ImVec2 mid = bezierAt(pa, c1, c2, pb, 0.5f);
        char buf[32];
        if (l.gain > 0.0f) std::snprintf(buf, sizeof buf, "%s%+.1f dB", fb ? "fb " : "", 20.0 * std::log10(static_cast<double>(l.gain)));
        else std::snprintf(buf, sizeof buf, "off");
        ImGui::PushFont(e.a->fontMono, 10.0f);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 4, mid.y - ts.y * 0.5f - 1.5f), ImVec2(mid.x + ts.x * 0.5f + 4, mid.y + ts.y * 0.5f + 1.5f), theme::withAlpha(K.lcdBg, 0.92f), 3);
        dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), K.lcdText, buf);
        ImGui::PopFont();
      }
    }
  }
  // ---- cable being dragged
  if (v.drag == Drag::Cable) {
    if (const Item* it = itemOf(v, v.cableNode))
      if (const Port* q = it->find(v.cablePort.kind, v.cablePort.out, v.cablePort.index)) {
        const ImVec2 a = portAt(X, *it, *q);
        const ImU32 col = kindColor(v.cablePort.kind);
        if (v.cablePort.out) drawCable(dl, a, m, v.zoom, col, 2.0f, false, true);
        else drawCable(dl, m, a, v.zoom, col, 2.0f, false, true);
      }
  }
  // ---- modules
  const float textZoom = std::clamp(v.zoom, 0.5f, 1.6f);
  for (const Item& it0 : v.items) {
    const Item it = shifted(it0);
    const ImVec2 a = X.pt(it.x, it.y), b = X.pt(it.x + it.w, it.y + it.h);
    if (b.x < p0.x - 12 || a.x > p1.x + 12 || b.y < p0.y - 12 || a.y > p1.y + 12) continue;
    const bool selected = v.sel.count(it.id) > 0;
    const bool hov = hoverNode == it.id;
    const JNode* jn = it.host ? nullptr : v.g.node(it.id);
    const bool dim = jn && jn->mode != JMode::On;
    const float rnd = 6.0f * std::min(1.0f, v.zoom + 0.2f);
    // Soft shadow.
    dl->AddRectFilled(ImVec2(a.x + 1, a.y + 3), ImVec2(b.x + 1, b.y + 4), theme::withAlpha(IM_COL32_BLACK, 0.28f), rnd);
    skin::gradientRect(dl, a, b, skin::lighten(K.panel, selected ? 0.13f : hov ? 0.09f : 0.05f), skin::darken(K.panel, 0.08f), rnd);
    const float hh = kHead * v.zoom;
    dl->AddRectFilled(a, ImVec2(b.x, a.y + hh), theme::withAlpha(it.tint, dim ? 0.1f : 0.2f), rnd, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, a.y + hh - std::max(1.0f, 1.5f * v.zoom)), ImVec2(b.x, a.y + hh), theme::withAlpha(it.tint, dim ? 0.35f : 0.95f));
    dl->AddRect(a, b, selected ? K.select : theme::withAlpha(hov ? K.text : K.textDim, hov ? 0.55f : 0.3f), rnd, 0, selected ? 1.6f : 1.0f);
    if (v.zoom > 0.42f) {
      ImGui::PushFont(e.a->fontBold, 12.0f * textZoom);
      theme::textEllipsis(dl, e.a->fontBold, 12.0f * textZoom, ImVec2(a.x + 8 * v.zoom, a.y + (hh - 12.0f * textZoom) * 0.5f - 1), (it.w - 30) * v.zoom, dim ? K.textDim : K.text, it.title.c_str());
      ImGui::PopFont();
    }
    // Activity lamp.
    if (jn && v.activity) {
      float level = 0;
      if (auto f = v.levels.find(it.id); f != v.levels.end()) level = f->second;
      const ImVec2 c(b.x - 11 * v.zoom, a.y + hh * 0.5f);
      const float lv = std::clamp(std::sqrt(level), 0.0f, 1.0f);
      dl->AddCircleFilled(c, 3.2f * v.zoom, theme::mix(theme::withAlpha(K.textDim, 0.4f), theme::litRole(theme::role::kPlay), lv), 12);
    }
    if (jn && jn->mode != JMode::On && v.zoom > 0.42f) {
      const char* tag = jn->mode == JMode::Bypass ? "BYPASS" : "MUTE";
      ImGui::PushFont(e.a->fontMono, 9.0f * textZoom);
      const ImVec2 ts = ImGui::CalcTextSize(tag);
      dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, b.y - ts.y - 3 * v.zoom), K.textDim, tag);
      ImGui::PopFont();
    }
    // Ports.
    for (const Port& q : it.ports) {
      if (!visible(v, q.p.kind)) continue;
      const ImVec2 c = portAt(X, it, q);
      const ImU32 col = kindColor(q.p.kind);
      bool connected = false;
      for (const JLink& l : v.g.links) {
        if (l.kind != q.p.kind) continue;
        if (q.p.out ? (l.src == it.id && (q.p.index < 0 || l.sp == q.p.index)) : (l.dst == it.id && (q.p.index < 0 || l.dp == q.p.index))) connected = true;
      }
      bool hit = hoverPort == &it0.ports[static_cast<size_t>(&q - it.ports.data())];
      const bool compatible = v.drag == Drag::Cable && q.p.kind == v.cablePort.kind && q.p.out != v.cablePort.out && it.id != v.cableNode;
      const float r = (hit ? kPortR + 1.6f : compatible ? kPortR + 1.0f : kPortR) * std::clamp(v.zoom, 0.55f, 1.3f);
      if (hit || compatible) dl->AddCircleFilled(c, r + 3.0f, theme::withAlpha(col, 0.25f), 16);
      dl->AddCircleFilled(c, r, skin::darken(K.panel, 0.25f), 16);
      if (connected || hit) dl->AddCircleFilled(c, r - 1.0f, col, 16);
      else dl->AddCircle(c, r - 0.6f, col, 16, 1.4f);
      if (v.labels && v.zoom > 0.72f) {
        ImGui::PushFont(e.a->fontRegular, 10.0f * textZoom);
        const float tw = ImGui::CalcTextSize(q.p.name.c_str()).x;
        const float tx = q.p.out ? c.x - tw - 8 * v.zoom : c.x + 8 * v.zoom;
        dl->AddText(ImVec2(tx, c.y - 10.0f * textZoom * 0.56f), theme::withAlpha(K.textDim, 0.95f), q.p.name.c_str());
        ImGui::PopFont();
      }
    }
    // Extra lines in the module.
    if (jn && (v.latency || v.perf) && v.zoom > 0.5f) {
      std::string line;
      if (v.latency) {
        const int lat = isHostedType(jn->plugin.type) ? 0 : gl::effectLatency(jn->plugin, e.a->builder.sampleRate());
        if (lat > 0) line += std::to_string(lat) + " smp ";
      }
      if (v.perf) {
        if (auto f = v.loads.find(it.id); f != v.loads.end()) {
          char load[24];
          std::snprintf(load, sizeof load, "%.1f%%", static_cast<double>(f->second) * 100.0);
          line += load;
        }
      }
      if (!line.empty()) {
        ImGui::PushFont(e.a->fontMono, 9.0f * textZoom);
        dl->AddText(ImVec2(a.x + 8 * v.zoom, b.y - 13 * textZoom), K.lcdText, line.c_str());
        ImGui::PopFont();
      }
    }
  }
  // ---- box selection
  if (v.drag == Drag::Box) {
    const ImVec2 a(std::min(v.dragStart.x, m.x), std::min(v.dragStart.y, m.y)), b(std::max(v.dragStart.x, m.x), std::max(v.dragStart.y, m.y));
    dl->AddRectFilled(a, b, theme::withAlpha(K.select, 0.12f), 2);
    dl->AddRect(a, b, theme::withAlpha(K.select, 0.7f), 2);
  }
  // ---- minimap
  ImVec2 mm0(0, 0), mm1(0, 0);
  float mmz = 1, mmx = 0, mmy = 0;
  if (v.minimap && v.items.size() > 2) {
    const float size = v.miniSize == 0 ? 90.0f : v.miniSize == 1 ? 136.0f : 190.0f;
    float x0, y0, x1, y1;
    bounds(v, x0, y0, x1, y1);
    const float vw0 = -v.panX / v.zoom, vh0 = -v.panY / v.zoom, vw1 = vw0 + W / v.zoom, vh1 = vh0 + H / v.zoom;
    x0 = std::min(x0, vw0), y0 = std::min(y0, vh0), x1 = std::max(x1, vw1), y1 = std::max(y1, vh1);
    mmz = size / std::max(1.0f, std::max(x1 - x0, (y1 - y0) * 1.4f));
    const float mw = (x1 - x0) * mmz + 8, mh = (y1 - y0) * mmz + 8;
    const float mx = v.miniCorner == 0 || v.miniCorner == 3 ? p0.x + 8 : p1.x - mw - 8;
    const float my = v.miniCorner < 2 ? p0.y + 8 : p1.y - mh - 8;
    mm0 = ImVec2(mx, my), mm1 = ImVec2(mx + mw, my + mh);
    mmx = mx + 4 - x0 * mmz, mmy = my + 4 - y0 * mmz;
    dl->AddRectFilled(mm0, mm1, theme::withAlpha(skin::darken(K.graphBg, 0.3f), 0.92f), 4);
    dl->AddRect(mm0, mm1, theme::withAlpha(K.textDim, 0.35f), 4);
    for (const Item& it : v.items)
      dl->AddRectFilled(ImVec2(mmx + it.x * mmz, mmy + it.y * mmz), ImVec2(mmx + (it.x + it.w) * mmz, mmy + (it.y + it.h) * mmz),
                        v.sel.count(it.id) ? K.select : theme::withAlpha(it.tint, 0.8f), 1);
    dl->AddRect(ImVec2(mmx + vw0 * mmz, mmy + vh0 * mmz), ImVec2(mmx + vw1 * mmz, mmy + vh1 * mmz), theme::withAlpha(K.text, 0.8f), 1);
  }
  const bool overMini = v.minimap && v.items.size() > 2 && m.x >= mm0.x && m.x < mm1.x && m.y >= mm0.y && m.y < mm1.y;
  // ---- hint
  if (hovered) {
    if (hoverPort) {
      e.a->setHint(hoverPort->p.name + i18n::tr(":  drag to another port to connect"));
    } else if (hoverNode > kJOutputs) {
      const JNode* n = v.g.node(hoverNode);
      const PluginInfo* info = n ? findPlugin(n->plugin.type) : nullptr;
      if (n) e.a->setHint(nodeTitle(*n) + (info ? "" : std::string("  ") + i18n::tr("(not available here)")) + i18n::tr(":  double click for its interface, right click for its menu"));
    } else if (hoverLink >= 0) {
      const JLink& l = v.g.links[static_cast<size_t>(hoverLink)];
      const bool fb = v.feedback[static_cast<size_t>(hoverLink)];
      e.a->setHint(l.kind == JKind::Param ? paramName(v.g, l) : std::string(fb ? i18n::tr("Feedback cable: arrives 256 samples later. Right click to set its level.") : i18n::tr("Right click a cable for its level, to remove it or to insert an effect.")));
    } else if (!hoverNode) {
      e.a->setHint(i18n::tr("Double click to add a module. Drag to select, wheel to zoom, Alt or middle drag to pan."));
    }
  }
  dl->PopClipRect();
  // ---- mouse
  const bool panKey = io.KeyAlt;
  if (hovered && v.drag == Drag::None) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (overMini) {
        v.drag = Drag::Mini;
      } else if (panKey) {
        v.drag = Drag::Pan;
      } else if (hoverPort) {
        v.drag = Drag::Cable;
        v.cableNode = hoverPortNode;
        v.cablePort = hoverPort->p;
      } else if (hoverNode) {
        if (io.KeyShift || io.KeyCtrl || io.KeySuper) {
          if (!v.sel.insert(hoverNode).second) v.sel.erase(hoverNode);
        } else if (!v.sel.count(hoverNode)) {
          v.sel = {hoverNode};
        }
        v.drag = Drag::Move;
        v.dragStart = m;
      } else {
        if (!io.KeyShift) v.sel.clear();
        v.drag = Drag::Box;
        v.dragStart = m;
      }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
      v.drag = Drag::Pan;
    }
  }
  if (v.drag == Drag::Pan) {
    if (active || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      v.panX += io.MouseDelta.x, v.panY += io.MouseDelta.y;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) v.drag = Drag::None;
  } else if (v.drag == Drag::Mini) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float cx = (m.x - mmx) / mmz, cy = (m.y - mmy) / mmz;
      v.panX = W * 0.5f - cx * v.zoom;
      v.panY = H * 0.5f - cy * v.zoom;
    } else {
      v.drag = Drag::None;
    }
  } else if (v.drag == Drag::Move && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    v.drag = Drag::None;
    if (moveBy.x != 0 || moveBy.y != 0) {
      uint32_t last = 0;
      for (uint32_t id : v.sel) {
        last = id;
        if (id == kJInputs) v.g.inX += moveBy.x, v.g.inY += moveBy.y;
        else if (id == kJOutputs) v.g.outX += moveBy.x, v.g.outY += moveBy.y;
        else if (JNode* n = v.g.node(id)) n->x += moveBy.x, n->y += moveBy.y;
      }
      // One module of an FL patch moved: its layout record is enough.
      edit(v, v.sel.size() == 1 && last >= kJFirstId ? Pending::Move : Pending::Graph, "move module", 0, last);
      rebuild(v, *e.st);
    }
  } else if (v.drag == Drag::Box && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    v.drag = Drag::None;
    const ImVec2 a = X.inv(ImVec2(std::min(v.dragStart.x, m.x), std::min(v.dragStart.y, m.y)));
    const ImVec2 b = X.inv(ImVec2(std::max(v.dragStart.x, m.x), std::max(v.dragStart.y, m.y)));
    if (b.x - a.x > 2 || b.y - a.y > 2)
      for (const Item& it : v.items)
        if (it.x < b.x && it.x + it.w > a.x && it.y < b.y && it.y + it.h > a.y) v.sel.insert(it.id);
  } else if (v.drag == Drag::Cable && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    v.drag = Drag::None;
    // The port under the pointer now (hover tests above run for the hovered item only: the
    // active button stays hovered while it holds the mouse).
    const Port* to = hoverPort;
    uint32_t toNode = hoverPortNode;
    if (to && to->p.kind == v.cablePort.kind && to->p.out != v.cablePort.out && toNode != v.cableNode) {
      JLink l;
      l.kind = to->p.kind;
      const bool fromOut = v.cablePort.out;
      l.src = fromOut ? v.cableNode : toNode;
      l.sp = fromOut ? v.cablePort.index : to->p.index;
      l.dst = fromOut ? toNode : v.cableNode;
      l.dp = fromOut ? to->p.index : v.cablePort.index;
      if (l.kind == JKind::Param && (l.dp < 0 || l.sp < 0)) {
        v.pendingLink = l;
        ImGui::OpenPopup("##jparam");
      } else {
        std::string why;
        if (jConnect(v.g, l, fx, &why)) edit(v, Pending::Graph, "connect");
        else e.a->notify(why, true);
      }
    } else if (!to && v.cablePort.kind != JKind::Param && hovered) {
      v.addAt = mc;
      v.addConnects = true;
      ImGui::OpenPopup("##jadd");
    }
  }
  // Double clicks and right clicks.
  if (hovered && !overMini) {
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      v.drag = Drag::None;
      if (hoverNode > kJOutputs) {
        const JNode* n = v.g.node(hoverNode);
        if (n && isHostedType(n->plugin.type)) {
          PluginTarget t{e.owner, e.slot};
          t.node = n->id;
          openHostedEditor(*e.a, t);
        }
      } else if (!hoverNode && !hoverPort) {
        v.addAt = mc;
        v.addConnects = false;
        ImGui::OpenPopup("##jadd");
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (hoverNode > kJOutputs) {
        if (!v.sel.count(hoverNode)) v.sel = {hoverNode};
        v.menuNode = hoverNode;
        std::snprintf(v.renameBuf, sizeof v.renameBuf, "%s", v.g.node(hoverNode)->name.c_str());
        ImGui::OpenPopup("##jnode");
      } else if (hoverLink >= 0) {
        v.menuLink = hoverLink;
        ImGui::OpenPopup("##jcable");
      } else {
        v.addAt = mc;
        v.addConnects = false;
        ImGui::OpenPopup("##jmapmenu");
      }
    }
    // Keys while the pointer is on the map.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
      bool any = false;
      for (uint32_t id : v.sel)
        if (id >= kJFirstId) jRemoveNode(v.g, id), any = true;
      if (any) {
        v.sel.clear();
        edit(v, Pending::Graph, "remove module");
      }
    }
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_A, false))
      for (const JNode& n : v.g.nodes) v.sel.insert(n.id);
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
      std::set<uint32_t> made;
      for (uint32_t id : v.sel)
        if (id >= kJFirstId)
          if (const uint32_t c = jDuplicateNode(v.g, id, 24, 24)) made.insert(c);
      if (!made.empty()) {
        v.sel = made;
        edit(v, Pending::Graph, "duplicate module");
      }
    }
    if (!e.a->typingKeyboard && !io.KeyCtrl && !io.KeySuper) {  // (letters play notes on the typing keyboard)
      if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fitView(v, W, H);
      if (ImGui::IsKeyPressed(ImGuiKey_M, false)) v.minimap = !v.minimap;
      if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        bool any = false;
        for (uint32_t id : v.sel)
          if (JNode* n = v.g.node(id)) n->mode = n->mode == JMode::Bypass ? JMode::On : JMode::Bypass, any = true;
        if (any) edit(v, Pending::Graph, "bypass module");
      }
    }
  }
  // ---- popups
  if (ImGui::BeginPopup("##jadd")) {
    addMenuBody(e, v);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jmapmenu")) {
    addMenuBody(e, v);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Arrange modules"))) {
      jAutoArrange(v.g);
      edit(v, Pending::Graph, "arrange modules");
      v.fitted = false;
    }
    if (ImGui::MenuItem(i18n::tr("Show everything"), "F")) fitView(v, W, H);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jparam")) {
    JLink& l = v.pendingLink;
    const JNode* dst = v.g.node(l.dst);
    const JNode* src = v.g.node(l.src);
    auto connect = [&](int sp, int dp) {
      JLink c = l;
      c.sp = sp, c.dp = dp;
      std::string why;
      if (jConnect(v.g, c, fx, &why)) edit(v, Pending::Graph, "connect parameter");
      else e.a->notify(why, true);
    };
    auto bySource = [&](int sp) {
      if (!dst) return;
      if (l.dp >= 0) {
        if (ImGui::MenuItem(i18n::tr("Connect"))) connect(sp, l.dp);
      } else {
        paramMenu(*dst, [&](int dp) { connect(sp, dp); });
      }
    };
    if (l.src == kJInputs) {
      // The Panel's knobs: the eight macros and every exposed parameter.
      for (int m2 = 0; m2 < kMacros; ++m2) {
        ImGui::PushID(m2);
        if (ImGui::BeginMenu(jControlName(v.g, m2).c_str())) {
          bySource(m2);
          ImGui::EndMenu();
        }
        ImGui::PopID();
      }
      for (const JControl& c : v.g.controls)
        if (c.param >= PhMap0) {
          ImGui::PushID(c.param);
          if (ImGui::BeginMenu(jControlName(v.g, c.param).c_str())) {
            bySource(c.param);
            ImGui::EndMenu();
          }
          ImGui::PopID();
        }
    } else if (l.sp < 0) {
      const PluginInfo* si = src ? findPlugin(src->plugin.type) : nullptr;
      if (si)
        for (size_t k = 0; k < si->outputs.size(); ++k) {
          ImGui::PushID(static_cast<int>(k));
          if (ImGui::BeginMenu(si->outputs[k].c_str())) {
            bySource(static_cast<int>(k));
            ImGui::EndMenu();
          }
          ImGui::PopID();
        }
    } else {
      bySource(l.sp);
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jnode")) {
    JNode* n = v.g.node(v.menuNode);
    if (!n) {
      ImGui::CloseCurrentPopup();
    } else {
      if (isHostedType(n->plugin.type) && ImGui::MenuItem(i18n::tr("Open interface"))) {
        PluginTarget t{e.owner, e.slot};
        t.node = n->id;
        openHostedEditor(*e.a, t);
      }
      ImGui::SetNextItemWidth(170);
      if (ImGui::InputTextWithHint("##name", i18n::tr("Name"), v.renameBuf, sizeof v.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        n->name = v.renameBuf;
        edit(v, Pending::Graph, "rename module");
      }
      {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        for (int k = 0; k < kSwatchCount; ++k) {
          ImGui::PushID(k);
          if (k) ImGui::SameLine(0, 3);
          const ImVec2 at = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("##c", ImVec2(16, 16))) {
            n->color = k;
            edit(v, Pending::Graph, "module colour");
          }
          const ImU32 c = k ? theme::litRole(kSwatches[k]) : theme::withAlpha(pal().textDim, 0.5f);
          pdl->AddCircleFilled(ImVec2(at.x + 8, at.y + 8), 6.5f, c, 16);
          if (n->color == k) pdl->AddCircle(ImVec2(at.x + 8, at.y + 8), 7.5f, pal().text, 16, 1.4f);
          ImGui::PopID();
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Bypass"), nullptr, n->mode == JMode::Bypass)) {
        n->mode = n->mode == JMode::Bypass ? JMode::On : JMode::Bypass;
        edit(v, Pending::Graph, "bypass module");
      }
      if (ImGui::MenuItem(i18n::tr("Mute"), nullptr, n->mode == JMode::Mute)) {
        n->mode = n->mode == JMode::Mute ? JMode::On : JMode::Mute;
        edit(v, Pending::Graph, "mute module");
      }
      if (ImGui::MenuItem(i18n::tr("Duplicate"))) {
        const uint32_t id = jDuplicateNode(v.g, n->id, 24, 24);
        if (id) v.sel = {id};
        edit(v, Pending::Graph, "duplicate module");
      }
      if (ImGui::MenuItem(i18n::tr("Disconnect all"))) {
        jDisconnectNode(v.g, n->id);
        edit(v, Pending::Graph, "disconnect module");
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Delete"), "Del")) {
        jRemoveNode(v.g, n->id);
        v.sel.erase(v.menuNode);
        edit(v, Pending::Graph, "remove module");
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jcable")) {
    if (v.menuLink < 0 || static_cast<size_t>(v.menuLink) >= v.g.links.size()) {
      ImGui::CloseCurrentPopup();
    } else {
      JLink& l = v.g.links[static_cast<size_t>(v.menuLink)];
      if (l.kind == JKind::Audio) {
        float db = l.gain > 0.0f ? 20.0f * std::log10(l.gain) : -60.0f;
        ImGui::SetNextItemWidth(170);
        if (ImGui::SliderFloat("##gain", &db, -60.0f, 12.0f, "%+.1f dB")) {
          l.gain = db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
          edit(v, Pending::Graph, "cable level", e.key ^ 0x4A474E00ull ^ static_cast<uint64_t>(v.menuLink));
        }
        if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
        if (ImGui::MenuItem(i18n::tr("Reset level"))) {
          l.gain = 1;
          edit(v, Pending::Graph, "cable level");
        }
        if (ImGui::BeginMenu(i18n::tr("Insert effect"))) {
          favui::pluginMenu(PluginKind::Effect, [&](const std::string& type) {
            const JLink old = l;
            const Item* a = itemOf(v, old.src);
            const Item* b = itemOf(v, old.dst);
            const float x = a && b ? (a->x + b->x) * 0.5f : mc.x, y = a && b ? (a->y + b->y) * 0.5f + 40 : mc.y;
            const uint32_t id = jAddNode(v.g, defaultPluginState(type), x, y);
            if (!id) return;
            v.g.links.erase(v.g.links.begin() + v.menuLink);
            JLink in = old, out = old;
            in.dst = id, in.dp = 0;
            out.src = id, out.sp = 0, out.gain = 1;
            v.g.links.push_back(in);
            v.g.links.push_back(out);
            v.sel = {id};
            edit(v, Pending::Graph, "insert effect");
          }, "");
          ImGui::EndMenu();
        }
      } else if (l.kind == JKind::Param && l.fl < 0) {
        float r[2] = {l.lo * 100, l.hi * 100};
        ImGui::SetNextItemWidth(190);
        if (ImGui::DragFloat2("##range", r, 0.5f, 0, 100, "%.0f%%")) {
          l.lo = std::clamp(r[0] / 100, 0.0f, 1.0f), l.hi = std::clamp(r[1] / 100, 0.0f, 1.0f);
          edit(v, Pending::Graph, "parameter range", e.key ^ 0x4A524E00ull ^ static_cast<uint64_t>(v.menuLink));
        }
        if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
      }
      if (ImGui::MenuItem(i18n::tr("Disconnect"))) {
        v.g.links.erase(v.g.links.begin() + v.menuLink);
        edit(v, Pending::Graph, "disconnect");
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  (void)P;
}

// ------------------------------------------------------------- the inspector
void nodeInspector(EditTarget& e, View& v, JNode& n, float W) {
  const auto& P = pal();
  const skin::Skin& K = sk::patcher();
  const PluginInfo* info = findPlugin(n.plugin.type);
  const std::string title = nodeTitle(n);
  ImGui::PushFont(e.a->fontBold, 13.0f);
  ImGui::TextColored(theme::vec(K.text), "%s", title.c_str());
  ImGui::PopFont();
  if (info && !n.name.empty()) {
    ImGui::SameLine(0, 8);
    ImGui::TextColored(theme::vec(K.textDim), "%s", info->name.c_str());
  }
  ImGui::SameLine(0, 14);
  if (info && info->kind == PluginKind::Effect) {
    float mix = n.mix * 100;
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##mix", &mix, 0, 100, i18n::tr("Mix %.0f%%"))) {
      n.mix = mix / 100;
      edit(v, Pending::Graph, "module mix", e.key ^ 0x4D495800ull ^ n.id);
    }
    if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
    ImGui::SameLine(0, 6);
  }
  if (ui::TextButton(i18n::tr("Bypass"), n.mode == JMode::Bypass, 60)) {
    n.mode = n.mode == JMode::Bypass ? JMode::On : JMode::Bypass;
    edit(v, Pending::Graph, "bypass module");
  }
  ImGui::SameLine(0, 3);
  if (ui::TextButton(i18n::tr("Mute"), n.mode == JMode::Mute, 50)) {
    n.mode = n.mode == JMode::Mute ? JMode::On : JMode::Mute;
    edit(v, Pending::Graph, "mute module");
  }
  if (info && !info->presets.empty() && !isHostedType(n.plugin.type)) {
    ImGui::SameLine(0, 10);
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##preset", i18n::tr("Presets"))) {
      for (size_t k = 0; k < info->presets.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::Selectable(info->presets[k].name.c_str())) {
          const std::string data = n.plugin.data;
          n.plugin = presetState(*info, k);
          if (n.plugin.data.empty()) n.plugin.data = data;
          edit(v, Pending::Node, "module preset", 0, n.id);
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  }
  if (isHostedType(n.plugin.type)) {
    ImGui::SameLine(0, 10);
    if (ui::TextButton(i18n::tr("Open interface"), false, 110)) {
      PluginTarget t{e.owner, e.slot};
      t.node = n.id;
      openHostedEditor(*e.a, t);
    }
  }
  if (!info) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Not available on this computer. Its state stays in the project."));
    return;
  }
  // Parameters as knobs; right click one to put it on the Panel.
  const float cell = 72;
  float used = 0;
  bool released = false;
  const size_t count = std::min(info->params.size(), n.plugin.params.size());
  for (size_t k = 0; k < count && k < 160; ++k) {
    const ParamDesc& d = info->params[k];
    if (d.name.empty()) continue;
    if (used > 0 && used + cell > W - 8) used = 0;
    else if (used > 0) ImGui::SameLine(0, 0);
    ImGui::PushID(static_cast<int>(k));
    ImGui::BeginGroup();
    const float x = ImGui::GetCursorPosX();
    double val = n.plugin.params[k];
    ImGui::SetCursorPosX(x + (cell - 28) * 0.5f);
    bool rel = false;
    const int paramIndex = static_cast<int>(k);
    bool driven = false;
    for (const JLink& l : v.g.links) driven = driven || (l.kind == JKind::Param && l.dst == n.id && l.dp == paramIndex);
    auto extra = [&] {
      if (ImGui::MenuItem(i18n::tr("Put on the Panel"))) {
        const int hp = jExpose(v.g, n.id, paramIndex, d.name);
        if (hp >= 0) edit(v, Pending::Graph, "put parameter on the panel");
        else e.a->notify(i18n::tr("The Panel is full."), true);
      }
      if (driven && ImGui::MenuItem(i18n::tr("Remove its cable"))) {
        v.g.links.erase(std::remove_if(v.g.links.begin(), v.g.links.end(), [&](const JLink& l) { return l.kind == JKind::Param && l.dst == n.id && l.dp == paramIndex; }), v.g.links.end());
        edit(v, Pending::Graph, "disconnect parameter");
      }
    };
    if (ui::Knob("k", &val, d, 14, nullptr, d.min < 0 && d.max > 0, &rel, extra)) {
      n.plugin.params[k] = static_cast<float>(d.clamp(val));
      edit(v, Pending::Node, "edit module parameter", e.key ^ 0x50484E50ull ^ (static_cast<uint64_t>(n.id) << 12) ^ k, n.id);
    }
    released = released || rel;
    v.devKnobs[static_cast<int>(k)] = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
    if (driven) {  // a cable drives it: a small mark
      const ImVec2 r1 = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(r1.x - 1, ImGui::GetItemRectMin().y + 2), 2.6f, kindColor(JKind::Param), 10);
    }
    const std::string val2 = d.format(n.plugin.params[k]);
    ImGui::PushFont(e.a->fontMono, 10.0f);
    ImVec2 ts = ImGui::CalcTextSize(val2.c_str());
    ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
    ImGui::TextColored(theme::vec(K.text), "%s", val2.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 11.0f);
    {  // A long name ends in an ellipsis; hovering shows it whole.
      const float nameW = ImGui::CalcTextSize(d.name.c_str()).x, room = cell - 4;
      ImVec2 at = ImGui::GetCursorScreenPos();
      at.x += std::max(2.0f, (cell - nameW) * 0.5f);
      theme::textEllipsis(ImGui::GetWindowDrawList(), ImGui::GetFont(), 11.0f, at, room, K.textDim, d.name.c_str());
      if (nameW > room && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + room, at.y + ImGui::GetTextLineHeight()))) ImGui::SetTooltip("%s", d.name.c_str());
      ImGui::SetCursorPosX(x);
      ImGui::Dummy(ImVec2(cell, ImGui::GetTextLineHeight()));
    }
    ImGui::PopFont();
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) e.a->setHint(d.name + ": " + d.format(n.plugin.params[k]));
    ImGui::PopID();
    used += cell;
  }
  if (released) e.a->doc.endGesture();
}

// ----------------------------------------------------------------- the Panel
void drawPanel(EditTarget& e, View& v, float W) {
  const skin::Skin& K = sk::patcher();
  const float cell = 92;
  const int cols = std::max(1, static_cast<int>((W - 12) / cell));
  std::vector<int> params;
  for (int m = 0; m < kMacros; ++m) params.push_back(m);
  for (const JControl& c : v.g.controls)
    if (c.param >= PhMap0) params.push_back(c.param);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  int erased = -1;
  for (size_t i = 0; i < params.size(); ++i) {
    const int hp = params[i];
    ImGui::PushID(hp);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6 + static_cast<float>(static_cast<int>(i) % cols) * cell, origin.y + 6 + static_cast<float>(static_cast<int>(i) / cols) * 96));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    skin::raisedFace(dl, at, ImVec2(at.x + cell - 8, at.y + 88), K.panelHi, 5);
    int links = 0;
    for (const JLink& l : v.g.links) links += l.kind == JKind::Param && l.src == kJInputs && l.sp == hp;
    sk::wheel(e, hp, ImVec2(at.x + (cell - 8 - 40) * 0.5f, at.y + 6), 40, K);
    ImGui::SetCursorScreenPos(ImVec2(at.x + 4, at.y + 50));
    ImGui::SetNextItemWidth(cell - 16);
    std::string name = jControlName(v.g, hp);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s", name.c_str());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushFont(nullptr, 11.0f);
    if (ImGui::InputText("##nm", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) || ImGui::IsItemDeactivatedAfterEdit()) {
      if (name != buf) {
        bool found = false;
        for (JControl& c : v.g.controls)
          if (c.param == hp) c.name = buf, found = true;
        if (!found) v.g.controls.push_back({hp, buf});
        edit(v, Pending::Graph, "rename control");
      }
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(at.x + 4, at.y + 70));
    char more[24];
    std::snprintf(more, sizeof more, links == 1 ? i18n::tr("1 cable") : i18n::tr("%d cables"), links);
    if (ui::TextButton(more, false, cell - 16)) ImGui::OpenPopup("##drives");
    if (ImGui::BeginPopup("##drives")) {
      for (size_t k = 0; k < v.g.links.size(); ++k) {
        JLink& l = v.g.links[k];
        if (l.kind != JKind::Param || l.src != kJInputs || l.sp != hp) continue;
        ImGui::PushID(static_cast<int>(k));
        const JNode* n = v.g.node(l.dst);
        const PluginInfo* info = n ? findPlugin(n->plugin.type) : nullptr;
        std::string text = (n ? nodeTitle(*n) : std::string("?")) + " / " +
                           (info && l.dp >= 0 && static_cast<size_t>(l.dp) < info->params.size() ? info->params[static_cast<size_t>(l.dp)].name : std::string("?"));
        ImGui::TextUnformatted(text.c_str());
        if (l.fl < 0) {
          ImGui::SameLine(250);
          float r[2] = {l.lo * 100, l.hi * 100};
          ImGui::SetNextItemWidth(150);
          if (ImGui::DragFloat2("##r", r, 0.5f, 0, 100, "%.0f%%")) {
            l.lo = std::clamp(r[0] / 100, 0.0f, 1.0f), l.hi = std::clamp(r[1] / 100, 0.0f, 1.0f);
            edit(v, Pending::Graph, "parameter range", e.key ^ 0x4A524E00ull ^ k);
          }
          if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
        }
        ImGui::SameLine(0, 6);
        if (ui::IconButton("x", ui::Icon::Close, false, 20, i18n::tr("Remove this cable"))) {
          v.g.links.erase(v.g.links.begin() + static_cast<long>(k));
          edit(v, Pending::Graph, "disconnect parameter");
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      ImGui::Separator();
      if (ImGui::BeginMenu(i18n::tr("Drive a parameter"))) {
        for (const JNode& n : v.g.nodes) {
          ImGui::PushID(static_cast<int>(n.id));
          if (ImGui::BeginMenu(nodeTitle(n).c_str())) {
            paramMenu(n, [&](int dp) {
              JLink l;
              l.kind = JKind::Param;
              l.src = kJInputs, l.sp = hp, l.dst = n.id, l.dp = dp;
              jConnect(v.g, l, e.st->type == "gl.patcherfx");
              edit(v, Pending::Graph, "connect parameter");
            });
            ImGui::EndMenu();
          }
          ImGui::PopID();
        }
        ImGui::EndMenu();
      }
      if (hp >= PhMap0 && ImGui::MenuItem(i18n::tr("Remove this control"))) erased = hp;
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (erased >= 0) {
    jRemoveControl(v.g, erased);
    edit(v, Pending::Graph, "remove control");
  }
  const int rows = (static_cast<int>(params.size()) + cols - 1) / cols;
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, origin.y + 8 + static_cast<float>(rows) * 96));
  // Snapshots.
  w::caption(i18n::tr("SNAPSHOTS"));
  ImGui::SameLine(0, 10);
  for (size_t k = 0; k < v.g.snaps.size(); ++k) {
    ImGui::PushID(static_cast<int>(k));
    const JSnapshot& s = v.g.snaps[k];
    if (ui::TextButton(s.name.c_str())) {
      PluginState st = *e.st;
      for (const auto& [p, val] : s.values)
        if (p >= 0 && static_cast<size_t>(p) < st.params.size()) st.params[static_cast<size_t>(p)] = val;
      e.set(st, "recall snapshot", 0);
    }
    if (ImGui::BeginPopupContextItem("##snap")) {
      if (ImGui::MenuItem(i18n::tr("Store the current values"))) {
        JSnapshot& mut = v.g.snaps[k];
        mut.values.clear();
        for (int hp : params) mut.values.push_back({hp, e.p(hp)});
        edit(v, Pending::Graph, "store snapshot");
      }
      if (ImGui::MenuItem(i18n::tr("Delete"))) {
        v.g.snaps.erase(v.g.snaps.begin() + static_cast<long>(k));
        edit(v, Pending::Graph, "delete snapshot");
        ImGui::EndPopup();
        ImGui::PopID();
        break;
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 4);
    ImGui::PopID();
  }
  if (ui::TextButton(i18n::tr("+ Store"))) {
    JSnapshot s;
    s.name = std::string(i18n::tr("Snap ")) + std::to_string(v.g.snaps.size() + 1);
    for (int hp : params) s.values.push_back({hp, e.p(hp)});
    v.g.snaps.push_back(std::move(s));
    edit(v, Pending::Graph, "store snapshot");
  }
  ImGui::Dummy(ImVec2(0, 6));
  // The host's own knobs.
  w::caption(i18n::tr("OUTPUT"));
  if (e.st->type == "gl.patcherfx") {
    w::knob(e, PhMix, "Mix");
    ImGui::SameLine(0, 0);
  }
  w::knob(e, PhGain, "Output");
  if (e.st->type == "gl.patcherfx") {
    ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
    ImGui::TextColored(theme::vec(K.textDim), "%s", i18n::tr("Out 2 to Out 5 go to these mixer tracks"));
    const Song& song = e.a->doc.song();
    for (int k = 0; k < 4; ++k) {
      if (k) ImGui::SameLine(0, 6);
      ImGui::PushID(k);
      const int track = static_cast<int>(std::lround(e.p(PhSend0 + k)));
      const std::string now = track >= 0 && static_cast<size_t>(track) < song.mixer.size() ? mixerTrackName(song, static_cast<size_t>(track)) : std::string(i18n::tr("Off"));
      if (ui::TextButton(now.c_str(), track >= 0, 96)) ImGui::OpenPopup("##track");
      if (ImGui::BeginPopup("##track")) {
        if (ImGui::MenuItem(i18n::tr("Off"), nullptr, track < 0)) e.a->doc.setParam(e.pid(PhSend0 + k), -1.0, false);
        for (size_t m = 0; m < song.mixer.size(); ++m) {
          ImGui::PushID(static_cast<int>(m));
          if (ImGui::MenuItem(mixerTrackName(song, m).c_str(), nullptr, static_cast<int>(m) == track)) e.a->doc.setParam(e.pid(PhSend0 + k), static_cast<double>(m), false);
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndGroup();
  }
}

bool patchHostBody(EditTarget& e) {
  if (!e.st) return false;
  const skin::Skin& K = sk::patcher();
  View& v = viewOf(e.key);
  gLastView = &v;
  v.devOwner = e.owner, v.devSlot = e.slot;
  const auto bodyStart = std::chrono::steady_clock::now();
  if (v.devStress > 0) {  // (dev) a large graph
    JGraph g;
    uint32_t last = kJInputs;
    for (int k = 0; k < v.devStress; ++k) {
      const uint32_t id = jAddNode(g, defaultPluginState("gl.filter"), 20.0f + static_cast<float>(k % 16) * 190.0f, 20.0f + static_cast<float>(k / 16) * 110.0f);
      JLink l;
      l.src = last, l.dst = id;
      g.links.push_back(l);
      last = id;
    }
    JLink o;
    o.src = last, o.dst = kJOutputs;
    g.links.push_back(o);
    v.g = std::move(g);
    v.devStress = 0;
    v.fitted = false;
    edit(v, Pending::Graph, "load patch");
    applyPending(e, v);
    rebuild(v, *e.st);
  }
  if (!v.devAdd.empty()) {  // (dev) a module wired through
    PluginState ps = defaultPluginState(v.devAdd);
    const uint32_t id = jAddNode(v.g, ps, 120, 60);
    if (id) {
      const bool fx = e.st->type == "gl.patcherfx";
      JLink in, out;
      in.kind = fx ? JKind::Audio : JKind::Notes;
      in.src = kJInputs, in.dst = id;
      out.src = id, out.dst = kJOutputs;
      jConnect(v.g, in, fx);
      jConnect(v.g, out, fx);
      edit(v, Pending::Graph, "add module");
      applyPending(e, v);
      rebuild(v, *e.st);
    }
    std::printf("junction added %u\n", id);
    v.devLastAdded = id;
    std::fflush(stdout);
    v.devAdd.clear();
  }
  if (v.devPatch >= 0) {
    if (static_cast<size_t>(v.devPatch) < junctionPatches().size()) {
      v.g = junctionPatch(static_cast<size_t>(v.devPatch), e.st->type == "gl.patcherfx");
      v.fitted = false;
      edit(v, Pending::Graph, "load patch");
      applyPending(e, v);
      rebuild(v, *e.st);
    }
    v.devPatch = -1;
  }
  if (v.cached != e.st->data || v.items.empty()) {
    v.g = junctionOf(*e.st);
    v.cached = e.st->data;
    rebuild(v, *e.st);
    for (auto it = v.sel.begin(); it != v.sel.end();)
      it = (*it >= kJFirstId && !v.g.node(*it)) ? v.sel.erase(it) : std::next(it);
  }
  // Live lamps.
  v.levels.clear();
  v.loads.clear();
  JRuntime* runtime = nullptr;
  if (e.owner != kNoId) {
    if (e.slot < 0) {
      if (auto rt = e.a->builder.channelRuntime(e.owner); rt && rt->gen) runtime = junctionRuntime(rt->gen.get());
    } else if (auto rt = e.a->builder.effectRuntime(e.owner, e.slot); rt && rt->fx) {
      runtime = junctionRuntime(rt->fx.get());
    }
  }
  if (runtime) {
    JStats& s = runtime->stats();
    s.measure.store(v.perf, std::memory_order_relaxed);
    for (size_t k = 0; k < v.g.nodes.size() && k < static_cast<size_t>(JStats::kMax); ++k) {
      const uint32_t id = s.id[k].load(std::memory_order_relaxed);
      if (id) {
        v.levels[id] = s.level[k].load(std::memory_order_relaxed);
        v.loads[id] = s.load[k].load(std::memory_order_relaxed);
      }
    }
  }
  const float W = e.width;
  // ---- Toolbar.
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
  ImGui::BeginGroup();
  {
    ImGui::Dummy(ImVec2(0, 2));
    if (ui::TextButton(i18n::tr("Map"), v.page == 0, 56)) v.page = 0;
    ImGui::SameLine(0, 2);
    if (ui::TextButton(i18n::tr("Panel"), v.page == 1, 56)) v.page = 1;
    ImGui::SameLine(0, 12);
    if (v.page == 0) {
      filterToggle(i18n::tr("Audio"), v.showAudio, kindColor(JKind::Audio), i18n::tr("Show audio cables"));
      ImGui::SameLine(0, 4);
      filterToggle(i18n::tr("Notes"), v.showNotes, kindColor(JKind::Notes), i18n::tr("Show note cables"));
      ImGui::SameLine(0, 4);
      filterToggle(i18n::tr("Params"), v.showParams, kindColor(JKind::Param), i18n::tr("Show parameter cables"));
      ImGui::SameLine(0, 10);
      if (ui::TextButton(i18n::tr("+ Add"), false, 56)) {
        const float z = v.zoom;
        v.addAt = ImVec2((W * 0.5f - v.panX) / z, (kCanvasH * 0.5f - v.panY) / z);
        v.addConnects = false;
        ImGui::OpenPopup("##jtopadd");
      }
      ImGui::SameLine(0, 3);
    }
    if (ui::TextButton(i18n::tr("Patches"), false, 64)) ImGui::OpenPopup("##jpatches");
    if (v.page == 0) {
      ImGui::SameLine(0, 3);
      if (ui::TextButton(i18n::tr("View"), false, 48)) ImGui::OpenPopup("##jviewmenu");
    }
  }
  ImGui::EndGroup();
  if (ImGui::BeginPopup("##jtopadd")) {
    addMenuBody(e, v);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jpatches")) {
    const bool fx = e.st->type == "gl.patcherfx";
    const auto& list = junctionPatches();
    for (size_t k = 0; k < list.size(); ++k)
      if ((fx ? list[k].forFx : list[k].forInstrument) && ImGui::MenuItem(i18n::tr(list[k].name))) {
        v.g = junctionPatch(k, fx);
        v.sel.clear();
        v.fitted = false;
        edit(v, Pending::Graph, "load patch");
      }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##jviewmenu")) {
    ImGui::MenuItem(i18n::tr("Activity"), nullptr, &v.activity);
    ImGui::MenuItem(i18n::tr("Labels"), nullptr, &v.labels);
    ImGui::MenuItem(i18n::tr("Latency"), nullptr, &v.latency);
    ImGui::MenuItem(i18n::tr("Performance"), nullptr, &v.perf);
    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("Minimap"), "M", &v.minimap);
    if (ImGui::BeginMenu(i18n::tr("Minimap size"))) {
      if (ImGui::MenuItem(i18n::tr("Small"), nullptr, v.miniSize == 0)) v.miniSize = 0;
      if (ImGui::MenuItem(i18n::tr("Medium"), nullptr, v.miniSize == 1)) v.miniSize = 1;
      if (ImGui::MenuItem(i18n::tr("Large"), nullptr, v.miniSize == 2)) v.miniSize = 2;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Minimap corner"))) {
      const char* names[4] = {"Top left", "Top right", "Bottom right", "Bottom left"};
      for (int k = 0; k < 4; ++k)
        if (ImGui::MenuItem(i18n::tr(names[k]), nullptr, v.miniCorner == k)) v.miniCorner = k;
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Arrange modules"))) {
      jAutoArrange(v.g);
      v.fitted = false;
      edit(v, Pending::Graph, "arrange modules");
    }
    if (ImGui::MenuItem(i18n::tr("Show everything"), "F")) v.fitted = false;
    ImGui::EndPopup();
  }
  ImGui::Dummy(ImVec2(0, 2));
  const float CW = W - 12;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
  ImGui::BeginGroup();
  if (v.page == 1) {
    drawPanel(e, v, CW);
  } else {
    if (!v.fitted) {
      fitView(v, CW, kCanvasH);
      v.fitted = true;
    }
    drawMap(e, v, K, CW, kCanvasH);
    // Status line.
    {
      size_t audio = 0, notes = 0, params = 0;
      for (const JLink& l : v.g.links) (l.kind == JKind::Audio ? audio : l.kind == JKind::Notes ? notes : params)++;
      char buf[160];
      std::snprintf(buf, sizeof buf, i18n::tr("%zu modules, %zu audio, %zu note, %zu parameter cables"), v.g.nodes.size(), audio, notes, params);
      std::string line = buf;
      if (v.latency && runtime) line += "   " + std::to_string(runtime->latency(v.g, e.a->builder.sampleRate())) + " smp";
      ImGui::PushFont(e.a->fontMono, 10.5f);
      ImGui::TextColored(theme::vec(K.textDim), "%s", line.c_str());
      ImGui::PopFont();
    }
    // Inspector.
    if (v.sel.size() == 1 && *v.sel.begin() >= kJFirstId) {
      if (JNode* n = v.g.node(*v.sel.begin())) {
        ImGui::Dummy(ImVec2(0, 2));
        nodeInspector(e, v, *n, CW);
      }
    } else {
      ImGui::PushFont(nullptr, 11.5f);
      ImGui::TextColored(theme::vec(K.textDim), "%s", i18n::tr("Select a module to edit it."));
      ImGui::PopFont();
    }
  }
  ImGui::EndGroup();
  const bool rebuildNeeded = v.pending != Pending::None;
  applyPending(e, v);
  if (rebuildNeeded) rebuild(v, *e.st);
  v.devBodyMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - bodyStart).count();
  v.devBodyWorstMs = std::max(v.devBodyWorstMs, v.devBodyMs);
  return true;
}

}  // namespace

namespace {
JRuntime* devRuntime(App& a, const View& v) {
  if (v.devSlot < 0) {
    auto r = a.builder.channelRuntime(v.devOwner);
    return r && r->gen ? junctionRuntime(r->gen.get()) : nullptr;
  }
  auto r = a.builder.effectRuntime(v.devOwner, v.devSlot);
  return r && r->fx ? junctionRuntime(r->fx.get()) : nullptr;
}
}  // namespace

// K-7 dev checks (--dev-input "<frame> junction ..."):
//   dump: the graph's counts, zoom, page, and every module's rectangle and ports on screen
//   patch <i>: loads factory patch i into the open Junction · page 0|1 · select <id> · fit
//   scan clap <bundle>: registers a test plugin · add <type>: a module wired into the patch
//   open <id>: its own interface (prints the result) · cc <d1> <d2>: a MIDI controller to the Junction's channel
//   value <id> <param>: the module's stored value and the undo position · counts: how builds reached the graph
//   stress <n>: a graph of n effects in series, to move a knob on · timing: the last body draw
bool junctionDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd != "junction") return false;
  View* v = gLastView;
  const std::string op = args.empty() ? "" : args[0];
  if (!v) {
    std::printf("junction: no editor drawn\n");
  } else if (op == "patch" && args.size() > 1) {
    v->devPatch = std::atoi(args[1].c_str());
  } else if (op == "page" && args.size() > 1) {
    v->page = std::atoi(args[1].c_str());
  } else if (op == "select" && args.size() > 1) {
    v->sel = {static_cast<uint32_t>(std::atoi(args[1].c_str()))};
  } else if (op == "fit") {
    v->fitted = false;
  } else if (op == "scan" && args.size() > 2) {
    std::string bundle = args[2];  // (a path with spaces arrives in pieces)
    for (size_t k = 3; k < args.size(); ++k) bundle += " " + args[k];
    const host::ScanResult r = host::findFormat(args[1])->scanInProcess(bundle);
    if (r.ok) host::registerScanResult(r);
    std::printf("junction scan ok %d classes %zu\n", r.ok ? 1 : 0, r.classes.size());
  } else if (op == "add" && args.size() > 1) {
    v->devAdd = args[1];
  } else if (op == "open" && args.size() > 1) {
    PluginTarget t{v->devOwner, v->devSlot};
    t.node = args[1] == "last" ? v->devLastAdded : static_cast<uint32_t>(std::atoi(args[1].c_str()));
    const bool ok = openHostedEditor(a, t);
    std::printf("junction open %s: %d shown %d instance %d\n", args[1].c_str(), ok ? 1 : 0, hostedEditorOpen(t) ? 1 : 0, hostedInstanceFor(a, t) ? 1 : 0);
  } else if (op == "cc" && args.size() > 2) {
    if (JRuntime* rt = devRuntime(a, *v)) rt->midi(0xB0, static_cast<uint8_t>(std::atoi(args[1].c_str())), static_cast<uint8_t>(std::atoi(args[2].c_str())));
  } else if (op == "value" && args.size() > 2) {
    PluginTarget t{v->devOwner, v->devSlot};
    t.node = args[1] == "last" ? v->devLastAdded : static_cast<uint32_t>(std::atoi(args[1].c_str()));
    const PluginState* st = t.state(a.doc.song());
    const size_t k = static_cast<size_t>(std::atoi(args[2].c_str()));
    std::string captured;
    if (host::HostedInstance* hi = hostedInstanceFor(a, t)) hi->captureState(captured);
    std::printf("junction value %s %s = %.4f undo %zu instance %s\n", args[1].c_str(), args[2].c_str(), st && k < st->params.size() ? static_cast<double>(st->params[k]) : -1.0, a.doc.history().position(),
                captured.empty() ? "none" : captured.c_str());
  } else if (op == "counts") {
    JRuntime* rt = devRuntime(a, *v);
    const JRuntime::GraphCounts c = rt ? rt->graphCounts() : JRuntime::GraphCounts{};
    std::printf("junction counts builds %llu values %llu unchanged %llu\n", static_cast<unsigned long long>(c.builds), static_cast<unsigned long long>(c.valueEdits), static_cast<unsigned long long>(c.unchanged));
  } else if (op == "stress" && args.size() > 1) {
    v->devStress = std::atoi(args[1].c_str());
  } else if (op == "timing") {
    std::printf("junction timing body %.2f ms (worst %.2f)\n", static_cast<double>(v->devBodyMs), static_cast<double>(v->devBodyWorstMs));
  } else if (op == "dump") {
    const Xf X{v->origin, v->panX, v->panY, v->zoom};
    std::printf("junction modules %zu links %zu controls %zu snaps %zu selected %zu zoom %.3f page %d v3 %d\n", v->g.nodes.size(), v->g.links.size(), v->g.controls.size(),
                v->g.snaps.size(), v->sel.size(), static_cast<double>(v->zoom), v->page, isJunction3(v->cached) ? 1 : 0);
    for (const auto& kv : v->devKnobs) std::printf("  knob %d at %.0f %.0f\n", kv.first, static_cast<double>(kv.second.x), static_cast<double>(kv.second.y));
    for (const Item& it : v->items) {
      const ImVec2 a = X.pt(it.x, it.y), b = X.pt(it.x + it.w, it.y + it.h);
      std::printf("  module %u at %.0f %.0f %.0f %.0f pos %.1f %.1f\n", it.id, static_cast<double>(a.x), static_cast<double>(a.y), static_cast<double>(b.x), static_cast<double>(b.y),
                  static_cast<double>(it.x), static_cast<double>(it.y));
      for (const Port& q : it.ports) {
        const ImVec2 c = portAt(X, it, q);
        std::printf("    port %u kind %d out %d index %d at %.0f %.0f\n", it.id, static_cast<int>(q.p.kind), q.p.out ? 1 : 0, q.p.index, static_cast<double>(c.x), static_cast<double>(c.y));
      }
    }
  }
  std::fflush(stdout);
  return true;
}

bool drawPatchHostEditor(EditTarget& e) {
  // Slate canvas under a darker top bar, amber selection.
  skin::Backdrop bd(sk::patcher(), e.width, e.key ^ 0x50415443ull, 4);
  return patchHostBody(e);
}

}  // namespace gl::packs::modulation
