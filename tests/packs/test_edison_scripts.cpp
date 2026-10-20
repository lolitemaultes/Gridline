// Gridline — J-57 tests: audio editor scripts (SC-05 / ED-09). Buffer vector
// operations, the audio object (selection, resize / insert at the cursor,
// markers, selection out), the shipped scripts' results, limits (instruction,
// time, memory), sandbox and one undo step in the editor history. Run from
// the source tree (assets/scripts/audio is read).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "../Check.h"
#include "packs/edison/edison.h"
#include "packs/edison/edison_script.h"

using namespace gl;
using namespace gl::packs::edison;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
double dsp_db(double db) { return std::pow(10.0, db / 20.0); }

Content sineContent(int64_t frames, double hz = 441, double amp = 0.5, int channels = 2) {
  Audio a = makeAudio(44100, channels, frames);
  for (int64_t i = 0; i < frames; ++i) {
    a.L[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2 * M_PI * hz * static_cast<double>(i) / 44100.0));
    a.R[static_cast<size_t>(i)] = channels == 1 ? a.L[static_cast<size_t>(i)] : static_cast<float>(-a.L[static_cast<size_t>(i)] * 0.5);
  }
  return makeContent(std::move(a));
}

AudioScriptResult runSrc(const std::string& src, const AudioScriptContext& ctx, script::Limits lim = {}) {
  AudioScript s;
  std::string err;
  if (!s.loadSource(src, "test", &err)) {
    AudioScriptResult r;
    r.error = "load: " + err;
    return r;
  }
  return s.run(ctx, {}, lim);
}

double peakOf(const std::vector<float>& v, int64_t a, int64_t b) {
  double p = 0;
  for (int64_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[static_cast<size_t>(i)])));
  return p;
}

std::string evalAudio(const std::string& expr) {
  AudioScriptContext ctx;
  ctx.content = sineContent(64);
  AudioScriptResult r = runSrc("function apply(audio) print(" + expr + ") end", ctx);
  if (!r.ok) return "ERR " + r.error;
  return r.log.empty() ? "" : r.log.back();
}

void testBufferOps() {
  expect(evalAudio("buffer.new(4, 0.5):sum()") == "2.0", "buffer: new + sum");
  expect(evalAudio("#buffer.new(10)") == "10", "buffer: length operator");
  expect(evalAudio("(function() local b = buffer.fromTable{1, -2, 3} return b[2] end)()") == "-2.0", "buffer: fromTable + index");
  expect(evalAudio("(function() local b = buffer.new(3) b[2] = 0.25 return b:get(2) end)()") == "0.25", "buffer: index assignment");
  expect(evalAudio("buffer.fromTable{1, 2, 3}:copy(2, 3):sum()") == "5.0", "buffer: copy range");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 2} b:scale(3) return b:sum() end)()") == "9.0", "buffer: scale");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 1} b:gainDb(-6.0206) return string.format('%.3f', b[1]) end)()") == "0.500",
         "buffer: gainDb");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 2, 3} b:offset(1, 2, 3) return b:sum() end)()") == "8.0", "buffer: offset on a range");
  expect(evalAudio("(function() local a, b = buffer.new(4), buffer.fromTable{1, 1} a:add(b, 2, 3) return table.concat(a:totable(), ',') end)()") ==
             "0.0,0.0,2.0,2.0",
         "buffer: add at a position with gain");
  expect(evalAudio("(function() local a = buffer.fromTable{2, 3} a:mul(buffer.fromTable{4, 5}) return a:sum() end)()") == "23.0", "buffer: mul");
  expect(evalAudio("(function() local b = buffer.new(5, 1) b:ramp(0, 1) return table.concat(b:totable(), ',') end)()") ==
             "0.0,0.25,0.5,0.75,1.0",
         "buffer: linear ramp hits both ends");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 2, 3} b:reverse() return b[1] end)()") == "3.0", "buffer: reverse");
  expect(evalAudio("(function() local b = buffer.fromTable{-1, 2} b:abs() b:invert() return b:sum() end)()") == "-3.0", "buffer: abs / invert");
  expect(evalAudio("(function() local b = buffer.fromTable{-3, 3} b:clip(-1, 1) return b[1] .. ' ' .. b[2] end)()") == "-1.0 1.0", "buffer: clip");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 0.1} b:softclip(2) return string.format('%.3f %.3f', b[1], b[2]) end)()") ==
             "1.000 0.205",
         "buffer: softclip keeps full scale at 1 and lifts quiet parts");
  expect(evalAudio("(function() local b = buffer.fromTable{0.3} b:quantize(2) return b[1] end)()") == "0.5", "buffer: quantize");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 3} local m = b:removeDc() return m .. ' ' .. b:sum() end)()") == "2.0 0.0",
         "buffer: removeDc");
  expect(evalAudio("buffer.fromTable{0.1, -0.9, 0.5}:peak()") == "0.89999997615814\t2", "buffer: peak and its index");
  expect(evalAudio("buffer.fromTable{1, -1, 1, -1}:rms()") == "1.0", "buffer: rms");
  expect(evalAudio("buffer.fromTable{1, -5, 3}:minmax()") == "-5.0\t3.0", "buffer: minmax");
  expect(evalAudio("table.concat(buffer.fromTable{-1, 1, -1, 1}:zeroCrossings('up'), ',')") == "2,4", "buffer: rising zero crossings");
  expect(evalAudio("#buffer.fromTable{-1, 1, -1, 1}:zeroCrossings()") == "3", "buffer: all zero crossings");
  expect(evalAudio("buffer.fromTable{1, 1, 1, -1, -1}:nearestZero(1)") == "4", "buffer: nearest zero crossing");
  expect(evalAudio("buffer.new(5, 1):nearestZero(2, 2)") == "nil", "buffer: no crossing in reach");
  expect(evalAudio("(function() local b = buffer.new(100) b:tone('sine', 441, 44100, 1) return string.format('%.4f %.4f', b[1], b[26]) end)()") ==
             "0.0000 1.0000",
         "buffer: sine tone (quarter period = 25 samples)");
  expect(evalAudio("(function() local b = buffer.new(100) return string.format('%.2f', b:tone('saw', 441, 44100)) end)()") == "0.00",
         "buffer: tone returns the end phase");
  expect(evalAudio("(function() local b = buffer.new(1000) b:noise('white', 1, 3) local c = buffer.new(1000) c:noise('white', 1, 3)"
                   " c:invert() c:add(b) return c:peak() .. ' ' .. tostring(b:rms() > 0.3) end)()") == "0.0 true",
         "buffer: noise is deterministic per seed");
  expect(evalAudio("(function() local b = buffer.new(44100) b:noise('pink', 1) local p = b:peak() return tostring(p > 0 and p <= 1) end)()") == "true",
         "buffer: pink noise in range");
  // Filters: a 10 kHz tone through a 200 Hz low-pass loses most of its level.
  expect(evalAudio("(function() local b = buffer.new(4410) b:tone('sine', 10000, 44100) b:biquad('lowpass', 200, 0.707, 0, 44100)"
                   " return tostring(b:rms() < 0.01) end)()") == "true",
         "buffer: biquad low-pass attenuates");
  expect(evalAudio("(function() local b = buffer.new(4410) b:tone('sine', 50, 44100) b:highpass(2000, 44100)"
                   " return tostring(b:rms() < 0.05) end)()") == "true",
         "buffer: one-pole high-pass attenuates");
  expect(evalAudio("(function() local b = buffer.new(4410) b:tone('sine', 1000, 44100) b:biquad('peak', 1000, 1, 12, 44100)"
                   " return tostring(b:peak() > 3.5) end)()") == "true",
         "buffer: peak EQ boosts");
  expect(evalAudio("(function() local b = buffer.new(10) b[1] = 1 b:delay(4, 0.5, 1) return b[5] .. ' ' .. b[9] end)()") == "0.5 0.25",
         "buffer: feedback delay");
  expect(evalAudio("(function() local b = buffer.new(100, 1) local e = b:envelope(0, 10, 44100) return string.format('%.2f', e[100]) end)()") ==
             "1.00",
         "buffer: envelope follower");
  expect(evalAudio("(function() local b = buffer.fromTable{1, 2} b:map(function(x, i) return x * i end) return b:sum() end)()") == "5.0",
         "buffer: map");
  expect(evalAudio("dsp.dbToGain(-20) .. ' ' .. dsp.gainToDb(10) .. ' ' .. dsp.noteToHz(69) .. ' ' .. dsp.hzToNote(880)") == "0.1 20.0 440.0 81.0",
         "dsp helpers");
  // Results stay finite.
  expect(evalAudio("(function() local b = buffer.new(2) b[1] = 1e300 b:scale(1e300) return b[1] end)()") == "1000000.0", "buffer: values clamped finite");
  expect(contains(evalAudio("buffer.new(2)[1] == nil or buffer.new(2):set(1, 0/0)"), "finite"), "buffer: NaN refused");
}

void testErrorsAndSandbox() {
  expect(contains(evalAudio("buffer.new(2):get(3)"), "out of range"), "errors: index out of range");
  expect(contains(evalAudio("buffer.new(-1)"), "length out of range"), "errors: negative length");
  expect(contains(evalAudio("buffer.new(3):tone('blip', 1, 44100)"), "sine, square"), "errors: unknown wave");
  expect(contains(evalAudio("buffer.new(3):biquad('lowpass', 100, 1, 0, 5)"), "sample rate"), "errors: bad rate");
  expect(contains(evalAudio("buffer.new(3):ramp(0, 1, 'wobbly')"), "curve"), "errors: unknown curve");
  expect(contains(evalAudio("buffer.new(3):add({})"), "gl.Buffer expected"), "errors: table is not a buffer");
  expect(evalAudio("getmetatable(buffer.new(1))") == "locked", "sandbox: buffer metatable locked");
  expect(evalAudio("getmetatable(audio)") == "locked", "sandbox: audio metatable locked");
  expect(contains(evalAudio("setmetatable(audio, {})"), "protected metatable"), "sandbox: audio metatable cannot be replaced");
  expect(contains(evalAudio("(function() buffer.new(1).x = 1 end)()"), "sample indices"), "sandbox: buffers take only samples");
  expect(evalAudio("type(_gl_read) .. type(_gl_audioMeta) .. type(io) .. type(os) .. type(load)") == "nilnilnilnilnil",
         "sandbox: no internals, io, os or load");
  // Budgets: C loops count against the instruction limit.
  AudioScriptContext ctx;
  ctx.content = sineContent(44100);
  script::Limits lim;
  lim.instructions = 2'000'000;
  AudioScriptResult r = runSrc("function apply(audio) local b = audio:channel(1) while true do b:scale(1) end end", ctx, lim);
  expect(!r.ok && r.limitHit && contains(r.error, "instruction limit"), "budget: endless vector loop stopped");
  r = runSrc("function apply(audio) local b = audio:channel(1) for i = 1, 100 do pcall(b.scale, b, 1) end while true do pcall(b.scale, b, 1) end end",
             ctx, lim);
  expect(!r.ok && r.limitHit, "budget: pcall cannot swallow the limit");
  lim.instructions = 1'000'000'000'000ull;
  lim.seconds = 0.2;
  r = runSrc("function apply(audio) local b = audio:channel(1) while true do b:biquad('lowpass', 100, 1, 0, 44100) end end", ctx, lim);
  expect(!r.ok && contains(r.error, "time limit"), "budget: time limit inside vector loops");
  lim = {};
  lim.memoryBytes = 8u << 20;
  r = runSrc("function apply(audio) local t = {} for i = 1, 1000 do t[i] = buffer.new(1000000) end end", ctx, lim);
  expect(!r.ok && contains(r.error, "memory"), "budget: memory cap");
  r = runSrc("function apply(audio) local b = audio:channel(1) b:map(function(x) while true do end end) end", ctx, {});
  expect(!r.ok && r.limitHit, "budget: map callbacks are budgeted");
  r = runSrc("function apply(audio) audio:setChannel(1, buffer.new(10)) end", ctx);
  expect(!r.ok && contains(r.error, "differ in length"), "errors: channels must keep equal lengths");
  r = runSrc("function apply(audio) audio._ch = 5 end", ctx);
  expect(!r.ok && contains(r.error, "_ch"), "errors: replaced channel table reported");
  r = runSrc("function apply(audio) audio._ch[2] = 'x' end", ctx);
  expect(!r.ok && contains(r.error, "not a buffer"), "errors: channel replaced by a string reported");
  r = runSrc("function apply(audio) error({}) end", ctx);
  expect(!r.ok && !r.changed && r.content.audio == ctx.content.audio, "errors: failed run leaves the audio alone");
}

void testAudioObject() {
  AudioScriptContext ctx;
  ctx.content = sineContent(1000);
  ctx.sel = {100, 300};
  AudioScriptResult r = runSrc("function apply(audio) print(audio.frames, audio.start, audio.channels, audio.selected, audio.fileFrames, audio.rate) end", ctx);
  expect(r.ok && r.log.size() == 1 && r.log[0] == "200\t100\t2\ttrue\t1000\t44100.0", "audio: fields");
  r = runSrc("function apply(audio) for c, b in audio:buffers() do b:fill(0) end end", ctx);
  const Audio& a = r.content.a();
  expect(r.ok && r.changed && peakOf(a.L, 100, 300) == 0 && peakOf(a.L, 0, 100) > 0.4 && peakOf(a.L, 300, 1000) > 0.4,
         "audio: only the selection changes");
  expect(r.out.a == 100 && r.out.b == 300 && r.sel.a == 100 && r.sel.b == 300, "audio: out / selection ranges");
  expect(r.preview.frames() == 200 && peakOf(r.preview.L, 0, 200) == 0, "audio: preview holds the processed frames");
  // No selection = whole file.
  ctx.sel = {};
  r = runSrc("function apply(audio) print(audio.frames, audio.selected) end", ctx);
  expect(r.ok && r.log[0] == "1000\tfalse" && !r.changed && r.content.audio == ctx.content.audio, "audio: whole file, unchanged result");
  // Resize: a longer replacement grows the file.
  ctx.sel = {100, 200};
  r = runSrc("function apply(audio) audio:resize(300) end", ctx);
  expect(r.ok && r.content.frames() == 1200 && r.out.b == 400, "audio: resize lengthens the file");
  // Generator at the cursor.
  AudioScript gen;
  gen.loadSource("-- target: cursor\nfunction apply(audio) audio:resize(50) for _, b in audio:buffers() do b:fill(0.25) end end", "g");
  ctx.sel = {};
  ctx.cursor = 500;
  r = gen.run(ctx);
  expect(r.ok && r.content.frames() == 1050 && std::fabs(r.content.a().L[520] - 0.25f) < 1e-6 && r.sel.a == 500 && r.sel.b == 550,
         "audio: target cursor inserts at the cursor");
  // Generating into an empty editor.
  AudioScriptContext empty;
  r = gen.run(empty);
  expect(r.ok && r.content.frames() == 50 && r.content.a().channels == 2, "audio: generates into an empty editor");
  // Reading context outside the selection.
  ctx.sel = {100, 200};
  r = runSrc("function apply(audio) local b = audio:read(1, 0, 100) print(#b, b:peak() > 0.4) end", ctx);
  expect(r.ok && r.log[0] == "100\ttrue", "audio: read outside the selection");
  // Markers and selection out.
  r = runSrc("function apply(audio) audio:addMarker(10, 'hit') audio:select(5, 20) end", ctx);
  expect(r.ok && r.changed && r.content.markers.size() == 1 && r.content.markers[0].pos == 110 && r.content.markers[0].name == "hit" &&
             r.sel.a == 105 && r.sel.b == 120,
         "audio: markers and selection");
  // A script without apply() works at the top level.
  r = runSrc("for _, b in audio:buffers() do b:fill(0) end", ctx);
  expect(r.ok && r.changed && peakOf(r.content.a().L, 100, 200) == 0, "audio: top-level script without apply");
  // Mono content keeps R == L.
  AudioScriptContext mono;
  mono.content = sineContent(100, 441, 0.5, 1);
  r = runSrc("function apply(audio) print(audio.channels) audio:channel(1):scale(0.5) end", mono);
  expect(r.ok && r.log[0] == "1" && r.content.a().channels == 1 && r.content.a().L == r.content.a().R, "audio: mono stays mono");
  // Form values reach apply.
  AudioScript s;
  s.loadSource("function form(f) f:knob('Gain', 0.5, 0, 1) end function apply(audio, p) audio:channel(1):fill(p.Gain) end", "f");
  std::vector<script::FormParam> v = s.form().params;
  v[0].value = 0.75;
  ctx.sel = {0, 10};
  r = s.run(ctx, v);
  expect(r.ok && std::fabs(r.content.a().L[5] - 0.75f) < 1e-6, "audio: form values");
}

AudioScript loadShipped(const std::string& id) {
  AudioScript s;
  std::string err;
  if (!s.loadFile((fs::path("assets") / "scripts" / "audio" / (id + ".lua")).string(), &err)) std::printf("# %s: %s\n", id.c_str(), err.c_str());
  return s;
}

std::vector<script::FormParam> with(const AudioScript& s, const std::string& name, double v) {
  std::vector<script::FormParam> p = s.form().params;
  for (auto& x : p)
    if (x.name == name) x.value = v;
  return p;
}

void testShippedScripts() {
  int n = 0;
  for (const auto& e : fs::directory_iterator(fs::path("assets") / "scripts" / "audio"))
    if (e.path().extension() == ".lua") {
      ++n;
      AudioScript s;
      std::string err;
      expect(s.loadFile(e.path().string(), &err) && !s.name().empty(), ("shipped: " + e.path().stem().string() + " loads " + err).c_str());
      AudioScriptContext ctx;
      ctx.content = sineContent(22050);
      ctx.sel = {1000, 12000};
      AudioScriptResult r = s.run(ctx);
      expect(r.ok, ("shipped: " + e.path().stem().string() + " runs " + r.error).c_str());
    }
  expect(n >= 6 && n <= 10, "shipped: 6 to 10 audio scripts");

  AudioScriptContext ctx;
  ctx.content = sineContent(10000);
  ctx.sel = {0, 10000};
  {
    AudioScript s = loadShipped("fade_in");
    AudioScriptResult r = s.run(ctx, with(s, "Curve", 1));
    expect(r.ok && r.content.a().L[0] == 0.f && std::fabs(r.content.a().L[9999] - ctx.content.a().L[9999]) < 1e-6 &&
               peakOf(r.content.a().L, 0, 1000) < 0.06,
           "fade in: silent start, full end");
  }
  {
    AudioScript s = loadShipped("fade_out");
    AudioScriptResult r = s.run(ctx);
    expect(r.ok && r.content.a().L[9999] == 0.f && peakOf(r.content.a().L, 9000, 10000) < 0.06 && peakOf(r.content.a().L, 0, 1000) > 0.45,
           "fade out: full start, silent end");
  }
  {
    AudioScript s = loadShipped("normalize");
    AudioScriptResult r = s.run(ctx, with(s, "Target", 0));
    expect(r.ok && std::fabs(peakOf(r.content.a().L, 0, 10000) - 1.0) < 1e-4 && std::fabs(peakOf(r.content.a().R, 0, 10000) - 0.5) < 1e-3,
           "normalize: peak to 0 dB, channels linked");
    r = s.run(ctx, with(s, "Channels separately", 1));
    expect(r.ok && std::fabs(peakOf(r.content.a().R, 0, 10000) - dsp_db(-1)) < 1e-3, "normalize: channels separately");
  }
  {
    AudioScript s = loadShipped("reverse_smooth");
    AudioScriptResult r = s.run(ctx, with(s, "Edge fade", 0));
    expect(r.ok && r.content.a().L[0] == ctx.content.a().L[9999] && r.content.a().L[9999] == ctx.content.a().L[0], "reverse: samples reversed");
    r = s.run(ctx);
    expect(r.ok && r.content.a().L[0] == 0.f && r.content.a().L[9999] == 0.f, "reverse: edge fades");
  }
  {
    AudioScript s = loadShipped("tone_generator");
    AudioScriptContext c;
    c.content = sineContent(1000);
    c.cursor = 1000;
    AudioScriptResult r = s.run(c, with(s, "Length", 0.5));
    expect(r.ok && r.content.frames() == 1000 + 22050 && s.atCursor(), "tone: inserted at the cursor");
    double expect12 = std::pow(10.0, -12.0 / 20.0);
    expect(std::fabs(peakOf(r.content.a().L, 1000, 23050) - expect12) < 1e-3, "tone: level in dB");
    // 440 Hz: count rising zero crossings over 0.5 s.
    int ups = 0;
    const auto& L = r.content.a().L;
    for (size_t i = 1001; i < 23050; ++i) ups += L[i - 1] < 0 && L[i] >= 0;
    expect(ups >= 218 && ups <= 221, "tone: frequency from the note");
  }
  {
    AudioScript s = loadShipped("noise_generator");
    AudioScriptContext c;
    c.content = sineContent(1000);
    c.sel = {0, 1000};
    AudioScriptResult r = s.run(c, with(s, "Colour", 1));
    const double p = peakOf(r.content.a().L, 0, 1000);
    expect(r.ok && r.content.frames() == 1000 && p > 0.02 && p <= std::pow(10.0, -18.0 / 20.0) + 1e-6 && r.content.a().L != r.content.a().R,
           "noise: replaces the selection at the level, channels differ");
  }
  {
    AudioScript s = loadShipped("zero_crossings");
    AudioScriptContext c;
    c.content = sineContent(10000, 441);  // period 100 samples: rising crossings at multiples of 100
    c.sel = {1030, 5070};
    AudioScriptResult r = s.run(c, with(s, "Mark crossings", 1));
    expect(r.ok && std::abs(r.sel.a - 1000) <= 1 && std::abs(r.sel.b - 5100) <= 1 && c.content.a().L[static_cast<size_t>(r.sel.a)] >= 0 &&
               c.content.a().L[static_cast<size_t>(r.sel.a) - 1] < 0,
           "zero crossings: selection snapped to rising crossings");
    expect(r.content.markers.size() >= 39 && r.content.markers.size() <= 41, "zero crossings: markers at the crossings");
    expect(r.content.audio == c.content.audio, "zero crossings: audio untouched");
  }
  {
    AudioScript s = loadShipped("tremolo");
    AudioScriptContext c;
    Audio dc = makeAudio(44100, 2, 44100);
    std::fill(dc.L.begin(), dc.L.end(), 1.f);
    std::fill(dc.R.begin(), dc.R.end(), 1.f);
    c.content = makeContent(dc);
    AudioScriptResult r = s.run(c, with(s, "Depth", 0.5));
    double mn = 2, mx = 0;
    for (float v : r.content.a().L) mn = std::min(mn, static_cast<double>(v)), mx = std::max(mx, static_cast<double>(v));
    expect(r.ok && std::fabs(mn - 0.5) < 0.01 && std::fabs(mx - 1.0) < 0.01, "tremolo: level swings between 1 - depth and 1");
  }
  {
    AudioScript s = loadShipped("stereo_width");
    AudioScriptResult r = s.run(ctx, with(s, "Width", 0));
    expect(r.ok && r.content.a().L == r.content.a().R, "stereo width: 0 = mono");
  }
  {
    AudioScript s = loadShipped("lofi");
    AudioScriptResult r = s.run(ctx);
    expect(r.ok && r.changed && peakOf(r.content.a().L, 0, 10000) <= 1.0001, "lofi: processed, in range");
  }
}

void testHistory() {
  // One script run = one step in the editor history, undone in one go.
  History h(sineContent(1000));
  AudioScript s = loadShipped("fade_in");
  AudioScriptContext ctx;
  ctx.content = h.cur();
  ctx.sel = {0, 1000};
  AudioScriptResult r = s.run(ctx);
  h.push("Script: " + s.name(), r.content);
  expect(h.labels().size() == 1 && h.canUndo(), "history: one step");
  const AudioPtr after = h.cur().audio;
  h.undo();
  expect(h.cur().audio == ctx.content.audio && h.cur().audio != after, "history: undo restores the audio");
}

}  // namespace

int main() {
  testBufferOps();
  testErrorsAndSandbox();
  testAudioObject();
  testShippedScripts();
  testHistory();
  return finish();
}
