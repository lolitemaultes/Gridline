#include "PluginCheck.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../core/Json.h"
#include "HostedPlugins.h"
#include "PluginScan.h"
#include "sandbox/Sandbox.h"

extern char** environ;

namespace fs = std::filesystem;

namespace gl::host {

namespace {

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (u < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", u);
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}

// ---------------------------------------------------------------- child

// One result line per check, flushed at once (a crash keeps what came before).
struct Reporter {
  FILE* f = nullptr;
  void check(const std::string& name, const char* status, const std::string& detail, double ms) {
    std::fprintf(f, "{\"check\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\",\"ms\":%.1f}\n", jsonEscape(name).c_str(), status,
                 jsonEscape(detail).c_str(), ms);
    std::fflush(f);
  }
  void begin(const std::string& name) {  // marks where a crash happened
    std::fprintf(f, "{\"begin\":\"%s\"}\n", jsonEscape(name).c_str());
    std::fflush(f);
  }
  void info(const std::string& key, const std::string& value) {
    std::fprintf(f, "{\"info\":\"%s\",\"value\":\"%s\"}\n", jsonEscape(key).c_str(), jsonEscape(value).c_str());
    std::fflush(f);
  }
};

// Runs f on a separate thread (the plugin's audio thread) and waits.
void onAudio(const std::function<void()>& f) {
  std::thread t(f);
  t.join();
}

struct Stats {
  bool finite = true;
  long denormals = 0;
  float peak = 0;
  double maxLoad = 0, sumLoad = 0;
  long blocks = 0;
  long midiOut = 0;
  std::string firstBad;
};

struct Feed {
  bool noise = false;
  bool notes = false;
  uint32_t seed = 12345;
  int64_t frame = 0;  // stream position
  int nextNote = 0;
  bool noteOn = false;
};

float nextNoise(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return (static_cast<float>((s >> 9) & 0x7FFF) / 16384.0f - 1.0f) * 0.25f;
}

// Processes one block of n frames with the feed's input; checks the outputs.
void runBlock(Backend& b, int n, double sr, Feed& fd, Stats& st, std::vector<float>* capture) {
  for (int c = 0; c < b.mainInputChannels(); ++c)
    if (float* d = b.inputChannel(c))
      for (int i = 0; i < n; ++i) d[i] = fd.noise ? nextNoise(fd.seed) : 0.0f;
  for (int c = 0; c < b.sidechainChannels(); ++c)
    if (float* d = b.sidechainChannel(c))
      for (int i = 0; i < n; ++i) d[i] = fd.noise ? nextNoise(fd.seed) * 0.5f : 0.0f;
  if (fd.notes && b.hasEventInput()) {
    // A note every 6000 frames, held 4000 frames; keys walk an arpeggio.
    for (int i = 0; i < n; ++i) {
      const int64_t f = fd.frame + i;
      if (f % 6000 == 0 || (fd.noteOn && f % 6000 == 4000)) {
        NoteEvent e;
        e.type = fd.noteOn ? NoteEvent::NoteOff : NoteEvent::NoteOn;
        e.offset = i;
        e.key = static_cast<int16_t>(48 + (fd.nextNote % 4) * 4);
        e.noteId = 1 + fd.nextNote;
        e.velocity = 0.8f;
        b.addEvent(e);
        if (fd.noteOn) ++fd.nextNote;
        fd.noteOn = !fd.noteOn;
      }
    }
  }
  Transport t;
  t.sampleRate = sr;
  t.tempo = 128;
  t.playing = true;
  t.projectQn = static_cast<double>(fd.frame) / sr * t.tempo / 60.0;
  t.barNumber = static_cast<int32_t>(t.projectQn / 4);
  t.barQn = t.barNumber * 4.0;
  t.projectSamples = fd.frame;
  t.continuousSamples = fd.frame;
  t.changed = fd.frame == 0;
  const double t0 = nowMs();
  b.process(n, t);
  const double load = (nowMs() - t0) / (n / sr * 1000.0);
  MidiOut m;
  while (b.popMidiOut(m)) ++st.midiOut;
  if (st.blocks > 8) st.maxLoad = std::max(st.maxLoad, load);  // after a warm-up
  st.sumLoad += load;
  ++st.blocks;
  for (int bus = 0; bus <= b.auxOutputCount(); ++bus)
    for (int c = 0; c < b.outputChannels(bus); ++c) {
      const float* o = b.outputChannel(bus, c);
      if (!o) continue;
      for (int i = 0; i < n; ++i) {
        const float x = o[i];
        if (!std::isfinite(x)) {
          if (st.finite) st.firstBad = "bus " + std::to_string(bus) + " channel " + std::to_string(c) + " frame " + std::to_string(fd.frame + i);
          st.finite = false;
        } else if (x != 0.0f && std::fpclassify(x) == FP_SUBNORMAL) {
          ++st.denormals;
        }
        st.peak = std::max(st.peak, std::fabs(x));
      }
      if (capture && bus == 0 && c == 0) capture->insert(capture->end(), o, o + n);
    }
  fd.frame += n;
}

std::string fmt(const char* f, double a) {
  char b[64];
  std::snprintf(b, sizeof b, f, a);
  return b;
}

std::string describe(const Stats& s) {
  std::string d = "peak " + fmt("%.4f", s.peak) + ", load avg " + fmt("%.1f%%", s.blocks ? 100.0 * s.sumLoad / s.blocks : 0.0) + " max " +
                  fmt("%.0f%%", 100.0 * s.maxLoad);
  if (s.denormals) d += ", " + std::to_string(s.denormals) + " denormal samples";
  if (s.midiOut) d += ", " + std::to_string(s.midiOut) + " MIDI out";
  if (!s.finite) d = "NaN/inf at " + s.firstBad + "; " + d;
  return d;
}

const char* verdict(const Stats& s) {
  if (!s.finite) return "fail";
  if (s.denormals > 0 || s.maxLoad > 1.0) return "warn";
  return "pass";
}

int childMain(const std::string& format, const std::string& entry, const std::string& uid, const std::string& out, bool editor) {
  FILE* f = std::fopen(out.c_str(), "w");
  if (!f) return 3;
  Reporter r{f};
  double t0 = nowMs();
  r.begin("instantiate");
  std::string err;
  std::unique_ptr<Backend> b = createBackend(format, entry, uid, &err);
  if (!b) {
    r.check("instantiate", "fail", err, nowMs() - t0);
    std::fclose(f);
    std::_Exit(2);
  }
  r.check("instantiate", "pass",
          std::string(b->format()) + ", " + std::to_string(b->params().size()) + " parameters, in " + std::to_string(b->mainInputChannels()) +
              " / out " + std::to_string(b->mainOutputChannels()) + (b->sidechainChannels() ? ", sidechain" : "") +
              (b->auxOutputCount() ? ", " + std::to_string(b->auxOutputCount()) + " extra outputs" : std::string()) +
              (b->hasEventInput() ? ", MIDI in" : "") + (b->hasEventOutput() ? ", MIDI out" : ""),
          nowMs() - t0);

  // Sample rates and block sizes, silence.
  const int kMax = 2048;
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    const std::string name = "silence @" + std::to_string(static_cast<int>(sr / 1000)) + (sr == 44100 ? ".1" : "") + " kHz";
    r.begin(name);
    t0 = nowMs();
    if (!b->prepare(sr, kMax)) {
      r.check(name, "fail", "prepare failed", nowMs() - t0);
      continue;
    }
    Stats st;
    Feed fd;
    onAudio([&] {
      for (int n : {32, 64, 128, 256, 512, 1024, 2048})
        for (int k = 0; k < 4; ++k) runBlock(*b, n, sr, fd, st, nullptr);
      uint32_t rs = 777;
      for (int k = 0; k < 48; ++k) {  // variable sizes, like an engine split at events
        rs = rs * 1664525u + 1013904223u;
        runBlock(*b, 1 + static_cast<int>((rs >> 8) % kMax), sr, fd, st, nullptr);
      }
    });
    r.check(name, verdict(st), "blocks 32..2048 + random sizes: " + describe(st), nowMs() - t0);
  }

  // Noise and notes at 48 kHz.
  const double sr = 48000;
  b->prepare(sr, kMax);
  const bool instrument = b->mainInputChannels() == 0 && b->hasEventInput();  // buses are known once prepared
  {
    r.begin("noise + notes");
    t0 = nowMs();
    Stats st;
    Feed fd;
    fd.noise = true;
    fd.notes = true;
    onAudio([&] {
      for (int k = 0; k < 120; ++k) runBlock(*b, 512, sr, fd, st, nullptr);
    });
    const char* v = verdict(st);
    std::string d = describe(st);
    if (st.peak == 0.0f && std::strcmp(v, "fail") != 0 && b->mainOutputChannels() > 0) {
      v = "warn";
      d = (instrument ? "no sound after notes (content / licence missing?): " : "silent output for noise input (licence / mode?): ") + d;
    }
    r.check("noise + notes", v, d, nowMs() - t0);
    r.info("cpu", fmt("%.2f", st.blocks ? st.sumLoad / st.blocks : 0.0));
    r.info("midiOut", std::to_string(st.midiOut));
  }

  // Latency and tail.
  r.check("latency / tail", b->latency() > static_cast<int>(sr) ? "warn" : "pass",
          "latency " + std::to_string(b->latency()) + " samples, tail " + std::to_string(b->tailSamples()) + " samples", 0);
  r.info("latency", std::to_string(b->latency()));

  // State: save → restore in a new instance → same state, same output.
  {
    r.begin("state round trip");
    t0 = nowMs();
    std::string data, data2;
    if (!b->getState(data)) {
      r.check("state round trip", "warn", "no state", nowMs() - t0);
    } else {
      std::unique_ptr<Backend> b2 = createBackend(format, entry, uid, &err);
      if (!b2 || !b2->prepare(sr, kMax) || !b2->setState(data)) {
        r.check("state round trip", "fail", b2 ? "restoring the state failed" : "second instance: " + err, nowMs() - t0);
      } else {
        b2->getState(data2);
        const bool same = data == data2;
        // Output bit-compare: both instances restored from the same state, same input.
        std::unique_ptr<Backend> b3 = createBackend(format, entry, uid, &err);
        std::string cmp = "not compared";
        bool identical = false;
        if (b3 && b3->prepare(sr, kMax) && b3->setState(data)) {
          std::vector<float> o2, o3;
          Stats s2, s3;
          Feed f2, f3;
          f2.noise = f3.noise = true;
          f2.notes = f3.notes = true;
          onAudio([&] {
            for (int k = 0; k < 48; ++k) runBlock(*b2, 512, sr, f2, s2, &o2);
            for (int k = 0; k < 48; ++k) runBlock(*b3, 512, sr, f3, s3, &o3);
          });
          float md = 0;
          for (size_t i = 0; i < std::min(o2.size(), o3.size()); ++i) md = std::max(md, std::fabs(o2[i] - o3[i]));
          identical = o2.size() == o3.size() && md == 0.0f;
          cmp = identical ? "outputs bit-identical" : "outputs differ (max " + fmt("%.2e", md) + "; random / free-running modulation?)";
        }
        const std::string d = std::to_string(data.size()) + " B (" + data.substr(0, data.find(':')) + "), restore " +
                              (same ? "identical" : "differs (" + std::to_string(data2.size()) + " B)") + "; " + cmp;
        r.check("state round trip", same && identical ? "pass" : "warn", d, nowMs() - t0);
        r.info("stateBytes", std::to_string(data.size()));
      }
    }
  }

  // Parameter sweep: each parameter to 0, 0.5 and 1.
  {
    r.begin("parameter sweep");
    t0 = nowMs();
    Stats st;
    Feed fd;
    fd.noise = true;
    fd.notes = true;
    std::vector<std::pair<uint32_t, double>> defaults;
    for (const auto& p : b->params())
      if (!(p.flags & kParamReadOnly)) defaults.push_back({p.id, b->paramNormalized(p.id)});
    const size_t cap = std::min<size_t>(defaults.size(), 3000);
    int mismatch = 0;
    for (size_t k = 0; k < cap; ++k) {
      for (double v : {0.0, 0.5, 1.0}) {
        onAudio([&] {
          b->addParamChange(defaults[k].first, v, 0);
          runBlock(*b, 128, sr, fd, st, nullptr);
        });
        b->setParamNormalized(defaults[k].first, v);  // the controller side, as the app's UI pump does
        b->idle();
      }
      const ParamInfo& pi = b->params()[static_cast<size_t>(std::max(0, b->paramIndex(defaults[k].first)))];
      if (pi.stepCount == 0 && std::fabs(b->paramNormalized(defaults[k].first) - 1.0) > 0.02) ++mismatch;
      onAudio([&] { b->addParamChange(defaults[k].first, defaults[k].second, 0); });
      b->setParamNormalized(defaults[k].first, defaults[k].second);
      if (!st.finite) break;
    }
    onAudio([&] { runBlock(*b, 128, sr, fd, st, nullptr); });
    std::string d = std::to_string(cap) + " parameters x {0, 0.5, 1}: " + describe(st);
    if (mismatch) d += "; " + std::to_string(mismatch) + " continuous parameters read back a different value";
    // Jumps to extreme values may cost a recalculation: CPU peaks are reported, not judged.
    r.check("parameter sweep", st.finite ? (st.denormals ? "warn" : "pass") : "fail", d, nowMs() - t0);
  }

  // Programs.
  if (b->programCount() > 1) {
    r.begin("program change");
    t0 = nowMs();
    const int pr = 1;
    const bool ok = b->selectProgram(pr);
    Stats st;
    Feed fd;
    fd.noise = fd.notes = true;
    onAudio([&] {
      for (int k = 0; k < 24; ++k) runBlock(*b, 512, sr, fd, st, nullptr);
    });
    r.check("program change", !ok ? "fail" : verdict(st),
            std::to_string(b->programCount()) + " programs, \"" + b->programName(pr) + "\": " + describe(st), nowMs() - t0);
  } else {
    r.check("program change", "skip", b->programCount() == 1 ? "a single program (nothing to switch to)" : "no programs", 0);
  }

  // Bypass.
  if (b->hasBypass()) {
    r.begin("bypass");
    t0 = nowMs();
    const bool ok = b->setBypass(true);
    Stats st;
    Feed fd;
    fd.noise = fd.notes = true;
    onAudio([&] {
      for (int k = 0; k < 16; ++k) runBlock(*b, 512, sr, fd, st, nullptr);
    });
    b->setBypass(false);
    onAudio([&] {
      for (int k = 0; k < 8; ++k) runBlock(*b, 512, sr, fd, st, nullptr);
    });
    r.check("bypass", !ok ? "fail" : verdict(st), describe(st), nowMs() - t0);
  } else {
    r.check("bypass", "skip", "no plugin bypass (the mixer slot bypasses it)", 0);
  }

  // Offline (export) mode.
  {
    r.begin("offline mode");
    t0 = nowMs();
    const bool known = b->setOffline(true);
    Stats st;
    Feed fd;
    fd.noise = fd.notes = true;
    onAudio([&] {
      for (int k = 0; k < 16; ++k) runBlock(*b, 1024, sr, fd, st, nullptr);
    });
    b->setOffline(false);
    r.check("offline mode", verdict(st), std::string(known ? "told the plugin" : "format has no offline notion") + ": " + describe(st), nowMs() - t0);
  }

  // Editor: open, idle, close, reopen — in a window that is never shown.
  if (!editor) {
    r.check("editor", "skip", "--no-editor", 0);
  } else if (!b->hasEditor()) {
    r.check("editor", "skip", "no editor of its own (generic editor)", 0);
  } else {
    r.begin("editor");
    t0 = nowMs();
    std::string d;
    const char* status = "pass";
    for (int round = 0; round < 2; ++round) {
      std::unique_ptr<Editor> e = b->createEditor();
      if (!e) {
        status = "fail";
        d = "createEditor failed (round " + std::to_string(round + 1) + ")";
        break;
      }
      int w = 640, h = 420;
      e->size(w, h);
      void* parent = offscreenEditorParent(w, h);
      e->requestResize = [](int, int) { return true; };
      if (!parent) {
        status = "skip";
        d = "no offscreen window on this platform";
        break;
      }
      if (!e->attach(parent)) {
        status = "fail";
        d = "attach failed";
        closeOffscreenEditorParent();
        break;
      }
      for (int k = 0; k < 75; ++k) {
        e->idle();
        b->idle();
        pumpOffscreenEvents(0.02);  // ~1.5 s open: web-view editors finish loading, like a user would see
      }
      int w2 = w, h2 = h;
      e->size(w2, h2);
      if (round == 0) d = std::to_string(w2) + "x" + std::to_string(h2) + (e->canResize() ? ", resizable" : "") + ", opened twice";
      e.reset();
      closeOffscreenEditorParent();
      pumpOffscreenEvents(0.2);
    }
    r.check("editor", status, d, nowMs() - t0);
  }

  r.begin("close");
  t0 = nowMs();
  b.reset();
  r.check("close", "pass", "", nowMs() - t0);
  std::fprintf(f, "{\"done\":true}\n");
  std::fclose(f);
  std::_Exit(0);  // skip module unload / static destructors
}

// ---------------------------------------------------------------- parent

struct Result {
  std::shared_ptr<const HostedClass> cls;
  std::string status = "pass";  // pass / warn / fail / crash / timeout
  std::vector<Json> checks;
  std::map<std::string, std::string> info;
  std::string where;  // crash / timeout: the check that was running
  double ms = 0;
};

Result runOne(const std::string& exe, const std::shared_ptr<const HostedClass>& c, double timeout, bool editor, const std::string& tmpDir,
              int serial) {
  Result res;
  res.cls = c;
  const std::string out = tmpDir + "/check-" + std::to_string(getpid()) + "-" + std::to_string(serial) + ".jsonl";
  std::vector<std::string> args = {exe, "plugin-check", "--child", c->format, c->bundle, c->uid, "--out", out};
  if (!editor) args.push_back("--no-editor");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = -1;
  const double t0 = nowMs();
  int rc = ::posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    res.status = "crash";
    res.where = "cannot start the check process";
    return res;
  }
  int st = 0;
  bool timedOut = false;
  for (;;) {
    pid_t w = ::waitpid(pid, &st, WNOHANG);
    if (w == pid) break;
    if (nowMs() - t0 > timeout * 1000.0) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &st, 0);
      timedOut = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  res.ms = nowMs() - t0;
  bool done = false;
  std::ifstream in(out);
  std::string line, current;
  while (std::getline(in, line)) {
    Json j;
    if (!jsonParse(line, j)) continue;
    if (j.find("done")) done = true;
    if (const Json* b = j.find("begin")) current = b->str;
    if (const Json* i = j.find("info")) res.info[i->str] = j.text("value");
    if (j.find("check")) {
      res.checks.push_back(j);
      const std::string s = j.text("status");
      if (s == "fail") res.status = "fail";
      else if (s == "warn" && res.status == "pass") res.status = "warn";
      current.clear();
    }
  }
  std::error_code ec;
  fs::remove(out, ec);
  if (timedOut) {
    res.status = "timeout";
    res.where = current.empty() ? "?" : current;
  } else if (!done && res.status != "fail") {
    res.status = "crash";
    res.where = (current.empty() ? std::string("?") : current) +
                (WIFSIGNALED(st) ? " (signal " + std::to_string(WTERMSIG(st)) + ")" : " (exit " + std::to_string(WEXITSTATUS(st)) + ")");
  }
  return res;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

int pluginCheckMain(int argc, char** argv) {
  // Child: plugin-check --child <format> <entry> <uid> --out <file> [--no-editor]
  if (argc >= 6 && std::string(argv[2]) == "--child") {
    std::string out;
    bool editor = true;
    for (int i = 6; i < argc; ++i) {
      if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
      else if (!std::strcmp(argv[i], "--no-editor")) editor = false;
    }
    if (out.empty()) return 1;
    return childMain(argv[3], argv[4], argv[5], out, editor);
  }
  std::vector<std::string> wanted;
  std::string json, onlyFormat;
  double timeout = 240;
  bool all = false, editor = true;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--all")) all = true;
    else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
    else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = std::max(5.0, std::atof(argv[++i]));
    else if (!std::strcmp(argv[i], "--no-editor")) editor = false;
    else if (!std::strcmp(argv[i], "--format") && i + 1 < argc) onlyFormat = argv[++i];
    else wanted.push_back(argv[i]);
  }
  std::vector<std::shared_ptr<const HostedClass>> classes;
  for (const auto& c : hostedClasses()) {
    if (!onlyFormat.empty() && c->format != onlyFormat) continue;
    bool take = all || (wanted.empty() && !onlyFormat.empty());
    for (const auto& w : wanted) take = take || c->type == w || lower(c->name).find(lower(w)) != std::string::npos;
    if (take) classes.push_back(c);
  }
  if (classes.empty()) {
    std::fprintf(stderr,
                 "plugin-check: nothing to check. Usage: gridline plugin-check [--all | <type | name>…] [--format vst3|clap|vst2|au]\n"
                 "                     [--json report.json] [--timeout seconds] [--no-editor]\n"
                 "(plugins come from the plugin cache: run `gridline scan-plugins` first)\n");
    return 1;
  }
  std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a->format != b->format ? a->format < b->format : lower(a->name) < lower(b->name); });
  const std::string exe = selfExecutablePath();
  std::error_code ec;
  const std::string tmp = fs::temp_directory_path(ec).string();
  std::vector<Result> results;
  int serial = 0;
  for (const auto& c : classes) {
    std::fprintf(stdout, "[%3d/%zu] %-5s %-34s ", serial + 1, classes.size(), c->formatLabel(), c->name.substr(0, 34).c_str());
    std::fflush(stdout);
    Result r = runOne(exe, c, timeout, editor, tmp, serial++);
    std::string notes;
    for (const auto& j : r.checks)
      if (j.text("status") == "fail" || j.text("status") == "warn") notes += (notes.empty() ? "" : "; ") + j.text("check") + ": " + j.text("detail");
    if (!r.where.empty()) notes = "in " + r.where + (notes.empty() ? "" : "; " + notes);
    std::fprintf(stdout, "%-7s %6.1fs  %s\n", r.status.c_str(), r.ms / 1000.0, notes.substr(0, 160).c_str());
    std::fflush(stdout);
    results.push_back(std::move(r));
  }
  int counts[5] = {};
  const char* names[5] = {"pass", "warn", "fail", "crash", "timeout"};
  for (const auto& r : results)
    for (int k = 0; k < 5; ++k) counts[k] += r.status == names[k];
  std::printf("\n%zu classes: %d pass, %d warn, %d fail, %d crash, %d timeout\n", results.size(), counts[0], counts[1], counts[2], counts[3], counts[4]);
  if (!json.empty()) {
    Json rep = Json::makeObject();
    Json arr = Json::makeArray();
    for (const auto& r : results) {
      Json o = Json::makeObject();
      o.object["type"] = Json::makeString(r.cls->type);
      o.object["name"] = Json::makeString(r.cls->name);
      o.object["vendor"] = Json::makeString(r.cls->vendor);
      o.object["format"] = Json::makeString(r.cls->formatLabel());
      o.object["kind"] = Json::makeString(r.cls->instrument ? "instrument" : "effect");
      o.object["status"] = Json::makeString(r.status);
      o.object["seconds"] = Json::makeNumber(std::round(r.ms) / 1000.0);
      if (!r.where.empty()) o.object["where"] = Json::makeString(r.where);
      Json ch = Json::makeArray();
      for (const auto& j : r.checks) ch.array.push_back(j);
      o.object["checks"] = std::move(ch);
      Json inf = Json::makeObject();
      for (const auto& [k, v] : r.info) inf.object[k] = Json::makeString(v);
      o.object["info"] = std::move(inf);
      arr.array.push_back(std::move(o));
    }
    rep.object["plugins"] = std::move(arr);
    Json sum = Json::makeObject();
    for (int k = 0; k < 5; ++k) sum.object[names[k]] = Json::makeNumber(counts[k]);
    rep.object["summary"] = std::move(sum);
    std::ofstream o(json, std::ios::binary);
    o << jsonStringify(rep, 1);
    std::printf("report: %s\n", json.c_str());
  }
  return counts[2] + counts[3] + counts[4] > 0 ? 2 : 0;
}

}  // namespace gl::host
