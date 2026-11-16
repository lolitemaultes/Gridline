// Gridline - K-308: a plugin bundle's member list (core/PluginSuite.h listedMembers): one engine
// binary per format serves the whole suite, one plugin, or the plugins an installer picked.
#include <string>
#include "Check.h"
#include "core/PluginSuite.h"

using namespace gl::suite;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }
}  // namespace

int main() {
  const auto& all = members("gridline");
  const auto& mini = members("mini");
  expect(all.size() > 50 && !mini.empty(), "both suites have members");
  expect(listedMembers("gridline", "").size() == all.size(), "no list: every member");
  expect(listedMembers("gridline", "# only a comment\n\n").size() == all.size(), "an empty list: every member");
  const std::string one = all[3].type;
  const auto single = listedMembers("gridline", "suite gridline\n" + one + "\n");
  expect(single.size() == 1 && single[0]->type == one, "a single-plugin list: that member");
  const std::string two = all[1].type;
  const auto picked = listedMembers("gridline", "suite gridline\r\n  " + one + "  \r\n" + two + "\r\ngl.nothing\r\n");
  expect(picked.size() == 2 && picked[0]->type == two && picked[1]->type == one, "picks in catalogue order, CRLF and spaces tolerated, unknown types dropped");
  expect(listedMembers("gridline", "suite mini\n" + mini[0].type + "\n").size() == all.size(), "another suite's list: every member");
  expect(listedMembers("mini", "suite mini\n" + one + "\n").size() == mini.size(), "a type of another suite never leaks in");
  const std::string text = memberListText("gridline", {one, "gl.nothing", two});
  const auto back = listedMembers("gridline", text);
  expect(back.size() == 2 && text.find("gl.nothing") == std::string::npos && text.find("suite gridline\n") != std::string::npos,
         "the written list reads back: " + text);
  bool mapped = true;
  for (const Member* m : listedMembers("gridline", memberListText("gridline", {one})))
    mapped = mapped && byClapId(m->clapId) == m && byVst3Uid(m->vst3Uid) == m;
  expect(mapped, "identities stay per type in a single-plugin bundle");
  return finish();
}
