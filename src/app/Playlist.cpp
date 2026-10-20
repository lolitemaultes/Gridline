// Gridline — playlist: pattern, audio and automation clips arranged on tracks
// (song mode), song markers in the ruler.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include "../core/I18n.h"
#include "Keys.h"
#include "ExportUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/Fades.h"
#include "../core/TrackModes.h"
#include "../core/MenuOpsExtra.h"
#include "../core/MenuParity.h"
#include "../core/PluginParams.h"
#include "../core/SmoothSnap.h"
#include "../engine/PlaylistTools.h"
#include "../engine/Sample.h"
#include "App.h"
#include "../core/Grouping.h"
#include "J31Ui.h"
#include "../core/NoteTools.h"
#include "AudioRecordUi.h"
#include "AutomationEditor.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "BrowserUi.h"
#include "TrackModeUi.h"
#include "ChopUi.h"
#include "../engine/Chop.h"

namespace gl {

using theme::pal;

namespace {

struct ClipGesture {
  enum Kind { None, Move, Resize, Trim, Select, Paint, Erase, Scrub, TimeSel, AutoPoint, AutoTension, Mute, Slip, Play, Zoom, SliceLine } kind = None;
  int muteTo = -1;     // mute tool: state given to every touched clip
  Tick slipBase = 0;   // slip tool: the clip's offset at the start
  // Paint: clips of `paintSrc` laid back to back from `paintAnchor`.
  Tick paintAnchor = 0, paintLen = 0;
  ClipKind paintKind = ClipKind::Pattern;
  ObjectId paintId = kNoId;
  ImVec2 start;
  Tick startTick = 0;
  int startTrack = 0;
  std::vector<Clip> base;
  ObjectId anchor = kNoId;
  uint64_t key = 0;
  bool moved = false;
  Tick lastScrub = -1;  // last position sent while scrubbing (only send changes)
  // Automation point editing.
  ObjectId autoId = kNoId;
  int point = -1;
  float baseTension = 0;
  bool grouped = false;
};
ClipGesture cg;
uint64_t clipGestureCounter = 1;

// J-37: smooth snapping. Drawn geometry of clips under a snapped drag:
// start tick, end tick, track (fractional while gliding between rows) and
// content position (offset in ticks; audio clips: source start in seconds).
// The document keeps the exact snapped values; only the drawing eases.
smooth::Tracker<4> clipAnim;
// PL-13: the slice tool's cut line(s) glide between grid lines the same way
// (key 1: the hover / start line, key 2: the far end of a region cut).
smooth::Tracker<1> sliceAnim;
smooth::Tracker<4>::Vec clipAnimValues(const Clip& c, Tick len) {
  return {static_cast<double>(c.start), static_cast<double>(c.start + len), static_cast<double>(c.track),
          c.kind == ClipKind::Audio ? c.audioStart : static_cast<double>(c.offset)};
}
bool gestureAnimates(ClipGesture::Kind k) {
  return k == ClipGesture::Move || k == ClipGesture::Resize || k == ClipGesture::Trim || k == ClipGesture::Slip || k == ClipGesture::Paint;
}

// Clip rectangles drawn this frame, in drawing (stacking) order (J-37 / J-38).
struct DrawnClip {
  int track;
  float x0, x1, y0, y1, body;  // body: top of the body (below the title bar)
  size_t ci;                   // playlist index
  ImU32 rim;
  bool sel;
};

void drawClipPreview(ImDrawList* dl, const Song& s, const Pattern& p, ImVec2 a, ImVec2 b, Tick clipLen, Tick offset, ImU32 col, float visX0,
                     float visX1) {
  Tick patLen = std::max<Tick>(1, s.patternContentLength(p));
  float w = b.x - a.x;
  if (w < 6) return;
  // Notes (pitch-scaled) from every channel, then steps as ticks at the bottom.
  // The clip shows pattern ticks [offset, offset + clipLen).
  int lo = 127, hi = 0;
  for (const auto& [ch, ns] : p.notes)
    for (const auto& n : ns) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
  float h = b.y - a.y;
  float span = static_cast<float>(std::max(hi - lo, 8));
  auto xAt = [&](Tick t) { return a.x + w * static_cast<float>(t - offset) / static_cast<float>(clipLen); };
  for (Tick rep = 0; rep < offset + clipLen; rep += patLen) {
    if (rep + patLen > offset && xAt(rep) > visX1) break;  // later repeats start right of the view
    for (const auto& [ch, ns] : p.notes)
      for (const auto& n : ns) {
        Tick t = rep + n.start;
        if (t >= offset + clipLen || t + n.length <= offset) continue;
        float x0 = xAt(std::max(t, offset)), x1 = xAt(std::min(offset + clipLen, t + n.length));
        if (std::max(x0 + 1.5f, x1) < visX0 || x0 > visX1) continue;  // outside the view
        float y = a.y + 2 + (h - 6) * (1.0f - (n.pitch - lo) / span);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x0 + 1.5f, x1), y + 2), col);
      }
    Tick stepT = s.ticksPerStep(p);
    if (stepT > 0) {
      int rowIdx = 0;
      for (const auto& [ch, row] : p.steps) {
        for (size_t k = 0; k < row.size(); ++k) {
          if (!row[k]) continue;
          Tick t = rep + static_cast<Tick>(k) * stepT;
          if (t >= offset + clipLen) break;
          if (t < offset) continue;
          float x = xAt(t);
          if (x > visX1) break;
          if (x + 2 < visX0) continue;
          float y = b.y - 3 - (rowIdx % 4) * 3.0f;
          dl->AddRectFilled(ImVec2(x, y - 2), ImVec2(x + 2, y), col);
        }
        ++rowIdx;
      }
    }
  }
}

// ---- audio clip waveforms: min/max per block of frames, cached per sample ----
struct Peaks {
  static constexpr size_t kBin = 128;
  std::vector<float> lo, hi;
};
const Peaks& peaksFor(const SamplePtr& s) {
  static std::map<const Sample*, std::pair<SamplePtr, Peaks>> cache;
  auto it = cache.find(s.get());
  if (it != cache.end()) return it->second.second;
  if (cache.size() > 256) cache.clear();
  Peaks p;
  size_t n = s->frames(), bins = (n + Peaks::kBin - 1) / Peaks::kBin;
  p.lo.assign(bins, 0.f);
  p.hi.assign(bins, 0.f);
  for (size_t b = 0; b < bins; ++b) {
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = b * Peaks::kBin; i < std::min(n, (b + 1) * Peaks::kBin); ++i) {
      float v = 0.5f * (s->left[i] + s->right[i]);
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    p.lo[b] = lo, p.hi[b] = hi;
  }
  return (cache[s.get()] = {s, std::move(p)}).second;
}

// Source seconds an audio clip plays at song tick t (display approximation of
// the engine's mapping: resample / stretch / tempo sync).
// The mapping's per-clip constants are computed once (the waveform evaluates
// it for every pixel column).
struct AudioMap {
  const TempoMap& tm;
  double audioStart = 0, start = 0, startSec = 0, syncScale = 0, speed = 1;
  bool sync = false;
  AudioMap(const Song& s, const Channel& ch, const Clip& c, const TempoMap& map) : tm(map) {
    double tf = std::max(0.01, c.stretch);
    bool sampler = ch.plugin.type == "gl.sampler" && ch.plugin.params.size() > static_cast<size_t>(pp::SmpTimeStretch);
    if (sampler) tf *= ch.plugin.params[pp::SmpTimeStretch];
    audioStart = c.audioStart;
    start = static_cast<double>(c.start);
    sync = c.syncBpm > 0;
    if (sync) {
      syncScale = 60.0 / (c.syncBpm * s.ppq) / tf;
      return;
    }
    speed = 1.0 / tf;
    if (sampler && ch.plugin.params[pp::SmpStretchMode] < 0.5f)
      speed *= std::pow(2.0, (ch.plugin.params[pp::SmpPitchShift] + c.pitch) / 1200.0);
    startSec = tm.secondsAt(start);
  }
  double at(double t) const { return sync ? audioStart + (t - start) * syncScale : audioStart + (tm.secondsAt(t) - startSec) * speed; }
};

void drawWaveform(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                  float pxPerTick, float visX0, float visX1, ImU32 col) {
  SamplePtr smp = sampleCache().request(ch.plugin.data);  // J-30: never waits for a decode (drawn once ready)
  if (!smp || smp->frames() < 2) return;
  const Peaks& pk = peaksFor(smp);
  const bool rev = c.reverse != (ch.plugin.type == "gl.sampler" && ch.plugin.params.size() > pp::SmpReverse &&
                                 ch.plugin.params[pp::SmpReverse] >= 0.5f);
  const double srcEnd = c.audioEnd > c.audioStart ? std::min(c.audioEnd, smp->seconds()) : smp->seconds();
  const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.5f;
  const float x0 = std::max(a.x, visX0), x1 = std::min(b.x, visX1);
  const AudioMap map(s, ch, c, tm);
  for (float x = std::floor(x0); x < x1; x += 1.0f) {
    double t0 = static_cast<double>(c.start) + (x - clipX0) / pxPerTick, t1 = t0 + 1.0 / pxPerTick;
    double s0 = map.at(t0), s1 = map.at(t1);
    if (s0 >= srcEnd) break;
    s1 = std::min(s1, srcEnd);
    double f0 = s0 * smp->sampleRate, f1 = s1 * smp->sampleRate;
    if (rev) {
      double last = static_cast<double>(smp->frames());
      std::swap(f0, f1);
      f0 = last - f0, f1 = last - f1;
    }
    long b0 = static_cast<long>(std::floor(std::min(f0, f1) / Peaks::kBin)), b1 = static_cast<long>(std::ceil(std::max(f0, f1) / Peaks::kBin));
    b0 = std::clamp<long>(b0, 0, static_cast<long>(pk.lo.size()) - 1);
    b1 = std::clamp<long>(std::max(b1, b0 + 1), 1, static_cast<long>(pk.lo.size()));
    float lo = 0, hi = 0;
    for (long k = b0; k < b1; ++k) lo = std::min(lo, pk.lo[static_cast<size_t>(k)]), hi = std::max(hi, pk.hi[static_cast<size_t>(k)]);
    float g = std::min(1.0f, c.gain);
    dl->AddLine(ImVec2(x + 0.5f, mid - std::clamp(hi * g, -1.f, 1.f) * half), ImVec2(x + 0.5f, mid - std::clamp(lo * g, -1.f, 1.f) * half + 1), col);
  }
}

// Automation curve geometry inside a clip body.
struct AutoGeom {
  ImVec2 a, b;  // body rect
  float clipX0 = 0, pxPerTick = 1;
  Tick start = 0, offset = 0, len = 0;
  float x(Tick content) const { return clipX0 + static_cast<float>(content - offset) * pxPerTick; }
  float y(double v) const { return b.y - static_cast<float>(v) * (b.y - a.y); }
  double valueAtY(float yy) const { return std::clamp(static_cast<double>((b.y - yy) / std::max(1.0f, b.y - a.y)), 0.0, 1.0); }
  bool visible(Tick content) const { return content >= offset && content <= offset + len; }
};

void drawAutomation(ImDrawList* dl, const AutomationClip& ac, const AutoGeom& g, float visX0, float visX1, ImU32 line, ImU32 fill,
                    bool handles, int hotPoint, int hotHandle, ImU32 hot) {
  float x0 = std::max(g.a.x, visX0), x1 = std::min(g.b.x, visX1);
  if (x1 <= x0) return;
  // One value per pixel column; the fill is one triangle strip and the curve
  // one polyline (J-25: was two line primitives per column).
  static std::vector<ImVec2> pts;
  pts.clear();
  for (float x = std::floor(x0); x <= x1; x += 1.0f) {
    double content = static_cast<double>(g.offset) + (x - g.clipX0) / g.pxPerTick;
    pts.emplace_back(x, g.y(automationValue(ac, content)));
  }
  const int n = static_cast<int>(pts.size());
  if (n >= 2 && (fill & IM_COL32_A_MASK)) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6 * (n - 1), 2 * n);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int i = 0; i < n; ++i) {
      dl->PrimWriteVtx(ImVec2(pts[static_cast<size_t>(i)].x + 0.5f, pts[static_cast<size_t>(i)].y), uv, fill);
      dl->PrimWriteVtx(ImVec2(pts[static_cast<size_t>(i)].x + 0.5f, g.b.y), uv, fill);
    }
    for (int i = 0; i + 1 < n; ++i) {
      const ImDrawIdx k = static_cast<ImDrawIdx>(base + 2 * i);
      dl->PrimWriteIdx(k);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 2));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 1));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 3));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + 2));
    }
  }
  if (n >= 2) dl->AddPolyline(pts.data(), n, line, ImDrawFlags_None, 1.5f);
  if (!handles) return;
  const auto& p = ac.points;
  for (size_t i = 0; i < p.size(); ++i) {
    if (!g.visible(p[i].pos)) continue;
    ImVec2 c(g.x(p[i].pos), g.y(p[i].value));
    bool hp = static_cast<int>(i) == hotPoint;
    dl->AddCircleFilled(c, hp ? 4.5f : 3.5f, hp ? hot : line);
    if (i == 0) continue;
    Tick mid = (p[i - 1].pos + p[i].pos) / 2;
    if (!g.visible(mid) || p[i].shape == AutoShape::Hold || g.x(p[i].pos) - g.x(p[i - 1].pos) < 14) continue;
    ImVec2 m(g.x(mid), g.y(automationValue(ac, static_cast<double>(mid))));
    dl->AddCircle(m, static_cast<int>(i) == hotHandle ? 4.0f : 3.0f, static_cast<int>(i) == hotHandle ? hot : line, 12, 1.2f);
  }
}

// Hit test: point index or tension handle (segment ending at point i).
void hitAutomation(const AutomationClip& ac, const AutoGeom& g, ImVec2 m, int& point, int& handle) {
  point = handle = -1;
  float best = 7.0f * 7.0f;
  const auto& p = ac.points;
  for (size_t i = 0; i < p.size(); ++i) {
    if (!g.visible(p[i].pos)) continue;
    float dx = m.x - g.x(p[i].pos), dy = m.y - g.y(p[i].value);
    if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, point = static_cast<int>(i);
  }
  if (point >= 0) return;
  for (size_t i = 1; i < p.size(); ++i) {
    Tick mid = (p[i - 1].pos + p[i].pos) / 2;
    if (!g.visible(mid) || p[i].shape == AutoShape::Hold || g.x(p[i].pos) - g.x(p[i - 1].pos) < 14) continue;
    float dx = m.x - g.x(mid), dy = m.y - g.y(automationValue(ac, static_cast<double>(mid)));
    if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, handle = static_cast<int>(i);
  }
}

std::string clipName(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) {
    const Channel* ch = s.channel(c.source);
    return ch ? ch->name : "Audio";
  }
  if (c.kind == ClipKind::Automation) {
    const AutomationClip* a = s.automation(c.source);
    return a ? a->name : "Automation";
  }
  const Pattern* p = s.pattern(c.pattern);
  return p ? p->name : "?";
}

bool clipValid(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Audio) return s.channel(c.source) != nullptr;
  if (c.kind == ClipKind::Automation) return s.automation(c.source) != nullptr;
  return s.pattern(c.pattern) != nullptr;
}

// Per-clip data the canvas needs every frame (validity, length, colour, name),
// rebuilt only when the document changes: Song::clipLength / lookups by id are
// linear scans (pattern length walks every note), far too slow for thousands
// of clips per frame (J-25).
struct ClipCache {
  uint64_t rev = ~0ull;
  const Clip* data = nullptr;
  size_t count = 0;
  std::vector<Tick> len;
  std::vector<uint8_t> valid;
  std::vector<Rgb> color;
  std::vector<const std::string*> name;
  Tick songLen = 0;
  // J-38: stacking and fades. Clips draw by track, then start (a later clip
  // lies on top of the one it overlaps); `cover` is the clip lying directly on
  // top of each clip (-1 = none), `fades` the resolved audio fades.
  std::vector<uint32_t> order;
  std::vector<int32_t> cover;
  std::vector<ClipFades> fades;
};
const ClipCache& clipCache(const App& a) {
  static ClipCache cc;
  const Song& s = a.doc.song();
  if (cc.rev == a.doc.revision() && cc.data == s.playlist.data() && cc.count == s.playlist.size()) return cc;
  cc.rev = a.doc.revision();
  cc.data = s.playlist.data();
  cc.count = s.playlist.size();
  std::unordered_map<ObjectId, size_t> pat, chan, aut;
  pat.reserve(s.patterns.size());
  for (size_t i = 0; i < s.patterns.size(); ++i) pat.emplace(s.patterns[i].id, i);
  for (size_t i = 0; i < s.channels.size(); ++i) chan.emplace(s.channels[i].id, i);
  for (size_t i = 0; i < s.automations.size(); ++i) aut.emplace(s.automations[i].id, i);
  std::vector<Tick> patLen(s.patterns.size(), -1);
  static const std::string kAudio = "Audio", kAuto = "Automation", kUnknown = "?";
  cc.len.assign(cc.count, 0);
  cc.valid.assign(cc.count, 0);
  cc.color.assign(cc.count, 0x7A8696);
  cc.name.assign(cc.count, &kUnknown);
  cc.songLen = 0;
  for (size_t i = 0; i < cc.count; ++i) {
    const Clip& c = s.playlist[i];
    if (c.kind == ClipKind::Audio) {
      auto it = chan.find(c.source);
      cc.valid[i] = it != chan.end();
      if (cc.valid[i]) cc.color[i] = s.channels[it->second].color, cc.name[i] = &s.channels[it->second].name;
      else cc.name[i] = &kAudio;
      cc.len[i] = s.clipLength(c);
    } else if (c.kind == ClipKind::Automation) {
      auto it = aut.find(c.source);
      cc.valid[i] = it != aut.end();
      if (cc.valid[i]) cc.color[i] = s.automations[it->second].color, cc.name[i] = &s.automations[it->second].name;
      else cc.name[i] = &kAuto;
      cc.len[i] = s.clipLength(c);
    } else {
      auto it = pat.find(c.pattern);
      cc.valid[i] = it != pat.end();
      if (cc.valid[i]) {
        const Pattern& pp = s.patterns[it->second];
        cc.color[i] = pp.color, cc.name[i] = &pp.name;
        Tick& pl = patLen[it->second];
        if (pl < 0) pl = s.patternContentLength(pp);
        cc.len[i] = c.length > 0 ? c.length : pl;
      } else {
        cc.len[i] = c.length > 0 ? c.length : 0;
      }
    }
    cc.songLen = std::max(cc.songLen, c.start + cc.len[i]);
  }
  // J-38: stacking order and the clip covering each clip.
  cc.order.resize(cc.count);
  for (size_t i = 0; i < cc.count; ++i) cc.order[i] = static_cast<uint32_t>(i);
  std::sort(cc.order.begin(), cc.order.end(), [&](uint32_t x, uint32_t y) {
    const Clip &p = s.playlist[x], &q = s.playlist[y];
    return p.track != q.track ? p.track < q.track : p.start != q.start ? p.start < q.start : x < y;
  });
  cc.cover.assign(cc.count, -1);
  for (size_t k = 0; k + 1 < cc.count; ++k) {
    const uint32_t i = cc.order[k], j = cc.order[k + 1];
    const Clip &p = s.playlist[i], &q = s.playlist[j];
    if (cc.valid[i] && cc.valid[j] && p.track == q.track && q.start < p.start + cc.len[i]) cc.cover[i] = static_cast<int32_t>(j);
  }
  TempoMap tm;
  tm.build(s);
  cc.fades = resolveAllFades(s, tm);
  return cc;
}

// Right-click menu of a clip: arrangement settings, then the pattern clip menu.
void arrangementClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) return;
  if (c->kind != ClipKind::Pattern) ImGui::TextDisabled("%s", clipName(s, *c).c_str());
  if (ImGui::MenuItem(i18n::tr("Mute clip"), nullptr, c->muted)) a.doc.editClip(id, "mute clip", [](Clip& x) { x.muted = !x.muted; });
  if (c->kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c->source); ch && ImGui::BeginMenu(i18n::tr("Rename..."))) {  // J-28: the clip's title (its channel's name)
      static char buf[128];
      if (ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", ch->name.c_str());
      ImGui::SetNextItemWidth(180);
      if (ImGui::InputText("##rn", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        a.doc.renameChannel(ch->id, buf);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndMenu();
    }
    if (const Channel* ch = s.channel(c->source); ch && ImGui::MenuItem(i18n::tr("Edit in audio editor"), nullptr, false, !ch->plugin.data.empty()))
      packs::openAudioEditor(a, ch->plugin.data, ch->id);  // J-7
    if (ImGui::MenuItem(i18n::tr("Export clip as WAV..."))) exportui::exportClipAudio(a, id);  // J-20
    static uint64_t key = 0x41430000;
    float gainDb = c->gain > 0 ? 20.0f * std::log10(c->gain) : -60.0f;
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat(i18n::tr("Gain"), &gainDb, 0.1f, -60.0f, 12.0f, "%.1f dB"))
      a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f); }, key);
    float cents = c->pitch;
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat(i18n::tr("Pitch"), &cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct"))
      a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = cents; }, key + 1);
    float fi = static_cast<float>(c->fadeIn * 1000.0), fo = static_cast<float>(c->fadeOut * 1000.0);
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat(i18n::tr("Fade in"), &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0, x.fadeInManual = true; }, key + 2);
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat(i18n::tr("Fade out"), &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
      a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0, x.fadeOutManual = true; }, key + 3);
    if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, c->reverse)) a.doc.editClip(id, "reverse clip", [](Clip& x) { x.reverse = !x.reverse; });
    if (ImGui::MenuItem(i18n::tr("Follow tempo"), nullptr, c->syncBpm > 0)) {
      TempoMap tm;
      tm.build(s);
      double bpm = tm.bpmAt(static_cast<double>(c->start));
      a.doc.editClip(id, "follow tempo", [bpm](Clip& x) { x.syncBpm = x.syncBpm > 0 ? 0.0 : bpm; });
    }
    if (ImGui::MenuItem(i18n::tr("Channel settings"))) a.settingsChannel = c->source;
    {  // J-28 (operations: J-27)
      std::set<ObjectId> sel = a.playlist.selected;
      if (!sel.count(id)) sel = {id};
      if (ImGui::MenuItem(i18n::tr("Make unique as sample"))) {
        if (ObjectId nch = ops::makeAudioClipUnique(a.doc, id)) a.settingsChannel = nch;
      }
      if (ImGui::MenuItem(i18n::tr("Pitch a semitone up"))) ops::pitchAudioClips(a.doc, sel, 100.0f);
      if (ImGui::MenuItem(i18n::tr("Pitch a semitone down"))) ops::pitchAudioClips(a.doc, sel, -100.0f);
    }
    a.doc.endGesture();
    ImGui::Separator();
    audioClipMenu(a, id);  // J-8: properties, takes, stretch, fit, normalize, slice, consolidate, bounce
  } else if (c->kind == ClipKind::Automation) {
    const AutomationClip* ac = s.automation(c->source);
    if (ac) {
      std::string target = ac->owner == kNoId ? std::string("no target") : paramDisplayName(s, {ac->owner, ac->index});
      ImGui::TextDisabled(i18n::tr("Drives: %s"), target.c_str());
      if (ImGui::MenuItem(i18n::tr("Edit automation..."))) openAutomationEditor(a, ac->id, id);
      if (ImGui::BeginMenu(i18n::tr("Curve of every segment"))) {
        for (int k = 0; k < static_cast<int>(AutoShape::Count); ++k)
          if (ImGui::MenuItem(autoShapeName(static_cast<AutoShape>(k)))) {
            ObjectId aid = ac->id;
            a.doc.editSong("automation curve", [aid, k](Song& x) {
              if (AutomationClip* q = x.automation(aid))
                for (auto& p : q->points) p.shape = static_cast<AutoShape>(k);
            });
          }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(i18n::tr("Reset tensions"))) {
        ObjectId aid = ac->id;
        a.doc.editSong("reset tensions", [aid](Song& x) {
          if (AutomationClip* q = x.automation(aid))
            for (auto& p : q->points) p.tension = 0;
        });
      }
      if (ImGui::MenuItem(i18n::tr("Merge selected automation clips"), nullptr, false, a.playlist.selected.size() >= 2)) {  // J-28 (op: J-27)
        ObjectId nc = ops::mergeAutomationClips(a.doc, a.playlist.selected);
        if (nc) a.playlist.selected = {nc};
        else a.notify("Merging needs two automation clips of the same parameter");
        return;  // the clip under the menu may be gone
      }
      if (ImGui::MenuItem(i18n::tr("Select all clips of this automation"))) {
        a.playlist.selected.clear();
        for (const auto& k : s.playlist)
          if (k.kind == ClipKind::Automation && k.source == ac->id) a.playlist.selected.insert(k.id);
      }
    }
  } else {
    ImGui::Separator();
    clipContextMenu(a, id);
  }
}

// ---- current clip source (J-28) ----
struct ClipSrcPayload {
  int kind;
  ObjectId id;
};

bool isAudioSource(const Channel& ch) { return ch.plugin.type == "gl.sampler" && !ch.plugin.data.empty(); }

pltools::Source currentSource(App& a) {
  const PlaylistState& st = a.playlist;
  if (st.srcKind != ClipKind::Pattern) {
    pltools::Source s{st.srcKind, st.srcId};
    if (pltools::sourceValid(a.doc.song(), s)) return s;
  }
  return {ClipKind::Pattern, a.curPattern};
}

void setSource(App& a, const pltools::Source& src) {
  a.playlist.srcKind = src.kind;
  a.playlist.srcId = src.id;
  if (src.kind == ClipKind::Pattern) a.selectPattern(src.id);
  if (src.kind == ClipKind::Audio) a.selectChannel(src.id);
}

void openSource(App& a, const pltools::Source& src) {
  if (src.kind == ClipKind::Pattern) {
    a.selectPattern(src.id);
    a.showRack = true;
    a.focusWindow = "Channel rack";
  } else if (src.kind == ClipKind::Audio) {
    a.settingsChannel = src.id;
  } else {
    openAutomationEditor(a, src.id);
  }
}

std::string sourceName(const Song& s, const pltools::Source& src) {
  if (src.kind == ClipKind::Audio) return s.channel(src.id) ? s.channel(src.id)->name : "Audio";
  if (src.kind == ClipKind::Automation) return s.automation(src.id) ? s.automation(src.id)->name : "Automation";
  return s.pattern(src.id) ? s.pattern(src.id)->name : "(none)";
}

// Pattern: step bars; audio: a wave; automation: a ramp with points.
void drawKindGlyph(ImDrawList* dl, ClipKind kind, ImVec2 c, float size, ImU32 col) {
  const float h = size * 0.5f;
  if (kind == ClipKind::Audio) {
    ui::drawIcon(dl, ui::Icon::Wave, c, size, col);
  } else if (kind == ClipKind::Automation) {
    const ImVec2 p0(c.x - h, c.y + h * 0.6f), p1(c.x - h * 0.2f, c.y - h * 0.5f), p2(c.x + h * 0.35f, c.y + h * 0.1f), p3(c.x + h, c.y - h * 0.7f);
    dl->AddLine(p0, p1, col, 1.4f);
    dl->AddLine(p1, p2, col, 1.4f);
    dl->AddLine(p2, p3, col, 1.4f);
    for (ImVec2 p : {p0, p1, p2, p3}) dl->AddCircleFilled(p, 1.8f, col);
  } else {
    for (int k = 0; k < 3; ++k) {
      const float x = c.x - h + 1 + static_cast<float>(k) * (size - 2) / 3.0f;
      dl->AddRectFilled(ImVec2(x, c.y - h * 0.7f), ImVec2(x + (size - 2) / 3.0f - 2, c.y + h * 0.7f), col, 1.0f);
    }
    dl->AddRectFilled(ImVec2(c.x - h, c.y + h * 0.7f), ImVec2(c.x + h, c.y + h * 0.7f + 1.5f), col);
  }
}

// Breadcrumb menu: every clip source by kind.
void sourceMenu(App& a) {
  const Song& s = a.doc.song();
  const pltools::Source cur = currentSource(a);
  ImGui::SeparatorText(i18n::tr("Patterns"));
  for (const auto& p : s.patterns) {
    ImGui::PushID(static_cast<int>(p.id));
    if (ImGui::MenuItem(p.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Pattern, p.id})) setSource(a, {ClipKind::Pattern, p.id});
    ImGui::PopID();
  }
  bool any = false;
  for (const auto& ch : s.channels) {
    if (!isAudioSource(ch)) continue;
    if (!any) ImGui::SeparatorText(i18n::tr("Audio"));
    any = true;
    ImGui::PushID(static_cast<int>(ch.id));
    if (ImGui::MenuItem(ch.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Audio, ch.id})) setSource(a, {ClipKind::Audio, ch.id});
    ImGui::PopID();
  }
  if (!s.automations.empty()) ImGui::SeparatorText(i18n::tr("Automation"));
  for (const auto& ac : s.automations) {
    ImGui::PushID(static_cast<int>(ac.id));
    if (ImGui::MenuItem(ac.name.c_str(), nullptr, cur == pltools::Source{ClipKind::Automation, ac.id})) setSource(a, {ClipKind::Automation, ac.id});
    ImGui::PopID();
  }
}

}  // namespace

void drawPlaylist(App& a) {
  if (!a.showPlaylist) return;
  if (!ui::BeginDockWindow("Playlist", &a.showPlaylist, ImVec2(1000, 480))) {
    ui::EndDockWindow();
    return;
  }
  const auto& P = pal();
  const Song& s = a.doc.song();
  PlaylistState& st = a.playlist;
  ImGuiIO& io = ImGui::GetIO();
  const float winW = ImGui::GetContentRegionAvail().x;
  // Tempo map for audio clip drawing (rebuilt when the song changes).
  static TempoMap tempoMap;
  static uint64_t tempoRev = ~0ull;
  if (tempoRev != a.doc.revision()) {
    tempoMap.build(s);
    tempoRev = a.doc.revision();
  }

  // ---------------- toolbar (FL order: options, snap, tools, breadcrumb) ----------------
  ui::BeginToolbar("##pltb");
  {
    const float fh = ImGui::GetFrameHeight();
    menuButton("plmenu", "Playlist options: edit, select, snap, view, picker panel, tracks", [&a]() { playlistOptionsMenu(a); });
    ImGui::SameLine(0, 4);
    // Magnet: the snap menu (right-click toggles snapping off / back on).
    if (ui::IconButton("snapicon", ui::Icon::Magnet, st.snap != Snap::None, fh,
                       "Snap: click for the grid menu, right-click toggles snapping (Alt while dragging bypasses it)",
                       theme::rgb(theme::role::kSnap)))
      ImGui::OpenPopup("##snapmenu");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      if (st.snap != Snap::None) st.lastSnap = st.snap, st.snap = Snap::None;
      else st.snap = st.lastSnap == Snap::None ? Snap::Line : st.lastSnap;
    }
    if (ImGui::BeginPopup("##snapmenu")) {
      ImGui::TextDisabled("%s", i18n::tr("Snap to"));
      for (int i = 0; i <= static_cast<int>(Snap::Bar); ++i) {
        std::string l = snapName(static_cast<Snap>(i));
        if ((static_cast<Snap>(i) == Snap::Line || static_cast<Snap>(i) == Snap::Cell) && st.effGrid > 0 && st.snap == static_cast<Snap>(i))
          l += "  (" + timeline::gridName(s, st.effGrid) + ")";
        if (ImGui::MenuItem(l.c_str(), nullptr, st.snap == static_cast<Snap>(i))) {
          st.snap = static_cast<Snap>(i);
          if (st.snap != Snap::None) st.lastSnap = st.snap;
        }
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 4);
    struct T { ui::Icon icon; Tool tool; const char* hint; uint32_t role; };
    const T tools[] = {
        {ui::Icon::Draw, Tool::Draw, "Draw (P): click places the current clip source, drag moves, drag an edge to resize / trim, Shift+drag copies, right-click deletes", theme::role::kDraw},
        {ui::Icon::Paint, Tool::Paint, "Paint (B): drag lays clips of the current source one after another", theme::role::kPaint},
        {ui::Icon::Erase, Tool::Erase, "Delete (D): click or drag over clips to delete them", theme::role::kDelete},
        {ui::Icon::Mute, Tool::Mute, "Mute (T): click or drag over clips to mute / unmute them", theme::role::kMute},
        {ui::Icon::Slip, Tool::Slip, "Slip (S): drag inside a clip to move its content without moving the clip", theme::role::kSlip},
        {ui::Icon::Slice, Tool::Slice, "Slice (C): click a clip to cut it, drag up / down to cut every clip the line crosses, drag sideways to cut out a region (Shift: all tracks)", theme::role::kSlice},
        {ui::Icon::Select, Tool::Select, "Select (E): drag a box (Ctrl+drag selects with any tool, Shift adds)", theme::role::kSelect},
        {ui::Icon::Zoom, Tool::Zoom, "Zoom (Z): drag a range to fill the view, click zooms in, right-click zooms out", theme::role::kZoom},
        {ui::Icon::Play, Tool::Play, "Playback (Y): hold to play from the pointer, drag to scrub", theme::role::kPlay}};
    for (const auto& t : tools) {
      if (ui::IconButton(t.hint, t.icon, st.tool == t.tool, fh, t.hint, theme::rgb(t.role))) st.tool = t.tool;
      ImGui::SameLine(0, 2);
    }
    // Breadcrumb: Playlist > arrangement > current clip source (each opens its menu).
    ImGui::SameLine(0, theme::kS3);
    const pltools::Source src = currentSource(a);
    const std::string arrName = arrangementName(s, s.arrangements.empty() ? 0 : s.currentArrangement);
    const std::string srcName = pltools::sourceValid(s, src) ? sourceName(s, src) : std::string("(none)");
    auto crumb = [&](const char* id, const std::string& text, bool clickable, ImU32 col, const char* hint) {
      const float room = ImGui::GetContentRegionAvail().x;
      const float tw = std::min(a.fontRegular->CalcTextSizeA(theme::kFontBody, 1e9f, 0, text.c_str()).x, 220.0f);
      if (room < tw + 24) return false;
      ImVec2 p = ImGui::GetCursorScreenPos();
      bool pressed = clickable ? ImGui::InvisibleButton(id, ImVec2(tw + 4, fh)) : (ImGui::Dummy(ImVec2(tw + 4, fh)), false);
      const bool hov = clickable && ImGui::IsItemHovered();
      ImDrawList* tdl = ImGui::GetWindowDrawList();
      if (hov) tdl->AddRectFilled(p, ImVec2(p.x + tw + 4, p.y + fh), theme::withAlpha(P.paper, 0.06f), theme::kRadiusSm);
      theme::textEllipsis(tdl, a.fontRegular, theme::kFontBody, ImVec2(p.x + 2, std::round(p.y + (fh - theme::kFontBody) * 0.5f) - 1), tw + 1,
                          hov ? P.text : col, text.c_str());
      if (hint) ui::Hint(hint);
      ImGui::SameLine(0, 0);
      // Separator chevron.
      ImVec2 q = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(14, fh));
      tdl->AddTriangleFilled(ImVec2(q.x + 5, q.y + fh * 0.5f - 4), ImVec2(q.x + 5, q.y + fh * 0.5f + 4), ImVec2(q.x + 9, q.y + fh * 0.5f), P.textFaint);
      ImGui::SameLine(0, 0);
      return pressed;
    };
    crumb("##crPl", "Playlist", false, P.textDim, nullptr);
    if (crumb("##crArr", arrName, true, P.text, "Arrangement: switch, add, duplicate, rename or delete arrangements")) ImGui::OpenPopup("##arrmenu");
    if (ImGui::BeginPopup("##arrmenu")) {
      arrangementMenu(a);
      ImGui::EndPopup();
    }
    if (crumb("##crSrc", srcName, true, pltools::sourceValid(s, src) ? P.text : P.textFaint,
              "Current clip source: what clicks with draw / paint place (pick one here or in the picker panel)"))
      ImGui::OpenPopup("##srcmenu");
    if (ImGui::BeginPopup("##srcmenu")) {
      sourceMenu(a);
      ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, 0));
  }
  ui::EndToolbar();

  // ---------------- clip source picker (patterns / audio / automation) ----------------
  const bool showPicker = st.pickerOpen && winW >= 560;
  if (showPicker) {
    const float pickerW = std::clamp(winW * 0.14f, 124.0f, 172.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS1, theme::kS1));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kS2, 2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    ImGui::BeginChild("##picker", ImVec2(pickerW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    {  // three filter tabs (J-38): one segmented control in the ruler band, so
       // the list starts level with the first track and the corner's divider runs on
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const float bandH = 14.0f + 26.0f - theme::kS1;  // the ruler band (rulerH below) minus the child padding
      const ImVec2 q0 = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x, th = 22.0f, ty = std::round(q0.y + (bandH - th) * 0.5f) - 1;
      const float tw = std::floor((w - 4) / 3.0f);
      pdl->AddRectFilled(ImVec2(q0.x, ty), ImVec2(q0.x + w, ty + th), P.dark ? P.well : P.surface950, 5.0f);
      pdl->AddRect(ImVec2(q0.x, ty), ImVec2(q0.x + w, ty + th), theme::withAlpha(P.ink, P.dark ? 0.55f : 0.18f), 5.0f);
      static const char* const tabHints[] = {"Pattern clip sources", "Audio clip sources (sample channels)", "Automation clip sources"};
      static const ClipKind tabKinds[] = {ClipKind::Pattern, ClipKind::Audio, ClipKind::Automation};
      for (int k = 0; k < 3; ++k) {
        ImGui::PushID(k);
        const ImVec2 c0(q0.x + 2 + static_cast<float>(k) * tw, ty + 2), c1(c0.x + tw, ty + th - 2);
        ImGui::SetCursorScreenPos(c0);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, th - 4))) st.pickerTab = k;
        const bool hov = ImGui::IsItemHovered(), on = st.pickerTab == k;
        ui::Hint(tabHints[k]);
        if (hov) ImGui::SetTooltip("%s", tabHints[k]);
        if (on) pdl->AddRectFilled(c0, c1, P.dark ? P.surface600 : P.surface700, 3.5f);
        else if (hov) pdl->AddRectFilled(c0, c1, theme::withAlpha(P.paper, P.dark ? 0.06f : 0.5f), 3.5f);
        drawKindGlyph(pdl, tabKinds[k], ImVec2(std::round((c0.x + c1.x) * 0.5f), std::round((c0.y + c1.y) * 0.5f)), 12, on ? P.hot : hov ? P.text : P.textDim);
        ImGui::PopID();
      }
      // The corner's bottom rule continues under the tabs.
      pdl->AddLine(ImVec2(q0.x - theme::kS1, q0.y + bandH - 1), ImVec2(q0.x + w + theme::kS1, q0.y + bandH - 1), P.dark ? P.ink : P.lineStrong);
      ImGui::SetCursorScreenPos(q0);
      ImGui::Dummy(ImVec2(w, bandH - ImGui::GetStyle().ItemSpacing.y));
    }
    const float rowH = 22.0f;
    // Rows: a mini clip (colour strip + name) like the clips they place.
    auto pickerRow = [&](const pltools::Source& ps, const std::string& name, Rgb color, const char* hint, const std::function<void()>& menu) {
      ImVec2 q = ImGui::GetCursorScreenPos();
      const bool sel = currentSource(a) == ps;
      float w = ImGui::GetContentRegionAvail().x;
      if (ImGui::InvisibleButton("##pp", ImVec2(w, rowH))) setSource(a, ps);
      const bool hov = ImGui::IsItemHovered();
      if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openSource(a, ps);
      if (ImGui::BeginDragDropSource()) {
        const ClipSrcPayload pl{static_cast<int>(ps.kind), ps.id};
        ImGui::SetDragDropPayload("GL_CLIPSRC", &pl, sizeof pl);
        ui::DragPreview(ps.kind == ClipKind::Audio ? ui::Icon::Wave : ui::Icon::Pattern, name.c_str(), theme::content(color));
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginPopupContextItem("##pmenu")) {
        menu();
        ImGui::EndPopup();
      }
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const ImU32 pc = theme::rgb(color);
      pdl->AddRectFilled(q, ImVec2(q.x + w, q.y + rowH), sel ? theme::mix(P.surface700, pc, 0.18f) : hov ? P.surface700 : theme::withAlpha(P.surface800, 0.55f),
                         theme::kRadiusSm);
      pdl->AddRectFilled(q, ImVec2(q.x + 4, q.y + rowH), pc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
      if (sel) pdl->AddRect(q, ImVec2(q.x + w, q.y + rowH), theme::withAlpha(P.hot, 0.55f), theme::kRadiusSm);
      theme::textEllipsis(pdl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + 12, std::round(q.y + (rowH - 13) * 0.5f) - 1), w - 18,
                          sel && P.dark ? P.paper : P.text, name.c_str());
      ui::Hint(hint);
    };
    int shown = 0;
    if (st.pickerTab == 0) {
      for (const auto& p : s.patterns) {
        ImGui::PushID(static_cast<int>(p.id));
        const ObjectId pid = p.id;
        pickerRow({ClipKind::Pattern, p.id}, p.name, p.color, "Click: current source \xC2\xB7 drag into the playlist \xC2\xB7 double-click: edit \xC2\xB7 right-click: menu",
                  [&a, pid]() { patternContextMenu(a, pid); });
        ImGui::PopID();
        ++shown;
      }
    } else if (st.pickerTab == 1) {
      for (const auto& ch : s.channels) {
        if (!isAudioSource(ch)) continue;
        ImGui::PushID(static_cast<int>(ch.id));
        const ObjectId cid = ch.id;
        pickerRow({ClipKind::Audio, ch.id}, ch.name, ch.color, "Click: current source \xC2\xB7 drag into the playlist \xC2\xB7 double-click: channel settings",
                  [&a, cid]() {
                    if (ImGui::MenuItem(i18n::tr("Channel settings"))) a.settingsChannel = cid;
                    if (ImGui::MenuItem(i18n::tr("Select its clips"))) {
                      a.playlist.selected.clear();
                      for (const auto& k : a.doc.song().playlist)
                        if (k.kind == ClipKind::Audio && k.source == cid) a.playlist.selected.insert(k.id);
                    }
                  });
        ImGui::PopID();
        ++shown;
      }
    } else {
      for (const auto& ac : s.automations) {
        ImGui::PushID(static_cast<int>(ac.id));
        const ObjectId aid = ac.id;
        pickerRow({ClipKind::Automation, ac.id}, ac.name, ac.color, "Click: current source \xC2\xB7 drag into the playlist \xC2\xB7 double-click: edit",
                  [&a, aid]() {
                    if (ImGui::MenuItem(i18n::tr("Edit automation..."))) openAutomationEditor(a, aid, kNoId);
                    if (ImGui::MenuItem(i18n::tr("Select its clips"))) {
                      a.playlist.selected.clear();
                      for (const auto& k : a.doc.song().playlist)
                        if (k.kind == ClipKind::Automation && k.source == aid) a.playlist.selected.insert(k.id);
                    }
                  });
        ImGui::PopID();
        ++shown;
      }
    }
    if (shown == 0) {
      ImGui::Dummy(ImVec2(0, 4));
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.textFaint), "%s",
                         st.pickerTab == 1 ? "No audio yet: drop samples from the browser onto the playlist"
                                           : st.pickerTab == 2 ? "No automation yet: right-click a control \xE2\x86\x92 Create automation clip"
                                                               : "No patterns");
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    // Divider between picker and arrangement.
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(r1.x, r0.y), ImVec2(r1.x, r1.y), P.line);
    ImGui::SameLine(0, 1);
  }

  // ---------------- arrangement canvas ----------------
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 120.0f);
  avail.y = std::max(avail.y, 100.0f);
  ImGui::InvisibleButton("##arr", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Left to right: a slim vertical scroller, the track headers, the grid. Above
  // the headers: two rows of view buttons (FL's header corner); above the grid:
  // a slim horizontal scroller, then the ruler.
  const float vbarW = 10.0f, hbarH = 14.0f;
  const float headW = vbarW + (st.wideHeaders ? std::clamp(avail.x * 0.2f, 150.0f, 230.0f) : std::clamp(avail.x * 0.13f, 96.0f, 150.0f));
  const float rulerH = hbarH + 26.0f;
  const float rY = origin.y + hbarH;  // ruler top
  const ImVec2 gMin(origin.x + headW, origin.y + rulerH), gMax(origin.x + avail.x, origin.y + avail.y);
  const float trackH = st.trackHeight;
  const int nTracks = static_cast<int>(s.tracks.size());
  // Per-track heights (relative to the zoomable track height): prefix offsets.
  // Tracks inside a folded group take no room.
  std::vector<char> folded(static_cast<size_t>(nTracks), 0);
  std::vector<int> groupDepth(static_cast<size_t>(nTracks), 0);
  for (int i = 0; i < nTracks; ++i) {
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(i)];
    for (int k = 1; k <= t.groupSize && i + k < nTracks; ++k) {
      if (t.folded || folded[static_cast<size_t>(i)]) folded[static_cast<size_t>(i + k)] = 1;
      groupDepth[static_cast<size_t>(i + k)] = groupDepth[static_cast<size_t>(i)] + 1;
    }
  }
  std::vector<float> trackY(static_cast<size_t>(nTracks) + 1, 0.0f);
  for (int i = 0; i < nTracks; ++i)
    trackY[static_cast<size_t>(i) + 1] = trackY[static_cast<size_t>(i)] +
        (folded[static_cast<size_t>(i)] ? 0.0f : std::round(trackH * std::clamp(s.tracks[static_cast<size_t>(i)].height, 0.25f, 4.0f)));
  const float totalH = trackY.back();
  st.scrollY = std::clamp(st.scrollY, 0.0f, std::max(0.0f, totalH - (gMax.y - gMin.y)));
  st.scrollX = std::max(0.0f, st.scrollX);
  auto xOf = [&](Tick t) { return gMin.x + static_cast<float>(t) * st.pxPerTick - st.scrollX; };
  auto yOf = [&](int tr) { return gMin.y + trackY[static_cast<size_t>(std::clamp(tr, 0, nTracks))] - st.scrollY; };
  auto hOf = [&](int tr) {
    tr = std::clamp(tr, 0, std::max(0, nTracks - 1));
    return nTracks ? trackY[static_cast<size_t>(tr) + 1] - trackY[static_cast<size_t>(tr)] : trackH;
  };
  auto tickAt = [&](float x) { return timeline::tickAtPx(x - gMin.x + st.scrollX, st.pxPerTick); };
  auto trackAt = [&](float y) {
    float yy = y - gMin.y + st.scrollY;
    auto it = std::upper_bound(trackY.begin(), trackY.end(), yy);
    return std::clamp(static_cast<int>(it - trackY.begin()) - 1, 0, std::max(0, nTracks - 1));
  };
  // ---- zoom: Ctrl+wheel at the pointer, PgUp/PgDn, Home = fit, toolbar buttons ----
  const float gridW = gMax.x - gMin.x;
  const float playX = static_cast<float>(a.engine.position()) * st.pxPerTick - st.scrollX;
  const float keyAnchor = hovered && io.MousePos.x >= gMin.x ? io.MousePos.x - gMin.x
                          : (playX >= 0 && playX <= gridW ? playX : gridW * 0.5f);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (keys::pressed("playlist.zoomIn")) st.zoomRequest *= 1.5f;
    if (keys::pressed("playlist.zoomOut")) st.zoomRequest /= 1.5f;
    if (keys::pressed("playlist.zoomFit")) st.fitRequest = true;
  }
  if (st.fitRequest) {
    Tick len = std::max<Tick>(clipCache(a).songLen, 4 * s.barTicks());
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, len + s.barTicks(), gridW));
    st.scrollX = 0;
    st.fitRequest = false;
  }
  if (st.quickZoom > 0) {  // J-28: quick zooms (FL Zoom menu): 4 / 16 / 64 bars across the view
    const Tick bars = st.quickZoom == 1 ? 4 : st.quickZoom == 2 ? 16 : 64;
    const float playTick = static_cast<float>(a.engine.position());
    st.pxPerTick = static_cast<float>(timeline::fitZoom(s, bars * std::max<Tick>(1, s.barTicks()), gridW));
    st.scrollX = std::max(0.0f, playTick * st.pxPerTick - gridW * 0.25f);
    st.quickZoom = 0;
  }
  if (st.zoomSelRequest) {  // zoom on the time selection, else on the selected clips
    std::pair<Tick, Tick> r{st.selStart, st.selEnd};
    if (r.second <= r.first) r = ops::clipsTimeRange(s, st.selected);
    if (r.second > r.first) {
      st.pxPerTick = static_cast<float>(timeline::fitZoom(s, r.second - r.first, gridW));
      st.scrollX = static_cast<float>(r.first) * st.pxPerTick;
    }
    st.zoomSelRequest = false;
  }
  if (st.zoomRequest != 1.0f) {
    timeline::zoomAt(s, st.pxPerTick, st.scrollX, st.zoomRequest, keyAnchor);
    st.zoomRequest = 1.0f;
  }
  st.pxPerTick = static_cast<float>(timeline::clampZoom(s, st.pxPerTick));
  // Auto scrolling: page along with the song position while the song plays
  // (not while the user drags or scrolls the view).
  if (a.engine.playing() && a.engine.mode() == PlayMode::Song && a.settings.getBool("ui.autoScroll") && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
    st.scrollX = ops::followScroll(st.scrollX, static_cast<float>(a.engine.position()) * st.pxPerTick, gridW);

  {  // the engine loops playback inside the time selection
    static Tick sentA = -1, sentB = -1;
    Tick la = st.selEnd > st.selStart ? st.selStart : 0, lb = st.selEnd > st.selStart ? st.selEnd : 0;
    if (la != sentA || lb != sentB) {
      a.engine.setLoopRange(la, lb);
      sentA = la;
      sentB = lb;
    }
  }
  // ---- snapping: the visible grid (Line/Cell) or a fixed size; Alt inverts ----
  const Tick visGrid = timeline::visibleGrid(s, st.pxPerTick);
  const Tick snapG = timeline::snapGrid(s, st.snap, st.pxPerTick, io.KeyAlt);
  st.effGrid = timeline::snapGrid(s, st.snap, st.pxPerTick, false);
  auto snapPos = [&](Tick t) { return timeline::snapTick(t, snapG, st.snap); };

  if (hovered && io.MouseWheel != 0) {
    if (io.KeyCtrl) {
      timeline::zoomAt(s, st.pxPerTick, st.scrollX, std::pow(1.2, static_cast<double>(io.MouseWheel)), io.MousePos.x - gMin.x);
    } else if (io.KeyAlt) {
      st.trackHeight = std::clamp(st.trackHeight + io.MouseWheel * 4, 24.0f, 120.0f);
    } else if (io.KeyShift) {
      st.scrollX = std::max(0.0f, st.scrollX - io.MouseWheel * 80);
    } else {
      st.scrollY -= io.MouseWheel * trackH;
    }
  }
  if (hovered && io.MouseWheelH != 0) st.scrollX = std::max(0.0f, st.scrollX - io.MouseWheelH * 80);
  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
    st.scrollX = std::max(0.0f, st.scrollX - io.MouseDelta.x);
    st.scrollY -= io.MouseDelta.y;
  }

  // Background & grid: alternate bar shading, then track separators and bar/beat lines.
  dl->PushClipRect(gMin, gMax, true);
  dl->AddRectFilled(gMin, gMax, P.well);
  const Tick barT = std::max<Tick>(1, s.barTicks()), beatT = std::max<Tick>(1, s.beatTicks());
  // Alternate bar shading (every other bar a step lighter), like the reference grid.
  for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), barT); xOf(t) < gMax.x; t += barT)
    if ((t / barT) % 2 == 1) dl->AddRectFilled(ImVec2(xOf(t), gMin.y), ImVec2(xOf(t + barT), gMax.y), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    float y = yOf(tr);
    dl->AddLine(ImVec2(gMin.x, y + hOf(tr)), ImVec2(gMax.x, y + hOf(tr)), P.gridBar);
  }
  {
    // Lines at the visible grid: bars strongest, then beats, then finer subdivisions.
    const ImU32 fine = theme::withAlpha(P.gridBeat, 0.45f);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), visGrid); xOf(t) < gMax.x; t += visGrid) {
      ImU32 c = t % barT == 0 ? P.gridBar : t % beatT == 0 ? P.gridBeat : fine;
      dl->AddLine(ImVec2(std::round(xOf(t)), gMin.y), ImVec2(std::round(xOf(t)), gMax.y), c);
    }
  }
  // Markers: faint full-height lines (labels in the ruler).
  for (const auto& mk : s.markers) {
    float x = std::round(xOf(mk.pos));
    if (x >= gMin.x && x <= gMax.x) dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), theme::withAlpha(P.textDim, 0.35f));
  }
  // Live area: clips before its end are launched by hand (live mode).
  const bool liveMode = a.engine.mode() == PlayMode::Live;
  if (s.live.areaEnd > 0 && xOf(0) < gMax.x) {
    float lx1 = std::min(gMax.x, xOf(s.live.areaEnd));
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(0)), gMin.y), ImVec2(lx1, gMax.y), theme::withAlpha(P.paper, liveMode ? 0.05f : 0.03f));
    if (lx1 < gMax.x) dl->AddLine(ImVec2(std::round(lx1), gMin.y), ImVec2(std::round(lx1), gMax.y), P.lineStrong, 2.0f);
  }
  // Time selection: tinted band across the tracks.
  if (st.selEnd > st.selStart)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(st.selStart)), gMin.y), ImVec2(std::min(gMax.x, xOf(st.selEnd)), gMax.y),
                      theme::withAlpha(P.accent, 0.07f));
  // Song end shading.
  const ClipCache& ccache = clipCache(a);
  const Tick songLen = ccache.songLen;
  if (songLen > 0 && xOf(songLen) < gMax.x)
    dl->AddRectFilled(ImVec2(std::max(gMin.x, xOf(songLen)), gMin.y), gMax, theme::withAlpha(P.surface950, 0.35f));
  // Clips.
  ObjectId hoverClip = kNoId, hoverMenuGlyph = kNoId;
  bool hoverEdge = false, hoverLeftEdge = false, hoverBody = false;
  // Audio clip fade handles (J-8; J-38: tension handles, crossfade partners):
  // 1 = fade-in length, 2 = fade-out length, 3 = fade-in tension, 4 = fade-out tension.
  static ObjectId fadeDragClip = kNoId;
  static int fadeDragKind = 0;
  static float fadeDragT0 = 0, fadeDragY0 = 0;
  static ObjectId fadeDragMirror = kNoId;  // tension: the crossfade partner whose opposite edge mirrors it
  ObjectId fadeHoverClip = kNoId;
  int fadeHoverKind = 0;
  ObjectId fadeHoverMirror = kNoId;
  int hoverPoint = -1, hoverHandle = -1;
  // Automation clip being edited: drawn from the live document (edits apply as they happen).
  // J-37: clips under a snapped drag glide between grid cells (drawing only);
  // free dragging (snap off, Alt, free grid at max zoom) follows the pointer.
  const double animNow = ImGui::GetTime();
  const bool animSnap = a.settings.getBool("ui.smoothSnap") && snapG > 1;
  const double animEpsTick = 0.25 / std::max(1e-6, static_cast<double>(st.pxPerTick));  // a quarter pixel
  auto xOfD = [&](double t) { return gMin.x + static_cast<float>(t * static_cast<double>(st.pxPerTick)) - st.scrollX; };
  static std::vector<DrawnClip> drawn;
  drawn.clear();
  // J-38: the empty grid redrawn under a clip where it lies over another clip
  // (the clip beneath stops showing through; the grid lines stay).
  auto gridPatch = [&](float x0, float x1, float y0, float y1) {
    x0 = std::max(x0, gMin.x), x1 = std::min(x1, gMax.x), y0 = std::max(y0, gMin.y), y1 = std::min(y1, gMax.y);
    if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), P.well);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(x0)), barT); xOf(t) < x1; t += barT)
      if ((t / barT) % 2 == 1)
        dl->AddRectFilled(ImVec2(std::max(x0, xOf(t)), y0), ImVec2(std::min(x1, xOf(t + barT)), y1), theme::withAlpha(P.paper, P.dark ? 0.035f : 0.018f));
    const ImU32 fine = theme::withAlpha(P.gridBeat, 0.45f);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(x0)), visGrid); xOf(t) < x1; t += visGrid) {
      const float gx = std::round(xOf(t));
      if (gx >= x0) dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), t % barT == 0 ? P.gridBar : t % beatT == 0 ? P.gridBeat : fine);
    }
  };
  // J-38: a fade curve over [xa, xb]: full level at yt, silence at yb. Fade-ins
  // rise left to right, fade-outs fall. `pts` receives kFadePts points.
  constexpr int kFadePts = 25;
  auto fadePath = [&](float xa, float xb, float yt, float yb, const EdgeFade& e, bool in, ImVec2* pts) {
    for (int k = 0; k < kFadePts; ++k) {
      const float f = static_cast<float>(k) / static_cast<float>(kFadePts - 1);
      const double g = fadeGain(in ? f : 1.0 - f, e.shape, e.tension);
      pts[k] = ImVec2(xa + (xb - xa) * f, yb - (yb - yt) * static_cast<float>(g));
    }
  };
  // Shade above the curve (the part the fade removes), then the curve itself.
  auto drawFade = [&](const ImVec2* pts, float yt, ImU32 shade, ImU32 line, float thick) {
    for (int k = 0; k + 1 < kFadePts; ++k)
      if (pts[k].y > yt + 0.25f || pts[k + 1].y > yt + 0.25f)
        dl->AddQuadFilled(ImVec2(pts[k].x, yt), ImVec2(pts[k + 1].x, yt), pts[k + 1], pts[k], shade);
    dl->AddPolyline(pts, kFadePts, line, ImDrawFlags_None, thick);
  };
  // Fade handles of the hovered clip (and of the crossfade partner under it).
  struct FadeHandle {
    ObjectId clip;
    int kind;
    ImVec2 pos;
    ObjectId mirror;
  };
  std::vector<FadeHandle> fadeHandles;
  auto drawClip = [&](size_t ci) {
    if (!ccache.valid[ci]) return;
    const Clip& lc = s.playlist[ci];  // logical clip (what the document holds)
    Tick len = ccache.len[ci];
    float x0 = xOf(lc.start), x1 = xOf(lc.start + len), y = yOf(lc.track), th = hOf(lc.track);
    Clip animClip;
    const Clip* cp = &lc;
    const bool gliding = !clipAnim.empty() && clipAnim.contains(lc.id);
    if (gliding) {
      const double secPerTick = 60.0 / (std::max(1.0, tempoMap.bpmAt(static_cast<double>(lc.start))) * std::max(1, s.ppq));
      const smooth::Tracker<4>::Vec eps{animEpsTick, animEpsTick, 0.01, lc.kind == ClipKind::Audio ? animEpsTick * secPerTick : animEpsTick};
      const auto v = clipAnim.follow(lc.id, clipAnimValues(lc, len), animNow, animSnap, eps);
      animClip = lc;
      animClip.start = static_cast<Tick>(std::llround(v[0]));
      len = std::max<Tick>(1, static_cast<Tick>(std::llround(v[1])) - animClip.start);
      if (lc.kind == ClipKind::Audio) animClip.audioStart = v[3];
      else animClip.offset = static_cast<Tick>(std::llround(v[3]));
      x0 = xOfD(v[0]), x1 = std::max(x0 + 1.0f, xOfD(v[1]));
      const int tr0 = std::clamp(static_cast<int>(std::floor(v[2])), 0, std::max(0, nTracks - 1));
      const int tr1 = std::min(tr0 + 1, std::max(0, nTracks - 1));
      const float f = std::clamp(static_cast<float>(v[2] - tr0), 0.0f, 1.0f);
      y = yOf(tr0) + (yOf(tr1) - yOf(tr0)) * f;
      th = hOf(tr0) + (hOf(tr1) - hOf(tr0)) * f;
      cp = &animClip;
    }
    const Clip& c = *cp;  // drawn clip
    if (x1 < gMin.x || x0 > gMax.x || y + th < gMin.y || y > gMax.y || th < 1) return;
    bool sel = st.selected.count(c.id) > 0;
    const bool livePlaying = liveMode && a.engine.liveActive(c.track) == c.id;
    const bool liveWaiting = liveMode && a.engine.liveQueued(c.track) == c.id;
    bool muted = c.muted || (c.track < nTracks && s.tracks[static_cast<size_t>(c.track)].muted);
    ImU32 base = theme::rgb(ccache.color[ci]);
    // Clip: translucent colour body (J-37: see-through, so the grid shows), a
    // more opaque title bar with the name, a crisp rim; selected clips get a lit
    // rim. Over the empty grid the composite matches the J-13 palette (body ~58 %
    // towards the well, title bar ~8 %).
    const float bodyA = muted ? 0.60f : 0.74f, headA = muted ? 0.80f : 0.90f;
    const ImU32 headSolid = theme::mix(base, P.well, muted ? 0.56f : 0.0f);  // title text contrast
    const ImU32 body = theme::withAlpha(theme::mix(base, P.well, muted ? 0.75f : 0.43f), bodyA);
    const ImU32 head = theme::withAlpha(headSolid, headA);
    const ImU32 rim = sel ? P.hot : theme::mix(base, P.ink, muted ? 0.7f : 0.45f);
    ImVec2 a0(std::round(x0) + 1, y + 1), a1(std::max(std::round(x0) + 5, std::round(x1) - 1), y + th - 1);
    const float headH = std::min(15.0f, (a1.y - a0.y) * 0.45f);
    // J-38: stacking. Where this clip lies over clips drawn before it on the
    // track, the grid is redrawn under it (the clip beneath no longer shows
    // through as a muddle of two bodies) and a soft shadow falls from its edge
    // onto the clip beneath.
    {
      const ImU32 shadow = theme::withAlpha(P.ink, P.dark ? 0.42f : 0.20f), clear = theme::withAlpha(P.ink, 0.0f);
      int budget = 64;
      for (size_t k = drawn.size(); k-- > 0 && budget-- > 0;) {
        const DrawnClip& p = drawn[k];
        if (p.track != lc.track || p.x1 <= a0.x || p.x0 >= a1.x) continue;
        const float ox0 = std::max(p.x0, a0.x), ox1 = std::min(p.x1, a1.x), oy0 = std::max(p.y0, a0.y), oy1 = std::min(p.y1, a1.y);
        if (oy1 - oy0 < 1.0f) continue;
        gridPatch(ox0, ox1, a0.y, a1.y);
        constexpr float kShadow = 7.0f;
        if (a0.x > p.x0 + 1 && a0.x < p.x1)  // our left edge over the clip beneath
          dl->AddRectFilledMultiColor(ImVec2(std::max(p.x0, a0.x - kShadow), oy0), ImVec2(a0.x, oy1), clear, shadow, shadow, clear);
        if (a1.x < p.x1 - 1 && a1.x > p.x0)  // our right edge too (a short clip inside a long one)
          dl->AddRectFilledMultiColor(ImVec2(a1.x, oy0), ImVec2(std::min(p.x1, a1.x + kShadow), oy1), shadow, clear, clear, shadow);
      }
    }
    dl->AddRectFilled(ImVec2(a0.x, a0.y + headH), a1, body, theme::kRadiusSm, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(a0, ImVec2(a1.x, a0.y + headH), head, theme::kRadiusSm, ImDrawFlags_RoundCornersTop);
    drawn.push_back({lc.track, a0.x, a1.x, a0.y, a1.y, a0.y + headH, ci, rim, sel});
    dl->AddLine(ImVec2(a0.x + 1, a0.y + 0.5f), ImVec2(a1.x - 1, a0.y + 0.5f), theme::withAlpha(P.paper, muted ? 0.08f : 0.28f));
    dl->AddLine(ImVec2(a0.x, a0.y + headH), ImVec2(a1.x, a0.y + headH), theme::withAlpha(P.ink, 0.35f));
    dl->AddRect(a0, a1, rim, theme::kRadiusSm, 0, sel ? 2.0f : 1.0f);
    if (livePlaying) dl->AddRect(ImVec2(a0.x - 1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1), P.accent, theme::kRadiusSm, 0, 2.0f);
    if (liveWaiting)
      dl->AddRect(ImVec2(a0.x - 1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1),
                  theme::withAlpha(P.accent, 0.35f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f))), theme::kRadiusSm, 0, 2.0f);
    dl->PushClipRect(ImVec2(std::max(a0.x, gMin.x), std::max(a0.y, gMin.y)), ImVec2(std::min(a1.x, gMax.x), std::min(a1.y, gMax.y)), true);
    const std::string mutedName = c.muted ? *ccache.name[ci] + "  (muted)" : std::string();
    const char* title = c.muted ? mutedName.c_str() : ccache.name[ci]->c_str();
    // J-38: the title stays in the part of the clip that shows. A clip lying on
    // top cuts it short (ellipsis before the top clip's edge); when that leaves
    // no room, the title moves past the top clip's end.
    float titleL = std::max(a0.x, gMin.x), titleR = a1.x;
    bool titleGlyph = true;
    if (!gliding && ccache.cover[ci] >= 0) {
      const size_t cv = static_cast<size_t>(ccache.cover[ci]);
      if (!(clipAnim.contains(s.playlist[cv].id))) {
        const float coverX0 = xOf(s.playlist[cv].start), coverX1 = xOf(s.playlist[cv].start + ccache.len[cv]);
        titleR = std::min(titleR, coverX0 - 2);
        if (titleR - titleL < 44 && a1.x - std::max(coverX1, gMin.x) > 52) titleL = std::max(coverX1, gMin.x) + 4, titleR = a1.x, titleGlyph = false;
      }
    }
    // Title bar: menu glyph (click: clip menu, like FL's clip icon), then the name.
    const float tx0 = titleL;
    const bool glyph = titleGlyph && titleR - tx0 > 34 && headH >= 9;
    if (glyph) {
      const float gx = tx0 + 7, gy = a0.y + headH * 0.5f;
      const bool hotGlyph = hovered && std::fabs(io.MousePos.x - gx) < 7 && io.MousePos.y >= a0.y && io.MousePos.y < a0.y + headH;
      if (hotGlyph) hoverMenuGlyph = c.id;
      dl->AddTriangleFilled(ImVec2(gx - 3.5f, gy - 2), ImVec2(gx + 3.5f, gy - 2), ImVec2(gx, gy + 2.5f),
                            hotGlyph ? P.hot : theme::withAlpha(theme::contrastText(headSolid), 0.8f));
    }
    if (titleR - tx0 > 12)
      theme::textEllipsis(dl, a.fontBold, std::min(theme::kFontCaption, headH - 2), ImVec2(tx0 + (glyph ? 15 : 6), a0.y + std::max(0.0f, (headH - theme::kFontCaption) * 0.5f)),
                          titleR - tx0 - (glyph ? 19 : 10), theme::contrastText(headSolid), title);
    const ImVec2 b0(a0.x + 3, a0.y + headH + 2), b1(a1.x - 3, a1.y - 3);
    // Clip content (notes, waveform, curve): lighter than the colour on dark
    // slate, darker than it on the light variant's pastel bodies.
    const ImU32 ink = P.dark ? theme::mix(base, P.paper, muted ? 0.0f : 0.45f) : theme::mix(base, P.ink, muted ? 0.15f : 0.42f);
    const ImU32 mark = P.dark ? P.paper : theme::mix(base, P.ink, 0.6f);
    bool inside = hovered && io.MousePos.x >= a0.x && io.MousePos.x < a1.x && io.MousePos.y >= a0.y && io.MousePos.y < a1.y && io.MousePos.y >= gMin.y;
    if (c.kind == ClipKind::Pattern) {
      drawClipPreview(dl, s, *s.pattern(c.pattern), b0, b1, len, c.offset, ink, gMin.x, gMax.x);
    } else if (c.kind == ClipKind::Audio) {
      if (b1.y - b0.y > 4) drawWaveform(dl, s, *s.channel(c.source), c, tempoMap, b0, b1, x0, st.pxPerTick, gMin.x, gMax.x, ink);
      // J-38: fades as they play (automatic crossfades included), with their curve shape.
      const ClipFades& fd = ci < ccache.fades.size() ? ccache.fades[ci] : ClipFades{};
      const double sec0 = tempoMap.secondsAt(static_cast<double>(c.start));
      const double dur = tempoMap.secondsAt(static_cast<double>(c.start + len)) - sec0;
      auto xAtSec = [&](double sec) { return xOfD(tempoMap.tickAtSeconds(sec0 + sec)); };
      const ImU32 shade = theme::withAlpha(P.ink, P.dark ? 0.30f : 0.16f);
      const ImU32 fline = P.dark ? theme::withAlpha(P.paper, 0.75f) : theme::mix(base, P.ink, 0.65f);
      ImVec2 pts[kFadePts];
      if (fd.in.len > 0 && b1.y - b0.y > 3) {
        fadePath(a0.x, std::min(xAtSec(fd.in.len), a1.x), b0.y, b1.y, fd.in, true, pts);
        drawFade(pts, b0.y, shade, fline, fd.in.crossfade ? 1.6f : 1.2f);
      }
      if (fd.out.len > 0 && b1.y - b0.y > 3) {
        fadePath(std::max(xAtSec(dur - fd.out.len), a0.x), a1.x, b0.y, b1.y, fd.out, false, pts);
        drawFade(pts, b0.y, shade, fline, fd.out.crossfade ? 1.6f : 1.2f);
      }
      // Fade handles (drawn after every clip, see below): this clip's own, and
      // the fade-out of the clip it lies on (its crossfade partner).
      if (st.showFades && (inside || fadeDragClip == c.id) && b1.y - b0.y > 8 && a1.x - a0.x > 24) {
        fadeHandles.clear();
        const float hin = std::max(a0.x + 4, std::min(xAtSec(fd.in.len), a1.x - 8));
        const float hout = std::min(a1.x - 4, std::max(xAtSec(dur - fd.out.len), a0.x + 8));
        // A tied crossfade (both sides follow the overlap): one tension handle steers both.
        auto tiedTo = [&](const EdgeFade& e, bool in) -> ObjectId {
          if (!e.crossfade) return kNoId;
          const int pi = s.clipIndex(e.partner);
          if (pi < 0 || static_cast<size_t>(pi) >= ccache.fades.size()) return kNoId;
          const EdgeFade& o = in ? ccache.fades[static_cast<size_t>(pi)].out : ccache.fades[static_cast<size_t>(pi)].in;
          return o.len > 0 && std::fabs(o.len - e.len) < 1e-6 ? e.partner : kNoId;
        };
        fadeHandles.push_back({c.id, 1, ImVec2(hin, b0.y), kNoId});
        fadeHandles.push_back({c.id, 2, ImVec2(hout, b0.y), kNoId});
        if (fd.in.len > 0 && hin - a0.x > 14) {
          fadePath(a0.x, std::min(xAtSec(fd.in.len), a1.x), b0.y, b1.y, fd.in, true, pts);
          fadeHandles.push_back({c.id, 3, pts[kFadePts / 2], tiedTo(fd.in, true)});
        }
        if (fd.out.len > 0 && a1.x - hout > 14) {
          fadePath(std::max(xAtSec(dur - fd.out.len), a0.x), a1.x, b0.y, b1.y, fd.out, false, pts);
          fadeHandles.push_back({c.id, 4, pts[kFadePts / 2], tiedTo(fd.out, false)});
        }
        // The clip beneath (crossfading into this one): its fade-out length at the bottom.
        if (fd.in.crossfade) {
          const int pi = s.clipIndex(fd.in.partner);
          if (pi >= 0 && static_cast<size_t>(pi) < ccache.fades.size() && ccache.fades[static_cast<size_t>(pi)].out.crossfade) {
            const Clip& pc = s.playlist[static_cast<size_t>(pi)];
            const double pEnd = tempoMap.secondsAt(static_cast<double>(pc.start + ccache.len[static_cast<size_t>(pi)]));
            const float px = xOfD(tempoMap.tickAtSeconds(pEnd - ccache.fades[static_cast<size_t>(pi)].out.len));
            if (px > a0.x - 2 && px < a1.x) fadeHandles.push_back({pc.id, 2, ImVec2(std::max(a0.x + 4, px), b1.y), kNoId});
          }
        }
      }
    } else if (c.kind == ClipKind::Automation && b1.y - b0.y > 6) {
      const AutomationClip& ac = *s.automation(c.source);
      AutoGeom g{b0, b1, x0, st.pxPerTick, c.start, c.offset, len};
      int hp = -1, hh = -1;
      if (inside && io.MousePos.y > a0.y + headH) hitAutomation(ac, g, io.MousePos, hp, hh);
      if (cg.kind == ClipGesture::AutoPoint && cg.anchor == c.id) hp = cg.point;
      if (cg.kind == ClipGesture::AutoTension && cg.anchor == c.id) hh = cg.point;
      drawAutomation(dl, ac, g, gMin.x, gMax.x, ink, theme::withAlpha(base, muted ? 0.08f : 0.18f), inside || sel, hp, hh, mark);
      if (inside) hoverPoint = hp, hoverHandle = hh;
    }
    dl->PopClipRect();
    if (inside) {
      hoverClip = c.id;
      float edge = std::min(8.0f, (a1.x - a0.x) * 0.3f);
      hoverEdge = io.MousePos.x > a1.x - edge;
      hoverLeftEdge = !hoverEdge && io.MousePos.x < a0.x + edge;
      hoverBody = io.MousePos.y > a0.y + headH;
      if (c.kind != ClipKind::Automation) hoverPoint = hoverHandle = -1;
      if (c.kind != ClipKind::Audio && fadeDragClip == kNoId) fadeHandles.clear();  // the top clip owns the pointer
    }
  };
  // Stacking (J-38): by track, then start (a later clip lies on the one it
  // overlaps); clips under a drag (gliding) draw last, on top of everything.
  const bool anyAnim = !clipAnim.empty();
  for (uint32_t ci : ccache.order)
    if (!anyAnim || !clipAnim.contains(s.playlist[ci].id)) drawClip(ci);
  if (anyAnim)
    for (uint32_t ci : ccache.order)
      if (clipAnim.contains(s.playlist[ci].id)) drawClip(ci);
  clipAnim.prune(animNow, gestureAnimates(cg.kind));
  // J-38: overlaps. The clip beneath keeps a dashed outline through the span
  // it shares with the clip on top (and its end edge), so both extents read at
  // a glance; an audio crossfade shows both curves crossing (an X).
  if (drawn.size() > 1) {
    std::vector<size_t> byTrack(drawn.size());
    for (size_t k = 0; k < drawn.size(); ++k) byTrack[k] = k;
    std::stable_sort(byTrack.begin(), byTrack.end(), [](size_t p, size_t q) { return drawn[p].track != drawn[q].track ? drawn[p].track < drawn[q].track : drawn[p].x0 < drawn[q].x0; });
    auto dashH = [&](float xa, float xb, float yy, ImU32 col) {
      for (float x = xa; x < xb; x += 7.0f) dl->AddLine(ImVec2(x, yy), ImVec2(std::min(x + 4.0f, xb), yy), col, 1.0f);
    };
    auto dashV = [&](float xx, float ya, float yb, ImU32 col) {
      for (float yy = ya; yy < yb; yy += 6.0f) dl->AddLine(ImVec2(xx, yy), ImVec2(xx, std::min(yy + 3.0f, yb)), col, 1.0f);
    };
    dl->PushClipRect(gMin, gMax, true);
    int budget = 4096;  // pathological stacks: bounded work per frame
    for (size_t u = 0; u < byTrack.size() && budget > 0; ++u)
      for (size_t v = u + 1; v < byTrack.size() && budget > 0; ++v, --budget) {
        const size_t iu = byTrack[u], iv = byTrack[v];
        if (drawn[iv].track != drawn[iu].track || drawn[iv].x0 >= drawn[iu].x1) break;
        const DrawnClip& p = drawn[std::min(iu, iv)];  // drawn first: beneath
        const DrawnClip& q = drawn[std::max(iu, iv)];  // drawn later: on top
        const float ox0 = std::max(p.x0, q.x0), ox1 = std::min(p.x1, q.x1);
        if (ox1 - ox0 < 1.0f) continue;
        const ImU32 ghost = p.sel ? P.hot : theme::withAlpha(P.dark ? theme::mix(p.rim, P.paper, 0.55f) : p.rim, 0.85f);
        dashH(ox0, ox1, std::max(p.y0, q.y0) + 0.5f, ghost);
        dashH(ox0, ox1, std::min(p.y1, q.y1) - 0.5f, ghost);
        if (p.x1 > q.x0 + 1 && p.x1 < q.x1 - 1) dashV(p.x1 - 0.5f, std::max(p.y0, q.y0), std::min(p.y1, q.y1), ghost);
        if (p.x0 > q.x0 + 1 && p.x0 < q.x1 - 1) dashV(p.x0 + 0.5f, std::max(p.y0, q.y0), std::min(p.y1, q.y1), ghost);
        // Crossfade: the fade-out of the clip beneath, drawn over the clip on top.
        const Clip& pc = s.playlist[p.ci];
        if (pc.kind == ClipKind::Audio && p.ci < ccache.fades.size() && ccache.fades[p.ci].out.len > 0 && q.body < q.y1 - 6 &&
            !clipAnim.contains(pc.id)) {
          const EdgeFade& e = ccache.fades[p.ci].out;
          const double pEnd = tempoMap.secondsAt(static_cast<double>(pc.start + ccache.len[p.ci]));
          const float xa = std::max(p.x0, xOfD(tempoMap.tickAtSeconds(pEnd - e.len)));
          if (p.x1 - xa > 2 && xa < q.x1) {
            ImVec2 pts[kFadePts];
            fadePath(xa, p.x1, q.body + 2, std::min(p.y1, q.y1) - 3, e, false, pts);
            const ImU32 col = P.dark ? theme::mix(theme::rgb(ccache.color[p.ci]), P.paper, 0.55f) : theme::mix(theme::rgb(ccache.color[p.ci]), P.ink, 0.55f);
            dl->AddPolyline(pts, kFadePts, theme::withAlpha(col, 0.95f), ImDrawFlags_None, 1.6f);
          }
        }
      }
    dl->PopClipRect();
  }
  // Fade handles on top of everything (J-38): squares set the length, rings the curve tension.
  if (!fadeHandles.empty() && hovered) {
    float best = 7.0f;
    for (const auto& h : fadeHandles) {
      const float d = std::max(std::fabs(io.MousePos.x - h.pos.x), std::fabs(io.MousePos.y - h.pos.y));
      if (fadeDragClip == kNoId && d < best) best = d, fadeHoverClip = h.clip, fadeHoverKind = h.kind, fadeHoverMirror = h.mirror;
    }
  }
  for (const auto& h : fadeHandles) {
    const bool hot = (fadeDragClip == h.clip && fadeDragKind == h.kind) || (fadeHoverClip == h.clip && fadeHoverKind == h.kind);
    const ImU32 fill = hot ? P.hot : theme::withAlpha(P.paper, 0.85f), edge = theme::withAlpha(P.ink, 0.7f);
    if (h.kind <= 2) {
      dl->AddRectFilled(ImVec2(h.pos.x - 3.5f, h.pos.y - 3.5f), ImVec2(h.pos.x + 3.5f, h.pos.y + 3.5f), edge, 1.0f);
      dl->AddRectFilled(ImVec2(h.pos.x - 2.5f, h.pos.y - 2.5f), ImVec2(h.pos.x + 2.5f, h.pos.y + 2.5f), fill, 1.0f);
    } else {
      dl->AddCircleFilled(h.pos, 4.0f, edge, 12);
      dl->AddCircleFilled(h.pos, 3.0f, hot ? P.hot : P.well, 12);
      dl->AddCircle(h.pos, 3.0f, fill, 12, 1.2f);
    }
  }
  // Empty arrangement: muted mark (guide §08 C).
  if (s.playlist.empty())
    ui::EmptyState(dl, gMin, gMax, "Empty arrangement", "Click to place the selected pattern, or drag one in from the list");
  // Playhead.
  if (a.engine.mode() == PlayMode::Song) {
    float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
    dl->AddLine(ImVec2(x, gMin.y), ImVec2(x, gMax.y), P.playhead, 1.5f);
  }
  if (cg.kind == ClipGesture::Select && active) {
    dl->AddRectFilled(cg.start, io.MousePos, theme::withAlpha(P.accent, 0.12f));
    dl->AddRect(cg.start, io.MousePos, P.accent, 0, 0, 1.5f);
  }
  if (cg.kind == ClipGesture::Zoom && active) {
    const ImU32 zc = theme::rgb(theme::role::kZoom);
    dl->AddRectFilled(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), theme::withAlpha(zc, 0.10f));
    dl->AddRect(ImVec2(cg.start.x, gMin.y), ImVec2(io.MousePos.x, gMax.y), zc, 0, 0, 1.2f);
  }
  // PL-13: slice line positions glide between grid lines (J-37 smooth snap).
  auto sliceX = [&](uint64_t key, Tick t) {
    const smooth::Tracker<1>::Vec target{static_cast<double>(t)};
    if (!sliceAnim.contains(key)) sliceAnim.seed(key, target, animNow);
    return std::round(xOfD(sliceAnim.follow(key, target, animNow, animSnap, {animEpsTick})[0]));
  };
  if (cg.kind == ClipGesture::SliceLine && active) {  // the cut line, over the tracks dragged across
    const float x = sliceX(1, cg.startTick);
    const int t0 = std::min(cg.startTrack, trackAt(io.MousePos.y)), t1 = std::max(cg.startTrack, trackAt(io.MousePos.y));
    const float y0 = io.KeyShift ? gMin.y : yOf(t0), y1 = io.KeyShift ? gMax.y : yOf(t1) + hOf(t1);
    const ImU32 sc = theme::rgb(theme::role::kSlice);
    const float dx = std::fabs(io.MousePos.x - cg.start.x), dy = std::fabs(io.MousePos.y - cg.start.y);
    if (dx > 6.0f && dx > dy) {  // horizontal drag: a region cut (both ends), the inside lit
      const float x2 = sliceX(2, std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(io.MousePos.x)), snapG, Snap::Line)));
      dl->AddRectFilled(ImVec2(std::min(x, x2), y0), ImVec2(std::max(x, x2), y1), theme::withAlpha(sc, 0.16f));
      dl->AddLine(ImVec2(x2, y0), ImVec2(x2, y1), sc, 2.0f);
      dl->AddTriangleFilled(ImVec2(x2 - 4, y0), ImVec2(x2 + 4, y0), ImVec2(x2, y0 + 5), sc);
    }
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), sc, 2.0f);
    dl->AddTriangleFilled(ImVec2(x - 4, y0), ImVec2(x + 4, y0), ImVec2(x, y0 + 5), sc);
  } else if (st.tool == Tool::Slice && hovered && io.MousePos.x >= gMin.x && io.MousePos.y >= gMin.y && hoverClip != kNoId) {
    const Tick cut = timeline::snapTick(std::max<Tick>(0, tickAt(io.MousePos.x)), snapG, Snap::Line);
    const float x = sliceX(1, cut);
    const Clip* sc = s.clip(hoverClip);
    float y0 = io.KeyShift ? gMin.y : yOf(sc->track), y1 = io.KeyShift ? gMax.y : yOf(sc->track) + hOf(sc->track);
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), theme::rgb(theme::role::kSlice), 1.5f);
  } else {
    sliceAnim.clear();
  }
  dl->PopClipRect();

  // Ruler: bar numbers, beat ticks, markers, song end marker, playhead cap.
  dl->AddRectFilled(origin, ImVec2(gMax.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(gMax.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  dl->PushClipRect(ImVec2(gMin.x, rY), ImVec2(gMax.x, gMin.y), true);
  int hoverMarker = -1;
  {
    // Time selection in the ruler.
    if (st.selEnd > st.selStart) {
      float sx0 = xOf(st.selStart), sx1 = xOf(st.selEnd);
      dl->AddRectFilled(ImVec2(sx0, rY + 2), ImVec2(sx1, gMin.y - 1), theme::withAlpha(P.accent, 0.38f), 2);
      dl->AddLine(ImVec2(sx0, rY + 2), ImVec2(sx0, gMin.y - 1), P.accent, 1.5f);
      dl->AddLine(ImVec2(sx1, rY + 2), ImVec2(sx1, gMin.y - 1), P.accent, 1.5f);
    }
    // Minor ticks on the visible grid; labels on a coarser grid that leaves room for text.
    const Tick labelGrid = timeline::visibleGrid(s, st.pxPerTick, 64.0);
    for (Tick t = snapDown(std::max<Tick>(0, tickAt(gMin.x)), visGrid); xOf(t) < gMax.x; t += visGrid) {
      float x = std::round(xOf(t));
      bool major = t % labelGrid == 0;
      float len = major ? 10.0f : t % beatT == 0 ? 6.0f : 3.0f;
      dl->AddLine(ImVec2(x, gMin.y - len), ImVec2(x, gMin.y - 1), major ? P.lineStrong : P.line);
      if (major) {
        std::string lbl = timeline::rulerLabel(s, t, labelGrid);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 4, rY + 4), t % barT == 0 ? P.text : P.textFaint, lbl.c_str());
      }
    }
    // Markers: a flag with the name (time signature markers show the meter).
    for (size_t i = 0; i < s.markers.size(); ++i) {
      const Marker& mk = s.markers[i];
      float x = std::round(xOf(mk.pos));
      if (x < gMin.x - 200 || x > gMax.x) continue;
      std::string lbl = mk.timeSigNum > 0 ? std::to_string(mk.timeSigNum) + "/" + std::to_string(mk.timeSigDen) : mk.name;
      if (mk.timeSigNum > 0 && !mk.name.empty() && mk.name != lbl) lbl += "  " + mk.name;
      if (lbl.empty()) lbl = "\xE2\x80\xA2";  // bullet for unnamed markers
      static const char* const acts[] = {"", "loop", "skip", "pause", "stop"};
      if (mk.action != MarkerAction::None) lbl += std::string("  \xC2\xB7 ") + acts[static_cast<int>(mk.action)];
      float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str()).x;
      ImVec2 f0(x, rY + 13), f1(x + tw + 8, gMin.y - 2);
      bool hov = hovered && io.MousePos.x >= f0.x && io.MousePos.x < f1.x && io.MousePos.y >= rY && io.MousePos.y < gMin.y;
      if (hov) hoverMarker = static_cast<int>(i);
      dl->AddRectFilled(f0, f1, hov ? P.surface600 : P.surface700, 2, ImDrawFlags_RoundCornersRight);
      dl->AddLine(ImVec2(x, rY + 2), ImVec2(x, gMin.y - 1), mk.timeSigNum > 0 ? P.textDim : P.text, 1.5f);
      dl->AddText(mk.timeSigNum > 0 ? a.fontMono : a.fontRegular, theme::kFontLabel, ImVec2(x + 4, f0.y), mk.timeSigNum > 0 ? P.textDim : P.text,
                  lbl.c_str());
    }
    if (s.live.areaEnd > 0) {
      float lx = std::round(xOf(s.live.areaEnd));
      if (lx > gMin.x - 60 && lx < gMax.x + 60) {
        dl->AddLine(ImVec2(lx, rY + 2), ImVec2(lx, gMin.y - 1), P.lineStrong, 2.0f);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(lx - 34, rY + 13), liveMode ? P.text : P.textFaint, "LIVE");
      }
    }
    if (songLen > 0) {
      float xe = std::round(xOf(songLen));
      dl->AddTriangleFilled(ImVec2(xe - 5, gMin.y - 9), ImVec2(xe + 5, gMin.y - 9), ImVec2(xe, gMin.y - 2), P.textFaint);
    }
    if (a.engine.mode() == PlayMode::Song) {
      float x = std::round(xOf(static_cast<Tick>(a.engine.position())));
      dl->AddTriangleFilled(ImVec2(x - 5, gMin.y - 9), ImVec2(x + 5, gMin.y - 9), ImVec2(x, gMin.y - 1), P.playhead);
    }
  }
  dl->PopClipRect();

  // Track headers (FL layout): name top left, "..." handle bottom left (drag:
  // reorder, click: track menu), round switch bottom right (mute; right-click:
  // solo), colour strip, resize grip along the bottom edge.
  const float headX0 = origin.x + vbarW;
  dl->PushClipRect(ImVec2(headX0, gMin.y), ImVec2(gMin.x, gMax.y), true);
  dl->AddRectFilled(ImVec2(headX0, gMin.y), ImVec2(gMin.x, gMax.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(gMin.x - 1, gMin.y), ImVec2(gMin.x - 1, gMax.y), P.dark ? P.ink : P.line);
  int headResize = -1;  // track whose header's bottom edge is under the pointer
  int headHandle = -1;  // track whose "..." handle is under the pointer
  int headLed = -1;     // track whose mute switch is under the pointer
  int headModeTrack = -1, headModeCtl = 0;  // PL-08: audio / instrument track control under the pointer
  static int inputTrack = -1;               // track whose input popup is open
  static int renaming = -1;
  static ObjectId menuClip = kNoId;
  static int dragTrack = -1;  // header being dragged to a new place
  for (int tr = trackAt(gMin.y); tr < nTracks && yOf(tr) < gMax.y; ++tr) {
    float y = yOf(tr), th = hOf(tr);
    if (th < 1) continue;  // inside a folded group
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    const float indent = 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    bool hovHead = hovered && io.MousePos.x >= headX0 && io.MousePos.x < gMin.x && io.MousePos.y >= y && io.MousePos.y < y + th &&
                   io.MousePos.y >= gMin.y;
    ImVec2 h0(headX0 + 2, y + 1), h1(gMin.x - 3, y + th - 1);
    const bool coloured = t.color != PlaylistTrack{}.color;
    const ImU32 tc = theme::content(t.color);
    ImU32 face = theme::mix(hovHead || dragTrack == tr ? P.surface700 : P.surface800, tc, t.muted ? 0.04f : coloured ? 0.16f : 0.06f);
    dl->AddRectFilled(h0, h1, theme::mix(face, P.ink, 0.35f), theme::kRadiusSm);
    dl->AddRectFilled(h0, ImVec2(h1.x, h1.y - 1), face, theme::kRadiusSm);
    dl->AddLine(ImVec2(h0.x + 6, h0.y + 0.5f), ImVec2(h1.x - 2, h0.y + 0.5f), theme::withAlpha(P.paper, P.dark ? 0.10f : 0.5f));
    if (coloured) dl->AddRectFilled(h0, ImVec2(h0.x + 4, h1.y), tc, theme::kRadiusSm, ImDrawFlags_RoundCornersLeft);
    if (dragTrack == tr) dl->AddRect(h0, h1, theme::withAlpha(P.hot, 0.7f), theme::kRadiusSm, 0, 1.5f);
    const bool nearEdge = hovered && io.MousePos.x >= headX0 && io.MousePos.x < gMin.x && std::fabs(io.MousePos.y - (y + th)) < 3.0f &&
                          io.MousePos.y >= gMin.y;
    if (nearEdge) headResize = tr;
    const bool roomy = th >= 30;  // two lines: name above, handle + switch below
    float nameY = roomy ? y + 4.0f : y + (th - theme::kFontCaption) * 0.5f - 1;
    float nameX = h0.x + 8 + indent;
    if (t.groupSize > 0) {  // group: fold arrow
      ImVec2 c(nameX + 4, nameY + 7);
      if (t.folded)
        dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 4), ImVec2(c.x - 3, c.y + 4), ImVec2(c.x + 3, c.y), P.textDim);
      else
        dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x, c.y + 3), P.textDim);
      nameX += 12;
    }
    const float lineY = roomy ? h1.y - 8.0f : nameY + 7.0f;  // handle / switch line
    // Mute switch: lit in the track colour (the accent purple for uncoloured tracks).
    const ImVec2 ledC(h1.x - 10, lineY);
    const bool ledHot = hovHead && std::fabs(io.MousePos.x - ledC.x) < 8 && std::fabs(io.MousePos.y - ledC.y) < 8;
    if (ledHot) headLed = tr;
    const ImU32 ledCol = coloured ? theme::mix(tc, P.paper, 0.25f) : theme::rgb(theme::role::kStamp);
    dl->AddCircleFilled(ledC, 5.0f, theme::mix(P.ink, face, 0.3f));
    dl->AddCircle(ledC, 5.0f, ledHot ? P.hot : theme::withAlpha(ledCol, t.muted ? 0.45f : 0.9f), 16, 1.4f);
    if (!t.muted) dl->AddCircleFilled(ledC, 2.6f, ledCol);
    // "..." handle.
    const ImVec2 hc0(h0.x + 6 + indent, lineY - 5);
    const bool handleHot = hovHead && !ledHot && io.MousePos.x >= hc0.x - 2 && io.MousePos.x < hc0.x + 20 && std::fabs(io.MousePos.y - lineY) < 7;
    if (handleHot) headHandle = tr;
    if (roomy || th >= 22)
      for (int d = 0; d < 3; ++d)
        dl->AddCircleFilled(ImVec2(hc0.x + 4 + static_cast<float>(d) * 4.5f, lineY + 1), 1.2f, handleHot || dragTrack == tr ? P.hot : P.textFaint, 6);
    const float nameRight = roomy ? h1.x - 6 : h1.x - 22;
    if (roomy || th >= 18) {
      float nx = roomy ? nameX : nameX + 18;
      nx += trackui::drawNameGlyph(a, dl, tr, ImVec2(nx, nameY));  // PL-08: audio / instrument track glyph
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(nx, nameY), nameRight - nx, t.muted ? P.textFaint : P.text,
                          playlistTrackName(s, static_cast<size_t>(tr)).c_str());
    }
    {  // PL-08: input / monitoring / arm (audio tracks), channel (instrument tracks) between the handle and the switch
      const float cx1 = h1.x - 20 - (t.locked ? 16.0f : 0.0f) - (liveMode ? 14.0f : 0.0f);
      if (const int ctl = trackui::drawHeader(a, dl, tr, hc0.x + 18, cx1, lineY, hovHead && !ledHot && !handleHot, roomy)) {
        headModeTrack = tr;
        headModeCtl = ctl;
      }
    }
    if (liveMode && a.engine.liveActive(tr) != kNoId)  // live: a clip plays on this track
      dl->AddTriangleFilled(ImVec2(h1.x - 30, lineY - 4), ImVec2(h1.x - 30, lineY + 4), ImVec2(h1.x - 24, lineY), P.accent);
    if (t.locked) ui::drawIcon(dl, ui::Icon::Lock, ImVec2(h1.x - (liveMode ? 42 : 26), lineY), 11, P.textDim);
    if (nearEdge) {
      const float gx = (h0.x + h1.x) * 0.5f;
      for (int d = -2; d <= 2; ++d) dl->AddCircleFilled(ImVec2(gx + static_cast<float>(d) * 4.0f, h1.y - 1.5f), 1.0f, P.hot, 6);
    }
  }
  dl->PopClipRect();

  // Vertical scroller (left of the headers) and horizontal scroller (above the ruler).
  const float gridH = gMax.y - gMin.y;
  const float contentW = std::max(static_cast<float>(songLen + 8 * std::max<Tick>(1, s.barTicks())) * st.pxPerTick, st.scrollX + gridW);
  const ImVec2 vb0(origin.x, gMin.y), vb1(origin.x + vbarW - 2, gMax.y);
  const ImVec2 hb0(gMin.x, origin.y + 1), hb1(gMax.x, rY - 1);
  const float vThumbH = std::max(18.0f, (vb1.y - vb0.y) * std::min(1.0f, gridH / std::max(1.0f, totalH)));
  const float vThumbY = vb0.y + (totalH > gridH ? (vb1.y - vb0.y - vThumbH) * st.scrollY / (totalH - gridH) : 0.0f);
  const float hThumbW = std::max(24.0f, (hb1.x - hb0.x) * std::min(1.0f, gridW / std::max(1.0f, contentW)));
  const float hThumbX = hb0.x + (contentW > gridW ? (hb1.x - hb0.x - hThumbW) * st.scrollX / (contentW - gridW) : 0.0f);
  static int barDrag = 0;  // 1 vertical, 2 horizontal
  static float barGrab = 0;
  const bool overV = hovered && io.MousePos.x >= vb0.x && io.MousePos.x < vb1.x + 2 && io.MousePos.y >= vb0.y && io.MousePos.y < vb1.y;
  const bool overH = hovered && io.MousePos.x >= hb0.x && io.MousePos.x < hb1.x && io.MousePos.y >= origin.y && io.MousePos.y < rY;
  dl->AddRectFilled(ImVec2(origin.x, gMin.y), ImVec2(headX0, gMax.y), P.dark ? P.well : P.surface950);
  dl->AddRectFilled(ImVec2(vb0.x + 2, vThumbY), ImVec2(vb1.x, vThumbY + vThumbH), barDrag == 1 || overV ? P.surface600 : P.surface700, 3);
  dl->AddRectFilled(ImVec2(gMin.x, origin.y), ImVec2(gMax.x, rY), P.dark ? P.well : P.surface950);
  dl->AddRectFilled(ImVec2(hThumbX, hb0.y + 2), ImVec2(hThumbX + hThumbW, hb1.y - 1), barDrag == 2 || overH ? P.surface600 : P.surface700, 3);
  {  // song overview in the horizontal scroller: clip extents as thin marks
    const float k = (hb1.x - hb0.x) / std::max(1.0f, contentW);
    for (size_t ci = 0; ci < s.playlist.size() && ci < ccache.count; ++ci) {  // cached lengths / colours (J-25)
      const Clip& c = s.playlist[ci];
      const float x0 = hb0.x + static_cast<float>(c.start) * st.pxPerTick * k, x1 = hb0.x + static_cast<float>(c.start + ccache.len[ci]) * st.pxPerTick * k;
      if (x0 > hb1.x || !ccache.valid[ci]) continue;
      dl->AddRectFilled(ImVec2(x0, hb1.y - 3), ImVec2(std::max(x0 + 1, std::min(x1, hb1.x)), hb1.y - 1), theme::withAlpha(theme::rgb(ccache.color[ci]), 0.8f));
    }
  }

  // Header corner (J-38 layout): the view buttons above the track headers as
  // equal flat cells like the toolbar's: audio editing (stretch, fades, zero
  // crossings) | tracks (add, clear the selection) | view (picker panel, header
  // width), groups split by a thin rule. One row in a wide header column, two
  // rows (view group right-aligned) in a narrow one. A lit (orange) glyph on a
  // raised cell = on; hovering shows what a cell does.
  dl->AddRectFilled(origin, ImVec2(gMin.x, gMin.y), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(origin.x, gMin.y - 1), ImVec2(gMin.x, gMin.y - 1), P.dark ? P.ink : P.lineStrong);
  struct CornerBtn {
    int id;
    ImVec2 c;
    bool on;
    const char* hint;
    bool shown;
  };
  constexpr float bw = 20.0f, bh = 16.0f, groupGap = 9.0f;
  const float cl = origin.x + vbarW + 4, cr = gMin.x - 4;  // usable span
  const bool oneRow = cr - cl >= 7 * bw + 2 * groupGap;
  const float ry1 = oneRow ? std::round(rY + (gMin.y - rY) * 0.5f) : origin.y + 10.5f, ry2 = oneRow ? ry1 : origin.y + 29.5f;
  const float x0 = cl + bw * 0.5f, xR = cr - bw * 0.5f;
  const float tx = oneRow ? x0 + 3 * bw + groupGap : x0;                // tracks group
  const bool roomR = oneRow || xR - bw * 0.5f >= x0 + 2.5f * bw + 6;  // two rows: the view cells fit beside the others
  const CornerBtn corner[] = {
      {0, ImVec2(x0, ry1), st.stretchResize, "Stretch: resizing an audio clip changes its speed instead of trimming it (Shift+M)", true},
      {1, ImVec2(x0 + bw, ry1), st.showFades, "Fade handles on audio clips: drag a square for the length, a ring for the curve (Shift+F)", true},
      {2, ImVec2(x0 + 2 * bw, ry1), st.zeroCross, "Zero crossings: audio clip edges snap to silent points (no clicks)", true},
      {3, ImVec2(oneRow ? xR - bw : xR, ry1), st.pickerOpen, "Clip source picker panel (Alt+P)", roomR},
      {4, ImVec2(tx, ry2), false, "Add tracks", true},
      {5, ImVec2(tx + bw, ry2), false, "Clear the selection (clips and time)", true},
      {6, ImVec2(xR, ry2), st.wideHeaders, "Track header width: narrow / wide", roomR}};
  {  // group rules
    const ImU32 rule = theme::withAlpha(P.dark ? P.paper : P.ink, 0.14f);
    auto vrule = [&](float gx, float cy) { dl->AddLine(ImVec2(std::round(gx) + 0.5f, cy - 6), ImVec2(std::round(gx) + 0.5f, cy + 6), rule); };
    if (oneRow) {
      vrule(x0 + 2.5f * bw + groupGap * 0.5f, ry1);
      vrule((tx + 1.5f * bw + xR - 1.5f * bw) * 0.5f, ry1);
    } else if (roomR) {
      vrule(xR - bw * 0.5f - 4, ry1);
      vrule(xR - bw * 0.5f - 4, ry2);
    }
  }
  int hoverCorner = -1;
  for (const auto& b : corner) {
    if (!b.shown) continue;
    const ImVec2 c0(b.c.x - bw * 0.5f, b.c.y - bh * 0.5f), c1(b.c.x + bw * 0.5f, b.c.y + bh * 0.5f);
    const bool hot = hovered && io.MousePos.x >= c0.x && io.MousePos.x < c1.x && io.MousePos.y >= c0.y - 1 && io.MousePos.y < c1.y + 1;
    if (hot) hoverCorner = b.id;
    if (b.on) dl->AddRectFilled(c0, c1, P.dark ? P.surface600 : P.surface700, 3.0f);
    else if (hot) dl->AddRectFilled(c0, c1, theme::withAlpha(P.paper, P.dark ? 0.08f : 0.5f), 3.0f);
    const ImU32 col = b.on ? P.hot : hot ? P.text : P.textDim;
    const float x = b.c.x, y = b.c.y;
    switch (b.id) {
      case 0: {  // stretch: a wave between two bars
        dl->AddLine(ImVec2(x - 6.5f, y - 4), ImVec2(x - 6.5f, y + 4), col, 1.3f);
        dl->AddLine(ImVec2(x + 6.5f, y - 4), ImVec2(x + 6.5f, y + 4), col, 1.3f);
        dl->AddBezierCubic(ImVec2(x - 4.5f, y), ImVec2(x - 2, y - 6), ImVec2(x + 2, y + 6), ImVec2(x + 4.5f, y), col, 1.3f);
        break;
      }
      case 1: {  // fades: a ramp up, a hold, a ramp down
        const ImVec2 pts[] = {ImVec2(x - 6.5f, y + 4), ImVec2(x - 2.5f, y - 4), ImVec2(x + 2.5f, y - 4), ImVec2(x + 6.5f, y + 4)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_None, 1.4f);
        break;
      }
      case 2: {  // zero crossing: a wave through its centre line, a dot on the crossing
        dl->AddLine(ImVec2(x - 6.5f, y), ImVec2(x + 6.5f, y), theme::withAlpha(col, 0.45f), 1.0f);
        dl->AddBezierCubic(ImVec2(x - 6.5f, y), ImVec2(x - 3.5f, y - 7), ImVec2(x + 3.5f, y + 7), ImVec2(x + 6.5f, y), col, 1.3f);
        dl->AddCircleFilled(ImVec2(x, y), 1.6f, col, 8);
        break;
      }
      case 3: ui::drawIcon(dl, st.pickerOpen ? ui::Icon::Left : ui::Icon::Right, b.c, 10.0f, col); break;
      case 4: ui::drawIcon(dl, ui::Icon::Plus, b.c, 10.0f, col); break;
      case 5: ui::drawIcon(dl, ui::Icon::Close, b.c, 9.0f, col); break;
      case 6: {  // double arrow
        dl->AddLine(ImVec2(x - 5, y), ImVec2(x + 5, y), col, 1.3f);
        dl->AddTriangleFilled(ImVec2(x - 7, y), ImVec2(x - 3.5f, y - 3), ImVec2(x - 3.5f, y + 3), col);
        dl->AddTriangleFilled(ImVec2(x + 7, y), ImVec2(x + 3.5f, y + 3), ImVec2(x + 3.5f, y - 3), col);
        break;
      }
      default: break;
    }
    if (hot) a.setHint(b.hint);
  }
  if (hoverCorner >= 0) ImGui::SetTooltip("%s", corner[hoverCorner].hint);

  // ---------------- interaction ----------------
  const ImVec2 m = io.MousePos;
  const bool inGrid = m.x >= gMin.x && m.x < gMax.x && m.y >= gMin.y && m.y < gMax.y;
  const bool inRuler = m.y >= rY && m.y < gMin.y && m.x >= gMin.x;
  const bool inHead = m.x >= headX0 && m.x < gMin.x && m.y >= gMin.y;
  const Clip* hc = hoverClip != kNoId ? s.clip(hoverClip) : nullptr;
  const bool hoverAuto = hc && hc->kind == ClipKind::Automation && hoverBody;
  const bool shiftDrag = io.KeyShift && !io.KeyCtrl;               // Draw: Shift+drag a clip copies it
  const bool slipDrag = io.KeyCtrl && io.KeyShift;                 // Draw: Ctrl+Shift+drag slips the content
  if (hovered && inGrid) {
    if (hoverPoint >= 0 || hoverHandle >= 0)
      ImGui::SetMouseCursor(hoverHandle >= 0 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_Hand);
    else if (hoverMenuGlyph != kNoId)
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (hoverClip != kNoId && st.tool == Tool::Draw)
      ImGui::SetMouseCursor(hoverEdge || hoverLeftEdge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
    else if (st.tool == Tool::Slip && hoverClip != kNoId)
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    std::string where = "Bar " + std::to_string(tickAt(m.x) / std::max<Tick>(1, s.barTicks()) + 1) + " · " +
                        playlistTrackName(s, static_cast<size_t>(trackAt(m.y)));
    if (liveMode && hc && inLiveArea(s, *hc))
      a.setHint(where + "   (live: click to launch on the track's launch grid \xC2\xB7 right-click to stop the track)");
    else if (hoverAuto)
      a.setHint(where + "   (drag a point · drag a ring: curve tension · double-click: add point · right-click a point: delete · double-click the title: editor)");
    else if (hoverMenuGlyph != kNoId)
      a.setHint(where + "   (click: clip menu)");
    else if (hc && st.tool == Tool::Draw)
      a.setHint(where + std::string("   (drag: move · edges: resize / trim") + (hc->kind == ClipKind::Audio ? (st.stretchResize ? " (stretching)" : "") : "") +
                " · Shift+drag: copy · Ctrl+Shift+drag: slip content · double-click: " +
                (hc->kind == ClipKind::Audio ? "channel settings" : hc->kind == ClipKind::Automation ? "edit" : "open") + " · right-click: delete)");
    else
      a.setHint(where + "   (wheel: scroll · Shift+wheel: horizontal · Ctrl+wheel: zoom · Alt+wheel: track height)");
  } else if (hovered && inRuler) {
    ImGui::SetMouseCursor(hoverMarker >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
    if (hoverMarker >= 0)
      a.setHint("Marker \"" + s.markers[static_cast<size_t>(hoverMarker)].name + "\": click to jump here \xC2\xB7 right-click: rename, time signature, action, loop");
    else
      a.setHint("Click or drag to move the song position (snaps to the grid, Alt = free)  \xC2\xB7  Ctrl+drag or right-drag: "
                "time selection (loops playback)  \xC2\xB7  double-click: clear the selection");
  } else if (hovered && inHead) {
    const char* modeHint = trackui::headerHint(a, headModeCtl ? headModeTrack : trackAt(m.y), headModeCtl);
    if (headLed >= 0) a.setHint("Click: mute / unmute the track \xC2\xB7 right-click: solo (mute every other track)");
    else if (headHandle >= 0) a.setHint("Drag up / down to move the track \xC2\xB7 click: track menu");
    else if (*modeHint) a.setHint(modeHint);  // PL-08
    else a.setHint("Double-click to rename \xC2\xB7 right-click: track menu \xC2\xB7 drag the bottom edge to resize");
  }

  // Automation point edits: one undo step per gesture.
  auto editAuto = [&](ObjectId aid, const std::function<void(AutomationClip&)>& fn) {
    if (!cg.grouped) {
      a.doc.beginGroup("edit automation");
      cg.grouped = true;
    }
    a.doc.editSong("edit automation", [&](Song& x) {
      if (AutomationClip* q = x.automation(aid)) fn(*q);
    });
  };
  auto group = [&](const char* label) {  // the gesture's edits become one undo step
    if (!cg.grouped) {
      a.doc.beginGroup(label);
      cg.grouped = true;
    }
  };

  bool lClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool rClick = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  {  // scrollers: drag the thumbs, click the rail to page
    if (barDrag != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) barDrag = 0;
    if (barDrag == 1 && totalH > gridH) {
      const float t = (m.y - barGrab - vb0.y) / std::max(1.0f, vb1.y - vb0.y - vThumbH);
      st.scrollY = std::clamp(t, 0.0f, 1.0f) * (totalH - gridH);
    } else if (barDrag == 2 && contentW > gridW) {
      const float t = (m.x - barGrab - hb0.x) / std::max(1.0f, hb1.x - hb0.x - hThumbW);
      st.scrollX = std::clamp(t, 0.0f, 1.0f) * (contentW - gridW);
    }
    if (lClick && overV) {
      if (m.y >= vThumbY && m.y < vThumbY + vThumbH) barDrag = 1, barGrab = m.y - vThumbY;
      else st.scrollY += (m.y < vThumbY ? -gridH : gridH) * 0.9f;
      lClick = false;
    } else if (lClick && overH) {
      if (m.x >= hThumbX && m.x < hThumbX + hThumbW) barDrag = 2, barGrab = m.x - hThumbX;
      else st.scrollX = std::max(0.0f, st.scrollX + (m.x < hThumbX ? -gridW : gridW) * 0.9f);
      lClick = false;
    }
    if (barDrag != 0) lClick = false;
  }
  if ((lClick || rClick) && hoverCorner >= 0) {  // header corner buttons
    switch (hoverCorner) {
      case 0: st.stretchResize = !st.stretchResize; break;
      case 1: st.showFades = !st.showFades; break;
      case 2: st.zeroCross = !st.zeroCross; break;
      case 3: st.pickerOpen = !st.pickerOpen; break;
      case 4: ImGui::OpenPopup("##addtracks"); break;
      case 5:
        st.selected.clear();
        st.selStart = st.selEnd = 0;
        break;
      case 6: st.wideHeaders = !st.wideHeaders; break;
      default: break;
    }
    lClick = rClick = false;
  }
  {  // Track resize: drag the bottom edge of a track header (one undo step per drag).
    static int resizing = -1;
    static float resizeY0 = 0, resizeH0 = 1;
    if (resizing >= 0 && resizing < nTracks) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float nh = std::clamp(resizeH0 + (m.y - resizeY0) / std::max(1.0f, trackH), 0.25f, 4.0f);
        if (std::fabs(nh - s.tracks[static_cast<size_t>(resizing)].height) > 0.01f)
          a.doc.editPlaylistTrack(resizing, "resize playlist track", [nh](PlaylistTrack& t) { t.height = nh; });
      } else {
        a.doc.endGroup();
        resizing = -1;
      }
      lClick = false;
    } else if (headResize >= 0 && headLed < 0) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      a.setHint("Drag to resize the track");
      if (lClick) {
        resizing = headResize;
        resizeY0 = m.y;
        resizeH0 = s.tracks[static_cast<size_t>(headResize)].height;
        a.doc.beginGroup("resize playlist track");
        lClick = false;
      }
    }
  }
  {  // Track reorder: drag the "..." handle (one undo step); a click opens the track menu.
    static bool dragMoved = false;
    if (dragTrack >= 0) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const int to = trackAt(std::clamp(m.y, gMin.y, gMax.y - 1));
        if (to != dragTrack && to >= 0 && to < nTracks) {
          const int from = dragTrack;
          a.doc.editSong("move playlist track", [from, to](Song& x) { pltools::moveTrack(x, from, to); });
          dragTrack = to;
          dragMoved = true;
        }
      } else {
        a.doc.endGroup();
        if (!dragMoved) {
          renaming = dragTrack;
          ImGui::OpenPopup("##trmenu");
        }
        dragTrack = -1;
      }
      lClick = false;
    } else if (lClick && headHandle >= 0) {
      dragTrack = headHandle;
      dragMoved = false;
      a.doc.beginGroup("move playlist track");
      lClick = false;
    }
  }
  if (inHead && headModeCtl && lClick) {  // PL-08: arm / monitoring / input / instrument channel
    if (headModeCtl == 3) inputTrack = headModeTrack;
    trackui::headerClick(a, headModeTrack, headModeCtl);
    lClick = false;
  }
  if (inHead && headLed >= 0 && (lClick || rClick)) {  // mute switch; right-click: solo
    if (lClick) {
      a.doc.editPlaylistTrack(headLed, "mute playlist track", [](PlaylistTrack& t) { t.muted = !t.muted; });
    } else {
      bool solo = !s.tracks[static_cast<size_t>(headLed)].muted;
      for (int k = 0; k < nTracks && solo; ++k) solo = k == headLed || s.tracks[static_cast<size_t>(k)].muted;
      const int keep = headLed;
      a.doc.editSong(solo ? "unsolo playlist track" : "solo playlist track", [keep, solo](Song& x) {
        for (size_t k = 0; k < x.tracks.size(); ++k) x.tracks[k].muted = solo ? false : static_cast<int>(k) != keep;
      });
    }
    lClick = rClick = false;
  }
  // Live mode: clicking a clip of the live area launches it (held = gate pads).
  static ObjectId liveHeld = kNoId;
  if (liveMode && lClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.launchClip(hc->id);
    liveHeld = hc->id;
    lClick = false;
  } else if (liveMode && rClick && inGrid && hc && inLiveArea(s, *hc)) {
    a.engine.stopLiveTrack(hc->track);
    rClick = false;
  }
  if (liveHeld != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(liveHeld);
    liveHeld = kNoId;
  }
  // Group fold arrow in the track header.
  if (lClick && inHead) {
    int tr = trackAt(m.y);
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
    float ax = headX0 + 2 + 8 + 4 + 8.0f * static_cast<float>(std::min(groupDepth[static_cast<size_t>(tr)], 3));
    if (t.groupSize > 0 && m.x >= ax - 6 && m.x < ax + 8 && m.y < yOf(tr) + 20) {
      a.doc.editPlaylistTrack(tr, "fold track group", [](PlaylistTrack& x) { x.folded = !x.folded; });
      lClick = false;
    }
  }
  // J-11: locked tracks keep their clips (only selection, play, zoom and menus work there).
  std::vector<char> lockedTracks(static_cast<size_t>(nTracks), 0);
  for (int k = 0; k < nTracks; ++k) lockedTracks[static_cast<size_t>(k)] = s.tracks[static_cast<size_t>(k)].locked ? 1 : 0;
  auto trackLocked = [&](int tr) { return tr >= 0 && tr < nTracks && lockedTracks[static_cast<size_t>(tr)]; };
  const bool editTool = st.tool != Tool::Select && st.tool != Tool::Play && st.tool != Tool::Zoom && !io.KeyCtrl;
  if (inGrid && editTool && trackLocked(trackAt(m.y))) {
    if (hovered) a.setHint("This track is locked (track menu \xE2\x86\x92 Lock)");
    if (lClick && hoverMenuGlyph == kNoId) lClick = false;
    if (rClick && !(hc && m.y < yOf(hc->track) + std::min(16.0f, (hOf(hc->track) - 4) * 0.45f) + 2)) rClick = false;
  }
  // Fade handle drags (J-8; J-38: tension, crossfades): one undo step per drag.
  // Dragging a length stores the fade on the clip (it stops following the
  // overlap); dragging a ring bends the curve, mirrored onto the crossfade
  // partner so the pair keeps its level.
  static ObjectId fadeMenuClip = kNoId;
  static int fadeMenuIn = 1;
  const uint64_t fadeKey = 0x46414445ull ^ (static_cast<uint64_t>(fadeDragClip) << 8);
  if (fadeDragClip != kNoId) {
    const Clip* fc = s.clip(fadeDragClip);
    const int fi = s.clipIndex(fadeDragClip);
    if (fc && fi >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ClipFades fd = static_cast<size_t>(fi) < ccache.fades.size() ? ccache.fades[static_cast<size_t>(fi)] : ClipFades{};
      const int kind = fadeDragKind;
      const ObjectId id = fadeDragClip, mirror = fadeDragMirror;
      char b[96];
      if (kind <= 2) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const double sec0 = tempoMap.secondsAt(static_cast<double>(fc->start));
        const double dur = tempoMap.secondsAt(static_cast<double>(fc->start + s.clipLength(*fc))) - sec0;
        const double at = std::clamp(tempoMap.secondsAt(static_cast<double>(std::max<Tick>(0, tickAt(m.x)))) - sec0, 0.0, dur);
        const double v = std::max(0.0, kind == 1 ? std::min(at, dur - fd.out.len) : std::min(dur - at, dur - fd.in.len));
        a.doc.editClip(id, kind == 1 ? "clip fade in" : "clip fade out", [kind, v](Clip& x) {
          if (kind == 1) x.fadeIn = v, x.fadeInManual = true;
          else x.fadeOut = v, x.fadeOutManual = true;
        }, fadeKey);
        std::snprintf(b, sizeof b, "Fade %s: %.0f ms (set by hand; right-click the handle: automatic, curve)", kind == 1 ? "in" : "out", v * 1000.0);
      } else {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        // J-43: drag up = louder (FL's positive tension); the handle moves on FL's
        // logarithmic tension scale, with a detent at the plain curve.
        const float t = std::clamp(fadeDragT0 - (m.y - fadeDragY0) / 70.0f, -1.0f, 1.0f);
        const float uq = std::fabs(t) < 0.03f ? 0.0f : t;
        const float tq = fadeTensionFromUi(uq);
        a.doc.editClips("fade curve", [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (x.id == id) (kind == 3 ? x.fadeInTension : x.fadeOutTension) = tq;
            if (mirror != kNoId && x.id == mirror) (kind == 3 ? x.fadeOutTension : x.fadeInTension) = -tq;
          }
        }, fadeKey);
        std::snprintf(b, sizeof b, "Fade curve: %+.0f %%%s", uq * 100.0f, mirror != kNoId ? "  (crossfade: the other clip mirrors it)" : "");
      }
      a.setHint(b);
    } else {
      a.doc.endGesture();
      fadeDragClip = kNoId;
    }
    lClick = false;
  } else if (fadeHoverClip != kNoId) {
    ImGui::SetMouseCursor(fadeHoverKind <= 2 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    a.setHint(fadeHoverKind <= 2 ? "Drag: fade length (sets it by hand) \xC2\xB7 right-click: fade menu (automatic crossfade, curve shape)"
                                 : "Drag up / down: fade curve \xC2\xB7 double-click: plain curve \xC2\xB7 right-click: fade menu");
    if (lClick) {
      const Clip* hcf = s.clip(fadeHoverClip);
      if (fadeHoverKind >= 3 && hcf && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const ObjectId id = fadeHoverClip, mirror = fadeHoverMirror;
        const int kind = fadeHoverKind;
        a.doc.editClips("plain fade curve", [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (x.id == id) (kind == 3 ? x.fadeInTension : x.fadeOutTension) = 0;
            if (mirror != kNoId && x.id == mirror) (kind == 3 ? x.fadeOutTension : x.fadeInTension) = 0;
          }
        });
      } else {
        fadeDragClip = fadeHoverClip;
        fadeDragKind = fadeHoverKind;
        fadeDragMirror = fadeHoverMirror;
        fadeDragY0 = m.y;
        fadeDragT0 = hcf ? fadeTensionToUi(fadeHoverKind == 3 ? hcf->fadeInTension : hcf->fadeOutTension) : 0.0f;
      }
      lClick = false;
    }
    if (rClick) {
      fadeMenuClip = fadeHoverClip;
      fadeMenuIn = fadeHoverKind == 1 || fadeHoverKind == 3;
      ImGui::OpenPopup("##fademenu");
      rClick = false;
    }
  }
  if (ImGui::BeginPopup("##fademenu")) {
    const Clip* fc = s.clip(fadeMenuClip);
    const int fi = s.clipIndex(fadeMenuClip);
    if (!fc || fi < 0 || static_cast<size_t>(fi) >= ccache.fades.size()) {
      ImGui::CloseCurrentPopup();
    } else {
      const bool in = fadeMenuIn != 0;
      const EdgeFade e = in ? ccache.fades[static_cast<size_t>(fi)].in : ccache.fades[static_cast<size_t>(fi)].out;
      const ObjectId id = fc->id, partner = e.crossfade ? e.partner : kNoId;
      // The partner edge is tied when it fades over the same span (both follow the overlap).
      bool tied = false;
      if (partner != kNoId)
        if (const int pi = s.clipIndex(partner); pi >= 0 && static_cast<size_t>(pi) < ccache.fades.size()) {
          const EdgeFade& o = in ? ccache.fades[static_cast<size_t>(pi)].out : ccache.fades[static_cast<size_t>(pi)].in;
          tied = o.len > 0 && std::fabs(o.len - e.len) < 1e-6;
        }
      auto editEdge = [&](const char* label, const std::function<void(Clip&, bool /*in*/)>& fn, bool alsoPartner) {
        a.doc.editClips(label, [&](std::vector<Clip>& v) {
          for (Clip& x : v) {
            if (x.id == id) fn(x, in);
            if (alsoPartner && partner != kNoId && x.id == partner) fn(x, !in);
          }
        });
      };
      ImGui::TextDisabled("%s", in ? "Fade in" : "Fade out");
      if (e.crossfade && ImGui::MenuItem(i18n::tr("Automatic crossfade"), nullptr, e.automatic, s.autoCrossfades)) {
        if (e.automatic)  // keep the current length, set by hand from now on
          editEdge("fade set by hand", [len = e.len](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = len, (i ? x.fadeInManual : x.fadeOutManual) = true; }, false);
        else
          editEdge("automatic crossfade", [](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = 0, (i ? x.fadeInManual : x.fadeOutManual) = false; }, false);
      }
      if (!e.crossfade || !s.autoCrossfades) ui::Hint(!e.crossfade ? "Overlap another audio clip on the track to crossfade with it" : "Automatic crossfades are off for this project (playlist menu)");
      if (ImGui::MenuItem(i18n::tr("No fade"), nullptr, e.len <= 0))
        editEdge("remove fade", [](Clip& x, bool i) { (i ? x.fadeIn : x.fadeOut) = 0, (i ? x.fadeInManual : x.fadeOutManual) = true; }, false);
      if (ImGui::MenuItem(i18n::tr("Reset")))
        editEdge("reset fade", [](Clip& x, bool i) {
          if (i) x.fadeIn = 0, x.fadeInManual = false, x.fadeInShape = 0, x.fadeInTension = 0;
          else x.fadeOut = 0, x.fadeOutManual = false, x.fadeOutShape = 0, x.fadeOutTension = 0;
        }, false);
      ui::Hint("Back to the defaults: automatic length (a crossfade where the clip overlaps another), smooth curve, no bend");
      ImGui::Separator();
      ImGui::TextDisabled("%s", tied ? "Crossfade curve (both clips)" : "Curve");
      const uint8_t cur = in ? fc->fadeInShape : fc->fadeOutShape;
      for (int k = 0; k < kFadeShapeCount; ++k) {
        std::string label = fadeShapeName(static_cast<FadeShape>(k));
        if (k == 0) label += "  (default)";
        if (ImGui::MenuItem(label.c_str(), nullptr, (cur == 1 ? 1 : 0) == k))
          editEdge("fade curve shape", [k](Clip& x, bool i) { (i ? x.fadeInShape : x.fadeOutShape) = static_cast<uint8_t>(k); }, tied);
      }
      if (ImGui::MenuItem(i18n::tr("Plain curve (no bend)"), nullptr, false, e.tension != 0.0f))
        editEdge("plain fade curve", [](Clip& x, bool i) { (i ? x.fadeInTension : x.fadeOutTension) = 0; }, tied);
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Automatic crossfades in this project"), nullptr, s.autoCrossfades))
        a.doc.editSong(s.autoCrossfades ? "automatic crossfades off" : "automatic crossfades on", [](Song& x) { x.autoCrossfades = !x.autoCrossfades; });
    }
    ImGui::EndPopup();
  }
  static int menuMarker = -1;
  if (inRuler && rClick && hoverMarker >= 0) {  // right-click a marker: its menu
    menuMarker = hoverMarker;
    ImGui::OpenPopup("##markmenu");
    rClick = false;
  }
  if (inGrid && (lClick || rClick) && hoverMenuGlyph != kNoId) {  // the clip's title glyph: clip menu
    menuClip = hoverMenuGlyph;
    ImGui::OpenPopup("##clipmenu");
    lClick = rClick = false;
  }
  const pltools::Source src = currentSource(a);
  if (lClick || rClick) {
    cg = ClipGesture{};
    cg.start = m;
    cg.startTick = tickAt(m.x);
    cg.startTrack = trackAt(m.y);
    cg.base = s.playlist;
    cg.anchor = hoverClip;
    cg.key = 0x504C000000000000ull ^ (clipGestureCounter++ << 8);
    if (inRuler && lClick && hoverMarker >= 0) {
      if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);  // J-49: no rewind when already in song mode
      j31::songJump(a, s.markers[static_cast<size_t>(hoverMarker)].pos);  // jump to the marker (J-31: quantized when the option is on)
    } else if (inRuler && lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      st.selStart = st.selEnd = 0;  // double-click clears the time selection
    } else if (inRuler && (rClick || (lClick && io.KeyCtrl))) {
      cg.kind = ClipGesture::TimeSel;
    } else if (inRuler && lClick) {
      if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);  // J-49: no rewind when already in song mode
      cg.kind = ClipGesture::Scrub;  // the position follows the pointer until release
      cg.lastScrub = std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      j31::songJump(a, cg.lastScrub);  // J-31: a click while playing waits for the jump grid
    } else if (inHead) {
      int tr = trackAt(m.y);
      if (lClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        renaming = tr;
        ImGui::OpenPopup("##trname");
      } else if (rClick) {
        renaming = tr;
        ImGui::OpenPopup("##trmenu");
      }
    } else if (inGrid && hoverAuto && rClick && hoverPoint >= 0) {
      const AutomationClip* ac = s.automation(hc->source);
      if (ac && ac->points.size() > 1) {  // right-click a point: delete it
        int k = hoverPoint;
        ObjectId aid = ac->id;
        cg.anchor = kNoId;
        a.doc.editSong("delete automation point", [aid, k](Song& x) {
          AutomationClip* q = x.automation(aid);
          if (q && k >= 0 && static_cast<size_t>(k) < q->points.size()) q->points.erase(q->points.begin() + k);
        });
      }
    } else if (inGrid && rClick && hc && m.y < yOf(hc->track) + std::min(16.0f, (hOf(hc->track) - 4) * 0.45f) + 2 && st.tool != Tool::Erase &&
               st.tool != Tool::Paint) {
      menuClip = hoverClip;  // right-click on the clip's title bar: clip menu
      ImGui::OpenPopup("##clipmenu");
    } else if (inGrid && rClick && st.tool == Tool::Zoom) {
      st.zoomRequest /= 1.5f;
    } else if (inGrid && rClick) {
      cg.kind = ClipGesture::Erase;  // right-drag deletes with every editing tool (like FL)
      group("delete clips");
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Mute) {
      cg.kind = ClipGesture::Mute;
      if (hc) cg.muteTo = hc->muted ? 0 : 1;
    } else if (inGrid && lClick && hc && ((!io.KeyCtrl && st.tool == Tool::Slip) || (st.tool == Tool::Draw && slipDrag))) {
      cg.kind = ClipGesture::Slip;  // every clip kind (audio: its sample position)
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Slice) {
      cg.kind = ClipGesture::SliceLine;  // cut on release: the clicked clip, or every clip the vertical line crossed
      cg.startTick = std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Play) {
      a.engine.setMode(PlayMode::Song);
      a.engine.setPosition(std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line)));
      if (!a.engine.playing()) a.engine.play();
      cg.kind = ClipGesture::Play;
    } else if (inGrid && lClick && !io.KeyCtrl && st.tool == Tool::Zoom) {
      cg.kind = ClipGesture::Zoom;
    } else if (inGrid && lClick && hoverAuto && st.tool == Tool::Draw && !io.KeyCtrl && !io.KeyShift && (hoverPoint >= 0 || hoverHandle >= 0)) {
      const AutomationClip* ac = s.automation(hc->source);
      cg.autoId = ac->id;
      if (hoverPoint >= 0) {
        cg.kind = ClipGesture::AutoPoint;
        cg.point = hoverPoint;
      } else {
        cg.kind = ClipGesture::AutoTension;
        cg.point = hoverHandle;
        cg.baseTension = ac->points[static_cast<size_t>(hoverHandle)].tension;
      }
    } else if (inGrid && lClick && hoverAuto && st.tool == Tool::Draw && !io.KeyCtrl && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      // Double-click on the curve: add a point there and keep dragging it.
      const AutomationClip* ac = s.automation(hc->source);
      const float headH = std::min(16.0f, (hOf(hc->track) - 4) * 0.45f);
      AutoGeom g{ImVec2(xOf(hc->start) + 4, yOf(hc->track) + 2 + headH + 2), ImVec2(xOf(hc->start + s.clipLength(*hc)) - 4, yOf(hc->track) + hOf(hc->track) - 5),
                 xOf(hc->start), st.pxPerTick, hc->start, hc->offset, s.clipLength(*hc)};
      Tick pos = hc->offset + std::max<Tick>(0, snapPos(tickAt(m.x)) - hc->start);
      AutoPoint np{pos, static_cast<float>(g.valueAtY(m.y)), 0.0f, AutoShape::Curve};
      ObjectId aid = ac->id;
      int idx = 0;
      cg.autoId = aid;
      editAuto(aid, [&](AutomationClip& q) { idx = insertAutoPoint(q, np); });
      cg.kind = ClipGesture::AutoPoint;
      cg.point = idx;
    } else if (inGrid && lClick) {
      if (hoverClip != kNoId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (hc->kind == ClipKind::Pattern) {
          a.selectPattern(hc->pattern);
          a.showRack = true;
          a.focusWindow = "Channel rack";
        } else if (hc->kind == ClipKind::Audio) {
          a.settingsChannel = hc->source;  // the channel that plays the clip (its sample, stretch, ...)
        } else if (hc->kind == ClipKind::Automation) {
          openAutomationEditor(a, hc->source, hc->id);  // J-5: automation clip editor
        }
      } else if (io.KeyCtrl || st.tool == Tool::Select) {
        cg.kind = ClipGesture::Select;
        if (!io.KeyShift) st.selected.clear();
      } else if (st.tool == Tool::Erase) {
        cg.kind = ClipGesture::Erase;
        group("delete clips");
      } else if (st.tool == Tool::Paint) {
        if (hc) {  // painting starts from a clip: it becomes the source
          setSource(a, pltools::sourceOf(*hc));
        }
        const pltools::Source ps = currentSource(a);
        if (pltools::sourceValid(s, ps)) {
          cg.kind = ClipGesture::Paint;
          cg.paintKind = ps.kind;
          cg.paintId = ps.id;
          cg.paintLen = hc ? s.clipLength(*hc) : pltools::sourceLength(s, ps);
          cg.paintAnchor = hc ? hc->start : std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
          group("paint clips");
        }
      } else if (hoverClip != kNoId) {
        if (!st.selected.count(hoverClip)) {
          if (!io.KeyShift || shiftDrag) st.selected.clear();
          st.selected.insert(hoverClip);
          st.selected = ops::expandClipGroups(s, st.selected);  // J-31: grouped clips are selected together
        }
        setSource(a, pltools::sourceOf(*hc));  // clicking a clip makes its source current (FL)
        cg.kind = hoverEdge ? ClipGesture::Resize : hoverLeftEdge ? ClipGesture::Trim : ClipGesture::Move;
        if (cg.kind == ClipGesture::Move && shiftDrag && !trackLocked(hc->track)) {
          // Shift+drag: copies of the selection are dragged, the originals stay.
          group("copy clips");
          std::vector<ObjectId> copies;
          const std::set<ObjectId> selNow = st.selected;
          a.doc.editSong("copy clips", [&](Song& x) { copies = pltools::cloneClips(x, selNow); });
          std::set<ObjectId> cs(copies.begin(), copies.end());
          // The anchor's copy is the one under the pointer.
          size_t k = 0;
          for (const auto& c : s.playlist)
            if (selNow.count(c.id)) {
              if (c.id == hoverClip && k < copies.size()) cg.anchor = copies[k];
              ++k;
            }
          st.selected = cs;
          cg.base = a.doc.song().playlist;
          cg.moved = true;
        }
      } else if (pltools::sourceValid(s, src) && !trackmodes::clipAllowed(s, pltools::makeClip(s, src, trackAt(m.y), 0).kind, trackAt(m.y))) {
        a.notify("Instrument tracks take pattern clips only", true);  // PL-08
      } else if (pltools::sourceValid(s, src)) {
        const Tick at = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
        const int tr = trackAt(m.y);
        ObjectId id = kNoId;
        group("add clip");
        a.doc.editSong("add clip", [&](Song& x) {
          Clip c = pltools::makeClip(x, src, tr, at);
          c.id = x.allocateId();
          x.playlist.push_back(c);
          id = c.id;
        });
        if (id) {
          st.selected = {id};
          cg.base = a.doc.song().playlist;
          cg.anchor = id;
          cg.startTick = at;
          cg.kind = ClipGesture::Move;
        }
      }
    }
  }

  if ((lClick || rClick) && gestureAnimates(cg.kind) && cg.kind != ClipGesture::Paint && a.settings.getBool("ui.smoothSnap")) {
    // J-37: the clips this drag changes start gliding from where they are drawn.
    const Song& ns = a.doc.song();
    const double now = ImGui::GetTime();
    for (const auto& c : ns.playlist)
      if (cg.kind == ClipGesture::Slip ? c.id == cg.anchor : st.selected.count(c.id) > 0) clipAnim.seed(c.id, clipAnimValues(c, ns.clipLength(c)), now);
  }
  if (active && cg.kind != ClipGesture::None) {
    // Dragging past the left/right edge scrolls the view (scrub, time selection, clips).
    if (cg.kind != ClipGesture::Select && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) + ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f)) {
      const float edge = 24.0f;
      if (m.x > gMax.x - edge) st.scrollX += (m.x - (gMax.x - edge)) * 0.35f;
      if (m.x < gMin.x + edge) st.scrollX = std::max(0.0f, st.scrollX - ((gMin.x + edge) - m.x) * 0.35f);
    }
    Tick curTick = std::max<Tick>(0, tickAt(std::clamp(m.x, gMin.x, gMax.x)));
    int curTrack = trackAt(m.y);
    auto baseClip = [&]() -> const Clip* {
      for (const auto& c : cg.base)
        if (c.id == cg.anchor) return &c;
      return nullptr;
    };
    auto applyClips = [&](const char* label, const std::vector<Clip>& after) {
      a.doc.editClips(label, [&](std::vector<Clip>& cs) {
        if (cs.size() == after.size()) cs = after;
      }, cg.key);
    };
    switch (cg.kind) {
      case ClipGesture::Scrub: {
        // Smooth: follows the pointer every frame, landing on the snap grid (Alt = free).
        Tick t = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
        if (t != cg.lastScrub) {
          j31::songJump(a, t);  // J-31: immediate when stopped, quantized while playing
          cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::TimeSel: {
        Tick t0 = std::max<Tick>(0, timeline::snapTick(cg.startTick, snapG, Snap::Line));
        Tick t1 = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
        st.selStart = std::min(t0, t1);
        st.selEnd = std::max(t0, t1);
        break;
      }
      case ClipGesture::Move: {
        const Clip* an = baseClip();
        if (!an) break;
        Tick target = snapPos(an->start + (curTick - cg.startTick));
        const Tick dt = target - an->start;
        const int dtr = curTrack - cg.startTrack;
        if (dt == 0 && dtr == 0 && !cg.moved) break;
        cg.moved = true;
        {
          std::vector<Clip> moved = pltools::moveClips(cg.base, st.selected, dt, dtr, nTracks, lockedTracks);
          bool allowed = true;  // PL-08: instrument tracks take pattern clips only
          for (const auto& c : moved)
            if (st.selected.count(c.id) && !trackmodes::clipAllowed(s, c.kind, c.track)) allowed = false;
          if (allowed) applyClips("move clips", std::move(moved));
          else a.setHint("Instrument tracks take pattern clips only");
        }
        break;
      }
      case ClipGesture::Resize: {
        const Clip* an = baseClip();
        if (!an) break;
        const Tick oldEnd = an->start + s.clipLength(*an);
        const Tick end = std::max(timeline::snapTick(curTick, snapG, Snap::Line), an->start + 1);
        const Tick d = end - oldEnd;
        if (d == 0 && !cg.moved) break;
        cg.moved = true;
        applyClips("resize clips", pltools::resizeEnds(a.doc.song(), cg.base, st.selected, d, st.stretchResize));
        break;
      }
      case ClipGesture::Trim: {  // left edge: later start, content stays in place
        const Clip* an = baseClip();
        if (!an) break;
        const Tick d = timeline::snapTick(curTick, snapG, Snap::Line) - an->start;
        if (d == 0 && !cg.moved) break;
        cg.moved = true;
        applyClips("trim clips", pltools::trimStarts(a.doc.song(), tempoMap, cg.base, st.selected, d, st.stretchResize, st.zeroCross));
        break;
      }
      case ClipGesture::AutoPoint: {
        const Clip* an = baseClip();
        const AutomationClip* ac = s.automation(cg.autoId);
        if (!an || !ac || cg.point < 0 || cg.point >= static_cast<int>(ac->points.size())) break;
        const Clip* live = s.clip(an->id);
        if (!live) break;
        Tick len = s.clipLength(*live);
        const float headH = std::min(16.0f, (hOf(live->track) - 4) * 0.45f);
        AutoGeom g{ImVec2(0, yOf(live->track) + 2 + headH + 2), ImVec2(0, yOf(live->track) + hOf(live->track) - 5), xOf(live->start),
                   st.pxPerTick, live->start, live->offset, len};
        int k = cg.point;
        Tick lo = k > 0 ? ac->points[static_cast<size_t>(k) - 1].pos : 0;
        Tick hi = k + 1 < static_cast<int>(ac->points.size()) ? ac->points[static_cast<size_t>(k) + 1].pos : std::numeric_limits<Tick>::max() / 4;
        Tick pos = std::clamp(live->offset + snapPos(curTick) - live->start, lo, hi);
        if (k == 0 && ac->points.size() > 1) pos = std::min<Tick>(pos, ac->points[1].pos);
        float val = static_cast<float>(g.valueAtY(m.y));
        const AutoPoint& p = ac->points[static_cast<size_t>(k)];
        if (pos == p.pos && val == p.value) break;
        editAuto(cg.autoId, [&](AutomationClip& q) {
          q.points[static_cast<size_t>(k)].pos = pos;
          q.points[static_cast<size_t>(k)].value = val;
        });
        break;
      }
      case ClipGesture::AutoTension: {
        const AutomationClip* ac = s.automation(cg.autoId);
        int k = cg.point;
        if (!ac || k < 1 || k >= static_cast<int>(ac->points.size())) break;
        float rise = ac->points[static_cast<size_t>(k)].value >= ac->points[static_cast<size_t>(k) - 1].value ? 1.0f : -1.0f;
        float t = std::clamp(cg.baseTension + (m.y - cg.start.y) / 120.0f * rise, -1.0f, 1.0f);
        if (t == ac->points[static_cast<size_t>(k)].tension) break;
        editAuto(cg.autoId, [&](AutomationClip& q) { q.points[static_cast<size_t>(k)].tension = t; });
        break;
      }
      case ClipGesture::Select: {
        ImVec2 lo(std::min(cg.start.x, m.x), std::min(cg.start.y, m.y)), hi(std::max(cg.start.x, m.x), std::max(cg.start.y, m.y));
        std::set<ObjectId> sel = io.KeyShift ? st.selected : std::set<ObjectId>{};
        for (const auto& c : s.playlist) {
          float x0 = xOf(c.start), x1 = xOf(c.start + s.clipLength(c)), y = yOf(c.track);
          if (hOf(c.track) >= 1 && x1 >= lo.x && x0 <= hi.x && y + hOf(c.track) >= lo.y && y <= hi.y) sel.insert(c.id);
        }
        st.selected = sel;
        break;
      }
      case ClipGesture::Paint: {
        const pltools::Source ps{cg.paintKind, cg.paintId};
        if (!pltools::sourceValid(s, ps) || cg.paintLen <= 0 || trackLocked(curTrack)) break;
        if (!trackmodes::clipAllowed(s, pltools::makeClip(s, ps, curTrack, 0).kind, curTrack)) break;  // PL-08
        std::vector<Tick> todo;
        for (Tick t : pltools::paintSlots(cg.paintAnchor, curTick, cg.paintLen))
          if (!pltools::occupied(s, curTrack, t, cg.paintLen)) todo.push_back(t);
        if (todo.empty()) break;
        const Tick len = cg.paintLen;
        std::vector<Clip> painted;
        a.doc.editSong("paint clips", [&](Song& x) {
          for (Tick t : todo) {
            Clip c = pltools::makeClip(x, ps, curTrack, t);
            if (c.kind != ClipKind::Pattern || len != x.clipLength(c)) c.length = len;
            c.id = x.allocateId();
            x.playlist.push_back(c);
            painted.push_back(c);
          }
        });
        if (a.settings.getBool("ui.smoothSnap") && snapG > 1)  // J-37: painted clips grow in from their start
          for (const Clip& c : painted) clipAnim.seed(c.id, clipAnimValues(c, 0), ImGui::GetTime());
        break;
      }
      case ClipGesture::Erase:
        if (hoverClip != kNoId && !trackLocked(s.clip(hoverClip)->track)) {
          st.selected.erase(hoverClip);
          a.doc.removeClip(hoverClip);
        }
        break;
      case ClipGesture::Mute: {  // every touched clip gets the first clip's new state
        if (hoverClip == kNoId || trackLocked(s.clip(hoverClip)->track)) break;
        const Clip* c = s.clip(hoverClip);
        if (cg.muteTo < 0) cg.muteTo = c->muted ? 0 : 1;
        if (c->muted == (cg.muteTo == 1)) break;
        const bool to = cg.muteTo == 1;
        a.doc.editClip(hoverClip, to ? "mute clips" : "unmute clips", [to](Clip& k) { k.muted = to; }, cg.key);
        break;
      }
      case ClipGesture::Slip: {  // content moves inside the clip (every kind)
        const Clip* an = baseClip();
        if (!an || trackLocked(an->track)) break;
        const Tick d = timeline::snapTick(curTick, snapG, Snap::Line) - timeline::snapTick(cg.startTick, snapG, Snap::Line);
        const Clip after = pltools::slipClip(a.doc.song(), tempoMap, *an, d);
        if (const Clip* live = s.clip(an->id); live && !(after == *live)) {
          a.doc.editClip(an->id, "slip clip content", [&after](Clip& k) { k = after; }, cg.key);
          if (after.kind == ClipKind::Audio) {
            char b[64];
            std::snprintf(b, sizeof b, "Audio starts at %.3f s", after.audioStart);
            a.setHint(b);
          } else {
            a.setHint("Content offset " + timeline::rulerLabel(s, after.offset, std::max<Tick>(1, s.ppq / 4)));
          }
        }
        break;
      }
      case ClipGesture::Play: {  // dragging moves the playback start
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
          Tick t = std::max<Tick>(0, timeline::snapTick(curTick, snapG, Snap::Line));
          if (t != cg.lastScrub) a.engine.setPosition(t), cg.lastScrub = t;
        }
        break;
      }
      case ClipGesture::Zoom: break;       // applied on release
      case ClipGesture::SliceLine: break;  // applied on release
      default: break;
    }
  }
  if (ImGui::IsItemDeactivated()) {
    if (cg.kind == ClipGesture::Play) a.engine.pause();  // the play tool plays while held
    if (cg.kind == ClipGesture::Zoom && ImGui::IsMousePosValid(&m)) {  // J-46: released outside the window: no zoom
      if (std::fabs(m.x - cg.start.x) < 4) {
        st.zoomRequest *= 1.5f;
      } else {
        Tick t0 = std::max<Tick>(0, tickAt(std::min(cg.start.x, m.x))), t1 = tickAt(std::max(cg.start.x, m.x));
        if (t1 > t0) {
          st.pxPerTick = static_cast<float>(timeline::clampZoom(s, gridW / static_cast<double>(t1 - t0)));
          st.scrollX = static_cast<float>(t0) * st.pxPerTick;
        }
      }
    }
    const bool regionCut = cg.kind == ClipGesture::SliceLine && std::fabs(m.x - cg.start.x) > 6.0f &&
                           std::fabs(m.x - cg.start.x) > std::fabs(m.y - cg.start.y);
    if (regionCut) {
      // PL-13: a horizontal drag cuts the clips at both ends of the dragged range
      // (Shift: on every track), selects the inside and makes it the time
      // selection, ready for Chop and stutter (Alt+C) or a time selection command.
      const Tick e = std::max<Tick>(0, timeline::snapTick(std::max<Tick>(0, tickAt(m.x)), snapG, Snap::Line));
      const Tick r0 = std::min(cg.startTick, e), r1 = std::max(cg.startTick, e);
      const int t0 = io.KeyShift ? 0 : std::min(cg.startTrack, trackAt(m.y)), t1 = io.KeyShift ? nTracks - 1 : std::max(cg.startTrack, trackAt(m.y));
      std::set<int> trs;
      for (int t = t0; t <= t1; ++t)
        if (!trackLocked(t)) trs.insert(t);
      if (r1 > r0 && !trs.empty()) {
        std::set<ObjectId> inside;
        a.doc.editSong("slice clips", [&](Song& x) { inside = chop::isolateRange(x, r0, r1, trs); });
        st.selected = inside;
        st.selStart = r0, st.selEnd = r1;
      }
    } else if (cg.kind == ClipGesture::SliceLine) {
      // Shift: the whole column; otherwise the tracks the line was dragged over.
      const int t0 = io.KeyShift ? 0 : cg.startTrack, t1 = io.KeyShift ? nTracks - 1 : trackAt(m.y);
      std::vector<ObjectId> ids;
      for (ObjectId id : pltools::clipsAcross(s, cg.startTick, t0, t1))
        if (!trackLocked(s.clip(id)->track)) ids.push_back(id);
      if (!ids.empty()) {
        std::vector<ObjectId> right;
        const Tick at = cg.startTick;
        a.doc.editSong(ids.size() > 1 ? "slice clips" : "slice clip", [&](Song& x) { right = pltools::sliceClips(x, ids, at); });
        st.selected = std::set<ObjectId>(right.begin(), right.end());
      }
    }
    if (cg.kind == ClipGesture::Move && !cg.moved && cg.anchor != kNoId && !io.KeyShift)
      st.selected = ops::expandClipGroups(a.doc.song(), {cg.anchor});  // the clicked clip and its group (J-31)
    if (cg.kind == ClipGesture::TimeSel) {
      if (st.selEnd > st.selStart) {
        a.engine.setMode(PlayMode::Song);
        a.engine.setPosition(st.selStart);  // playback starts at the selection, like FL
      } else {
        st.selStart = st.selEnd = 0;
      }
    }
    if (cg.grouped) a.doc.endGroup();
    a.doc.endGesture();
    cg = ClipGesture{};
  }

  // Drop a clip source from the picker (or a pattern from elsewhere).
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PATTERN")) {
      ObjectId pid = *static_cast<const ObjectId*>(pl->Data);
      ObjectId id = a.doc.addClip(pid, trackAt(m.y), std::max<Tick>(0, snapDown(tickAt(m.x), snapG)));
      if (id) st.selected = {id};
    }
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CLIPSRC")) {
      const ClipSrcPayload ps = *static_cast<const ClipSrcPayload*>(pl->Data);
      const pltools::Source dsrc{static_cast<ClipKind>(ps.kind), ps.id};
      const int tr = trackAt(m.y);
      const Tick at = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
      if (pltools::sourceValid(s, dsrc) && !trackLocked(tr) && !trackmodes::clipAllowed(s, pltools::makeClip(s, dsrc, tr, at).kind, tr)) {
        a.notify("Instrument tracks take pattern clips only", true);  // PL-08
      } else if (pltools::sourceValid(s, dsrc) && !trackLocked(tr)) {
        ObjectId id = kNoId;
        a.doc.editSong("add clip", [&](Song& x) {
          Clip c = pltools::makeClip(x, dsrc, tr, at);
          c.id = x.allocateId();
          x.playlist.push_back(c);
          id = c.id;
        });
        st.selected = {id};
        setSource(a, dsrc);
      }
    }
    // Browser files (J-16): samples become audio clips at the drop point.
    browserui::DropPlace place;
    place.track = trackAt(m.y);
    place.tick = std::max<Tick>(0, snapDown(tickAt(m.x), snapG));
    browserui::acceptFileDrop(a, browse::DropTarget::Playlist, place);
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PLUGIN")) {  // instruments: a new channel
      std::string type(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (const PluginInfo* info = findPlugin(type); info && info->kind == PluginKind::Generator) a.settingsChannel = a.addInstrument(type);
    }
    ImGui::EndDragDropTarget();
  }

  // Add-tracks menu (header corner "+").
  if (ImGui::BeginPopup("##addtracks")) {
    const int n = nTracks;
    if (ImGui::MenuItem(i18n::tr("Add 1 track"), nullptr, false, n < kMaxPlaylistTracks)) a.doc.setPlaylistTrackCount(n + 1);
    if (ImGui::MenuItem(i18n::tr("Add 8 tracks"), nullptr, false, n + 8 <= kMaxPlaylistTracks)) a.doc.setPlaylistTrackCount(n + 8);
    if (ImGui::MenuItem(i18n::tr("Add 32 tracks"), nullptr, false, n + 32 <= kMaxPlaylistTracks)) a.doc.setPlaylistTrackCount(n + 32);
    // PL-08: a new audio track (on the next free mixer track) or instrument track, on the first free playlist track.
    ImGui::SeparatorText(i18n::tr("Track with a mode"));
    const int free = trackmodes::freeTrack(s);
    if (ImGui::MenuItem(i18n::tr("New audio track"), nullptr, false, free >= 0)) {
      std::string why;
      if (!trackmodes::makeAudioTrack(a.doc, free, kNoId, &why)) a.notify(why, true);
    }
    if (ImGui::BeginMenu(i18n::tr("New instrument track"), free >= 0)) {
      for (const auto& info : pluginCatalog()) {
        if (info.kind != PluginKind::Generator) continue;
        if (ImGui::MenuItem(info.name.c_str())) {
          std::string why;
          const ObjectId ch = trackmodes::newInstrumentTrack(a.doc, free, info.type, &why);
          if (ch == kNoId) a.notify(why, true);
          else a.selectChannel(ch);
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  // Track popups.
  if (ImGui::BeginPopup("##trname")) {
    static char buf[128];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof buf, "%s", renaming >= 0 && renaming < nTracks ? s.tracks[static_cast<size_t>(renaming)].name.c_str() : "");
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputText("##n", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::string n = buf;
      a.doc.editPlaylistTrack(renaming, "rename playlist track", [&](PlaylistTrack& t) { t.name = n; });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trmenu")) {
    playlistTrackMenu(a, renaming);
    ImGui::EndPopup();
  }
  trackui::inputPopup(a, inputTrack);  // PL-08
  if (ImGui::BeginPopup("##clipmenu")) {
    arrangementClipMenu(a, menuClip);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##markmenu")) {
    markerMenu(a, menuMarker);
    ImGui::EndPopup();
  }

  // ---------------- shortcuts ----------------
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (keys::pressed("playlist.delete") && !st.selected.empty()) {
      a.doc.beginGroup("delete clips");
      for (ObjectId id : std::set<ObjectId>(st.selected))
        if (const Clip* c = s.clip(id); c && !trackLocked(c->track)) a.doc.removeClip(id);
      a.doc.endGroup();
      st.selected.clear();
    }
    if (keys::pressed("playlist.selectAll")) {
      st.selected.clear();
      for (const auto& c : s.playlist) st.selected.insert(c.id);
    }
    if (keys::pressed("playlist.deselect")) st.selected.clear();
    // Copies keep every clip setting (kind, source, offsets, audio settings).
    auto duplicate = [&](const std::vector<Clip>& src, const char* label) {
      if (src.empty()) return;
      Tick lo = src[0].start, hi = 0;
      for (const auto& c : src) lo = std::min(lo, c.start), hi = std::max(hi, c.start + s.clipLength(c));
      Tick shift = snapNearest(hi - lo, std::max<Tick>(s.barTicks(), 1));
      if (shift <= 0) shift = s.barTicks();
      std::set<ObjectId> added;
      a.doc.editSong(label, [&](Song& x) {
        for (Clip c : src) {
          if (!clipValid(x, c)) continue;
          c.id = x.allocateId();
          c.start += shift;
          x.playlist.push_back(c);
          added.insert(c.id);
        }
      });
      st.selected = added;
    };
    if (keys::pressed("playlist.copy") && !st.selected.empty()) {
      st.clipboard.clear();
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
      a.notify(std::to_string(st.clipboard.size()) + " clips copied");
    }
    if (keys::pressed("playlist.paste") && !st.clipboard.empty()) {
      duplicate(st.clipboard, "paste clips");
      st.clipboard.clear();
      for (const auto& c : a.doc.song().playlist)
        if (st.selected.count(c.id)) st.clipboard.push_back(c);
    }
    chopui::shortcuts(a);  // PL-13: chop window, slice at the grid, time selection edits
    if (keys::pressed("playlist.duplicate") && st.selected.empty() && st.selEnd > st.selStart) chopui::duplicateTime(a);
    if (keys::pressed("playlist.duplicate") && !st.selected.empty()) {
      std::vector<Clip> src;
      for (const auto& c : s.playlist)
        if (st.selected.count(c.id)) src.push_back(c);
      duplicate(src, "duplicate clips");
    }
    if (!a.typingKeyboard) {  // tool keys
      struct K { const char* cmd; Tool tool; };
      const K toolKeys[] = {{"playlist.toolDraw", Tool::Draw}, {"playlist.toolPaint", Tool::Paint}, {"playlist.toolDelete", Tool::Erase},
                            {"playlist.toolMute", Tool::Mute}, {"playlist.toolSlip", Tool::Slip},   {"playlist.toolSlice", Tool::Slice},
                            {"playlist.toolSelect", Tool::Select}, {"playlist.toolZoom", Tool::Zoom}, {"playlist.toolPlay", Tool::Play}};
      for (const auto& k : toolKeys)
        if (keys::pressed(k.cmd)) st.tool = k.tool;
    }
    if (keys::pressed("playlist.marker")) {  // marker at the song position
      ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
    }
    // FL view toggles: Shift+M stretch on resize, Shift+F fade handles, Alt+P picker panel.
    if (keys::pressed("playlist.stretchResize")) {
      st.stretchResize = !st.stretchResize;
      a.notify(st.stretchResize ? "Resizing audio clips stretches them" : "Resizing audio clips trims them");
    }
    if (keys::pressed("playlist.fades")) st.showFades = !st.showFades;
    if (keys::pressed("playlist.picker")) st.pickerOpen = !st.pickerOpen;
    if (keys::pressed("playlist.loopMarkers") && st.selEnd > st.selStart)
      ops::placeLoopMarkers(a.doc, st.selStart, st.selEnd);  // Shift+T: place loop (FL)
    // Quick zooms (Shift+1..3) and zoom on selection (Shift+4).
    if (keys::pressed("playlist.quickZoom1")) st.quickZoom = 1;
    if (keys::pressed("playlist.quickZoom2")) st.quickZoom = 2;
    if (keys::pressed("playlist.quickZoom3")) st.quickZoom = 3;
    if (keys::pressed("playlist.zoomSelection")) st.zoomSelRequest = true;
    // M: mute / unmute the selected clips.
    if (keys::pressed("playlist.mute") && !st.selected.empty()) {
      auto sel = st.selected;
      a.doc.editClips("mute clips", [&](std::vector<Clip>& cs) {
        for (auto& c : cs)
          if (sel.count(c.id)) c.muted = !c.muted;
      });
    }
  }
  ui::EndDockWindow();
  drawClipProperties(a);  // J-8: audio clip properties window
  chopui::draw(a);         // PL-13: chop and stutter window
}

}  // namespace gl
