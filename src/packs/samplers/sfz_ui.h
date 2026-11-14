// Gridline: the Multisampler's SFZ view (K-144). Included by samplers_ui.cpp inside its anonymous
// namespace, after the zone helpers. An SFZ instrument plays as it is; the editor shows its map
// (keys across, velocity up, one colour per group), keyswitches, its labelled controllers and
// what the reader could not play, and converts it to editable zones on request.
#pragma once
#include "../../core/SfzMap.h"

struct SfzView {
  std::string key;
  std::shared_ptr<const gl::sfz::Instrument> ins;
  double checked = -1;
};
std::map<std::string, SfzView> gSfzViews;

// The instrument for the editor, read again when the file changes (checked twice a second).
std::shared_ptr<const gl::sfz::Instrument> sfzForEditor(App& a, const std::string& path) {
  SfzView& v = gSfzViews[path];
  if (v.ins && a.now - v.checked < 0.5) return v.ins;
  v.checked = a.now;
  std::error_code ec;
  const auto t = fs::last_write_time(fs::u8path(path), ec);
  const std::string key = ec ? std::string("-") : std::to_string(static_cast<long long>(t.time_since_epoch().count()));
  if (v.ins && key == v.key) return v.ins;
  auto ins = std::make_shared<gl::sfz::Instrument>();
  gl::sfz::load(path, *ins);
  v.ins = ins;
  v.key = key;
  return v.ins;
}

std::map<uint64_t, int> gSfzView;  // editor -> the keyswitch whose regions the map shows (-1: all)

// One colour per group, by the order groups first appear (neighbours differ by the golden angle).
ImU32 groupColour(const gl::sfz::Instrument& ins, int64_t g, float alpha) {
  int rank = 0;
  std::vector<int64_t> seen;
  for (const auto& r : ins.regions) {
    if (std::find(seen.begin(), seen.end(), r.group) == seen.end()) seen.push_back(r.group);
    if (r.group == g) break;
  }
  rank = static_cast<int>(seen.size()) - 1;
  const float h = std::fmod(0.10f + static_cast<float>(rank) * 0.618034f, 1.0f);
  float r, gg, b;
  ImGui::ColorConvertHSVtoRGB(h, 0.55f, 0.9f, r, gg, b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, gg, b, alpha));
}

float sfzCcValue(const ZoneMap& m, const gl::sfz::Instrument& ins, int cc) {
  for (const auto& [c, v] : m.sfzCc)
    if (c == cc) return v;
  const float s = cc >= 0 && cc < gl::sfz::kCcCount ? ins.setCc[static_cast<size_t>(cc)] : -1.f;
  if (s >= 0) return s;
  return cc == 7 ? 100.f / 127.f : cc == 10 ? 0.5f : cc == 11 ? 1.f : 0.f;
}

// Region map: keys across, velocity up. Click a key strip to hear it.
void sfzRegionMap(App& a, const PluginTarget& t, const gl::sfz::Instrument& ins, const iskin::Skin& k, float x, float y, float w, float h, int view) {
  using namespace gl::sfz;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int lo = 127, hi = 0;
  for (const Region& r : ins.regions) lo = std::min(lo, r.lokey), hi = std::max(hi, r.hikey);
  if (ins.swLokey <= ins.swHikey) lo = std::min(lo, ins.swLokey), hi = std::max(hi, ins.swHikey);
  if (lo > hi) lo = 0, hi = 127;
  lo = std::max(0, lo - 2), hi = std::min(127, hi + 2);
  const int keys = hi - lo + 1;
  const float keyH = 34;
  const ImVec2 p0 = k.at(x, y), p1 = k.at(x + w, y + h - keyH);
  dl->AddRectFilled(p0, p1, iskin::rgb(0x1E2023), k.px(2));
  const float kw = (p1.x - p0.x) / static_cast<float>(keys);
  for (int n = lo; n <= hi; ++n)  // octave lines
    if (n % 12 == 0) dl->AddLine(ImVec2(p0.x + (n - lo) * kw, p0.y), ImVec2(p0.x + (n - lo) * kw, p1.y), iskin::rgb(0x34383D));
  auto vy = [&](int v) { return p1.y - (p1.y - p0.y) * static_cast<float>(v) / 128.f; };
  // regions sounding now (the Multisampler publishes them while it plays an SFZ instrument)
  std::vector<uint8_t> lit(ins.regions.size(), 0);
  if (t.isChannel())
    if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen)
      if (const RegionActivity* ra = rt->gen->regionActivity())
        for (int s = 0; s < RegionActivity::kSlots; ++s) {
          const int r = ra->region(s);
          if (r >= 0 && r < static_cast<int>(lit.size())) lit[static_cast<size_t>(r)] = 1;
        }
  const Region* hover = nullptr;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool inMap = mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y && ImGui::IsWindowHovered();
  for (const Region& r : ins.regions) {
    const float rx0 = p0.x + (std::max(r.lokey, lo) - lo) * kw, rx1 = p0.x + (std::min(r.hikey, hi) - lo + 1) * kw;
    const float ry0 = vy(r.hivel + 1), ry1 = vy(r.lovel);
    const bool rel = r.trigger == Trigger::Release || r.trigger == Trigger::ReleaseKey;
    const bool on = lit[static_cast<size_t>(&r - ins.regions.data())] != 0;
    const bool other = view >= 0 && r.swLast >= 0 && r.swLast != view;  // another articulation's region
    if (other && !on) continue;
    const ImU32 col = groupColour(ins, r.group, rel ? 0.10f : 0.32f);
    dl->AddRectFilled(ImVec2(rx0 + 1, ry0 + 1), ImVec2(rx1 - 1, ry1 - 1), on ? iskin::rgb(0xFE8025) : col);
    dl->AddRect(ImVec2(rx0 + 1, ry0 + 1), ImVec2(rx1 - 1, ry1 - 1), groupColour(ins, r.group, rel ? 0.35f : 0.9f), 0, 0, 1.0f);
    if (inMap && mouse.x >= rx0 && mouse.x < rx1 && mouse.y >= ry0 && mouse.y < ry1) hover = &r;
  }
  if (hover) {
    const Region& r = *hover;
    std::string names;
    for (const std::string* l : {&r.globalLabel, &r.masterLabel, &r.groupLabel, &r.regionLabel})
      if (!l->empty()) names += (names.empty() ? "" : " / ") + *l;
    std::string tip = (names.empty() ? std::string() : names + "\n") + (r.sample.empty() ? std::string("-") : r.sample) + "\n" + noteName(r.lokey) + " - " + noteName(r.hikey) + "   " + i18n::tr("velocity") + " " +
                      std::to_string(r.lovel) + " - " + std::to_string(r.hivel) + "\n" + i18n::tr("root") + " " +
                      noteName(r.pitchKeycenter >= 0 ? r.pitchKeycenter : 60);
    if (r.group) tip += "   " + std::string(i18n::tr("group")) + " " + std::to_string(r.group);
    if (r.trigger == Trigger::Release || r.trigger == Trigger::ReleaseKey) tip += "\n" + std::string(i18n::tr("plays when the key is let go"));
    if (r.swLast >= 0) tip += "\n" + std::string(i18n::tr("keyswitch")) + " " + noteName(r.swLast);
    ImGui::SetTooltip("%s", tip.c_str());
  }
  // keyboard strip with keyswitches
  const float ky0 = p1.y + k.px(2), ky1 = k.at(0, y + h).y;
  for (int n = lo; n <= hi; ++n) {
    const float kx0 = p0.x + (n - lo) * kw, kx1 = kx0 + kw;
    const bool black = isBlack(n);
    const bool sw = n >= ins.swLokey && n <= ins.swHikey;
    ImU32 c = black ? iskin::rgb(0x2F3031) : iskin::rgb(0xCFD3DA);
    if (sw) c = black ? iskin::rgb(0x8A3A2E) : iskin::rgb(0xE88A6E);
    dl->AddRectFilled(ImVec2(kx0 + 0.5f, ky0), ImVec2(kx1 - 0.5f, black ? ky0 + (ky1 - ky0) * 0.62f : ky1), c);
    if (n == ins.swDefault) dl->AddCircleFilled(ImVec2((kx0 + kx1) * 0.5f, ky1 - k.px(5)), std::max(1.5f, kw * 0.22f), iskin::rgb(0x7FF0AA));
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x, ky0));
  ImGui::InvisibleButton("##sfzkeys", ImVec2(std::max(1.f, p1.x - p0.x), std::max(1.f, ky1 - ky0)));
  {  // held, the key under the pointer plays: a drag glides from key to key (KeyGlide.h)
    static keyglide::Held glide;
    const int under = std::clamp(lo + static_cast<int>(std::floor((ImGui::GetIO().MousePos.x - p0.x) / kw)), lo, hi);
    const int held = keyglide::followItem(a, t.isChannel() ? t.owner : kNoId, under, glide, 0.8f);
    keyglide::devNoteStrip("sfz", ImVec2(p0.x, ky0), ImVec2(p1.x, ky1), lo, p0.x, kw, held);
  }
  if (ImGui::IsItemHovered()) {
    const int n = std::clamp(lo + static_cast<int>((mouse.x - p0.x) / kw), 0, 127);
    auto it = ins.swLabels.find(n);
    ImGui::SetTooltip("%s%s%s", noteName(n).c_str(), it != ins.swLabels.end() ? "  " : "", it != ins.swLabels.end() ? it->second.c_str() : "");
  }
}

// The SFZ view: replaces the zone panels while the Multisampler plays an .sfz.
void sfzPanels(App& a, const PluginTarget& t, const iskin::Skin& k, ZoneMap& m) {
  using namespace gl::sfz;
  auto insp = sfzForEditor(a, m.sfzPath);
  const Instrument& ins = *insp;
  const Report& rep = ins.report;
  // ---- header strip and map
  iskin::bevel(k, 7, 40, 722, 352, iskin::rgb(0x282B2E), k.c.panelLine, 3);
  iskin::text(k, 14, 42, 300, 16, i18n::tr("SFZ INSTRUMENT"), 10, k.c.label, 0, false, true);
  char counts[160];
  std::snprintf(counts, sizeof counts, i18n::tr("%d regions \xC2\xB7 %d groups \xC2\xB7 %d samples"), static_cast<int>(ins.regions.size()), rep.groups, rep.samples);
  iskin::text(k, 150, 42, 300, 16, counts, 10, k.c.text, 0);
  const int missing = static_cast<int>(rep.missingSamples.size() + rep.missingIncludes.size());
  if (missing > 0) {
    char miss[80];
    std::snprintf(miss, sizeof miss, i18n::tr("%d files missing"), missing);
    iskin::text(k, 455, 42, 130, 16, miss, 10, iskin::rgb(0xF07A5A), 0, false, true);
  }
  PluginTarget tt = t;
  if (iskin::button(k, "##sfzconv", 590, 41, 134, 17, i18n::tr("CONVERT TO ZONES"), false, 8.5f)) {
    int dropped = 0;
    ZoneMap z = sfzZoneMap(m.sfzPath, &dropped);
    if (z.zones.empty()) {
      a.notify(i18n::tr("This instrument has no sample regions to convert"), true);
    } else {
      gMs[tt.key()].sel = 0;
      commitMap(a, tt, z, "convert SFZ to zones");
      if (dropped > 0) a.notify(std::to_string(dropped) + " " + i18n::tr("regions used controllers, filters or modulators that zones do not keep"));
    }
  }
  auto vit = gSfzView.find(t.key());
  if (vit == gSfzView.end()) vit = gSfzView.emplace(t.key(), ins.swDefault).first;
  sfzRegionMap(a, t, ins, k, 14, 60, 708, 326, vit->second);
  // ---- lower page: controllers, keyswitches, notes from the reader
  iskin::bevel(k, 7, 396, 683, 225, iskin::rgb(0x282B2E), k.c.panelLine, 0);
  iskin::text(k, 14, 400, 200, 14, i18n::tr("CONTROLS"), 10, k.c.label, 0, false, true);
  int i = 0;
  for (const auto& [cc, label] : ins.ccLabels) {
    if (i >= 14) break;
    const float cx = 16 + static_cast<float>(i % 7) * 60, cy = 420 + static_cast<float>(i / 7) * 66;
    double v = sfzCcValue(m, ins, cc) * 127.0;
    const ParamDesc d = pd(label.c_str(), "", 0, 127, sfzCcValue(ZoneMap{}, ins, cc) * 127.0);
    char id[24];
    std::snprintf(id, sizeof id, "##sfzcc%d", cc);
    if (iskin::wheelValue(a, k, id, v, d, cx + 9, cy, 38, k.c.fill)) {
      const float nv = static_cast<float>(std::clamp(v / 127.0, 0.0, 1.0));
      bool found = false;
      for (auto& [c, x] : m.sfzCc)
        if (c == cc) x = nv, found = true;
      if (!found) m.sfzCc.emplace_back(cc, nv);
      commitMap(a, t, m, "set SFZ controller", mergeKey(t, 4000 + cc));
    }
    iskin::text(k, cx - 2, cy + 40, 60, 12, label.c_str(), 9, k.c.text, 1);
    char num[16];
    std::snprintf(num, sizeof num, "CC %d", cc);
    iskin::text(k, cx - 2, cy + 52, 60, 10, num, 8, k.c.textDim, 1, true);
    ++i;
  }
  if (ins.ccLabels.empty()) iskin::text(k, 14, 422, 420, 14, i18n::tr("This instrument names no controllers"), 9.5f, k.c.textDim, 0);
  // keyswitches
  iskin::text(k, 452, 400, 230, 14, i18n::tr("KEYSWITCHES (click to show)"), 10, k.c.label, 0, false, true);
  float ky = 418;
  if (ins.swLabels.empty() && ins.swLokey > ins.swHikey) iskin::text(k, 452, ky, 230, 14, i18n::tr("None"), 9.5f, k.c.textDim, 0), ky += 15;
  int shown = 0;
  for (const auto& [key, label] : ins.swLabels) {
    if (shown++ >= 6) break;
    const bool cur = key == vit->second;
    ImGui::SetCursorScreenPos(k.at(452, ky));
    ImGui::PushID(key);
    if (ImGui::InvisibleButton("##sw", ImVec2(k.px(230), k.px(14)))) vit->second = cur ? -1 : key;
    ImGui::PopID();
    if (cur) iskin::bevel(k, 450, ky, 234, 14, iskin::rgb(0x3A4A40), iskin::rgb(0x7FF0AA), 2);
    iskin::text(k, 454, ky, 230, 14, (noteName(key) + "  " + label + (key == ins.swDefault ? "  *" : "")).c_str(), 9.5f, cur ? iskin::rgb(0x7FF0AA) : k.c.text, 0);
    ky += 15;
  }
  // what the reader reported
  iskin::text(k, 452, 516, 230, 14, i18n::tr("READER NOTES"), 10, k.c.label, 0, false, true);
  std::vector<std::string> notes;
  for (const auto& f : rep.missingSamples) notes.push_back(std::string(i18n::tr("missing: ")) + fs::u8path(f).filename().u8string());
  for (const auto& [op, n] : rep.unknown) notes.push_back(std::string(i18n::tr("not an SFZ opcode: ")) + op);
  for (const auto& [op, n] : rep.reported) notes.push_back(std::string(i18n::tr("no counterpart: ")) + op);
  for (const auto& w : rep.warnings) notes.push_back(w);
  if (notes.empty()) notes.push_back(i18n::tr("Every opcode in this instrument is played"));
  float ny = 533;
  for (size_t n = 0; n < notes.size() && n < 5; ++n, ny += 14) iskin::text(k, 452, ny, 232, 13, notes[n].c_str(), 9, n == 0 && notes.size() == 1 ? k.c.textDim : k.c.text, 0);
  if (notes.size() > 5) {
    char more[48];
    std::snprintf(more, sizeof more, i18n::tr("and %d more"), static_cast<int>(notes.size() - 5));
    iskin::text(k, 452, ny, 232, 13, more, 9, k.c.textDim, 0);
  }
}
