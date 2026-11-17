// Gridline - K-112: the region shell of each layout (core/LayoutShell.h).
//   - every region a home names exists in that layout's shell (and nothing is homed in a region
//     that does not exist);
//   - at every supported window size, with the default regions, with every region shown and with
//     every optional one hidden, the visible regions tile the dock area exactly: inside it, no
//     overlap, no gap, none smaller than a usable size;
//   - fixed bands keep their pixel size; hiding a region gives its space back to its parent;
//   - the per-person region state round-trips through its settings text, drops unknown regions,
//     survives broken text, and is empty for the default state (nothing written).
#include <cmath>
#include <set>
#include <string>
#include "Check.h"
#include "core/LayoutHomes.h"
#include "core/LayoutShell.h"
#include "core/Settings.h"

using namespace gl;
using layout::Preset;

namespace {

bool tiles(const std::vector<layout::Placed>& v, float w, float h, std::string& why) {
  double area = 0;
  std::vector<const layout::Placed*> leaves;
  // A region with children is the part its children left: every visible region counts once.
  for (const auto& p : v)
    if (p.visible) leaves.push_back(&p);
  for (const auto* p : leaves) {
    const auto& r = p->rect;
    if (r.x < -0.01f || r.y < -0.01f || r.right() > w + 0.01f || r.bottom() > h + 0.01f) {
      why = p->id + " outside";
      return false;
    }
    if (r.w < 40 || r.h < 40) {
      why = p->id + " too small";
      return false;
    }
    area += static_cast<double>(r.w) * r.h;
    for (const auto* q : leaves) {
      if (q == p) continue;
      const auto& s = q->rect;
      const float ox = std::min(r.right(), s.right()) - std::max(r.x, s.x);
      const float oy = std::min(r.bottom(), s.bottom()) - std::max(r.y, s.y);
      if (ox > 0.01f && oy > 0.01f) {
        why = p->id + " overlaps " + q->id;
        return false;
      }
    }
  }
  if (std::fabs(area - static_cast<double>(w) * h) > 1.0) {
    why = "gap: covered " + std::to_string(area) + " of " + std::to_string(static_cast<double>(w) * h);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  // Homes and regions agree.
  std::string unknown;
  for (const layout::Item& it : layout::items())
    for (int k = 0; k < layout::kLayoutCount; ++k) {
      const std::string r = it.home[k].region;
      if (!r.empty() && !layout::hasRegion(static_cast<Preset>(k), r)) unknown += " " + std::string(it.id) + ":" + r;
    }
  expect(unknown.empty(), ("every region a home names exists in its layout's shell" + unknown).c_str());
  for (int k = 0; k < layout::kLayoutCount; ++k) {
    const layout::Shell& sh = layout::shell(static_cast<Preset>(k));
    bool own = std::string(sh.centre).rfind(layout::regionPrefix(sh.preset), 0) == 0;
    std::set<std::string> ids{sh.centre};
    for (const auto& r : sh.regions) {
      own &= std::string(r.id).rfind(layout::regionPrefix(sh.preset), 0) == 0;
      own &= ids.insert(r.id).second;
      own &= !*r.parent || ids.count(r.parent);  // parents come first
    }
    expect(own, (std::string(layout::id(sh.preset)) + ": region ids are unique, its own, and parents come first").c_str());
  }

  // Geometry at every supported size, in three states.
  const float sizes[][2] = {{1272, 718}, {1432, 818}, {2552, 1358}, {1016, 600}, {2872, 1718}};
  for (int k = 0; k < layout::kLayoutCount; ++k) {
    const Preset p = static_cast<Preset>(k);
    const layout::Shell& sh = layout::shell(p);
    layout::State all, none;
    const layout::State def;
    for (const auto& r : sh.regions) {
      layout::setVisible(sh, all, r.id, true);
      layout::setVisible(sh, none, r.id, false);
    }
    for (const auto& sz : sizes)
      for (const layout::State* st : {&def, static_cast<const layout::State*>(&all), static_cast<const layout::State*>(&none)}) {
        std::string why;
        const bool ok = tiles(layout::solve(p, sz[0], sz[1], *st), sz[0], sz[1], why);
        if (!ok)
          expect(false, (std::string(layout::id(p)) + " " + std::to_string(static_cast<int>(sz[0])) + "x" + std::to_string(static_cast<int>(sz[1])) +
                         ": regions tile the area (" + why + ")").c_str());
      }
    expect(true, (std::string(layout::id(p)) + ": regions tile the area at every size and state").c_str());
    // With every optional region hidden, the centre takes the whole area.
    const auto v = layout::solve(p, 1432, 818, none);
    expect(v.back().id == sh.centre && std::fabs(v.back().rect.w * v.back().rect.h - 1432.0f * 818.0f) < 1.0f,
           (std::string(layout::id(p)) + ": hiding every region gives the whole area to the centre").c_str());
  }
  {
    const auto v = layout::solve(Preset::Live, 1432, 818, {});
    float deviceH = 0;
    for (const auto& pl : v)
      if (pl.id == "live.device") deviceH = pl.rect.h;
    const auto v2 = layout::solve(Preset::Live, 2552, 1358, {});
    float deviceH2 = 0;
    for (const auto& pl : v2)
      if (pl.id == "live.device") deviceH2 = pl.rect.h;
    // K-112c checkpoint 57 (N56-08): 236 px, the clip band's height too (one detail view).
    expect(deviceH == 236 && deviceH2 == 236, "the Live device band keeps its fixed 236 px height at every size");
  }
  {  // K-112c (round 45): a new Clips set shows one detail band (devices); clip and device bands take turns.
    const layout::Shell& sh = layout::shell(Preset::Live);
    layout::State st;
    expect(layout::visible(sh, st, "live.browser") && layout::visible(sh, st, "live.device") && !layout::visible(sh, st, "live.clip") &&
               !layout::visible(sh, st, "live.mixer"),
           "Clips default: the browser and the device band, no clip band, no mixer band");
    layout::setVisible(sh, st, "live.clip", true);
    expect(layout::visible(sh, st, "live.clip") && !layout::visible(sh, st, "live.device"), "showing the clip band hides the device band");
    {  // K-112c checkpoint 57 (N56-08): and takes its height: the main view does not jump
      float clipH = 0, mainH = 0, mainH0 = 0;
      for (const auto& pl : layout::solve(Preset::Live, 1432, 818, st)) {
        if (pl.id == "live.clip") clipH = pl.rect.h;
        if (pl.id == "live.main") mainH = pl.rect.h;
      }
      for (const auto& pl : layout::solve(Preset::Live, 1432, 818, {})) {
        if (pl.id == "live.main") mainH0 = pl.rect.h;
      }
      expect(clipH == 236 && mainH == mainH0, "the clip band is as tall as the device band: the main view keeps its height");
    }
    layout::setVisible(sh, st, "live.device", true);
    expect(!layout::visible(sh, st, "live.clip") && layout::visible(sh, st, "live.device"), "and the other way round");
    {  // K-112c (checkpoint 48): the band toggles stack the clip band over the device band (refs 456, 466)
      layout::State both = st;
      layout::setVisible(sh, both, "live.clip", true, false);
      expect(layout::visible(sh, both, "live.clip") && layout::visible(sh, both, "live.device"), "a stacked clip band keeps the device band");
      float clipY = -1, devY = -1, clipH = 0, devH = 0;
      for (const auto& pl : layout::solve(Preset::Live, 1440, 860, both)) {
        if (pl.id == "live.clip") clipY = pl.rect.y, clipH = pl.rect.h;
        if (pl.id == "live.device") devY = pl.rect.y, devH = pl.rect.h;
      }
      expect(clipY >= 0 && devY > clipY && clipH == 236 && devH == 236, "stacked: the clip band sits over the device band, both full size");
    }
    layout::setVisible(sh, st, "live.mixer", true);
    expect(layout::visible(sh, st, "live.mixer") && layout::visible(sh, st, "live.device"), "the mixer band is not in the group");
    expect(layout::defaultVersion(Preset::Live) == 2 && layout::defaultVersion(Preset::Logic) == 1, "the Clips default replaces saved layouts once");
  }

  // Region state.
  {
    const Preset p = Preset::Logic;
    layout::State st;
    expect(layout::toJson(st).empty(), "the default region state writes nothing");
    layout::setVisible(layout::shell(p), st, "logic.inspector", false);
    layout::setVisible(layout::shell(p), st, "logic.lower", true);  // closed by default since K-112B checkpoint 40
    st.member["logic.lower"] = "Mixer";
    st.memberSize["logic.lower|Mixer"] = 340;
    st.memberSize["logic.lower|Piano roll"] = 280;
    const std::string text = layout::toJson(st);
    expect(layout::fromJson(p, text) == st, "the region state round-trips through its settings text");
    expect(!layout::visible(layout::shell(p), st, "logic.inspector"), "a hidden region is not visible");
    const auto v = layout::solve(p, 1432, 818, st);
    float lowerH = 0;
    for (const auto& pl : v)
      if (pl.id == "logic.lower") lowerH = pl.rect.h;
    expect(lowerH == 340, "an exclusive region takes the size of the member it shows");
    expect(layout::fromJson(p, "{\"hidden\":[\"nope\",\"logic.inspector\"],\"member\":{\"x\":\"y\"}}").hidden ==
               std::set<std::string>{"logic.inspector"},
           "unknown regions are dropped from the state");
    expect(layout::fromJson(p, "{broken") == layout::State{}, "broken state text gives the default state");
    expect(layout::fromJson(Preset::Live, text).hidden.empty(), "one layout's regions mean nothing in another's state");
    Settings s;
    expect(s.set(layout::stateKey(p), text) && s.getString("layout.logic.state") == text, "each layout's state has its own settings key");
  }

  // K-297: a region's divider the person dragged keeps its place (fixed bands too), within the limits.
  {
    layout::State st;
    st.regionSize["live.device"] = 260;
    st.regionSize["live.browser"] = 340;
    const std::string text = layout::toJson(st);
    expect(layout::fromJson(Preset::Live, text) == st, "dragged region sizes round-trip through the settings text");
    expect(layout::fromJson(Preset::Logic, text).regionSize.empty(), "another layout drops them");
    float deviceH = 0, browserW = 0;
    for (const auto& pl : layout::solve(Preset::Live, 1432, 818, st)) {
      if (pl.id == "live.device") deviceH = pl.rect.h;
      if (pl.id == "live.browser") browserW = pl.rect.w;
    }
    expect(deviceH == 260, "a fixed band takes the height it was dragged to");
    expect(browserW == 340, "a side region takes the width it was dragged to");
    st.regionSize["live.device"] = 2000;
    for (const auto& pl : layout::solve(Preset::Live, 1432, 818, st))
      if (pl.id == "live.device") deviceH = pl.rect.h;
    expect(deviceH == std::floor(818 * 0.7f), "a dragged size may pass the default's limit, up to 70 % of the parent");
    st.regionSize.clear();
    for (const auto& pl : layout::solve(Preset::Live, 1432, 500, st))
      if (pl.id == "live.device") deviceH = pl.rect.h;
    expect(deviceH == std::floor(500 * 0.35f), "the default size keeps the layout's own limit");
    const layout::State member = layout::fromJson(Preset::Logic, "{\"shown\":[\"logic.lower\"],\"member\":{\"logic.lower\":\"Mixer\"},\"size\":{\"logic.lower|Mixer\":300},\"region\":{\"logic.lower\":200}}");
    float lowerH = 0;
    for (const auto& pl : layout::solve(Preset::Logic, 1432, 818, member))
      if (pl.id == "logic.lower") lowerH = pl.rect.h;
    expect(lowerH == 300, "an exclusive region's member size wins over a region size");
  }
  return finish();
}
