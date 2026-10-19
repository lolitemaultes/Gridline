// Gridline — J-24 tests: script sandbox and limits, piano roll script API and
// forms, the shipped piano roll and controller scripts, controller script
// hooks with simulated MIDI, the script console, templates, i18n hook, log
// and the new settings keys. Run from the source tree (assets/ is read).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/ControllerScript.h"
#include "core/Document.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/NoteTools.h"
#include "core/Params.h"
#include "core/PianoRollScript.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "core/Templates.h"

using namespace gl;
using namespace gl::script;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
  fs::path p = fs::temp_directory_path() / ("gridline-j24-" + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

void writeFile(const fs::path& p, const std::string& text) {
  std::ofstream os(p, std::ios::binary);
  os << text;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// Runs code in a fresh sandbox; returns the error ("" = success).
std::string runIn(const std::string& code, Limits lim = {}, const std::string& root = "", bool write = false) {
  Vm vm(lim, root, write);
  std::string err;
  if (vm.exec(code, "test", &err)) return "";
  return err.empty() ? std::string("?") : err;
}

// Value of `expr` printed by the sandbox.
std::string evalIn(const std::string& expr, const std::string& root = "", bool write = false) {
  Vm vm({}, root, write);
  std::string err;
  if (!vm.exec("print(" + expr + ")", "test", &err)) return "ERR " + err;
  return vm.log().empty() ? "" : vm.log().back();
}

// -------------------------------------------------------------- sandbox ----
void testSandbox() {
  for (const char* g : {"os", "io", "require", "dofile", "loadfile", "load", "package", "debug", "loadstring", "warn"})
    expect(evalIn(std::string("type(") + g + ")") == "nil", (std::string("sandbox: ") + g + " is not available").c_str());
  expect(evalIn("type(string.dump)") == "nil", "sandbox: string.dump removed");
  expect(evalIn("type(getmetatable('').__index.dump)") == "nil", "sandbox: string.dump unreachable through the string metatable");
  expect(evalIn("type(math.sin) .. type(table.sort) .. type(utf8.char) .. type(coroutine.wrap)") == "functionfunctionfunctionfunction",
         "sandbox: math / table / utf8 / coroutine present");
  expect(contains(runIn("collectgarbage('stop')"), "not available"), "sandbox: collectgarbage('stop') refused");
  expect(runIn("local k = collectgarbage('count'); assert(k > 0)").empty(), "sandbox: collectgarbage('count') allowed");
  // Binary chunks cannot be loaded even through the host's loader.
  {
    Vm vm;
    std::string err;
    expect(!vm.exec(std::string("\x1bLua", 4) + "junk", "bin", &err), "sandbox: binary chunk refused");
  }
  // files API without a root.
  expect(evalIn("files.read('x.txt')") == "nil\tno file access for this script", "files: no root = no access");
  fs::path root = tempDir("files");
  fs::path outside = tempDir("outside");
  writeFile(outside / "secret.txt", "secret");
  writeFile(root / "data.txt", "hello");
  const std::string r = root.string();
  expect(evalIn("files.read('data.txt')", r) == "hello", "files: read inside the folder");
  expect(contains(evalIn("files.read('../gridline-j24-outside/secret.txt')", r), "not allowed"), "files: '..' refused");
  expect(contains(evalIn("files.read('" + (outside / "secret.txt").string() + "')", r), "not allowed"), "files: absolute path refused");
  expect(contains(evalIn("files.read('sub/x.txt')", r), "not allowed"), "files: sub-folders refused");
  expect(contains(evalIn("files.read('.hidden')", r), "not allowed"), "files: hidden names refused");
  std::error_code ec;
  fs::create_symlink(outside / "secret.txt", root / "link.txt", ec);
  if (!ec) expect(contains(evalIn("files.read('link.txt')", r), "links"), "files: symlink out of the folder refused");
  expect(contains(evalIn("files.write('new.txt', 'x')", r, false), "may not write"), "files: read-only VM cannot write");
  expect(evalIn("files.write('new.txt', 'written')", r, true) == "true", "files: writable VM writes");
  expect(evalIn("files.read('new.txt')", r) == "written", "files: written file reads back");
  expect(evalIn("table.concat(files.list(), ',')", r) == "data.txt,new.txt", "files: list shows plain files only");
  expect(evalIn("files.exists('data.txt') and not files.exists('nope.txt')", r) == "true", "files: exists");
  expect(contains(evalIn("files.write('big.txt', string.rep('x', 2 * 1024 * 1024))", r, true), "too large"), "files: write size cap");
  // Output goes to the VM log, not stdout.
  {
    Vm vm;
    std::vector<std::string> sink;
    vm.logSink = [&](const std::string& l) { sink.push_back(l); };
    vm.exec("print('a', 1, true) log('b')", "t");
    expect(vm.log().size() == 2 && vm.log()[0] == "a\t1\ttrue" && sink.size() == 2, "print / log captured");
  }
  // Deterministic: same seed, same numbers; table order stable.
  expect(evalIn("math.random(1000000)") == evalIn("math.random(1000000)"), "math.random deterministic per VM");
  expect(evalIn("(function() local t = {alpha=1, beta=2, gamma=3, delta=4} local s = '' for k in pairs(t) do s = s .. k end return s end)()") ==
             evalIn("(function() local t = {alpha=1, beta=2, gamma=3, delta=4} local s = '' for k in pairs(t) do s = s .. k end return s end)()"),
         "pairs order stable across VMs");
  fs::remove_all(root, ec);
  fs::remove_all(outside, ec);
}

// --------------------------------------------------------------- limits ----
void testLimits() {
  Limits small;
  small.instructions = 200000;
  std::string e = runIn("while true do end", small);
  expect(contains(e, "instruction limit"), "limits: endless loop stopped by the instruction budget");
  Limits timed;
  timed.instructions = ~0ull;
  timed.seconds = 0.15;
  e = runIn("while true do end", timed);
  expect(contains(e, "time limit"), "limits: endless loop stopped by the time budget");
  e = runIn("while true do pcall(function() while true do end end) end", small);
  expect(contains(e, "instruction limit"), "limits: pcall cannot swallow the stop");
  e = runIn("local co = coroutine.wrap(function() while true do end end) co()", small);
  expect(contains(e, "instruction limit"), "limits: coroutines are limited too");
  Limits mem;
  mem.memoryBytes = 8u << 20;
  e = runIn("local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1000) .. i end", mem);
  expect(contains(e, "memory"), "limits: memory cap");
  e = runIn("local s = string.rep('x', 300000) return s:find('y')");
  expect(contains(e, "too long"), "limits: pattern matching on huge subjects refused");
  // A VM is reusable after a stopped call.
  Vm vm(small);
  std::string err;
  vm.exec("while true do end", "a", &err);
  expect(vm.exec("x = 1 + 1", "b", &err), "limits: budget resets for the next call");
  // Recursion depth is bounded by Lua itself (C stack overflow -> error, no crash).
  e = runIn("local function f() return 1 + f() end f()");
  expect(!e.empty(), "limits: runaway recursion is an error");
}

// ------------------------------------------------------- note API / forms ----
PianoRollContext sampleContext() {
  PianoRollContext c;
  c.ppq = 96;
  c.length = 96 * 4;
  Note a(0, 96, 60, 100);
  a.pan = -20;
  a.fine = 35;
  a.release = 90;
  a.modX = 10;
  a.modY = 200;
  a.color = 3;
  a.slide = true;
  Note b(0, 96, 64, 80);
  b.porta = true;
  Note d(192, 48, 67, 64);
  d.muted = true;
  c.notes = {a, b, d};
  c.sel = {false, true, false};
  Marker m;
  m.pos = 96;
  m.name = "Verse";
  m.timeSigNum = 3;
  m.timeSigDen = 4;
  c.markers = {m};
  c.channel = "Lead";
  c.pattern = "Pattern 1";
  c.seed = 7;
  return c;
}

PianoRollResult runSource(const std::string& src, const PianoRollContext& c, std::vector<FormParam> vals = {}) {
  PianoRollScript s;
  std::string err;
  if (!s.loadSource(src, "inline", &err)) {
    PianoRollResult r;
    r.error = "load: " + err;
    return r;
  }
  return s.run(c, vals);
}

void testNoteApi() {
  PianoRollContext c = sampleContext();
  PianoRollResult r = runSource("-- name: Nothing\nfunction apply(score, p) end", c);
  expect(r.ok && r.notes == c.notes && r.sel == c.sel, "note API: untouched notes round-trip with every property");
  expect(r.ok && !r.markersChanged && r.markers == c.markers, "note API: markers round-trip");
  r = runSource("for i, n in score:notes() do n.velocity = n.velocity + 1 end", c);  // top-level script
  expect(r.ok && r.notes[0].velocity == 101 && r.notes[2].velocity == 65 && r.notes[0].pan == -20, "note API: top-level script edits");
  r = runSource(R"(function apply(score)
    assert(score.ppq == 96 and score.length == 384 and score.bar == 384 and score.channel == 'Lead')
    assert(score:count() == 3 and score:hasSelection())
    local sel = score:selection()
    assert(#sel == 1 and sel[1] == 2 and score:get(2).pitch == 64)
    score:remove(1)
    score:add{time = 384, pitch = 72, velocity = 127, selected = true}
    score:get(1).selected = false
    score:addMarker{time = 0, name = 'Intro'}
  end)",
                c);
  expect(r.ok, ("note API: assertions inside the script hold " + r.error).c_str());
  expect(r.ok && r.notes.size() == 3 && r.notes.back().pitch == 72 && r.notes.back().start == 384 && r.notes.back().length == 24,
         "note API: add uses defaults (one step long)");
  expect(r.ok && r.sel == std::vector<bool>({false, false, true}), "note API: selection flags come back");
  expect(r.ok && r.markersChanged && r.markers.size() == 2 && r.markers[0].name == "Intro" && r.markers[1].timeSigNum == 3,
         "note API: markers added and sorted, other marker kept");
  r = runSource("function apply(score) score:get(1).pitch = 'high' end", c);
  expect(!r.ok && contains(r.error, "note 1") && contains(r.error, "pitch"), "note API: wrong field type reported");
  r = runSource("function apply(score) score:get(1).pitch = 500; score:get(1).time = -5; score:get(1).velocity = 0/0 end", c);
  expect(!r.ok && contains(r.error, "finite"), "note API: NaN refused");
  r = runSource("function apply(score) score:get(1).pitch = 500; score:get(1).time = -5 end", c);
  expect(r.ok && r.notes[1].pitch == 127 && r.notes[1].start == 0 && r.notes[0].pitch == 64, "note API: out-of-range values clamped (and re-sorted)");
  r = runSource("function apply(score) score:get(9) end", c);
  expect(!r.ok && contains(r.error, "no note at index 9"), "note API: bad index is an error");
  r = runSource("function apply(score) score._notes = 5 end", c);
  expect(!r.ok, "note API: replacing the list with junk is an error");
  // Scale helpers.
  PianoRollContext sc = c;
  sc.root = 2;  // D
  for (size_t i = 0; i < tools::scales().size(); ++i)
    if (std::string(tools::scales()[i].name) == "Major") sc.scale = static_cast<int>(i);
  r = runSource(R"(function apply(score)
    assert(score.scaleName == 'Major' and #score.scaleSteps == 7)
    assert(score:inScale(62) and score:inScale(66) and not score:inScale(65))
    assert(score:snap(65) == 64 and score:degree(62, 2) == 66 and score:degree(62, -1) == 61)
  end)",
                sc);
  expect(r.ok, ("note API: scale helpers " + r.error).c_str());
  // Determinism and seeds.
  const char* rnd = "function apply(score) for _, n in score:notes() do n.velocity = math.random(1, 127) end end";
  PianoRollResult r1 = runSource(rnd, c), r2 = runSource(rnd, c);
  PianoRollContext c2 = c;
  c2.seed = 8;
  PianoRollResult r3 = runSource(rnd, c2);
  expect(r1.ok && r1.notes == r2.notes, "note API: same seed, same result");
  expect(r3.ok && r3.notes != r1.notes, "note API: another seed, another result");
  // Script errors carry the line number.
  r = runSource("function apply(score)\n  error('boom')\nend", c);
  expect(!r.ok && contains(r.error, "inline:2") && contains(r.error, "boom"), "note API: error message with line number");
  r = runSource("function apply(score) print('hello', score:count()) end", c);
  expect(r.ok && r.log.size() == 1 && r.log[0] == "hello\t3", "note API: print goes to the result log");
  // Limits apply to scripts too.
  PianoRollScript loop;
  loop.loadSource("function apply() while true do end end", "loop");
  Limits lim;
  lim.instructions = 100000;
  r = loop.run(c, {}, lim);
  expect(!r.ok && contains(r.error, "instruction limit"), "note API: runaway script stopped");
}

void testForms() {
  const char* src = R"(-- name: Form test
-- category: Testing
-- description: A form with every control.
function form(f)
  f:description('Long text')
  f:knob('Amount', 2, 0, 1, 'clamped to max')
  f:knobInt('Count', 3.6, 1, 8)
  f:group('Group A')
  f:combo('Mode', {'One', 'Two', 'Three'}, 2)
  f:check('Flag', true)
  f:endGroup()
  f:text('Label', 'abc')
end
function apply(score, p)
  print(p.Amount, p.Count, p.Mode, tostring(p.Flag), p.Label)
end)";
  PianoRollScript s;
  std::string err;
  expect(s.loadSource(src, "form", &err), ("forms: script loads " + err).c_str());
  const Form& f = s.form();
  expect(s.name() == "Form test" && s.header().get("category") == "Testing", "forms: header parsed");
  expect(f.params.size() == 5 && f.description == "Long text", "forms: five controls and the description");
  if (f.params.size() == 5) {
    expect(f.params[0].kind == FormKind::Knob && f.params[0].value == 1 && f.params[0].hint == "clamped to max", "forms: knob clamped");
    expect(f.params[1].kind == FormKind::KnobInt && f.params[1].value == 4 && f.params[1].min == 1 && f.params[1].max == 8,
           "forms: integer knob rounded");
    expect(f.params[2].kind == FormKind::Combo && f.params[2].items.size() == 3 && f.params[2].value == 2 && f.params[2].group == "Group A",
           "forms: combo with group");
    expect(f.params[3].kind == FormKind::Check && f.params[3].value == 1 && f.params[3].group == "Group A", "forms: check box");
    expect(f.params[4].kind == FormKind::Text && f.params[4].text == "abc" && f.params[4].group.empty(), "forms: text field, group ended");
  }
  PianoRollResult r = s.run(sampleContext());
  expect(r.ok && r.log.size() == 1 && r.log[0] == "1.0\t4\t2\ttrue\tabc", "forms: defaults passed to apply");
  std::vector<FormParam> vals = f.params;
  vals[0].value = 0.25;
  vals[2].value = 3;
  vals[3].value = 0;
  vals[4].text = "xyz";
  vals[1].value = 99;  // clamped
  r = s.run(sampleContext(), vals);
  expect(r.ok && r.log.size() == 1 && r.log[0] == "0.25\t8\t3\tfalse\txyz", "forms: edited values passed (and clamped)");
  PianoRollScript bad;
  expect(!bad.loadSource("function form(f) f:knob('A', 0, 0, 1) f:knob('A', 0, 0, 1) end", "dup", &err) && contains(err, "duplicate"),
         "forms: duplicate names refused");
  expect(!bad.loadSource("function form(f) f:combo('C', {}, 1) end", "empty", &err) && contains(err, "at least one"), "forms: empty combo refused");
  expect(!bad.loadSource("function form(f) f:knob('K', 0/0, 0, 1) end", "nan", &err) && contains(err, "finite"), "forms: NaN refused");
  expect(!bad.loadSource("function form(f) while true do end end", "loop", &err) && contains(err, "limit"), "forms: endless form() stopped");
  expect(!bad.loadSource("this is not lua", "syntax", &err) && contains(err, "syntax"), "forms: syntax errors reported");
  Header h = parseHeader("-- name: A\n-- description: one\n-- description: two\n\n-- author: Me\nlocal x = 1\n-- late: no\n");
  expect(h.get("name") == "A" && h.get("description") == "one two" && h.get("author") == "Me" && h.get("late").empty(),
         "header: fields, continuation, stops at code");
}

// ---------------------------------------------------- shipped piano roll ----
// Synthetic pattern: two chords (C major, F major with mixed velocities) and a short melody.
PianoRollContext synthetic() {
  PianoRollContext c;
  c.ppq = 96;
  c.length = 96 * 4 * 2;
  for (int p : {48, 60, 64, 67}) c.notes.push_back(Note(0, 384, p, 90 + p % 7));
  int v = 70;
  for (int p : {53, 65, 69, 72}) c.notes.push_back(Note(384, 384, p, v)), v += 15;
  int mel[] = {84, 86, 88, 86, 84, 83, 84, 91};
  for (int i = 0; i < 8; ++i) c.notes.push_back(Note(i * 96, 90, mel[i], 100));
  std::vector<bool> sel(c.notes.size(), false);
  c.sel = sel;
  std::vector<size_t> order(c.notes.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(c.notes.begin(), c.notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  c.seed = 42;
  return c;
}

std::vector<FormParam> with(const Form& f, const std::map<std::string, double>& v) {
  std::vector<FormParam> out = f.params;
  for (auto& p : out)
    if (auto it = v.find(p.name); it != v.end()) p.value = it->second;
  return out;
}

int countPitch(const std::vector<Note>& n, int pitch) {
  return static_cast<int>(std::count_if(n.begin(), n.end(), [&](const Note& x) { return x.pitch == pitch; }));
}

void testShippedPianoRollScripts() {
  auto list = scanScripts({{"assets/scripts/pianoroll", false}});
  expect(list.size() >= 8 && list.size() <= 12, "shipped: 8..12 piano roll scripts");
  std::set<std::string> ids;
  for (const auto& s : list) ids.insert(s.id);
  for (const char* id : {"humanize", "euclidean", "chord_progression", "strum_by_velocity", "mirror_in_scale", "markov_melody",
                         "ratchet", "velocity_shaper", "bass_from_chords", "select_notes"})
    expect(ids.count(id) == 1, (std::string("shipped: ") + id + " present").c_str());
  std::map<std::string, PianoRollScript> scripts;
  for (const auto& info : list) {
    PianoRollScript s;
    std::string err;
    bool ok = s.loadFile(info.path, &err);
    expect(ok && !info.name.empty() && !info.description.empty() && info.category != "Other",
           ("shipped: " + info.id + " loads with name, category and description " + err).c_str());
    // Every script runs on the synthetic pattern with its defaults, deterministically.
    PianoRollResult a = s.run(synthetic()), b = s.run(synthetic());
    expect(a.ok && b.ok && a.notes == b.notes && a.sel == b.sel, ("shipped: " + info.id + " runs deterministically " + a.error).c_str());
    scripts[info.id] = std::move(s);
  }
  const PianoRollContext syn = synthetic();
  auto run = [&](const std::string& id, const std::map<std::string, double>& v = {}, PianoRollContext c = synthetic()) {
    const PianoRollScript& s = scripts[id];
    return s.run(c, with(s.form(), v));
  };

  // Humanize: same notes, moved a little, velocities valid, different with another seed.
  {
    PianoRollResult r = run("humanize");
    bool small = r.notes.size() == syn.notes.size();
    int changed = 0;
    for (size_t i = 0; small && i < r.notes.size(); ++i) {
      small = small && r.notes[i].velocity >= 1 && r.notes[i].velocity <= 127 && r.notes[i].length >= 1;
      changed += !(r.notes[i] == syn.notes[i]);
    }
    expect(r.ok && small && changed > 0, "humanize: notes nudged, values in range");
    PianoRollContext c = synthetic();
    c.seed = 43;
    expect(run("humanize", {}, c).notes != r.notes, "humanize: new seed = new variation");
  }
  // Euclidean: defaults over a 2-bar pattern -> voice 1: 4 per 16 steps.
  {
    PianoRollResult r = run("euclidean");
    expect(r.ok && countPitch(r.notes, 36) == 8 && countPitch(r.notes, 38) == 12 && countPitch(r.notes, 42) == 14,
           "euclidean: hit counts per voice");
    expect(r.ok && r.notes.front().start == 0 && countPitch(r.notes, 84) == 0, "euclidean: replaces the notes, starts on the downbeat");
    // 3 of 8: steps 0, 3, 6 (evenly spread, rotation 0).
    std::vector<Tick> starts;
    for (const auto& n : r.notes)
      if (n.pitch == 38 && n.start < 8 * 24) starts.push_back(n.start);
    expect(starts == std::vector<Tick>({0, 72, 144}), "euclidean: 3 hits over 8 steps land on 0, 3, 6");
    PianoRollResult rot = run("euclidean", {{"Rotate 2", 1}});
    starts.clear();
    for (const auto& n : rot.notes)
      if (n.pitch == 38 && n.start < 8 * 24) starts.push_back(n.start);
    expect(starts.size() == 3 && starts != std::vector<Tick>({0, 72, 144}), "euclidean: rotation moves the hits");
  }
  // Chord progression: 4 bars of I-V-vi-IV in C with a bass note.
  {
    PianoRollResult r = run("chord_progression");
    expect(r.ok && r.notes.size() == 16, "chords: four chords of three notes plus bass");
    std::vector<int> first;
    for (const auto& n : r.notes)
      if (n.start == 0) first.push_back(n.pitch);
    std::sort(first.begin(), first.end());
    expect(first == std::vector<int>({48, 60, 64, 67}), "chords: first chord is C major with bass");
    std::vector<int> second;
    for (const auto& n : r.notes)
      if (n.start == 384) second.push_back(n.pitch);
    std::sort(second.begin(), second.end());
    expect(second == std::vector<int>({55, 67, 71, 74}), "chords: second chord is G major");
    PianoRollResult m = run("chord_progression", {{"Progression", 7}, {"Mode", 2}, {"Key", 11}, {"Bass note", 0}, {"Sevenths", 1}});
    std::vector<int> am;
    for (const auto& n : m.notes)
      if (n.start == 0) am.push_back(n.pitch);
    std::sort(am.begin(), am.end());
    expect(am == std::vector<int>({69, 72, 76, 79}), "chords: A minor seventh in A minor");
  }
  // Strum by velocity: in the F chord (velocities 70, 85, 100, 115) the loudest starts first.
  {
    PianoRollResult r = run("strum_by_velocity");
    std::map<int, Tick> st;
    for (const auto& n : r.notes)
      if (n.start >= 384 && (n.pitch == 53 || n.pitch == 65 || n.pitch == 69 || n.pitch == 72)) st[n.pitch] = n.start;
    expect(r.ok && st.size() == 4 && st[72] == 384 && st[53] > st[65] && st[65] > st[69] && st[69] > st[72],
           "strum: louder notes earlier");
  }
  // Mirror in scale: around the middle of a C-major line.
  {
    PianoRollContext c;
    c.ppq = 96;
    c.notes = {Note(0, 96, 60, 100), Note(96, 96, 62, 100), Note(192, 96, 64, 100), Note(288, 96, 65, 100)};
    c.sel.assign(4, false);
    PianoRollResult r = run("mirror_in_scale", {{"Scale", 2}, {"Axis", 4}, {"Fixed axis", 62}}, c);
    std::vector<int> p;
    for (const auto& n : r.notes) p.push_back(n.pitch);
    expect(r.ok && p == std::vector<int>({64, 62, 60, 59}), "mirror: C D E F around D -> E D C B");
    PianoRollResult twice = scripts["mirror_in_scale"].run(
        [&] {
          PianoRollContext d = c;
          d.notes = r.notes;
          return d;
        }(),
        with(scripts["mirror_in_scale"].form(), {{"Scale", 2}, {"Axis", 4}, {"Fixed axis", 62}}));
    expect(twice.ok && twice.notes == c.notes, "mirror: mirroring twice restores the melody");
  }
  // Markov melody: new notes in range and in C major, deterministic, seed-dependent.
  {
    PianoRollContext c = synthetic();
    c.scale = 0;
    PianoRollResult r = run("markov_melody", {{"Scale", 2}, {"Key", 2}}, c);
    bool ok = r.ok && r.notes.size() > 4;
    const int major[] = {0, 2, 4, 5, 7, 9, 11};
    for (const auto& n : r.notes) {
      ok = ok && n.pitch >= 48 && n.pitch <= 84 && n.start < 2 * 384;
      ok = ok && std::find(std::begin(major), std::end(major), n.pitch % 12) != std::end(major);
    }
    expect(ok, "markov: melody in range, in key, within the bars");
    c.seed = 99;
    expect(run("markov_melody", {{"Scale", 2}, {"Key", 2}}, c).notes != r.notes, "markov: another seed, another melody");
    PianoRollContext empty;
    empty.ppq = 96;
    PianoRollResult e = run("markov_melody", {}, empty);
    expect(e.ok && !e.notes.empty(), "markov: works without notes to learn from");
  }
  // Ratchet: every note becomes four pieces.
  {
    PianoRollResult r = run("ratchet");
    expect(r.ok && r.notes.size() == syn.notes.size() * 4, "ratchet: four pieces per note");
    PianoRollContext c;
    c.ppq = 96;
    c.notes = {Note(0, 96, 60, 100)};
    c.sel = {false};
    PianoRollResult one = run("ratchet", {{"Pitch step", 2}}, c);
    expect(one.ok && one.notes.size() == 4 && one.notes[1].start == 24 && one.notes[3].pitch == 66 && one.notes[0].velocity == 100 &&
               one.notes[3].velocity == 50,
           "ratchet: timing, pitch steps and fade");
  }
  // Velocity shaper: ramp from Low to High over the selection.
  {
    PianoRollContext c = synthetic();
    for (size_t i = 0; i < c.notes.size(); ++i) c.sel[i] = c.notes[i].pitch >= 80;  // the melody
    PianoRollResult r = run("velocity_shaper", {}, c);
    int firstV = -1, lastV = -1;
    for (size_t i = 0; i < r.notes.size(); ++i)
      if (r.sel[i]) (firstV < 0 ? firstV : lastV) = r.notes[i].velocity, lastV = r.notes[i].velocity;
    expect(r.ok && firstV == 50 && lastV == 115, "velocity shaper: ramp from Low to High");
    bool chordsKept = true;
    for (size_t i = 0; i < r.notes.size(); ++i)
      if (!r.sel[i]) chordsKept = chordsKept && r.notes[i].velocity == c.notes[i].velocity;
    expect(chordsKept, "velocity shaper: unselected notes untouched");
  }
  // Bass from chords: roots an octave below the lowest chord notes, selected.
  {
    PianoRollContext c;
    c.ppq = 96;
    for (int p : {60, 64, 67}) c.notes.push_back(Note(0, 384, p, 90));
    for (int p : {57, 60, 64}) c.notes.push_back(Note(384, 384, p, 90));
    std::sort(c.notes.begin(), c.notes.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
    c.sel.assign(c.notes.size(), false);
    PianoRollResult r = run("bass_from_chords", {}, c);
    std::vector<int> bass;
    for (size_t i = 0; i < r.notes.size(); ++i)
      if (r.sel[i]) bass.push_back(r.notes[i].pitch);
    expect(r.ok && bass.size() == 8 && countPitch(r.notes, 48) == 4 && countPitch(r.notes, 45) == 4, "bass: quarter-note roots an octave down");
    PianoRollResult w = run("bass_from_chords", {{"Rhythm", 6}}, c);
    bool approach = false;
    for (const auto& n : w.notes) approach = approach || (n.start == 288 && n.pitch == 46);
    expect(w.ok && approach, "bass: walk approaches the next root");
  }
  // Select notes: every second note, then narrowed by pitch.
  {
    PianoRollResult r = run("select_notes");
    bool alt = r.ok && r.notes == syn.notes;
    for (size_t i = 0; alt && i < r.sel.size(); ++i) alt = r.sel[i] == (i % 2 == 0);
    expect(alt, "select: every 2nd note selected, notes unchanged");
    PianoRollContext c = synthetic();
    c.sel = r.sel;
    PianoRollResult n = run("select_notes", {{"Rule", 2}, {"From", 70}, {"To", 80}, {"Mode", 2}}, c);
    bool ok = n.ok;
    for (size_t i = 0; ok && i < n.sel.size(); ++i)
      ok = n.sel[i] == (r.sel[i] && n.notes[i].pitch >= 70 && n.notes[i].pitch <= 80);
    expect(ok, "select: narrow by pitch range");
  }
}

// -------------------------------------------------- controller scripts ----
void testControllerScripts() {
  Document doc(makeEmptySong());
  doc.addChannel("Kick", "gl.synth");
  doc.addChannel("Lead", "gl.synth");
  SimpleHost host(doc);
  ControllerScript cs;
  std::string err;
  bool ok = cs.loadFile("assets/scripts/controllers/mixer_transport.lua", host, &err);
  expect(ok && cs.active(), ("controller: mixer script loads " + err).c_str());
  expect(cs.match() == std::vector<std::string>({"Gridline 8-strip"}), "controller: match header");
  expect(host.out.size() == 35, "controller: onInit lights 32 strip LEDs and 3 transport LEDs");
  host.out.clear();
  bool handled = false;
  // Fader 1 to the top: insert 1 volume -> normalized 1.
  cs.onMidiMsg(MidiMessage{0xB0, 20, 127, 0}, &handled);
  ObjectId ins1 = doc.song().mixer[1].id;
  auto d = describeParam(doc.song(), mixerParam(ins1, MixerParam::Volume));
  expect(handled && d && std::fabs(d->toNormalized(doc.song().mixer[1].volume) - 1.0) < 1e-6, "controller: fader drives insert volume");
  cs.onMidiMsg(MidiMessage{0xB0, 31, 0, 0}, &handled);
  expect(handled && doc.song().mixer[2].pan < -0.99f, "controller: knob 2 pans insert 2 hard left");
  // Mute button 1 -> insert 1 muted, LED lit.
  host.out.clear();
  cs.onMidiMsg(MidiMessage{0x90, 0, 127, 0}, &handled);
  bool led = std::find(host.out.begin(), host.out.end(), std::vector<uint8_t>({0x90, 0, 127})) != host.out.end();
  expect(handled && doc.song().mixer[1].muted && led, "controller: mute button toggles and lights");
  cs.onMidiMsg(MidiMessage{0x90, 9, 127, 0}, &handled);
  expect(doc.song().mixer[2].solo, "controller: solo button");
  cs.onMidiMsg(MidiMessage{0x90, 18, 127, 0}, &handled);
  expect(host.armed(3), "controller: arm button");
  cs.onMidiMsg(MidiMessage{0x90, 27, 127, 0}, &handled);
  expect(host.mixerSel == 4 && !host.hints.empty(), "controller: select button + hint");
  cs.onMidiMsg(MidiMessage{0x90, 40, 127, 0}, &handled);
  expect(host.isPlaying && host.actions.back() == RemoteAction::PlayPause, "controller: play");
  cs.onMidiMsg(MidiMessage{0x90, 42, 127, 0}, &handled);
  expect(host.isRecording, "controller: record");
  cs.onMidiMsg(MidiMessage{0x90, 47, 127, 0}, &handled);
  cs.onMidiMsg(MidiMessage{0xB0, 20, 0, 0}, &handled);
  expect(doc.song().mixer[9].volume < 0.01f, "controller: bank right moves fader 1 to insert 9");
  cs.onMidiMsg(MidiMessage{0xB1, 20, 0, 0}, &handled);
  expect(!handled, "controller: other MIDI channel not handled");
  cs.onMidiMsg(MidiMessage{0x90, 60, 100, 0}, &handled);
  expect(!handled, "controller: unmapped notes pass through");
  host.out.clear();
  expect(cs.onRefresh(RefreshFlags::all()) && host.out.size() == 35, "controller: refresh relights strips and transport");
  // Undo works on script edits.
  size_t before = doc.history().position();
  cs.onMidiMsg(MidiMessage{0x90, 1, 127, 0}, &handled);
  expect(doc.history().position() == before + 1 && doc.song().mixer[10].muted, "controller: edits are undoable steps");
  doc.undo();
  expect(!doc.song().mixer[10].muted, "controller: undo restores");

  // Plugin knobs / pads script.
  SimpleHost h2(doc);
  h2.focus = {doc.song().channels[1].id, -1};
  ControllerScript pads;
  ok = pads.loadFile("assets/scripts/controllers/plugin_knobs_pads.lua", h2, &err);
  expect(ok, ("controller: pads script loads " + err).c_str());
  ParamId p1 = channelPluginParam(doc.song().channels[1].id, 1);  // encoder 2 = parameter 2 (a continuous level)
  auto pd = describeParam(doc.song(), p1);
  double v0 = pd->toNormalized(*getParam(doc.song(), p1));
  pads.onMidiMsg(MidiMessage{0xB0, 71, 118, 0}, &handled);  // 10 detents down
  double v1 = pd->toNormalized(*getParam(doc.song(), p1));
  expect(handled && v1 <= v0 && std::fabs(v1 - std::max(0.0, v0 - 10.0 / 128)) < 1e-3, "controller: encoder turns down (two's complement)");
  pads.onMidiMsg(MidiMessage{0xB0, 71, 5, 0}, &handled);  // 5 detents up
  double v2 = pd->toNormalized(*getParam(doc.song(), p1));
  expect(std::fabs(v2 - v1 - 5.0 / 128) < 1e-3, "controller: encoder nudges the focused plugin's parameter up");
  expect(!h2.hints.empty() && contains(h2.hints.back(), "="), "controller: parameter hint");
  pads.onMidiMsg(MidiMessage{0x99, 37, 100, 0}, &handled);
  expect(handled && h2.channelSel == 1, "controller: pad 2 selects channel 2");
  pads.onMidiMsg(MidiMessage{0x99, 50, 100, 0}, &handled);
  expect(handled && h2.channelSel == 1, "controller: pad beyond the channel count ignored");
  h2.isRecording = true;
  h2.out.clear();
  for (int i = 0; i < 50; ++i) pads.onIdle();
  int blinks = 0;
  for (const auto& m : h2.out) blinks += m.size() == 3 && m[0] == 0x99 && m[1] == 52;
  expect(blinks == 2, "controller: record LED blinks from onIdle");

  // Errors disable the script; limits stop runaway hooks.
  ControllerScript broken;
  broken.limits.instructions = 100000;
  ok = broken.loadSource("function onMidiMsg(m) error('bad') end function onIdle() while true do end end", "broken", host, &err);
  expect(ok, "controller: broken script loads");
  expect(!broken.onMidiMsg(MidiMessage{0x90, 1, 1, 0}, &handled) && !broken.active() && contains(broken.error(), "bad"),
         "controller: error disables the script");
  ControllerScript spin;
  spin.limits.instructions = 100000;
  spin.loadSource("function onIdle() while true do end end", "spin", host, &err);
  expect(!spin.onIdle() && contains(spin.error(), "limit"), "controller: endless onIdle stopped");
  ControllerScript sys;
  sys.loadSource("function onInit() device.sendSysex({0xF0, 0x7D, 0x01, 0xF7}) end", "sys", host, &err);
  expect(!host.sysex.empty() && host.sysex.back().size() == 4, "controller: sysex out");
  ControllerScript badSys;
  expect(!badSys.loadSource("device.sendSysex({1, 2, 3})", "badsys", host, &err) && contains(err, "0xF0"), "controller: malformed sysex refused");
  ControllerScript tr;
  tr.loadSource("transport.setTempo(128) transport.action('metronome') assert(transport.ppq() == 96)", "tr", host, &err);
  expect(std::fabs(doc.song().tempo - 128) < 1e-9 && host.actions.back() == RemoteAction::Metronome, "controller: tempo and named actions");

  // Console.
  Vm con;
  openControllerApi(con, host);
  std::string cerr;
  auto out = runConsole(con, "1 + 2", &cerr);
  expect(out.size() == 1 && out[0] == "3", "console: expressions print their value");
  out = runConsole(con, "x = mixer.count()", &cerr);
  out = runConsole(con, "x, channels.name(1)", &cerr);
  expect(out.size() == 1 && out[0] == "17\tKick", "console: statements keep state; API reachable");
  out = runConsole(con, "error('oops')", &cerr);
  expect(contains(cerr, "oops"), "console: errors reported");
}

// ------------------------------------------------------------ templates ----
void testTemplates() {
  fs::path root = tempDir("templates");
  Document d(makeEmptySong());
  d.addChannel("Kick", "gl.synth");
  Song s = d.song();
  s.tempo = 123;
  std::string path, err;
  expect(saveTemplate(root.string(), "Beats", "My beat", "A kick.\nSecond line.\n\nNot shown.", s, &path, &err),
         ("templates: save " + err).c_str());
  expect(saveTemplate(root.string(), "", "Loose", "", s, nullptr, &err), "templates: save without a category");
  expect(saveTemplate(root.string(), "Beats", "a/b:c", "", s, nullptr, &err), "templates: unsafe names made safe");
  auto list = scanTemplates({{root.string(), true}});
  expect(list.size() == 3, "templates: three found");
  const TemplateInfo* mine = nullptr;
  for (const auto& t : list)
    if (t.name == "My beat") mine = &t;
  expect(mine && mine->category == "Beats" && mine->description == "A kick. Second line." && mine->user, "templates: category and preview text");
  bool loose = false, safe = false;
  for (const auto& t : list) loose = loose || (t.name == "Loose" && t.category == "User"), safe = safe || t.name == "a-b-c";
  expect(loose && safe, "templates: root files are 'User'; names sanitized");
  Song back;
  expect(mine && loadTemplate(mine->path, back, &err) && back.channels.size() == 1 && back.channels[0].name == "Kick" &&
             std::fabs(back.tempo - 123) < 1e-9 && back.info.title == "Untitled",
         "templates: load gives an untitled copy");
  // Overriding: a user template with the same category/name replaces a shipped one.
  fs::path shipped = tempDir("shipped");
  saveTemplate(shipped.string(), "Beats", "My beat", "shipped", s, nullptr, &err);
  auto both = scanTemplates({{shipped.string(), false}, {root.string(), true}});
  int n = 0;
  for (const auto& t : both) n += t.name == "My beat";
  expect(n == 1, "templates: user copy overrides the shipped one");
  // Shipped templates.
  auto ship = scanTemplates({{"assets/templates", false}});
  expect(ship.size() >= 3 && ship.size() <= 6, "templates: 3..6 shipped templates");
  bool allLoad = true, allDescribed = true;
  std::set<std::string> cats;
  for (const auto& t : ship) {
    Song x;
    allLoad = allLoad && loadTemplate(t.path, x, &err);
    allDescribed = allDescribed && !t.description.empty();
    cats.insert(t.category);
  }
  expect(allLoad && allDescribed && cats.size() >= 2, "templates: shipped ones load, have preview text and categories");
  expect(!templateSummary(s).empty() && contains(templateSummary(s), "123 BPM"), "templates: summary line");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::remove_all(shipped, ec);
}

// --------------------------------------------------- i18n / log / settings ----
void testMisc() {
  std::string ps = i18n::pseudo("Save %d files##save");
  expect(contains(ps, "%d") && ps.size() > std::string("Save %d files##save").size() && ps.substr(ps.size() - 6) == "##save" && ps[0] == '[',
         "i18n: pseudo keeps ids and printf conversions");
  fs::path dir = tempDir("lang");
  writeFile(dir / "xx.json", R"({"name": "Test language", "strings": {"Audio": "Klang", "Save": "Sichern"}})");
  auto langs = i18n::available(dir.string());
  bool found = false;
  for (const auto& l : langs) found = found || (l.code == "xx" && l.name == "Test language");
  expect(langs.size() == 3 && found, "i18n: catalogues listed with English and pseudo");
  expect(i18n::setLanguage("xx", dir.string()) && std::string(i18n::tr("Audio")) == "Klang" &&
             std::string(i18n::tr("Save##btn")) == "Sichern##btn" && std::string(i18n::tr("Other")) == "Other",
         "i18n: translation, ids kept, fallback to English");
  std::string err;
  expect(!i18n::setLanguage("missing", dir.string(), &err) && i18n::language() == "en", "i18n: missing language falls back to English");
  i18n::setLanguage("pseudo", dir.string());
  expect(std::string(i18n::tr("Audio")) != "Audio", "i18n: pseudo locale active");
  i18n::setLanguage("en", dir.string());
  expect(std::string(i18n::tr("Audio")) == "Audio", "i18n: back to English");

  logging::clear();
  logging::setMaxLevel(logging::Level::Info);
  logging::info("test", "hello");
  logging::verbose("test", "dropped");
  logging::error("script", "bad");
  auto e = logging::entries();
  expect(e.size() == 2 && e[0].text == "hello" && e[1].level == logging::Level::Error, "log: levels filter");
  fs::path lf = dir / "log.txt";
  logging::setFile(lf.string());
  logging::warning("midi", "to file");
  logging::setFile("");
  std::ifstream is(lf);
  std::string line;
  std::getline(is, line);
  expect(contains(line, "to file") && contains(line, "warning"), "log: mirrored to a file");
  for (int i = 0; i < 5000; ++i) logging::info("x", "y");
  expect(logging::entries().size() == logging::kMaxEntries, "log: bounded");
  logging::clear();

  Settings st;
  expect(st.getString("general.startup") == "template" && st.getString("general.language") == "en" && st.getBool("scripts.enabled") &&
             st.getNumber("scripts.maxSeconds") > 0 && st.getNumber("project.defaultNum") == 4 && !st.getBool("backup.autosave") &&
             st.getNumber("debug.logLevel") == 2,
         "settings: J-24 keys registered with defaults");
  expect(st.set("general.defaultTemplate", std::string("Basics/Beat starter")) && !st.set("general.startup", 3.0),
         "settings: typed setters");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  testSandbox();
  testLimits();
  testNoteApi();
  testForms();
  testShippedPianoRollScripts();
  testControllerScripts();
  testTemplates();
  testMisc();
  return finish();
}
