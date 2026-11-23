// Gridline — the guide to the built-in instruments (GitHub #73): core/InstrumentGuide.h and
// docs/instruments.md stay in step with the instruments the pickers offer.
//   * every instrument the pickers offer (a built-in generator that is not hidden) has an entry,
//     and every entry is such an instrument or one of the flagship effects;
//   * the flagships lead, in the user's order: Plasma, Seltmann, Platter, Tideline, Chater, Loupe;
//   * the page has a section per entry, in the same order, each with its picture (a file that is
//     there) and the entry's engine and good-for words;
//   * the short line the pickers show reads "<engine>. Good for <what>." and public text has no
//     dashes of the long kinds.
// Run from the source folder (ctest sets it).
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include "Check.h"
#include "core/InstrumentGuide.h"
#include "core/PluginInfo.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { ::expect(ok, name.c_str()); }

std::string readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const PluginInfo* info(const std::string& type) {
  for (const auto& p : pluginCatalog())
    if (p.type == type) return &p;
  return nullptr;
}

bool hasLongDash(const std::string& s) { return s.find("\xE2\x80\x93") != std::string::npos || s.find("\xE2\x80\x94") != std::string::npos; }

void testCoverage() {
  std::set<std::string> listed;
  for (const auto& e : guide::instruments()) {
    expect(listed.insert(e.type).second, std::string("guide: listed once: ") + e.type);
    const PluginInfo* p = info(e.type);
    expect(p != nullptr, std::string("guide: a built-in plugin: ") + e.type);
    if (!p) continue;
    const bool flagEffect = std::strcmp(e.group, "Flagships") == 0 && p->kind == PluginKind::Effect;
    expect((p->kind == PluginKind::Generator && !p->hidden) || flagEffect, std::string("guide: an offered instrument (or a flagship effect): ") + e.type);
    expect(e.engine[0] && e.goodFor[0] && !hasLongDash(e.engine) && !hasLongDash(e.goodFor), std::string("guide: plain words: ") + e.type);
  }
  int offered = 0;
  for (const auto& p : pluginCatalog()) {
    if (p.kind != PluginKind::Generator || p.hidden) continue;
    ++offered;
    expect(listed.count(p.type) == 1, "guide: every offered instrument has an entry: " + p.type + " (" + p.name + ")");
  }
  std::printf("# %d instruments offered, %zu entries\n", offered, guide::instruments().size());
  const char* flags[] = {"gl.plasma", "gl.seltmann", "gl.platter", "gl.tideline", "gl.chater", "gl.edison"};
  for (size_t i = 0; i < 6; ++i)
    expect(guide::instruments().size() > i && std::strcmp(guide::instruments()[i].type, flags[i]) == 0,
           std::string("guide: flagship ") + std::to_string(i + 1) + " is " + flags[i]);
  expect(guide::shortLine("gl.analog").rfind("Virtual analog", 0) == 0 && guide::shortLine("gl.analog").find(". Good for classic pads") != std::string::npos,
         "guide: the pickers' short line");
  expect(guide::shortLine("gl.reverb").empty() && guide::find("gl.nope") == nullptr, "guide: no line for other plugins");
  expect(std::string(guide::pageUrl()).find("docs/instruments.md") != std::string::npos, "guide: the pickers link to the page");
}

void testPage() {
  const std::string page = readAll("docs/instruments.md");
  expect(!page.empty(), "page: docs/instruments.md is there");
  if (page.empty()) return;
  expect(!hasLongDash(page), "page: no long dashes");
  size_t at = 0;
  for (const auto& e : guide::instruments()) {
    const PluginInfo* p = info(e.type);
    if (!p) continue;
    // "### <Name>" then, before the next heading, the type, the picture, the engine and good-for words.
    const std::string head = "\n### " + p->name + "\n";
    const size_t h = page.find(head, at);
    expect(h != std::string::npos, "page: a section for " + p->name + " after the one before it");
    if (h == std::string::npos) continue;
    at = h + head.size();
    const size_t next = page.find("\n#", at);
    const std::string body = page.substr(at, next == std::string::npos ? std::string::npos : next - at);
    expect(body.find(std::string("`") + e.type + "`") != std::string::npos, "page: " + p->name + " names its type");
    expect(body.find(e.engine) != std::string::npos, "page: " + p->name + " says its engine as the pickers do");
    expect(body.find(e.goodFor) != std::string::npos, "page: " + p->name + " says what it is good for as the pickers do");
    const size_t img = body.find("](images/instruments/");
    expect(img != std::string::npos, "page: " + p->name + " has a picture");
    if (img == std::string::npos) continue;
    const size_t end = body.find(')', img);
    const std::string rel = body.substr(img + 2, end - img - 2);
    expect(fs::is_regular_file(fs::path("docs") / rel), "page: " + p->name + "'s picture is there (" + rel + ")");
  }
  const std::string readme = readAll("README.md");
  expect(readme.find("docs/instruments.md") != std::string::npos, "README links to the guide");
}

}  // namespace

int main() {
  testCoverage();
  testPage();
  return finish();
}
