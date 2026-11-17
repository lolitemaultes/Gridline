// Gridline: the playlist chop at a file's own slices. A speech sample (cue
// point per word) placed as an audio clip chops into one piece per word, each
// piece starting where its word starts in the file; a file without slices
// stays whole; it is one undo step.
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Speech.h"
#include "engine/Chop.h"
#include "engine/ClipOps.h"
#include "engine/Sample.h"

using namespace gl;

namespace {
constexpr int kSr = 44100;

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

ObjectId addAudioClip(Song& s, const std::string& path, Tick start, Tick length) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Speech";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.start = start;
  c.length = length;
  s.playlist.push_back(c);
  return c.id;
}

speech::Voice plain() {
  speech::Voice v = *speech::findVoice("Narrator");
  v.breath = 0, v.vibrato = 0;
  return v;
}
}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("k313_chop_words");
  const std::string text = "Here(2) are five_words(4) said, slowly(-1).";  // five words (five_words is one)
  const speech::Parsed parsed = speech::parse(text);
  const speech::Rendered r = speech::renderWords(text, plain(), kSr);
  const size_t words = parsed.words.size();
  expect(words == 5 && r.wordStarts.size() == words, "five words");
  const std::string marked = writeFile(dir + "/speech.wav", speech::wavWithWords(r, parsed.words, kSr, "speech: test\n" + text));
  speech::Rendered none = r;
  none.wordStarts.clear();
  const std::string bare = writeFile(dir + "/bare.wav", speech::wavWithWords(none, {}, kSr, ""));

  const SamplePtr smp = sampleCache().get(marked);
  expect(smp && smp->frames() == r.samples.size(), "the speech file loads");
  const std::vector<int64_t> cuts = smp ? fileSliceFrames(*smp) : std::vector<int64_t>{};
  expect(cuts.size() + 1 == words, "a cut at every word after the first (" + std::to_string(cuts.size()) + ")");

  // The clip plays the whole file at its own speed: its length is the file's length in ticks.
  Song s = makeEmptySong();
  s.tempo = 120;
  const double secs = static_cast<double>(r.samples.size()) / kSr;
  const Tick len = static_cast<Tick>(std::floor(secs * 2.0 * s.ppq));  // 120 BPM: 2 beats a second
  const ObjectId clip = addAudioClip(s, marked, s.barTicks(), len);
  const ObjectId plainClip = addAudioClip(s, bare, s.barTicks() * 8, len);
  expect(chop::clipsWithFileSlices(s, {clip, plainClip}) == 1, "one of the two clips has slices in its file");
  const Song before = s;
  TempoMap map;
  map.build(before);

  const std::vector<ObjectId> pieces = chop::sliceAtFileSlices(s, {clip});
  expect(pieces.size() == parsed.words.size(), "one piece per word (" + std::to_string(pieces.size()) + ")");
  bool atWords = pieces.size() == r.wordStarts.size(), tiles = atWords;
  Tick end = s.barTicks();
  for (size_t k = 0; atWords && k < pieces.size(); ++k) {
    const Clip* c = s.clip(pieces[k]);
    if (!c) {
      atWords = tiles = false;
      break;
    }
    // Each piece starts at its word: within a tick of the word's start in the file.
    const double src = clipSourceAt(before, *before.clip(clip), map, static_cast<double>(c->start));
    const double word = static_cast<double>(r.wordStarts[k]) / kSr;
    const double tickSecs = 60.0 / s.tempo / s.ppq;
    atWords = std::fabs(src - word) <= tickSecs + 1e-9;
    tiles = tiles && c->start == end;
    end = c->start + s.clipLength(*c);
  }
  expect(atWords, "each piece starts at its word");
  expect(tiles && end == s.barTicks() + len, "the pieces tile the clip");

  // A file without slices stays whole.
  Song t = before;
  expect(chop::sliceAtFileSlices(t, {plainClip}).size() == 1 && t.playlist.size() == before.playlist.size(), "a file without slices stays whole");

  // One undo step.
  Document d;
  d.reset(before);
  const size_t pos = d.history().position();
  d.editSong("chop clips", [&](Song& x) { chop::sliceAtFileSlices(x, {clip}); });
  expect(d.history().position() == pos + 1 && d.song().playlist.size() == before.playlist.size() + words - 1, "one undo step adds the pieces");
  d.undo();
  expect(d.song() == before, "undo restores the clip");
  return finish();
}
