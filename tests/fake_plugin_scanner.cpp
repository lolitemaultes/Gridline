// Gridline tests: a stand-in for the plugin scanner child (`gridline --scan-plugin <format> <entry>
// --out <file>`), so scan tests never load a real plugin. It answers from the entry's file name:
//   *hang*   never answers (a plugin stuck in a dialog or a loop): killed by the timeout;
//   *crash*  crashes (abort);
//   *slow*   answers after 1 s (well within the tests' 3 s timeout);
//   *shell*  a plugin holding 120 others (like a VST 2 shell), 40 ms each, telling the parent after
//            each one (<out>.progress grows), so a scan that takes longer than the timeout still ends;
//   *junk*   exits with an error and writes nothing (a file that is not a plugin);
//   else     one plugin after 20 ms.
// Every plugin it reports has 120 parameters, so the cache grows like a real collection's.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::string escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

std::string pluginJson(const std::string& format, const std::string& uid, const std::string& name, bool instrument) {
  std::string uidText = uid;
  if (format == "vst3") {  // 32 hex digits
    unsigned long long h = 1469598103934665603ull;
    for (char c : uid) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char b[40];
    std::snprintf(b, sizeof b, "%016llX%016llX", h, h * 31u + 7u);
    uidText = b;
  } else if (format == "vst2") {  // 8 hex digits
    unsigned h = 2166136261u;
    for (char c : uid) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    char b[16];
    std::snprintf(b, sizeof b, "%08X", h);
    uidText = b;
  }
  std::string j = "{\"uid\":\"" + escape(uidText) + "\",\"name\":\"" + escape(name) + "\",\"vendor\":\"Stress Test Audio\",\"version\":\"1.0\",";
  j += std::string("\"subCategories\":\"") + (instrument ? "Instrument|Synth" : "Fx|Reverb") + "\",\"instrument\":" + (instrument ? "true" : "false");
  j += ",\"eventInput\":true,\"inputs\":2,\"outputs\":2,\"sidechain\":0,\"latency\":0,\"params\":[";
  for (int k = 0; k < 120; ++k) {
    if (k) j += ',';
    j += "{\"id\":" + std::to_string(k) + ",\"title\":\"Parameter " + std::to_string(k) + " of " + escape(name) + "\",\"units\":\"dB\",\"def\":0.5,\"flags\":1}";
  }
  return j + "]}";
}

// K-150: waits by the clock the parent's timeout reads, in short steps. One long sleep can last
// far longer than asked on a busy virtual machine (a NetBSD CI VM slept 40 ms steps at 108 ms each),
// which made a slow but working plugin look hung.
void sleepMs(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now())
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(end - now, std::chrono::milliseconds(10)));
}

}  // namespace

int main(int argc, char** argv) {
  std::string format, entry, out;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--scan-plugin") && i + 2 < argc) {
      format = argv[++i];
      entry = argv[++i];
    } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    }
  }
  if (format.empty() || out.empty()) return 64;
  std::string name = entry;
  while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
  if (const size_t s = name.find_last_of("/\\"); s != std::string::npos) name = name.substr(s + 1);
  if (const size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);  // the same plugin in each format
  if (name.find("hang") != std::string::npos)
    for (;;) sleepMs(1000);
  if (name.find("crash") != std::string::npos) std::abort();
  if (name.find("junk") != std::string::npos) return 2;
  std::string classes;
  if (name.find("shell") != std::string::npos) {
    for (int i = 0; i < 120; ++i) {
      sleepMs(40);
      if (i) classes += ',';
      classes += pluginJson(format, name + "#" + std::to_string(i), name + " " + std::to_string(i), i % 3 == 0);
      std::ofstream(out + ".progress", std::ios::app) << '+';
    }
  } else {
    sleepMs(name.find("slow") != std::string::npos ? 1000 : 20);
    classes = pluginJson(format, name, name, name.find("inst") != std::string::npos);
  }
  std::ofstream f(out, std::ios::binary);
  f << "{\"format\":\"" << escape(format) << "\",\"bundle\":\"" << escape(entry) << "\",\"ok\":true,\"classes\":[" << classes << "]}";
  return f ? 0 : 3;
}
