// Gridline: the "Audio clip" window's plan (core/ClipWindow.h): one window for an audio clip and its
// sampler channel. Checks:
//  * the tabs, their groups and what each changes (the sample's four tabs in the middle);
//  * a sampler channel with no clip: the clip's two tabs hidden, a hidden tab falls back to Playback;
//  * each way in opens its tab (a double-click and Edit clip: Clip; Channel settings: Playback);
//  * the cards' rows: as many as fit, stretched to the width exactly, never narrower than asked;
//  * the checklist of the old controls: every control of the two old windows has one home, every
//    key is unique, and the window's source marks each key where it draws the control;
//  * missing(): what a window (or a layout's own places) with or without a clip, on a clip of the
//    clip grid or not, must show.
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/ClipWindow.h"

using namespace gl;
using namespace gl::clipwin;

namespace {

std::string readFile(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int main() {
  // ---- tabs and scopes
  expect(TabCount == 12, "twelve tabs (ten of modules, Edit audio and Analysis)");
  expect(std::string(tabName(TabClip)) == "Clip" && std::string(tabName(TabTime)) == "Warp", "the clip's tabs");
  expect(std::string(tabName(TabFilter)) == "Filter" && std::string(tabName(TabNoteFx)) == "Note effects" && std::string(tabName(TabMidi)) == "MIDI and macros",
         "the tabs the old Envelope and Channel tabs were split into");
  expect(TabPlayback == 3, "the tab numbers up to Playback are as before (tests and the dev commands use them)");
  {
    bool ok = true;
    for (int t = 0; t < TabCount; ++t) ok = ok && hasModules(t) == (t != TabEdit && t != TabAnalysis);
    expect(ok, "every tab but Edit audio and Analysis has modules");
  }
  expect(std::string(tabName(TabPlayback)) == "Playback" && std::string(tabName(TabEnvelope)) == "Envelope", "Playback and Envelope keep their names");
  expect(std::string(tabName(TabCount)).empty() && std::string(tabName(-1)).empty(), "no name out of range");
  expect(scopeOf(TabClip) == Scope::Clip && scopeOf(TabTime) == Scope::Clip, "Clip and Warp change the clip");
  expect(scopeOf(TabSample) == Scope::Sample && scopeOf(TabPlayback) == Scope::Sample && scopeOf(TabEnvelope) == Scope::Sample &&
             scopeOf(TabFilter) == Scope::Sample && scopeOf(TabSampleFx) == Scope::Sample,
         "the five sample tabs change the sample");
  expect(scopeOf(TabChannel) == Scope::Channel && scopeOf(TabNoteFx) == Scope::Channel && scopeOf(TabMidi) == Scope::Channel, "Channel, Note effects and MIDI and macros change the channel");
  expect(scopeOf(TabEdit) == Scope::Tools && scopeOf(TabAnalysis) == Scope::Tools, "Edit audio and Analysis are tools");
  {
    int next = 0;
    bool ok = true;
    for (const Group& g : groups()) {
      ok = ok && g.first == next && g.count > 0;
      for (int t = g.first; t < g.first + g.count; ++t) ok = ok && scopeOf(t) == scopeOf(g.first);
      next = g.first + g.count;
    }
    expect(ok && next == TabCount && groups().size() == 4, "four groups cover the tabs in order, one scope each");
  }

  // ---- a sampler channel with no clip
  for (int t = 0; t < TabCount; ++t) {
    expect(tabShown(t, true), (std::string("with a clip: ") + tabName(t) + " shown").c_str());
    expect(tabShown(t, false) == (scopeOf(t) != Scope::Clip), (std::string("no clip: ") + tabName(t) + (tabShown(t, false) ? " shown" : " hidden")).c_str());
  }
  expect(shownTab(TabClip, false) == TabPlayback && shownTab(TabTime, false) == TabPlayback, "no clip: the clip's tabs fall back to Playback");
  expect(shownTab(TabEnvelope, false) == TabEnvelope && shownTab(TabClip, true) == TabClip, "a shown tab stays");
  expect(!tabShown(-1, true) && !tabShown(TabCount, true), "no tab out of range");

  // ---- the ways in
  expect(entryTab(Entry::DoubleClick) == TabClip && entryTab(Entry::EditClip) == TabClip, "a double-click and Edit clip open the Clip tab");
  expect(entryTab(Entry::ChannelSettings) == TabPlayback && entryTab(Entry::DoubleClickSetting) == TabPlayback,
         "Channel settings and the Playback setting open the Playback tab");


  // ---- the cards' rows
  {
    std::vector<float> w;
    const auto rows = flowRows({240, 340, 160, 220, 210}, 1400, 10, &w);
    expect(rows.size() == 1 && rows[0].count == 5, "five cards in one row at 1400");
    float sum = 0;
    bool wide = true;
    const float mins[] = {240, 340, 160, 220, 210};
    for (size_t i = 0; i < w.size(); ++i) sum += w[i], wide = wide && w[i] >= mins[i];
    expect(std::fabs(sum + 40 - 1400) < 0.5f, "the row ends at the width exactly");
    expect(wide, "no card narrower than it asked");
  }
  {
    std::vector<float> w;
    const auto rows = flowRows({240, 340, 160, 220, 210}, 1000, 10, &w);
    expect(rows.size() == 2 && rows[0].count == 4 && rows[1].first == 4 && rows[1].count == 1, "at 1000: four, then one");
    expect(std::fabs(w[4] - 1000) < 0.5f, "a card alone takes the row");
  }
  {
    std::vector<float> w;
    const auto rows = flowRows({720, 280}, 600, 10, &w);
    expect(rows.size() == 2 && w[0] >= 720, "a card wider than the panel keeps its width, alone in its row");
    expect(flowRows({}, 600, 10).empty(), "no cards, no rows");
  }
  {
    // Shares of the spare width: a card that takes none keeps its least width, the rest share by weight.
    std::vector<float> w;
    const std::vector<float> grow = {0, 1, 3};
    const auto rows = flowRows({300, 300, 300}, 1320, 10, &w, &grow);
    expect(rows.size() == 1 && rows[0].count == 3, "shares: one row");
    expect(std::fabs(w[0] - 300) < 0.5f, "shares: a card that takes none keeps its width");
    expect(std::fabs(w[1] - 300 - 100) < 1.5f && std::fabs(w[2] - 300 - 300) < 1.5f, "shares: one part and three parts of the 400 spare");
    expect(std::fabs(w[0] + w[1] + w[2] + 20 - 1320) < 0.5f, "shares: the row ends at the width exactly");
    const std::vector<float> none = {0, 0};
    flowRows({200, 200}, 610, 10, &w, &none);
    expect(std::fabs(w[0] - 300) < 0.5f && std::fabs(w[1] - 300) < 0.5f, "shares: a row of cards that all take none share it equally");
  }

  // ---- the checklist
  const std::vector<Item>& inv = inventory();
  {
    std::set<std::string> keys;
    bool unique = true, homed = true, tabs = true;
    int fromEditor = 0, fromSettings = 0;
    for (const Item& it : inv) {
      unique = unique && keys.insert(it.key).second;
      homed = homed && it.home && *it.home && it.what && *it.what;
      tabs = tabs && it.tab >= -1 && it.tab < TabCount;
      if (it.was & WasClipEditor) ++fromEditor;
      if (it.was & WasChannelSettings) ++fromSettings;
    }
    expect(unique, "every checklist key once");
    expect(homed, "every old control has a home");
    expect(tabs, "every home is a tab or the window's top");
    expect(fromEditor >= 40 && fromSettings >= 40, "both old windows' controls are listed");
    // The three Reverses and the two Normalizes each have a home of their own.
    expect(keys.count("reverse.clip") && keys.count("reverse.notes") && keys.count("edit.ops"), "Reverse: the clip's, the sample's, the file's");
    expect(keys.count("normgain") && keys.count("normalize.sample"), "Normalize: the clip's gain and the older sample setting's note");
  }
  {
    // Every key is marked where the window draws its control: reach("key") in app/ClipEditor.cpp.
    const std::string src = readFile(std::string(GL_SOURCE_DIR) + "/src/app/ClipEditor.cpp");
    expect(!src.empty(), "the window's source read");
    std::string none;
    for (const Item& it : inv)
      if (src.find(std::string("reach(\"") + it.key + "\")") == std::string::npos && src.find(std::string("\"") + it.key + "\"") == std::string::npos)
        none += std::string(" ") + it.key;
    expect(none.empty(), ("every key is marked in the window" + (none.empty() ? std::string() : ": missing" + none)).c_str());
  }
  {
    std::set<std::string> all;
    for (const Item& it : inv) all.insert(it.key);
    expect(missing(all, true, true).empty(), "everything reached: nothing missing");
    expect(!all.count("chips"), "no chip row (the Clips band shows its own cards, nothing pops over the views above)");
    const auto noClip = missing({}, false, false);
    bool onlyAlways = true;
    for (const std::string& k : noClip)
      for (const Item& it : inv)
        if (k == it.key) onlyAlways = onlyAlways && it.need == NeedAlways;
    expect(onlyAlways && !noClip.empty(), "no clip: only what every window shows is needed");
    const auto withClip = missing({}, true, false);
    expect(withClip.size() > noClip.size(), "a clip needs its own controls too");
    bool launch = false, tools = false;
    for (const std::string& k : missing({}, true, true)) launch = launch || k == "launch";
    for (const std::string& k : withClip) tools = tools || k == "tools";
    expect(launch, "a clip of the clip grid needs its launch settings");
    expect(tools, "a clip needs the tools row in every place (the Clips band's sample display too)");
    bool noLaunch = true;
    for (const std::string& k : withClip) noLaunch = noLaunch && k != "launch";
    expect(noLaunch, "off the clip grid no launch settings are needed");
  }
  return finish();
}
