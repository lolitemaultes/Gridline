// Gridline - K-112: the menu tree as data (core/MenuTree.h): items in order, find by path and
// occurrence, by command, counts, and the dump the menu golden compares.
#include "core/MenuTree.h"
#include "Check.h"

using namespace gl;
using menutree::Node;

namespace {
Node item(const std::string& label, const std::string& shortcut = "", const std::string& command = "", int occ = 0) {
  Node n;
  n.raw = n.shown = n.en = label;
  n.shortcut = shortcut;
  n.command = command;
  n.occ = occ;
  return n;
}
Node sub(const std::string& label, std::vector<Node> children) {
  Node n = item(label);
  n.submenu = true;
  n.children = std::move(children);
  return n;
}
}  // namespace

int main() {
  menutree::Tree t;
  t.style = "studio";
  Node tmpl = sub("New from template", {item("Empty project"), item("Empty project", "", "", 1), item("Starter kit")});
  t.roots.push_back(sub("File", {item("New", "Ctrl+N", "file.new"), tmpl, item("Save", "Ctrl+S", "file.save")}));
  Node off = item("Panels##x");
  off.en = "Panels";
  off.enabled = false;
  Node checked = item("Mixer", "F9", "view.mixer");
  checked.checked = true;
  t.roots.push_back(sub("View", {checked, off}));

  expect(menutree::countItems(t) == 7, "seven items");
  expect(menutree::countSubmenus(t) == 3, "three submenus (two roots, one nested)");
  const auto all = menutree::items(t);
  expect(all.size() == 7 && all[0].item->en == "New" && all[1].item->en == "Empty project" && all[4].item->en == "Save", "items in menu order");
  expect(all[1].root == 0 && all[1].path.size() == 2 && all[1].rawPath() == std::vector<std::string>{"New from template"}, "path of a nested item");
  expect(all[5].root == 1 && all[5].rawPath().empty(), "an item of the second root");

  const Node* second = menutree::find(t, 0, {"New from template"}, "Empty project", 1);
  expect(second == &t.roots[0].children[1].children[1], "find: second occurrence");
  expect(menutree::find(t, 0, {"New from template"}, "Empty project", 2) == nullptr, "find: no third occurrence");
  expect(menutree::find(t, 0, {"Nope"}, "Empty project", 0) == nullptr, "find: unknown submenu");
  expect(menutree::find(t, 5, {}, "New", 0) == nullptr, "find: unknown root");
  expect(menutree::find(t, 1, {}, "Panels##x", 0) != nullptr, "find: by raw label (with its id)");

  const auto save = menutree::byCommand(t, "file.save");
  expect(save.size() == 1 && save[0].item->shortcut == "Ctrl+S", "by command");
  expect(menutree::byCommand(t, "").empty(), "no command: nothing");

  const std::string d = menutree::dump(t);
  expect(d.find("File > New\tCtrl+N\tfile.new\te-\n") == 0, "dump: first line");
  expect(d.find("File > New from template > Empty project\t\t\te-\n") != std::string::npos, "dump: nested path in English");
  expect(d.find("View > Mixer\tF9\tview.mixer\tec\n") != std::string::npos, "dump: checked");
  expect(d.find("View > Panels\t\t\t--\n") != std::string::npos, "dump: disabled, English without the id");
  return finish();
}
