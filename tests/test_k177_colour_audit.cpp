// Gridline - K-177 (GitHub issue 42): no new hard-coded light colours in UI code. Scans the UI
// sources (src/app, src/plugin and the packs' *_ui* / *_widgets* / *_skin* files) for literal
// colours (IM_COL32(r, g, b[, a]), rgb / hex / litRole / content / lit (0xRRGGBB), ImVec4 float
// literals) that are light and neutral (white or light grey: luminance >= 0.6, channel spread
// < 48, opaque enough to read as a bar, panel or glyph: alpha >= 0.35). Light colours in UI code
// must come from the theme (theme::pal() roles, darkFaces(), tint(), clipInk()).
//
// Deliberate ones are listed per file in tests/theme_colour_allowlist.tsv with the most allowed
// and the reason (brand assets, the palette itself, plugin faceplates, opt-in control styles).
// The test fails when a file not on the list has one, or a listed file has more than its count.
//   gridline_tests_k177_colour_audit <source root> <allowlist.tsv>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include "Check.h"

namespace fs = std::filesystem;

namespace {

struct Allow {
  int max = 0;
  std::string why;
};

bool lightNeutral(int r, int g, int b, float a) {
  const float lum = (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b)) / 255.0f;
  return lum >= 0.6f && std::max({r, g, b}) - std::min({r, g, b}) < 48 && a >= 0.35f;
}

bool uiFile(const std::string& rel) {
  if (rel.rfind("src/app/", 0) == 0 || rel.rfind("src/plugin/", 0) == 0) return true;
  if (rel.rfind("src/packs/", 0) != 0) return false;
  const std::string name = fs::path(rel).filename().string();
  return name.find("_ui") != std::string::npos || name.find("_widgets") != std::string::npos || name.find("_skin") != std::string::npos ||
         name.find("_editor") != std::string::npos || name.find("ui.") != std::string::npos;
}

// Light neutral literals in one line (comments cut).
int countLine(std::string s, std::string* first) {
  if (const auto c = s.find("//"); c != std::string::npos) s.resize(c);
  static const std::regex col32(R"(IM_COL32\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*(\d+)\s*)?\))");
  static const std::regex fn(R"(\b(?:rgb|hex|litRole|content|lit)\(\s*0x([0-9A-Fa-f]{6})\b\s*(?:,\s*([0-9.]+)f?)?)");
  static const std::regex vec4(R"(ImVec4\(\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*\))");
  int n = 0;
  auto hit = [&](const std::smatch& m) {
    if (!n && first) *first = m.str(0);
    ++n;
  };
  for (auto it = std::sregex_iterator(s.begin(), s.end(), col32); it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    const float a = m[4].matched ? std::stof(m[4].str()) / 255.0f : 1.0f;
    if (lightNeutral(std::stoi(m[1].str()), std::stoi(m[2].str()), std::stoi(m[3].str()), a)) hit(m);
  }
  for (auto it = std::sregex_iterator(s.begin(), s.end(), fn); it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    const unsigned v = static_cast<unsigned>(std::stoul(m[1].str(), nullptr, 16));
    const float a = m[2].matched ? std::stof(m[2].str()) : 1.0f;
    if (lightNeutral(static_cast<int>(v >> 16 & 255), static_cast<int>(v >> 8 & 255), static_cast<int>(v & 255), a)) hit(m);
  }
  for (auto it = std::sregex_iterator(s.begin(), s.end(), vec4); it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    float c[4];
    bool unit = true;
    for (int k = 0; k < 4; ++k) c[k] = std::stof(m[k + 1].str()), unit = unit && c[k] <= 1.0f;
    if (unit && lightNeutral(static_cast<int>(c[0] * 255), static_cast<int>(c[1] * 255), static_cast<int>(c[2] * 255), c[3])) hit(m);
  }
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <source root> <allowlist.tsv>\n", argv[0]);
    return 2;
  }
  const fs::path root = argv[1];
  std::map<std::string, Allow> allow;
  {
    std::ifstream in(argv[2]);
    expect(static_cast<bool>(in), "allowlist readable");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      std::string path, max, why;
      if (!std::getline(ls, path, '\t') || !std::getline(ls, max, '\t')) continue;
      std::getline(ls, why);
      allow[path] = Allow{std::atoi(max.c_str()), why};
      expect(!why.empty(), ("allowlist gives a reason: " + path).c_str());
    }
  }
  // Self-check of the matcher on known lines.
  expect(countLine("dl->AddRect(a, b, IM_COL32(255, 255, 255, 255));", nullptr) == 1, "matcher: opaque white IM_COL32");
  expect(countLine("dl->AddRect(a, b, IM_COL32(255, 255, 255, 12));", nullptr) == 0, "matcher: a faint white sheen is not a bar");
  expect(countLine("ImU32 c = theme::rgb(0xE4ECEE);", nullptr) == 1, "matcher: light grey rgb()");
  expect(countLine("ImU32 c = theme::rgb(0xFFB547);", nullptr) == 0, "matcher: a lit role colour is not neutral");
  expect(countLine("ImU32 c = theme::rgb(0x262C30);", nullptr) == 0, "matcher: dark slate is not light");
  expect(countLine("ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1), x);", nullptr) == 1, "matcher: ImVec4 light grey");
  expect(countLine("ImU32 c = P.text;  // rgb(0xFFFFFF)", nullptr) == 0, "matcher: comments are skipped");

  std::map<std::string, std::pair<int, std::string>> found;
  int files = 0;
  for (const char* dir : {"src/app", "src/plugin", "src/packs"}) {
    for (const auto& e : fs::recursive_directory_iterator(root / dir)) {
      if (!e.is_regular_file()) continue;
      const std::string ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h" && ext != ".mm") continue;
      const std::string rel = fs::relative(e.path(), root).generic_string();
      if (!uiFile(rel)) continue;
      ++files;
      std::ifstream in(e.path());
      std::string line;
      int lineNo = 0;
      while (std::getline(in, line)) {
        ++lineNo;
        std::string first;
        const int n = countLine(line, &first);
        if (!n) continue;
        auto& f = found[rel];
        if (f.first == 0) f.second = std::to_string(lineNo) + ": " + first;
        f.first += n;
      }
    }
  }
  expect(files > 100, "scanned the UI sources");
  int total = 0;
  for (const auto& [rel, f] : found) {
    total += f.first;
    const auto a = allow.find(rel);
    if (a == allow.end()) {
      expect(false, ("hard-coded light colour (use a theme role): " + std::to_string(f.first) + " in " + rel + ", first at " + f.second).c_str());
    } else if (f.first > a->second.max) {
      expect(false, (rel + ": " + std::to_string(f.first) + " light literals, the allowlist allows " + std::to_string(a->second.max) +
                     " (use a theme role); first at " + f.second)
                        .c_str());
    } else if (f.first < a->second.max) {
      std::printf("note: %s now has %d (allowlist %d): lower its count\n", rel.c_str(), f.first, a->second.max);
    }
  }
  for (const auto& [rel, a] : allow)
    if (!found.count(rel)) std::printf("note: %s has none left: drop it from the allowlist\n", rel.c_str());
  std::printf("%d light neutral literals in %zu allowlisted files\n", total, found.size());
  return finish();
}
