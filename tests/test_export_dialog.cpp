// Gridline tests: the Export audio dialog's choices (engine/ExportChoices.h) and the fixed tail
// (Render.h TailMode::Fixed). Every ticked format is written (Opus included); old %-patterns read
// as name pieces and back unchanged; adding and removing pieces; the folder on one line (home as
// ~, the middle shortened, the last folder whole); a fixed tail adds exactly its length, at any
// oversampling, and keeps the sound that rings into it.
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Project.h"
#include "engine/Export.h"
#include "engine/ExportChoices.h"
#include "engine/ExportJob.h"
#include "engine/Render.h"

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::exportchoice;

namespace {

std::string scratch;

// A sample that rings for 2 s (a decaying tone), so a tail has something in it.
std::string ringWav() {
  static std::string p;
  if (p.empty()) {
    AudioBuffer b(2, 88200);
    for (int i = 0; i < 88200; ++i) {
      const float v = 0.5f * static_cast<float>(std::sin(i * 0.05) * std::exp(-i / 30000.0));
      b.plane(0)[i] = v;
      b.plane(1)[i] = v;
    }
    p = (fs::path(scratch) / "ring.wav").string();
    writeWavFile(p, b, 44100, 32);
  }
  return p;
}

// 120 BPM, 96 PPQ: one bar is 2 s. One note at the start of a one-bar pattern clip.
Song ringSong() {
  Song s = makeEmptySong();
  s.tempo = 120.0;
  s.ppq = 96;
  Channel c;
  c.id = s.allocateId();
  c.name = "Ring";
  c.plugin = defaultPluginState("gl.sampler");
  c.plugin.data = ringWav();
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  Pattern p;
  p.id = s.allocateId();
  p.name = "Hit";
  p.notes[c.id].push_back(Note(0, 24, 60, 100));
  s.patterns.push_back(p);
  Clip k;
  k.id = s.allocateId();
  k.pattern = p.id;
  k.track = 0;
  k.start = 0;
  k.length = 384;
  s.playlist.push_back(k);
  return s;
}

float peakFrom(const AudioBuffer& b, size_t from) {
  float m = 0;
  for (size_t i = from; i < b.frames; ++i) m = std::max(m, std::fabs(b.plane(0)[i]));
  return m;
}

}  // namespace

int main() {
  scratch = (fs::path(gl::test::hermeticTestEnv("export_dialog")) / "work").lexically_normal().string();
  fs::create_directories(scratch);

  {  // every ticked format is written, Opus included
    bool ticks[static_cast<int>(ExportFormat::Count)] = {true, false, false, false, false, true};
    const std::vector<ExportFormat> f = tickedFormats(ticks, static_cast<int>(ExportFormat::Count));
    expect(f.size() == 2 && f[0] == ExportFormat::Wav && f[1] == ExportFormat::Opus, "formats: WAV and Opus ticked give WAV and Opus");
    bool only[static_cast<int>(ExportFormat::Count)] = {false, false, false, false, false, true};
    expect(tickedFormats(only, static_cast<int>(ExportFormat::Count)) == std::vector<ExportFormat>{ExportFormat::Opus}, "formats: Opus alone");
    bool none[static_cast<int>(ExportFormat::Count)] = {};
    expect(tickedFormats(none, static_cast<int>(ExportFormat::Count)).empty(), "formats: none ticked");
  }

  {  // old %-patterns read as pieces, and back unchanged
    const std::vector<NamePiece> p = parseNamePattern("%P - %N %T", kPartTokens);
    expect(p.size() == 5 && p[0].token == 'P' && p[1].text == " - " && p[2].token == 'N' && p[3].text == " " && p[4].token == 'T',
           "pieces: the old part pattern %P - %N %T");
    expect(joinNamePattern(p) == "%P - %N %T", "pieces: the part pattern joins back unchanged");
    const std::vector<NamePiece> a = parseNamePattern("%P - %M", kAreaTokens);
    expect(a.size() == 3 && a[0].token == 'P' && a[2].token == 'M', "pieces: the area pattern %P - %M");
    // a token this name does not offer, "%%" and a lone "%" are plain text, and survive a round trip
    const std::vector<NamePiece> o = parseNamePattern("Mix %M 100%% %", kFileTokens);
    expect(o.size() == 1 && o[0].token == 0 && o[0].text == "Mix %M 100% %", "pieces: other tokens and %% read as text");
    expect(parseNamePattern(joinNamePattern(o), kFileTokens) == o, "pieces: text with % reads back the same");
    expect(parseNamePattern("", kFileTokens).empty(), "pieces: an empty pattern has none");
    expect(std::string(tokenLabel('P')) == "Project" && std::string(tokenLabel('M')) == "Marker" && std::string(tokenLabel('T')) == "Track",
           "pieces: names");
  }

  {  // adding and removing pieces
    expect(appendToken("", 'P', kAreaTokens) == "%P", "add: the first piece alone");
    expect(appendToken("%P", 'M', kAreaTokens) == "%P - %M", "add: a dash between pieces");
    expect(appendToken("%P_", 'N', kAreaTokens) == "%P_%N", "add: no dash after a separator");
    expect(appendText("%P", " final", kAreaTokens) == "%P final", "add: text");
    expect(appendText("%P", "50%", kAreaTokens) == "%P50%%", "add: a percent sign in text stays text");
    expect(removePiece("%P - %M", 2, kAreaTokens) == "%P", "remove: the last piece and its dash");
    expect(removePiece("%P - %M", 0, kAreaTokens) == "%M", "remove: the first piece and its dash");
    expect(removePiece("%P - %N %T", 2, kPartTokens) == "%P - %T", "remove: a middle piece");
    expect(removePiece("%P", 5, kPartTokens) == "%P", "remove: out of range changes nothing");
  }

  {  // the folder on one line
    const auto upTo = [](size_t n) { return [n](const std::string& s) { return s.size() <= n; }; };
    const std::string home = "/Users/someone";
    const std::string path = "/Users/someone/Music/Gridline/Live Set November/Bounces";
    expect(shortFolder(path, home, upTo(100)) == "~/Music/Gridline/Live Set November/Bounces", "folder: home as ~ when it fits");
    expect(shortFolder(path, home, upTo(40)) == "~/Music/\xE2\x80\xA6/Live Set November/Bounces", "folder: the middle shortened");
    const std::string s30 = shortFolder(path, home, upTo(32));
    expect(s30.size() >= 7 && s30.compare(s30.size() - 7, 7, "Bounces") == 0, "folder: the last folder always whole");
    expect(shortFolder(path, home, upTo(3)) == "\xE2\x80\xA6/Bounces", "folder: only the last folder when nothing else fits");
    expect(shortFolder("/Volumes/Disk/a/b/c", home, upTo(100)) == "/Volumes/Disk/a/b/c", "folder: outside home unchanged");
    expect(shortFolder(home + "else/x", home, upTo(100)) == home + "else/x", "folder: a longer name is not home");  // (home's name with more after it)
    expect(shortFolder("/Users/someone/Music/Bounces/", home, upTo(100)) == "~/Music/Bounces/", "folder: a trailing separator kept when it fits");
  }

  {  // the fixed tail
    const Song s = ringSong();
    RenderJob j;
    j.sampleRate = 44100;
    j.scope = RenderScope::Range;
    j.start = 0;
    j.end = 192;  // half a bar: 1 s, the note still rings at the end
    j.tail = TailMode::Cut;
    const RenderResult cut = renderAudio(s, j);
    expect(cut.error.empty() && cut.audio.frames == 44100, "fixed tail: Cut ends at the scope end");
    j.tail = TailMode::Fixed;
    j.fixedTailSeconds = 0.5;
    const RenderResult fx = renderAudio(s, j);
    expect(fx.error.empty() && fx.audio.frames == 44100 + 22050, "fixed tail: exactly 0.5 s after the end");
    expect(fx.bodyFrames == 44100, "fixed tail: the body is unchanged");
    expect(peakFrom(fx.audio, 44100) > 0.01f, "fixed tail: the ring-out is in the tail");
    bool sameBody = true;
    for (size_t i = 0; i < 44100 && sameBody; ++i) sameBody = std::fabs(fx.audio.plane(0)[i] - cut.audio.plane(0)[i]) < 1e-6f;
    expect(sameBody, "fixed tail: the body matches the cut render");
    j.fixedTailSeconds = 3.0;  // longer than the sound: silence is kept, never trimmed
    const RenderResult longer = renderAudio(s, j);
    expect(longer.audio.frames == 44100 + 132300, "fixed tail: a tail past the sound keeps its length");
    j.fixedTailSeconds = 0.0;
    expect(renderAudio(s, j).audio.frames == 44100, "fixed tail: 0 s is a cut");
    j.fixedTailSeconds = 0.5;
    j.oversample = 2;
    const RenderResult os = renderAudio(s, j);
    expect(os.audio.frames == 44100 + 22050, "fixed tail: the same length with 2x oversampling");
    j.oversample = 1;
    j.sampleRate = 48000;
    expect(renderAudio(s, j).audio.frames == 48000 + 24000, "fixed tail: at 48 kHz");
  }

  {  // an export with a fixed tail writes the longer file
    ExportSettings es;
    es.render.sampleRate = 44100;
    es.render.scope = RenderScope::Range;
    es.render.start = 0;
    es.render.end = 192;
    es.render.tail = TailMode::Fixed;
    es.render.fixedTailSeconds = 1.0;
    es.folder = scratch;
    es.baseName = "fixed";
    es.formats = {ExportFormat::Wav};
    const ExportOutcome o = runExport(ringSong(), es);
    expect(o.errors.empty() && o.files.size() == 1 && o.frames == 88200, "export: a fixed tail of 1 s");
  }

  return finish();
}
