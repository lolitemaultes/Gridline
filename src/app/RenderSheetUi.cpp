// Gridline: render MIDI to audio (K-254), the interface. See RenderSheetUi.h.
#include "RenderSheetUi.h"
#include "../core/ThreadName.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../collab/Controller.h"
#include "../collab/Session.h"
#include "../core/I18n.h"
#include "../core/PluginParams.h"
#include "../engine/ClipOps.h"
#include "../engine/Export.h"
#include "../engine/SampledInstrument.h"
#include "../host/HostedPlugins.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
#include "DialogLayout.h"
#include "ExportUi.h"
#include "HostedUi.h"
#include "LayoutShellUi.h"
#include "LayoutStyleUi.h"
#include "LogicPlugins.h"
#include "Theme.h"
#include "TrackerPages.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::renderui {

namespace sr = srcrender;
using sr::Dest;
using sr::Kind;

namespace {

constexpr int kRates[] = {44100, 48000, 88200, 96000};
// The destinations' ids in dev commands and "render state" (not shown to users).
const char* const kDestIds[] = {"audioclip", "replace", "slicer", "oneshot", "drumpad", "loupe", "library", "file"};
constexpr int kBits[] = {16, 24, 32};

// ---- the remembered options (the quick rows use them; the sheet starts from them) ----
struct Mem {
  bool loaded = false;
  int through[static_cast<int>(Kind::Count)] = {-1, -1, -1, -1};  // -1: the source's default
  int tail = -1;                // -1: the source's default (one-shots ring out)
  double maxTail = 4.0;
  int trim = -1;                // -1: one-shots trim, the rest not
  bool mute = true;
  bool normalize = false;
  int rate = -1;                // index in kRates, -1: the project's rate
  int bits = 2;                 // index in kBits (file destination)
  int sliceAt = 0;
  int dest = static_cast<int>(Dest::Slicer);
};
Mem gMem;

void loadMem(App& a) {
  if (gMem.loaded) return;
  gMem.loaded = true;
  const std::string t = a.settings.getString("render.lastOptions");
  size_t at = 0;
  while (at < t.size()) {
    size_t e = t.find(';', at);
    if (e == std::string::npos) e = t.size();
    const std::string kv = t.substr(at, e - at);
    at = e + 1;
    const size_t q = kv.find('=');
    if (q == std::string::npos) continue;
    const std::string k = kv.substr(0, q);
    const double v = std::atof(kv.c_str() + q + 1);
    const int iv = static_cast<int>(v);
    if (k == "tail") gMem.tail = std::clamp(iv, -1, static_cast<int>(TailMode::Count) - 1);
    else if (k == "maxTail") gMem.maxTail = std::clamp(v, 0.1, 30.0);
    else if (k == "trim") gMem.trim = std::clamp(iv, -1, 1);
    else if (k == "mute") gMem.mute = iv != 0;
    else if (k == "normalize") gMem.normalize = iv != 0;
    else if (k == "rate") gMem.rate = std::clamp(iv, -1, 3);
    else if (k == "bits") gMem.bits = std::clamp(iv, 0, 2);
    else if (k == "sliceAt") gMem.sliceAt = std::clamp(iv, 0, static_cast<int>(sr::SliceAt::Count) - 1);
    else if (k == "dest") gMem.dest = std::clamp(iv, 0, static_cast<int>(Dest::Count) - 1);
    else if (k.size() == 2 && k[0] == 'w' && k[1] >= '0' && k[1] <= '3') gMem.through[k[1] - '0'] = std::clamp(iv, -1, 2);
  }
}

void saveMem(App& a) {
  char b[256];
  std::snprintf(b, sizeof b, "tail=%d;maxTail=%.2f;trim=%d;mute=%d;normalize=%d;rate=%d;bits=%d;sliceAt=%d;dest=%d;w0=%d;w1=%d;w2=%d;w3=%d", gMem.tail,
                gMem.maxTail, gMem.trim, gMem.mute ? 1 : 0, gMem.normalize ? 1 : 0, gMem.rate, gMem.bits, gMem.sliceAt, gMem.dest, gMem.through[0],
                gMem.through[1], gMem.through[2], gMem.through[3]);
  a.settings.set("render.lastOptions", std::string(b));
}

int projectRate(const App& a) { return a.audioSampleRate > 0 ? a.audioSampleRate : 44100; }

// The options a source renders with, from the remembered ones.
sr::Options optionsFor(const App& a, const sr::Source& src) {
  sr::Options o = sr::defaultOptions(src);
  const int k = static_cast<int>(src.kind);
  if (gMem.through[k] >= 0) o.through = static_cast<sr::Through>(gMem.through[k]);
  if (gMem.tail >= 0 && !sr::isOneShot(src)) o.tail = static_cast<TailMode>(gMem.tail);
  o.maxTailSeconds = gMem.maxTail;
  if (gMem.trim >= 0) o.oneShotTrim = gMem.trim == 1;
  o.sampleRate = gMem.rate >= 0 ? kRates[gMem.rate] : projectRate(a);
  return o;
}

// ---- layout words ----
bool smooth(const App& a) { return logicplug::active(a); }
bool tracker(const App& a) { return trackerpages::active(a); }
const char* clipWord(const App& a) { return smooth(a) ? i18n::tr("audio region") : tracker(a) ? i18n::tr("sample in the song") : i18n::tr("audio clip"); }

std::string fmt(const char* f, const std::string& a) {
  char b[512];
  std::snprintf(b, sizeof b, f, a.c_str());
  return b;
}
std::string fmt2(const char* f, const std::string& a, const std::string& b2) {
  char b[512];
  std::snprintf(b, sizeof b, f, a.c_str(), b2.c_str());
  return b;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string channelName(const Song& s, ObjectId id) {
  const Channel* c = s.channel(id);
  return c ? c->name : std::string(i18n::tr("Channel"));
}
std::string patternName(const Song& s, ObjectId id) {
  const Pattern* p = s.pattern(id);
  return p ? p->name : std::string(i18n::tr("Pattern"));
}

// "Kick, steps in Break A", "Break A, every channel", "Break A clip 3", "Reese, 3 notes".
std::string sourceTitle(const Song& s, const sr::Source& src) {
  switch (src.kind) {
    case Kind::Channel: {
      if (src.channels.size() > 1)
        return fmt2(i18n::tr("%s rows of %s"), std::to_string(src.channels.size()), patternName(s, src.pattern));
      const Pattern* p = s.pattern(src.pattern);
      const ObjectId ch = src.channels.empty() ? kNoId : src.channels[0];
      bool steps = false, notes = false;
      if (p) {
        if (auto it = p->steps.find(ch); it != p->steps.end()) steps = std::any_of(it->second.begin(), it->second.end(), [](uint8_t v) { return v > 0; });
        notes = p->notes.count(ch) && !p->notes.at(ch).empty();
      }
      const char* f = steps && notes ? i18n::tr("%s, steps and notes in %s") : notes ? i18n::tr("%s, notes in %s") : i18n::tr("%s, steps in %s");
      return fmt2(f, channelName(s, ch), patternName(s, src.pattern));
    }
    case Kind::Pattern: return fmt(i18n::tr("%s, every channel"), patternName(s, src.pattern));
    case Kind::Clip: return sr::sourceName(s, src);
    case Kind::Notes:
      return channelName(s, src.channels.empty() ? kNoId : src.channels[0]) + ", " +
             i18n::plural(static_cast<long>(src.notes.size()), "%d note", "%d notes");
    default: return std::string();
  }
}

// The menu's heading: "THIS SEQUENCE (KICK, BREAK A)".
std::string menuHeadingText(const Song& s, const sr::Source& src) {
  switch (src.kind) {
    case Kind::Channel:
      if (src.channels.size() > 1) return upper(fmt2(i18n::tr("These rows (%s, %s)"), std::to_string(src.channels.size()), patternName(s, src.pattern)));
      return upper(fmt2(i18n::tr("This sequence (%s, %s)"), channelName(s, src.channels[0]), patternName(s, src.pattern)));
    case Kind::Pattern: return upper(fmt(i18n::tr("The whole pattern (%s)"), patternName(s, src.pattern)));
    case Kind::Clip: return upper(fmt(i18n::tr("This clip (%s)"), sr::sourceName(s, src)));
    case Kind::Notes: return upper(i18n::plural(static_cast<long>(src.notes.size()), "%d selected note", "%d selected notes"));
    default: return std::string();
  }
}

// ---- collaboration: what renders where ----
struct Busy {
  uint32_t kind = 0, id = 0;
};
Busy gBusy;

Busy busyOf(const sr::Source& src) {
  switch (src.kind) {
    case Kind::Channel:
    case Kind::Notes: return {1, static_cast<uint32_t>(src.channels.empty() ? kNoId : src.channels[0])};
    case Kind::Pattern: return {2, static_cast<uint32_t>(src.pattern)};
    case Kind::Clip: return {3, static_cast<uint32_t>(src.clip)};
    default: return {};
  }
}

// ---- the background job ----
struct JobExtra {
  bool normalize = false;
  std::string filePath;  // Dest::File
  int bits = 32;
  int libTag = -1;  // Dest::Library: -1 the automatic tag (sr::tagsAsLoop), 0 loop, 1 one-shot
  sr::PlaceOptions place;
};
bool gJobRunning = false;
ImVec4 devTagRect;    // the library tag chip last drawn (screen: x0 y0 x1 y1)
bool gDevGo = false;  // dev: press the sheet's Render button next frame
int gDevFinished = 0;
std::string gDevLast;

void finish(App& a, const sr::Source& src, const sr::Options& opts, const std::shared_ptr<sr::Output>& outp, const std::string& path, const JobExtra& ex);

void start(App& a, const sr::Source& src, const sr::Options& opts, const JobExtra& ex) {
  std::string why;
  if (!sr::renderable(a.doc.song(), src, &why)) {
    a.notify(why, true);
    return;
  }
  const std::string other = peerRendering(a, src);
  if (!other.empty()) {
    a.notify(fmt(i18n::tr("%s is rendering this already"), other), true);
    return;
  }
  captureHostedStates(a);  // instruments hosted from installed plugins: their state now
  const Song song = a.doc.song();
  const std::string dir = renderedDir(a);
  const Busy busy = busyOf(src);
  const bool started = exportui::startFileJob(a, i18n::N_("Rendering to audio"), song, [song, src, opts, ex, dir](const ExportControl& c) -> exportui::FileJobDone {
    sr::Options o = opts;
    o.cancel = c.cancel;
    o.progress = [&c](double p) {
      if (c.progress) c.progress(0.95 * p, i18n::N_("Rendering"));
    };
    auto out = std::make_shared<sr::Output>(sr::render(song, src, o));
    if (out->cancelled || c.cancelled())
      return [](App& app) {
        gBusy = {};
        gJobRunning = false;
        app.notify("Render cancelled");
      };
    if (!out->error.empty())
      return [e = out->error](App& app) {
        gBusy = {};
        gJobRunning = false;
        app.notify(e, true);
      };
    if (ex.normalize) normalizeBuffer(out->audio, -1.0);
    if (ex.place.dest == Dest::Library) out->oneShot = ex.libTag >= 0 ? ex.libTag == 1 : !sr::tagsAsLoop(song, *out);
    std::string err;
    const std::string path = sr::writeRender(*out, dir, &err);
    if (path.empty())
      return [err](App& app) {
        gBusy = {};
        gJobRunning = false;
        app.notify("The render could not be written: " + err, true);
      };
    if (ex.place.dest == Dest::File && !ex.filePath.empty()) {
      if (c.progress) c.progress(0.97, i18n::N_("Writing the file"));
      AudioFileOptions fo;
      std::string ext = fs::u8path(ex.filePath).extension().u8string();
      for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      fo.format = ext == ".flac" ? ExportFormat::Flac : ExportFormat::Wav;
      fo.bits = fo.format == ExportFormat::Flac ? std::min(ex.bits, 24) : ex.bits;
      fo.meta.tempo = out->tempo;
      fo.meta.beats = static_cast<int>(std::lround(static_cast<double>(out->length) / std::max(1, out->ppq)));
      fo.meta.loop = !out->oneShot;
      fo.cancel = c.cancel;
      std::string ferr;
      if (!writeAudioFile(ex.filePath, out->audio, out->sampleRate, fo, &ferr))
        return [ferr](App& app) {
          gBusy = {};
          gJobRunning = false;
          app.notify("Export failed: " + ferr, true);
        };
    }
    return [src, opts, out, path, ex](App& app) { finish(app, src, opts, out, path, ex); };
  });
  if (started) {
    gBusy = busy;
    gJobRunning = true;
  }
}

std::string resultText(const App& a, const Song& s, const sr::Source& src, const sr::Output& out, const sr::Placed& r, const JobExtra& ex) {
  const std::string what = src.kind == Kind::Channel && src.channels.size() == 1 ? channelName(s, src.channels[0]) : out.name;
  std::string t;
  switch (ex.place.dest) {
    case Dest::AudioClip: t = fmt2(i18n::tr("Rendered %s as an %s"), what, clipWord(a)); break;
    case Dest::Replace: t = fmt(i18n::tr("Replaced %s with its audio"), what); break;
    case Dest::Slicer:
      t = fmt(i18n::tr("Rendered %s into a slicer channel"), what) + ": " + i18n::plural(r.slices, "%d slice", "%d slices");
      if (r.pattern != kNoId) t += ", " + fmt(i18n::tr("pattern \"%s\" added"), patternName(s, r.pattern));
      break;
    case Dest::OneShot: t = fmt2(i18n::tr("Rendered %s as a one-shot sampler channel, root %s"), what, sampled::noteLabel(out.rootNote)); break;
    case Dest::DrumPad: t = fmt2(i18n::tr("Rendered %s onto pad %s"), what, std::to_string(r.pad + 1)) + " (" + channelName(s, r.channel) + ")"; break;
    case Dest::Loupe: t = fmt(i18n::tr("Opened the render of %s in Loupe"), what); break;
    case Dest::Library:
      t = fmt(out.oneShot ? i18n::tr("Saved %s to the project's Rendered folder, tagged one-shot") : i18n::tr("Saved %s to the project's Rendered folder, tagged loop"), what);
      break;
    case Dest::File: t = fmt(i18n::tr("Saved %s"), fs::u8path(ex.filePath).filename().u8string()); break;
    default: break;
  }
  if (ex.place.muteSource && sr::editsSong(ex.place.dest) && ex.place.dest != Dest::Replace) {
    t += ". ";
    t += src.kind == Kind::Clip ? i18n::tr("The clip is muted.")
         : src.kind == Kind::Pattern ? i18n::tr("The pattern's clips are muted.")
         : src.kind == Kind::Notes ? i18n::tr("The notes are muted.")
         : fmt(i18n::tr("The %s row is muted."), what);
  }
  return t;
}

void openInLoupe(App& a, const std::string& path, ObjectId channel) {
  if (logicplug::active(a)) {  // the Smooth layout edits audio in its lower pane
    a.settings.set("ui.loupePinned", true);
    layoutui::setRegionVisible(a, "logic.lower", true);
    a.showPiano = a.showRack = a.showMixer = false;
  }
  packs::openAudioEditor(a, path, channel);
}

void finish(App& a, const sr::Source& src, const sr::Options& opts, const std::shared_ptr<sr::Output>& outp, const std::string& path, const JobExtra& ex) {
  gBusy = {};
  gJobRunning = false;
  ++gDevFinished;
  const sr::Output& out = *outp;
  sr::Placed r;
  uint64_t serial = 0;
  if (sr::editsSong(ex.place.dest)) {
    std::string why;
    if (!sr::renderable(a.doc.song(), src, &why) && src.kind != Kind::Notes) {
      a.notify(i18n::tr("The source changed while it rendered; nothing was placed"), true);
      return;
    }
    a.doc.editSong("render to audio", [&](Song& x) { r = sr::place(x, src, out, path, ex.place); });
    if (!r.error.empty()) {
      a.doc.undo();  // nothing to keep (the edit made no change)
      a.notify(r.error, true);
      gDevLast = "error " + r.error;
      return;
    }
    serial = a.doc.history().stateSerial();
    if (r.channel != kNoId && ex.place.dest != Dest::AudioClip && ex.place.dest != Dest::Replace) a.selectChannel(r.channel);
    if (!r.clips.empty()) a.playlist.selected = std::set<ObjectId>(r.clips.begin(), r.clips.end());
    else if (r.clip != kNoId) a.playlist.selected = {r.clip};
  } else if (ex.place.dest == Dest::Loupe) {
    openInLoupe(a, path, kNoId);
  } else if (ex.place.dest == Dest::Library) {
    browserui::showProjectTab(a);
  }
  gDevLast = std::string("done ") + kDestIds[static_cast<int>(ex.place.dest)] + " channel " + std::to_string(r.channel) + " clip " + std::to_string(r.clip) +
             " slices " + std::to_string(r.slices) + " frames " + std::to_string(out.audio.frames) + (out.oneShot ? " tag oneshot" : " tag loop");
  const std::string text = resultText(a, a.doc.song(), src, out, r, ex);
  std::vector<Toast::Action> acts;
  if (serial != 0)
    acts.push_back({i18n::N_("Undo"), [&a, serial] {
                      if (a.doc.history().stateSerial() == serial) a.doc.undo();
                      else a.notify("Other edits came after the render: use Edit > Undo");
                    }});
  if (ex.place.dest != Dest::Loupe) acts.push_back({i18n::N_("Open in Loupe"), [&a, path] { openInLoupe(a, path, kNoId); }});
  acts.push_back({i18n::N_("Options..."), [&a, src] { openSheet(a, src); }});
  a.notifyAction(text, std::move(acts), 12.0);
  (void)opts;
}

// ---- the sheet ----
struct Preview {
  std::thread worker;
  std::atomic<bool> running{false}, cancel{false};
  std::mutex mu;
  std::shared_ptr<sr::Output> out;  // the finished preview
  std::string key;                  // what it shows
  std::string want;                 // what it should show
};
Preview& preview() {
  static Preview p;
  return p;
}

struct Sheet {
  bool open = false;
  bool appearing = false;
  int fitFrames = 0;        // frames left to size the window to its content
  float fitW = 0, fitH = 0;  // the content's size last frame
  sr::Source src;           // the source the menu opened on
  sr::Source shown;         // the scope picked in the sheet (the source, the rows, the pattern)
  int scope = 0;            // 0 the source, 1 the selected rows / clip, 2 the whole pattern
  sr::Options opts;
  Dest dest = Dest::Slicer;
  int sliceAt = 0;
  bool mute = true;
  bool normalize = false;
  bool remember = false;
  int rate = 0, bits = 2;
  int lengthMode = 0;       // 0 the span, 1 loops
  int loops = 1;
  int libTag = -1;          // the library's tag: -1 automatic, 0 loop, 1 one-shot (the chip flipped)
};
Sheet gSheet;

void stopPreview() {
  Preview& p = preview();
  p.cancel = true;
  if (p.worker.joinable()) {
    host::serveUiThreadTasksWhile(p.running);
    p.worker.join();
  }
  p.cancel = false;
}

std::string previewKey(const Sheet& sh) {
  char b[256];
  std::snprintf(b, sizeof b, "%d|%d|%d|%d|%.2f|%d|%d|%d", static_cast<int>(sh.shown.kind), static_cast<int>(sh.opts.through), sh.opts.loops,
                static_cast<int>(sh.opts.tail), sh.opts.maxTailSeconds, sh.opts.oneShotTrim ? 1 : 0, sh.opts.sampleRate, sh.normalize ? 1 : 0);
  std::string k = b;
  for (ObjectId id : sh.shown.channels) k += "," + std::to_string(id);
  k += "|" + std::to_string(sh.shown.pattern) + "|" + std::to_string(sh.shown.clip);
  for (int n : sh.shown.notes) k += ";" + std::to_string(n);
  return k;
}

void startPreview(App& a, const Sheet& sh) {
  Preview& p = preview();
  stopPreview();
  {
    std::lock_guard<std::mutex> lk(p.mu);
    p.out.reset();
    p.key.clear();
  }
  p.want = previewKey(sh);
  captureHostedStates(a);
  const Song song = a.doc.song();
  sr::Options o = sh.opts;
  o.cancel = &p.cancel;
  o.progress = {};
  const sr::Source src = sh.shown;
  const bool norm = sh.normalize;
  const std::string key = p.want;
  p.running = true;
  p.worker = std::thread([song, src, o, norm, key]() { gl::threads::setName("gl-render-prev");
    Preview& pv = preview();
    {
      host::UiThreadRenderScope scope;
      auto out = std::make_shared<sr::Output>(sr::render(song, src, o));
      if (norm && out->error.empty()) normalizeBuffer(out->audio, -1.0);
      if (!pv.cancel.load()) {
        std::lock_guard<std::mutex> lk(pv.mu);
        pv.out = out;
        pv.key = key;
      }
    }
    pv.running = false;
  });
}

void initSheet(App& a, const sr::Source& src) {
  loadMem(a);
  Sheet& sh = gSheet;
  sh.src = src;
  sh.shown = src;
  sh.scope = 0;
  sh.opts = optionsFor(a, src);
  sh.dest = static_cast<Dest>(gMem.dest);
  if (sh.dest == Dest::DrumPad && sr::drumPadChannel(a.doc.song()) == kNoId) sh.dest = Dest::Slicer;
  if (sh.dest == Dest::Replace && src.kind == Kind::Notes) sh.dest = Dest::OneShot;
  sh.sliceAt = gMem.sliceAt;
  sh.mute = gMem.mute;
  sh.normalize = gMem.normalize;
  sh.remember = false;
  sh.rate = gMem.rate >= 0 ? gMem.rate : 0;
  if (gMem.rate < 0)
    for (int i = 0; i < 4; ++i)
      if (kRates[i] == projectRate(a)) sh.rate = i;
  sh.opts.sampleRate = kRates[sh.rate];
  sh.bits = gMem.bits;
  sh.lengthMode = sh.opts.loops > 1 ? 1 : 0;
  sh.loops = std::max(1, sh.opts.loops);
  sh.libTag = -1;
  sh.open = true;
  sh.appearing = true;
  sh.fitFrames = 3;
}

// The tag a render to the library gets: the chip's, else the automatic one (from the preview once it
// is ready, else from the span and options).
bool sheetTagsLoop(const Song& s, const Sheet& sh) {
  if (sh.libTag >= 0) return sh.libTag == 0;
  Preview& pv = preview();
  std::lock_guard<std::mutex> lk(pv.mu);
  if (pv.out && pv.key == pv.want && pv.out->error.empty() && pv.out->audio.frames > 0) return sr::tagsAsLoop(s, *pv.out);
  return sr::expectsLoop(s, sh.shown, sh.opts);
}

const char* destTitle(const App& a, Dest d) {
  if (d == Dest::AudioClip) return smooth(a) ? i18n::tr("Audio region") : tracker(a) ? i18n::tr("Sample in the song") : i18n::tr("Audio clip");
  return i18n::tr(sr::destName(d));
}

std::string destSub(const App& a, const Song& s, const Sheet& sh, Dest d) {
  const sr::Source& src = sh.shown;
  switch (d) {
    case Dest::AudioClip: {
      const Tick at = sr::sourceSongStart(s, src);
      const long bar = static_cast<long>(at / std::max<Tick>(1, s.barTicks())) + 1;
      return fmt(smooth(a) ? i18n::tr("On a free track at bar %s") : i18n::tr("On a free playlist track at bar %s"), std::to_string(bar));
    }
    case Dest::Replace:
      switch (src.kind) {
        case Kind::Clip: return i18n::tr("The clip becomes audio, in the same place");
        case Kind::Pattern: return i18n::tr("Every clip of this pattern becomes audio");
        case Kind::Notes: return i18n::tr("The notes give way to one sampler note");
        default: return i18n::tr("Its steps and notes give way to one sampler note");
      }
    case Dest::Slicer:
      return src.kind == Kind::Channel || src.kind == Kind::Notes ? i18n::tr("Kerf, one slice per hit, playing in this pattern")
                                                                   : i18n::tr("Kerf, one slice per hit, a pattern that plays them");
    case Dest::OneShot: {
      int root = 60;
      if (src.kind == Kind::Notes)
        if (const Pattern* p = s.pattern(src.pattern); p && !src.channels.empty() && p->notes.count(src.channels[0]) && !src.notes.empty()) {
          const auto& ns = p->notes.at(src.channels[0]);
          int best = -1;
          for (int i : src.notes)
            if (i >= 0 && i < static_cast<int>(ns.size()) && (best < 0 || ns[static_cast<size_t>(i)].start < ns[static_cast<size_t>(best)].start)) best = i;
          if (best >= 0) root = ns[static_cast<size_t>(best)].pitch;
        }
      return fmt(i18n::tr("Trimmed, root %s, new channel below"), sampled::noteLabel(root));
    }
    case Dest::DrumPad: {
      const ObjectId dc = sr::drumPadChannel(s);
      if (dc == kNoId) return i18n::tr("No drum pad channel in the project");
      const int pad = sr::freePad(s, dc);
      if (pad < 0) return fmt(i18n::tr("Every pad of %s holds a sound"), channelName(s, dc));
      return fmt2(i18n::tr("Pad %s of %s"), std::to_string(pad + 1), channelName(s, dc));
    }
    case Dest::Loupe: return i18n::tr("Open the take to edit it");
    case Dest::Library: return sheetTagsLoop(s, sh) ? i18n::tr("Browser, Rendered, tagged loop") : i18n::tr("Browser, Rendered, tagged one-shot");
    case Dest::File: return i18n::tr("WAV or FLAC, you pick where");
    default: return std::string();
  }
}

bool destEnabled(const Song& s, const sr::Source& src, Dest d) {
  if (d == Dest::DrumPad) {
    const ObjectId dc = sr::drumPadChannel(s);
    return dc != kNoId && sr::freePad(s, dc) >= 0;
  }
  if (d == Dest::Replace && src.kind == Kind::Pattern) {
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Pattern && c.pattern == src.pattern) return true;
    return false;
  }
  return true;
}

ui::Icon destIcon(Dest d) {
  switch (d) {
    case Dest::AudioClip: return ui::Icon::Wave;
    case Dest::Replace: return ui::Icon::SwapArrows;
    case Dest::Slicer: return ui::Icon::SliceWave;
    case Dest::OneShot: return ui::Icon::CatSampler;
    case Dest::DrumPad: return ui::Icon::CatDrums;
    case Dest::Loupe: return ui::Icon::DisplayBars;
    case Dest::Library: return ui::Icon::Folder;
    case Dest::File: return ui::Icon::SendOut;
    default: return ui::Icon::Audio;
  }
}

const char* renderLabel(const App& a, Dest d) {
  switch (d) {
    case Dest::AudioClip: return smooth(a) ? i18n::tr("Render to an audio region") : i18n::tr("Render to an audio clip");
    case Dest::Replace: return i18n::tr("Render and replace");
    case Dest::Slicer: return i18n::tr("Render to slicer");
    case Dest::OneShot: return i18n::tr("Render to a one-shot");
    case Dest::DrumPad: return i18n::tr("Render to the pad");
    case Dest::Loupe: return i18n::tr("Render to Loupe");
    case Dest::Library: return i18n::tr("Render to the library");
    case Dest::File: return i18n::tr("Render to a file...");
    default: return i18n::tr("Render");
  }
}

ImU32 withAlpha(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); }

// A section box: caption at the top left, `right` faint at the top right, in a header row one
// frame high (a control can sit at its right: sectionHeaderPos); the body follows. Width 0: the
// whole row; height 0: as tall as its content.
ImVec2 gHeaderPos;  // the current section's header row, top left (screen)
float gHeaderW = 0;
void sectionBegin(const char* id, const char* caption, const char* right, ImVec2 size = ImVec2(0, 0)) {
  const theme::Palette& P = theme::pal();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(P.surface800));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding | (size.y <= 0 ? ImGuiChildFlags_AutoResizeY : 0),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  gHeaderPos = p;
  gHeaderW = ImGui::GetContentRegionAvail().x;
  const float fh = ImGui::GetFrameHeight();
  const float fs = ImGui::GetFontSize() * 0.82f;
  const float ty = std::round(p.y + (fh - fs) * 0.5f);
  ImFont* f = ImGui::GetFont();
  // letter-spaced caption, as the section captions elsewhere
  float x = p.x;
  for (const char* c = caption; *c; ++c) {
    const char ch[2] = {*c, 0};
    dl->AddText(f, fs, ImVec2(x, ty), P.textFaint, ch);
    x += f->CalcTextSizeA(fs, FLT_MAX, 0, ch).x + 1.2f;
  }
  if (right) {
    const float w = f->CalcTextSizeA(fs, FLT_MAX, 0, right).x;
    dl->AddText(f, fs, ImVec2(p.x + gHeaderW - w, ty), P.textFaint, right);
  }
  ImGui::Dummy(ImVec2(1, fh));
}
void sectionEnd() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

// One of n as compact pills sized to their labels (the sheet's choices: scope, through, length,
// tail, slice at): the chosen one outlined in the accent. Returns true when *index changed.
float pillsWidth(const char* const* labels, int n) {
  float w = 4;
  for (int i = 0; i < n; ++i) w += ImGui::CalcTextSize(labels[i]).x + 16;
  return w;
}
bool pills(const char* id, const char* const* labels, int n, int* index, const char* const* hints = nullptr, bool enabled = true) {
  const theme::Palette& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fh = ImGui::GetFrameHeight();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float W = pillsWidth(labels, n);
  dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + fh), P.field, 4.0f);
  bool changed = false;
  ImGui::PushID(id);
  float x = p0.x + 2;
  for (int i = 0; i < n; ++i) {
    const float w = ImGui::CalcTextSize(labels[i]).x + 16;
    ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
    ImGui::PushID(i);
    const bool click = ImGui::InvisibleButton("##pill", ImVec2(w, fh)) && enabled;
    const bool hov = ImGui::IsItemHovered() && enabled;
    ImGui::PopID();
    if (click && *index != i) *index = i, changed = true;
    if (hints && hints[i] && ImGui::IsItemHovered()) ui::Tooltip(hints[i]);
    const bool on = *index == i;
    const ImVec2 a(x, p0.y + 2), b(x + w, p0.y + fh - 2);
    if (on) {
      dl->AddRectFilled(a, b, withAlpha(P.hot, enabled ? 0.12f : 0.05f), 3.0f);
      dl->AddRect(a, b, enabled ? P.hot : withAlpha(P.hot, 0.4f), 3.0f);
    } else if (hov) {
      dl->AddRectFilled(a, b, P.surface600, 3.0f);
    }
    const ImU32 tc = !enabled ? P.textFaint : on ? P.hot : P.textDim;
    dl->AddText(ImVec2(x + 8, std::round(p0.y + (fh - ImGui::GetFontSize()) * 0.5f)), tc, labels[i]);
    x += w;
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(W, fh));
  return changed;
}

// The live preview: waveform, slice points, the tail shaded.
void drawPreview(App& a, const Sheet& sh, ImVec2 size) {
  const theme::Palette& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
  ImGui::Dummy(size);
  dl->AddRectFilled(p0, p1, P.well, 4.0f);
  ImFont* mono = a.fontMono ? a.fontMono : ImGui::GetFont();
  const float fs = ImGui::GetFontSize() * 0.82f;
  dl->AddText(mono, fs, ImVec2(p0.x + 8, p0.y + 6), P.textFaint, i18n::tr("PREVIEW"));
  Preview& pv = preview();
  std::shared_ptr<sr::Output> out;
  {
    std::lock_guard<std::mutex> lk(pv.mu);
    if (pv.key == pv.want) out = pv.out;
  }
  if (!out) {
    const char* t = i18n::tr("Rendering the preview...");
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), P.textDim, t);
    return;
  }
  if (!out->error.empty() || out->audio.frames == 0) {
    const std::string t = out->error.empty() ? std::string(i18n::tr("Silent")) : std::string(i18n::tr(out->error.c_str()));
    const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), P.textDim, t.c_str());
    return;
  }
  const double n = static_cast<double>(out->audio.frames);
  const float x0 = p0.x + 2, x1 = p1.x - 2, mid = (p0.y + p1.y) * 0.5f, half = size.y * 0.42f;
  const float w = x1 - x0;
  // the tail (after the span) shaded
  if (out->bodyFrames > 0 && static_cast<double>(out->bodyFrames) < n) {
    const float xt = x0 + w * static_cast<float>(static_cast<double>(out->bodyFrames) / n);
    dl->AddRectFilled(ImVec2(xt, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1), withAlpha(P.surface950, 0.55f), 4.0f);
    char b[48];
    std::snprintf(b, sizeof b, "%s %.2f s", i18n::tr("tail"), (n - static_cast<double>(out->bodyFrames)) / std::max(1, out->sampleRate));
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, b).x;
    dl->AddText(mono, fs, ImVec2(p1.x - tw - 8, p0.y + 6), P.textFaint, b);
  }
  const float peak = std::max(1e-6f, out->audio.peak());
  const float scale = 1.0f / std::max(peak, 0.25f);
  const int cols = std::max(1, static_cast<int>(w));
  const ImU32 wave = P.hot;
  for (int c = 0; c < cols; ++c) {
    const size_t a0 = static_cast<size_t>(n * c / cols), a1 = std::max(a0 + 1, static_cast<size_t>(n * (c + 1) / cols));
    float lo = 0, hi = 0;
    for (size_t i = a0; i < a1 && i < out->audio.frames; ++i) {
      const float v = 0.5f * (out->audio.at(0, i) + out->audio.at(out->audio.channels > 1 ? 1 : 0, i));
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    const float x = x0 + static_cast<float>(c) + 0.5f;
    dl->AddLine(ImVec2(x, mid - hi * scale * half), ImVec2(x, mid - lo * scale * half + 1.0f), wave);
  }
  dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), withAlpha(wave, 0.5f));
  // slice points (Kerf)
  if (sh.dest == Dest::Slicer) {
    const std::vector<int64_t> cuts = sr::slicePoints(*out, static_cast<sr::SliceAt>(sh.sliceAt), 0.5);
    const ImU32 mk = ImGui::ColorConvertFloat4ToU32(ImVec4(0.50f, 0.67f, 1.0f, 1.0f));
    for (int64_t f : cuts) {
      const float x = std::round(x0 + w * static_cast<float>(static_cast<double>(f) / n)) + 0.5f;
      for (float y = p0.y + 8; y < p1.y - 2; y += 6) dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 3, p1.y - 2)), mk);
      dl->AddRectFilled(ImVec2(x - 3, p0.y + 1), ImVec2(x + 3, p0.y + 7), mk, 1.0f);
    }
  }
}

// The destination cards, four to a row.
void drawCards(App& a, Sheet& sh) {
  const theme::Palette& P = theme::pal();
  const Song& s = a.doc.song();
  const float gap = 8.0f, avail = ImGui::GetContentRegionAvail().x;
  const float cw = std::floor((avail - 3 * gap) / 4), ch = ImGui::GetFontSize() * 4.3f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < static_cast<int>(Dest::Count); ++i) {
    const Dest d = static_cast<Dest>(i);
    if (i % 4) ImGui::SameLine(0, gap);
    const bool on = sh.dest == d, enabled = destEnabled(s, sh.shown, d) && !(d == Dest::Replace && sh.shown.kind == Kind::Notes && false);
    ImGui::PushID(i);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##dest", ImVec2(cw, ch));
    const bool hov = ImGui::IsItemHovered();
    if (clicked && enabled) sh.dest = d;
    const ImVec2 p1(p0.x + cw, p0.y + ch);
    dl->AddRectFilled(p0, p1, on ? withAlpha(P.hot, 0.10f) : hov && enabled ? P.surface600 : P.surface700, 5.0f);
    dl->AddRect(p0, p1, on ? P.hot : P.line, 5.0f, 0, on ? 1.5f : 1.0f);
    const ImU32 tc = !enabled ? P.textFaint : on ? P.hot : P.text;
    const float fs = ImGui::GetFontSize();
    ui::drawIconBox(dl, destIcon(d), ImVec2(p0.x + 10 + fs * 0.5f, p0.y + 10 + fs * 0.5f), fs, tc);
    ImFont* bold = a.fontBold ? a.fontBold : ImGui::GetFont();
    dl->AddText(bold, fs, ImVec2(p0.x + 16 + fs, p0.y + 9), tc, destTitle(a, d));
    const std::string sub = destSub(a, s, sh, d);
    dl->PushClipRect(p0, ImVec2(p1.x - 6, p1.y - 4), true);
    dl->AddText(ImGui::GetFont(), fs * 0.86f, ImVec2(p0.x + 10, p0.y + 14 + fs), enabled ? P.textDim : P.textFaint, sub.c_str(), nullptr, cw - 20);
    dl->PopClipRect();
    if (hov && !enabled) ui::Tooltip(sub.c_str());
    ImGui::PopID();
  }
}

void sheetScopeRow(App& a, Sheet& sh) {
  const Song& s = a.doc.song();
  std::vector<std::string> labels;
  std::vector<sr::Source> scopes;
  const sr::Source& src = sh.src;
  if (src.kind == Kind::Channel) {
    if (src.channels.size() == 1) {
      labels.push_back(fmt(i18n::tr("%s only"), channelName(s, src.channels[0])));
      scopes.push_back(src);
    } else {
      labels.push_back(fmt(i18n::tr("Selected rows (%s)"), std::to_string(src.channels.size())));
      scopes.push_back(src);
    }
    labels.push_back(i18n::tr("Whole pattern"));
    scopes.push_back(sr::patternSource(src.pattern));
  } else if (src.kind == Kind::Clip) {
    labels.push_back(i18n::tr("This clip"));
    scopes.push_back(src);
    if (const Clip* c = s.clip(src.clip); c && c->kind == ClipKind::Pattern) {
      labels.push_back(i18n::tr("Its whole pattern"));
      scopes.push_back(sr::patternSource(c->pattern));
    }
  } else if (src.kind == Kind::Notes) {
    labels.push_back(i18n::plural(static_cast<long>(src.notes.size()), "%d selected note", "%d selected notes"));
    scopes.push_back(src);
    if (!src.channels.empty()) {
      labels.push_back(fmt(i18n::tr("All of %s"), channelName(s, src.channels[0])));
      scopes.push_back(sr::channelSource(src.pattern, src.channels[0]));
    }
  } else {
    labels.push_back(i18n::tr("Whole pattern"));
    scopes.push_back(src);
  }
  std::vector<const char*> lp;
  for (const auto& l : labels) lp.push_back(l.c_str());
  // in the section's header row, at its right
  const ImVec2 back = ImGui::GetCursorScreenPos();
  const float w = pillsWidth(lp.data(), static_cast<int>(lp.size()));
  ImGui::SetCursorScreenPos(ImVec2(gHeaderPos.x + gHeaderW - w, gHeaderPos.y));
  int sc = std::min(sh.scope, static_cast<int>(lp.size()) - 1);
  const bool picked = pills("##scope", lp.data(), static_cast<int>(lp.size()), &sc);
  ImGui::SetCursorScreenPos(back);
  if (picked) {
    sh.scope = sc;
    const Kind before = sh.shown.kind;
    sh.shown = scopes[static_cast<size_t>(sc)];
    if (sh.shown.kind != before) {  // the new kind's own defaults (one-shot trim, through)
      const sr::Options d = optionsFor(a, sh.shown);
      sh.opts.through = d.through;
      sh.opts.oneShotTrim = d.oneShotTrim;
      sh.opts.tail = d.tail;
    }
  }
}

void drawSheet(App& a) {
  Sheet& sh = gSheet;
  if (!sh.open) return;
  const Song& s = a.doc.song();
  std::string why;
  if (!sr::renderable(s, sh.src, &why) && sh.src.kind != Kind::Notes) {  // the source went away (undo, a collaborator)
    sh.open = false;
    stopPreview();
    return;
  }
  const float fs = ImGui::GetFontSize();
  ui::PlaceNextDialog(ImVec2(fs * 64, fs * 45.5f), ImGuiCond_Appearing, ImVec2(fs * 56, fs * 30));
  if (!sh.appearing && sh.fitFrames > 0 && sh.fitH > 0) ImGui::SetNextWindowSize(ImVec2(sh.fitW, std::round(sh.fitH)));
  const std::string title = std::string(i18n::tr("Render to audio")) + "###k254render";
  if (sh.appearing) ImGui::SetNextWindowFocus();
  bool open = true;
  const bool vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  ui::KeepWindowInViewport();
  if (vis) {
    const theme::Palette& P = theme::pal();
    // ---- source ----
    sectionBegin("##src", i18n::tr("SOURCE"), nullptr);
    sheetScopeRow(a, sh);
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + 3, p.y + fs * 2.5f), P.hot, 1.5f);
      ImGui::SetCursorScreenPos(ImVec2(p.x + 12, p.y));
      ImGui::PushFont(a.fontBold ? a.fontBold : ImGui::GetFont());
      ImGui::TextUnformatted(sourceTitle(s, sh.shown).c_str());
      ImGui::PopFont();
      const Tick span = sr::sourceSpan(s, sh.shown) * std::max(1, sh.opts.loops);
      const double bars = static_cast<double>(span) / std::max<Tick>(1, s.barTicks());
      char facts[160];
      const int hits = sr::sourceHitCount(s, sh.shown) * std::max(1, sh.opts.loops);
      std::snprintf(facts, sizeof facts, "%s %s %.0f BPM, %s", bars == std::floor(bars) ? std::to_string(static_cast<long>(bars)).c_str() : std::to_string(bars).substr(0, 4).c_str(),
                    bars == 1.0 ? i18n::tr("bar at") : i18n::tr("bars at"), s.tempo, i18n::plural(hits, "%d hit", "%d hits").c_str());
      std::string f = facts;
      {
        Preview& pv = preview();
        std::lock_guard<std::mutex> lk(pv.mu);
        if (pv.out && pv.key == pv.want && pv.out->audio.frames > 0) {
          char b[64];
          std::snprintf(b, sizeof b, ", %.2f s %s", static_cast<double>(pv.out->audio.frames) / std::max(1, pv.out->sampleRate),
                        pv.out->audio.frames > static_cast<size_t>(pv.out->bodyFrames) ? i18n::tr("with the tail") : "");
          f += b;
        }
      }
      const ImVec2 sub(p.x + 12, ImGui::GetCursorScreenPos().y);
      ImGui::SetCursorScreenPos(sub);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textDim), "%s", f.c_str());
      const ImVec2 below = ImGui::GetCursorScreenPos();
      // through: at the right, level with the facts line
      const char* th[] = {i18n::tr("Dry"), i18n::tr("Channel effects"), i18n::tr("Mixer track")};
      const float tw = pillsWidth(th, 3);
      const char* cap = i18n::tr("THROUGH");
      const float capFs = fs * 0.82f;
      const float capW = ImGui::GetFont()->CalcTextSizeA(capFs, FLT_MAX, 0, cap).x;
      const float fh = ImGui::GetFrameHeight();
      const float rx = gHeaderPos.x + gHeaderW, ty = std::round(sub.y + (fs - fh) * 0.5f);
      dl->AddText(ImGui::GetFont(), capFs, ImVec2(rx - tw - capW - 10, std::round(ty + (fh - capFs) * 0.5f)), P.textFaint, cap);
      ImGui::SetCursorScreenPos(ImVec2(rx - tw, ty));
      int t = static_cast<int>(sh.opts.through);
      const char* hints[] = {i18n::tr("The instrument alone: no note effects, no channel echo"),
                             i18n::tr("The instrument with its note effects and echo, before the channel's volume and pan (as a freeze)"),
                             i18n::tr("Through the channel's mixer tracks and their effects")};
      if (pills("##through", th, 3, &t, hints)) sh.opts.through = static_cast<sr::Through>(t);
      ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(below.y, ty + fh) + 6));
      drawPreview(a, sh, ImVec2(ImGui::GetContentRegionAvail().x, fs * 6.4f));
    }
    sectionEnd();
    ImGui::Dummy(ImVec2(1, 2));
    // ---- destinations ----
    sectionBegin("##dst", i18n::tr("SEND IT TO"), i18n::tr("Each lands as one undo step"));
    drawCards(a, sh);
    sectionEnd();
    ImGui::Dummy(ImVec2(1, 2));
    // ---- options: length and tail | slices and format (two boxes of four rows, the same height) ----
    const float fh = ImGui::GetFrameHeight(), sy = ImGui::GetStyle().ItemSpacing.y;
    const float colW = std::floor((ImGui::GetContentRegionAvail().x - 8) * 0.5f), optH = 16 + fh + sy + 4 * fh + 3 * sy + 2;
    float lx = 0;
    auto labelColumn = [&](std::initializer_list<const char*> ls) {
      lx = 0;
      for (const char* l : ls) lx = std::max(lx, ImGui::CalcTextSize(l).x);
      lx += 18;
    };
    auto label = [&](const char* l) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(l);
      ImGui::SameLine(lx + 10);
    };
    sectionBegin("##len", i18n::tr("LENGTH AND TAIL"), nullptr, ImVec2(colW, optH));
    labelColumn({i18n::tr("Length"), i18n::tr("Tail"), i18n::tr("Longest tail"), i18n::tr("One-shot trim")});
    {
      label(i18n::tr("Length"));
      const Tick span = sr::sourceSpan(s, sh.shown);
      const double bars = static_cast<double>(span) / std::max<Tick>(1, s.barTicks());
      char b0[96];
      const char* what = sh.shown.kind == Kind::Clip ? i18n::tr("Clip") : sh.shown.kind == Kind::Notes ? i18n::tr("Notes") : i18n::tr("Pattern");
      std::snprintf(b0, sizeof b0, "%s (%.4g %s)", what, bars, bars == 1.0 ? i18n::tr("bar") : i18n::tr("bars"));
      const char* lm[] = {b0, i18n::tr("Loops")};
      if (pills("##lenmode", lm, 2, &sh.lengthMode)) sh.opts.loops = sh.lengthMode == 1 ? sh.loops : 1;
      ImGui::SameLine(0, 4);
      ImGui::SetNextItemWidth(fs * 3.2f);
      ImGui::BeginDisabled(sh.lengthMode == 0);
      if (ImGui::InputInt("##loops", &sh.loops, 0, 0)) {
        sh.loops = std::clamp(sh.loops, 1, 64);
        sh.opts.loops = sh.lengthMode == 1 ? sh.loops : 1;
      }
      ImGui::EndDisabled();
      label(i18n::tr("Tail"));
      const char* tails[] = {i18n::tr("Cut"), i18n::tr("Let ring"), i18n::tr("Wrap to start")};
      const char* tailHints[] = {i18n::tr("Stop at the end of the span"), i18n::tr("Let the sound ring out after the span, up to the longest tail"),
                                 i18n::tr("Fold the tail back over the start, so the render loops without a gap")};
      int tm = static_cast<int>(sh.opts.tail);
      if (pills("##tail", tails, 3, &tm, tailHints)) sh.opts.tail = static_cast<TailMode>(tm);
      label(i18n::tr("Longest tail"));
      ImGui::SetNextItemWidth(fs * 5.0f);
      float mt = static_cast<float>(sh.opts.maxTailSeconds);
      ImGui::BeginDisabled(sh.opts.tail == TailMode::Cut);
      if (ImGui::DragFloat("##maxtail", &mt, 0.05f, 0.1f, 30.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp)) sh.opts.maxTailSeconds = mt;
      ImGui::EndDisabled();
      label(i18n::tr("One-shot trim"));
      ImGui::Checkbox("##trim", &sh.opts.oneShotTrim);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textDim), "%s", i18n::tr("start at the first sound, end at -60 dB"));
    }
    sectionEnd();
    ImGui::SameLine(0, 8);
    sectionBegin("##slices", i18n::tr("SLICES AND FORMAT"), i18n::tr("Slices: Kerf only"), ImVec2(colW, optH));
    labelColumn({i18n::tr("Slice at"), i18n::tr("Slices"), i18n::tr("Format"), i18n::tr("Normalize")});
    {
      const bool slicing = sh.dest == Dest::Slicer;
      label(i18n::tr("Slice at"));
      const char* at[] = {i18n::tr("Each step hit"), i18n::tr("Notes"), i18n::tr("Beats"), i18n::tr("Transients")};
      pills("##sliceat", at, 4, &sh.sliceAt, nullptr, slicing);
      ImGui::BeginDisabled(!slicing);
      label(i18n::tr("Slices"));
      {
        Preview& pv = preview();
        std::shared_ptr<sr::Output> out;
        {
          std::lock_guard<std::mutex> lk(pv.mu);
          if (pv.key == pv.want) out = pv.out;
        }
        char b[48];
        if (out && out->error.empty()) std::snprintf(b, sizeof b, "%d %s %d", static_cast<int>(sr::slicePoints(*out, static_cast<sr::SliceAt>(sh.sliceAt), 0.5).size()), i18n::tr("of"), pp::kMaxSlices);
        else std::snprintf(b, sizeof b, "-");
        ImGui::PushFont(a.fontMono ? a.fontMono : ImGui::GetFont());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(b);
        ImGui::PopFont();
      }
      ImGui::EndDisabled();
      label(i18n::tr("Format"));
      const char* rates[] = {"44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz"};
      ImGui::SetNextItemWidth(fs * 5.6f);
      if (ImGui::Combo("##rate", &sh.rate, rates, 4)) sh.opts.sampleRate = kRates[sh.rate];
      ImGui::SameLine();
      const char* bits[] = {i18n::tr("16 bit"), i18n::tr("24 bit"), i18n::tr("32 bit float")};
      ImGui::SetNextItemWidth(fs * 6.6f);
      ImGui::Combo("##bits", &sh.bits, bits, 3);
      if (ImGui::IsItemHovered()) ui::Tooltip(i18n::tr("The file you pick is written at this depth; the project's own copy is always 32 bit float"));
      label(i18n::tr("Normalize"));
      ImGui::Checkbox("##norm", &sh.normalize);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textDim), "%s", i18n::tr("peak to -1 dB"));
    }
    sectionEnd();
    // ---- footer ----
    ImGui::Dummy(ImVec2(1, 4));
    const bool canMute = sr::editsSong(sh.dest) && sh.dest != Dest::Replace;
    if (sh.dest == Dest::Library) {
      // The library's tag as a chip: automatic (a whole number of bars ending on a bar is a loop),
      // flipped with a click before it is saved.
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(i18n::tr("Tagged"));
      ImGui::SameLine();
      const char* tags[] = {i18n::tr("Loop"), i18n::tr("One-shot")};
      const char* tagHints[] = {i18n::tr("Saved as a loop: it plays in time with the song"), i18n::tr("Saved as a one-shot: it plays once at its own speed")};
      int tg = sheetTagsLoop(s, sh) ? 0 : 1;
      if (pills("##libtag", tags, 2, &tg, tagHints)) sh.libTag = tg;
      devTagRect = ImVec4(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.textFaint), "%s", sh.libTag < 0 ? i18n::tr("automatic") : i18n::tr("your choice"));
      if (ImGui::IsItemHovered())
        ui::Tooltip(i18n::tr("A render that lasts a whole number of bars and ends on a bar is tagged a loop; anything else a one-shot"));
    } else {
      ImGui::BeginDisabled(!canMute);
      const char* muteLabel = sh.shown.kind == Kind::Clip ? i18n::tr("Mute the clip after rendering")
                              : sh.shown.kind == Kind::Pattern ? i18n::tr("Mute the pattern's clips after rendering")
                              : sh.shown.kind == Kind::Notes ? i18n::tr("Mute the notes after rendering")
                                                             : i18n::tr("Mute the source rows after rendering");
      ImGui::Checkbox(muteLabel, &sh.mute);
      ImGui::EndDisabled();
    }
    ImGui::SameLine(0, 24);
    ImGui::Checkbox(i18n::tr("Remember for the quick menu rows"), &sh.remember);
    const char* go = renderLabel(a, sh.dest);
    const float bw = ui::TextButtonWidth(go) + 12, cw = ui::TextButtonWidth(i18n::tr("Cancel"));
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw - cw - 10);
    if (ui::TextButton(i18n::tr("Cancel"))) open = false;
    ImGui::SameLine();
    const bool can = !gJobRunning && destEnabled(s, sh.shown, sh.dest);
    const bool devGo = gDevGo && can;
    gDevGo = false;
    if (ui::PrimaryButton(go, bw, nullptr, can) || devGo) {
      if (sh.remember) {
        gMem.through[static_cast<int>(sh.shown.kind)] = static_cast<int>(sh.opts.through);
        if (!sr::isOneShot(sh.shown)) gMem.tail = static_cast<int>(sh.opts.tail);
        gMem.maxTail = sh.opts.maxTailSeconds;
        gMem.trim = sr::isOneShot(sh.shown) ? -1 : (sh.opts.oneShotTrim ? 1 : -1);
        gMem.mute = sh.mute;
        gMem.normalize = sh.normalize;
        gMem.rate = sh.rate;
        gMem.bits = sh.bits;
        gMem.sliceAt = sh.sliceAt;
        gMem.dest = static_cast<int>(sh.dest);
        saveMem(a);
      }
      JobExtra ex;
      ex.normalize = sh.normalize;
      ex.bits = kBits[sh.bits];
      ex.place.dest = sh.dest;
      ex.place.sliceAt = static_cast<sr::SliceAt>(sh.sliceAt);
      ex.place.muteSource = canMute && sh.mute;
      ex.libTag = sh.libTag;
      const sr::Source src = sh.shown;
      const sr::Options o = sh.opts;
      if (sh.dest == Dest::File) {
        menus::fileDialog(a, i18n::tr("Render to a file"), true, {".wav", ".flac"}, cleanFileName(sr::sourceName(s, src)) + ".wav", [&a, src, o, ex](const std::string& p) {
          JobExtra e = ex;
          e.filePath = p;
          start(a, src, o, e);
        });
      } else {
        start(a, src, o, ex);
      }
      open = false;
    }
    // The sheet's height follows its content for its first frames (each layout's fonts and frames differ).
    sh.fitW = ImGui::GetWindowWidth();
    sh.fitH = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
  }
  ImGui::End();
  sh.appearing = false;
  if (sh.fitFrames > 0) --sh.fitFrames;
  if (!open) {
    sh.open = false;
    stopPreview();
    return;
  }
  // The preview follows the options.
  Preview& pv = preview();
  if (previewKey(sh) != pv.want && !pv.running.load()) startPreview(a, sh);
  else if (previewKey(sh) != pv.want && pv.running.load()) pv.cancel = true;  // the next frame starts the new one
}

}  // namespace

// ---- public ----

srcrender::Source rowsSource(const App& a, ObjectId pattern, ObjectId channel) {
  if (a.selChannels.count(channel) && a.selChannels.size() > 1) {
    std::vector<ObjectId> rows;
    for (const auto& c : a.doc.song().channels)
      if (a.selChannels.count(c.id)) rows.push_back(c.id);
    return sr::rowsSource(pattern, rows);
  }
  return sr::channelSource(pattern, channel);
}

srcrender::Source pianoNotesSource(const App& a) {
  const ObjectId pat = a.curPattern, ch = a.selChannel;
  std::vector<int> idx;
  if (const Pattern* p = a.doc.song().pattern(pat); p && p->notes.count(ch)) {
    const size_t n = p->notes.at(ch).size();
    if (a.piano.selPattern == pat && a.piano.selChannel == ch)
      for (size_t i : a.piano.selected)
        if (i < n) idx.push_back(static_cast<int>(i));
    if (idx.empty())
      for (size_t i = 0; i < n; ++i) idx.push_back(static_cast<int>(i));
  }
  return sr::notesSource(pat, ch, idx);
}

bool menuBody(App& a, const srcrender::Source& src) {
  loadMem(a);
  const Song& s = a.doc.song();
  std::string why;
  const bool ok = sr::renderable(s, src, &why) && !gJobRunning;
  layoutui::menuHeading(menuHeadingText(s, src).c_str());
  if (!why.empty()) ImGui::TextDisabled("%s", i18n::tr(why.c_str()));
  bool chosen = false;
  auto row = [&](const char* label, Dest d, const char* hint, bool enabled = true) {
    if (ImGui::MenuItem(label, nullptr, false, ok && enabled)) {
      quick(a, src, d);
      chosen = true;
    }
    ui::Hint(hint);
  };
  row(smooth(a) ? i18n::tr("As an audio region") : tracker(a) ? i18n::tr("As a sample in the song") : i18n::tr("As an audio clip"), Dest::AudioClip,
      i18n::tr("The render on a free track where the source plays"));
  row(i18n::tr("Into a slicer channel"), Dest::Slicer, i18n::tr("Kerf, one slice per hit, with notes that play the slices"));
  row(i18n::tr("As a one-shot sampler channel"), Dest::OneShot, i18n::tr("A new sampler channel below, trimmed, its root key the note's"));
  if (sr::drumPadChannel(s) != kNoId)
    row(i18n::tr("Onto the next free drum pad"), Dest::DrumPad, i18n::tr("The next free pad of the drum pad channel takes the render"),
        sr::freePad(s, sr::drumPadChannel(s)) >= 0);
  row(i18n::tr("Open in Loupe"), Dest::Loupe, i18n::tr("The render opens in Loupe to edit it"));
  row(i18n::tr("Save to the library"), Dest::Library, i18n::tr("Saved in the project's Rendered folder, shown in the browser"));
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Render options..."), nullptr, false, ok)) {
    openSheet(a, src);
    chosen = true;
  }
  ui::Hint("Every destination and option: through what, length, tail, slices and format");
  if (src.kind == Kind::Channel) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("The whole pattern (every channel)..."), nullptr, false, !gJobRunning)) {
      openSheet(a, sr::patternSource(src.pattern));
      chosen = true;
    }
  }
  return chosen;
}

bool menu(App& a, const srcrender::Source& src, const char* label) {
  const std::string other = peerRendering(a, src);
  if (!other.empty()) {
    ImGui::MenuItem(fmt(i18n::tr("Rendering... (%s)"), other).c_str(), nullptr, false, false);
    return false;
  }
  bool chosen = false;
  if (ImGui::BeginMenu(label ? label : i18n::tr("Render to audio"))) {
    chosen = menuBody(a, src);
    ImGui::EndMenu();
  }
  return chosen;
}

bool clipMenu(App& a, ObjectId clip) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c || c->kind != ClipKind::Pattern) return false;
  return menu(a, sr::clipSource(clip));
}

bool channelMenu(App& a, ObjectId channel) {
  if (!a.doc.song().channel(channel) || !a.doc.song().pattern(a.curPattern)) return false;
  return menu(a, rowsSource(a, a.curPattern, channel));
}

bool notesMenu(App& a) {
  const sr::Source src = pianoNotesSource(a);
  if (src.notes.empty()) {
    ImGui::MenuItem(i18n::tr("Render selected notes"), nullptr, false, false);
    return false;
  }
  return menu(a, src, i18n::tr("Render selected notes"));
}

void quick(App& a, const srcrender::Source& src, srcrender::Dest dest) {
  loadMem(a);
  a.later([&a, src, dest] {
    JobExtra ex;
    ex.normalize = gMem.normalize;
    ex.bits = kBits[gMem.bits];
    ex.place.dest = dest;
    ex.place.sliceAt = static_cast<sr::SliceAt>(gMem.sliceAt);
    ex.place.muteSource = gMem.mute && sr::editsSong(dest) && dest != Dest::Replace && dest != Dest::AudioClip;
    start(a, src, optionsFor(a, src), ex);
  });
}

void openSheet(App& a, const srcrender::Source& src) {
  a.later([&a, src] {
    stopPreview();
    initSheet(a, src);
  });
}

bool sheetOpen() { return gSheet.open; }

void draw(App& a) { drawSheet(a); }

void shutdown() {
  gSheet.open = false;
  stopPreview();
}

std::string peerRendering(const App& a, const srcrender::Source& src) {
  if (!a.collab || !a.collab->active()) return std::string();
  const collab::Session* s = a.collab->session();
  if (!s) return std::string();
  const Busy b = busyOf(src);
  if (!b.kind) return std::string();
  for (const auto& [site, pv] : s->peers()) {
    if (!pv.connected || pv.presence.busyKind != b.kind || pv.presence.busyId != b.id) continue;
    return pv.profile.name.empty() ? std::string(i18n::tr("A collaborator")) : pv.profile.name;
  }
  return std::string();
}

void localBusy(uint32_t& kind, uint32_t& id) {
  kind = gBusy.kind;
  id = gBusy.id;
}

// dev-input:
//   render rows <channel index> [dest] [through]   the current pattern's row with the last options
//   render notes [dest]                             the piano roll's notes (all of the selected channel)
//   render clip <clip index> [dest]                 a playlist clip
//   render pattern [dest]                           the current pattern
//   render sheet rows <channel index> | notes | clip <i> | pattern   opens the sheet
//   render state                                    "render state job <0|1> sheet <0|1> finished <n> last <...>"
// dest: audioclip, replace, slicer, oneshot, drumpad, loupe, library (file needs the dialog).
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int /*frame*/) {
  if (cmd != "render" || args.empty()) return false;
  const Song& s = a.doc.song();
  auto destOf = [](const std::string& w) {
    for (int i = 0; i < static_cast<int>(Dest::Count); ++i)
      if (w == kDestIds[i]) return static_cast<Dest>(i);
    return Dest::Slicer;
  };
  size_t k = 0;
  const bool sheet = args[0] == "sheet";
  if (sheet) k = 1;
  if (args[0] == "state") {
    std::printf("render state job %d sheet %d finished %d last %s\n", gJobRunning ? 1 : 0, gSheet.open ? 1 : 0, gDevFinished, gDevLast.c_str());
    if (gSheet.open)
      std::printf("render sheet dest %s tag %s %s chip %.0f %.0f %.0f %.0f\n", kDestIds[static_cast<int>(gSheet.dest)], sheetTagsLoop(s, gSheet) ? "loop" : "oneshot",
                  gSheet.libTag < 0 ? "auto" : "chosen", devTagRect.x, devTagRect.y, devTagRect.z, devTagRect.w);
    std::fflush(stdout);
    return true;
  }
  if (args[0] == "dest" && args.size() > 1) {  // render dest <id>: the sheet's destination
    gSheet.dest = destOf(args[1]);
    return true;
  }
  if (args[0] == "go") {  // render go: the sheet's Render button, next frame
    gDevGo = true;
    return true;
  }
  if (k >= args.size()) return true;
  sr::Source src;
  const std::string what = args[k];
  size_t next = k + 1;
  if (what == "rows" && next < args.size()) {
    const int i = std::atoi(args[next].c_str());
    ++next;
    if (i < 0 || i >= static_cast<int>(s.channels.size())) return true;
    src = sr::channelSource(a.curPattern, s.channels[static_cast<size_t>(i)].id);
  } else if (what == "notes") {
    src = pianoNotesSource(a);
  } else if (what == "clip" && next < args.size()) {
    const int i = std::atoi(args[next].c_str());
    ++next;
    if (i < 0 || i >= static_cast<int>(s.playlist.size())) return true;
    src = sr::clipSource(s.playlist[static_cast<size_t>(i)].id);
  } else if (what == "pattern") {
    src = sr::patternSource(a.curPattern);
  } else {
    return true;
  }
  if (sheet) {
    stopPreview();
    initSheet(a, src);
    return true;
  }
  const Dest d = next < args.size() ? destOf(args[next]) : Dest::Slicer;
  quick(a, src, d);
  return true;
}

}  // namespace gl::renderui
