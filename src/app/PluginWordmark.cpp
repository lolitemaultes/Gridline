// Gridline — J-130 plugin wordmarks in the editors: the Tile logo of each
// built-in plugin at the top left of its editor header (drawPluginHeader) and
// in title strips that ask for it, and the suite logos.
//
// Colours come from the theme at draw time (wordmarkColours): the tile takes
// the plugin's family colour as a lit role (deepened on light faces), the name
// the theme's text colour, the cut-outs are true holes, so the logo sits on
// any header surface a control theme paints.
#include <algorithm>
#include <cmath>
#include <string>
#include "../core/PluginBrand.h"
#include "../core/PluginInfo.h"
#include "App.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "icons/Wordmark.h"

namespace gl {
namespace pui {

namespace {

// The role colours of one wordmark on the current theme. A control theme that
// needs its own text or family colours for the header changes them here.
icons::wordmark::Colours wordmarkColours(const std::string& key) {
  const auto& P = theme::pal();
  icons::wordmark::Colours c;
  const ImU32 mark = icons::wordmark::markColour(key);
  c.mark = key.compare(0, 5, "suite") == 0 || !mark ? P.hot : theme::lit(mark);  // suites: "mini" in the lit-glyph colour
  c.word = P.text;
  c.dim = P.textDim;
  return c;
}

}  // namespace

bool wordmark(App& a, const std::string& type, float size) {
  (void)a;
  // Same item as the J-94 wordmark it replaces: max(frame height, size + 4) px tall.
  const float h = std::max(ImGui::GetFrameHeight(), size + 4.0f);
  const float drawH = std::min(h, std::round(size * 24.0f / 18.0f));  // the 24-unit wordmark at size 18
  ImVec2 sz;
  if (!icons::wordmark::size(type, drawH, &sz)) return false;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const std::string name = pluginDisplayName(type);
  ImGui::InvisibleButton((name + "##wordmark").c_str(), ImVec2(sz.x, h));  // the name stays the item's label
  if (ImGui::IsItemHovered()) {
    const std::string tag = pluginTagline(type);
    ui::Hint(tag.empty() ? name : name + "  \xC2\xB7  " + tag);
  }
  icons::wordmark::draw(ImGui::GetWindowDrawList(), type, ImVec2(p.x, p.y + (h - drawH) * 0.5f), drawH, wordmarkColours(type));
  return true;
}

float wordmarkAt(ImDrawList* dl, const std::string& type, ImVec2 pos, float height, ImU32 word) {
  ImVec2 sz;
  if (!icons::wordmark::size(type, height, &sz)) return 0;
  icons::wordmark::Colours c = wordmarkColours(type);
  if (word) c.word = word;
  icons::wordmark::draw(dl, type, pos, height, c);
  return sz.x;
}

ImVec2 wordmarkSize(const std::string& type, float height) {
  ImVec2 sz(0, 0);
  if (!icons::wordmark::size(type, height, &sz)) return ImVec2(0, 0);
  return sz;
}

namespace {
// A title wordmark `h` px tall at `at`, with the name and tagline as the hint
// on hover (as in the editor header).
bool titleAt(ImDrawList* dl, const std::string& type, ImVec2 at, float h, ImU32 word) {
  at = ImVec2(std::round(at.x), std::round(at.y));
  const float w = wordmarkAt(dl, type, at, h, word);
  if (w <= 0) return false;
  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(at, ImVec2(at.x + w, at.y + h))) {
    const std::string name = pluginDisplayName(type), tag = pluginTagline(type);
    app().setHint(tag.empty() ? name : name + "  \xC2\xB7  " + tag);
  }
  return true;
}
}  // namespace

bool titleWordmark(ImDrawList* dl, const std::string& type, ImVec2 pos, float fontSize, ImU32 word) {
  const float h = std::round(fontSize * kTitleWordmarkScale);
  return titleAt(dl, type, ImVec2(pos.x, pos.y - (h - fontSize) * 0.5f), h, word);
}

bool titleWordmarkIn(ImDrawList* dl, const std::string& type, ImVec2 r0, ImVec2 r1, float fontSize, int align, ImU32 word) {
  const ImVec2 unit = wordmarkSize(type, 24.0f);
  if (unit.x <= 0) return false;
  float h = std::min(std::round(fontSize * kTitleWordmarkScale), r1.y - r0.y);
  h = std::floor(std::min(h, (r1.x - r0.x) * 24.0f / unit.x));
  if (h < 6) return false;
  const float w = wordmarkSize(type, h).x;
  const float x = align == 1 ? (r0.x + r1.x - w) * 0.5f : align == 2 ? r1.x - w : r0.x;
  return titleAt(dl, type, ImVec2(x, (r0.y + r1.y - h) * 0.5f), h, word);
}

float suiteLogo(ImDrawList* dl, ImVec2 pos, float height, bool mini) {
  const std::string key = mini ? "suite-mini" : "suite";
  ImVec2 sz;
  if (!icons::wordmark::size(key, height, &sz)) return 0;
  icons::wordmark::draw(dl, key, pos, height, wordmarkColours(key));
  return sz.x;
}

ImVec2 suiteLogoSize(float height, bool mini) {
  ImVec2 sz(0, 0);
  if (!icons::wordmark::size(mini ? "suite-mini" : "suite", height, &sz)) return ImVec2(0, 0);
  return sz;
}

}  // namespace pui
}  // namespace gl
