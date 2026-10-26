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
