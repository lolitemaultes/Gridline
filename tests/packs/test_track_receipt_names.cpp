// Gridline - Track Receipt: song identities of sample file names (the noise left out, the
// song kept), grouping in the report, the user's merges and files kept apart, and the saved
// fields of 0.7.8 projects. Made-up names only.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../../src/core/TrackReceipt.h"
#include "../Check.h"

using namespace gl;
using namespace gl::receipt;

namespace {

bool near(double a, double b, double tol = 0.051) { return std::fabs(a - b) <= tol; }

const char* kBless = "VOCALS_86BPM_GMinor_VBV3_bless_ya_life";
const char* kMike = "VOCALS_98BPM_DMajor_VBV3_mike_ransom_world_wide_master__98.00bpm_bm__clearance_required_";

std::string key(const std::string& name) { return songIdentity(name).key; }

bool dropped(const SongIdentity& id, const std::string& word, const std::string& why) {
  for (const auto& [w, y] : id.dropped)
    if (w == word && y == why) return true;
  return false;
}

void testUserExamples() {
  const SongIdentity a = songIdentity(std::string("/samples/") + kBless + ".wav");
  expect(a.key == "bless ya life" && a.title == "bless ya life", "names: the first example is the song 'bless ya life'");
  expect(dropped(a, "VOCALS", "stem") && dropped(a, "86BPM", "tempo") && dropped(a, "GMinor", "key") && dropped(a, "VBV3", "code"),
         "names: the first example leaves out the stem, tempo, key and pack code");
  expect(!a.clearance, "names: the first example does not say clearance required");
  const SongIdentity b = songIdentity(std::string(kMike) + ".wav");
  expect(b.key == "mike ransom world wide", "names: the second example is the song 'mike ransom world wide'");
  expect(dropped(b, "master", "stem") && dropped(b, "98.00bpm", "tempo") && dropped(b, "bm", "key") && dropped(b, "DMajor", "key") &&
             dropped(b, "clearance", "note") && dropped(b, "required", "note"),
         "names: the second example leaves out master, both tempos, both keys and the store note");
  expect(b.clearance, "names: 'clearance_required' is read as the store note");
  expect(a.key != b.key, "names: the two examples are different songs (different titles never group)");
}

void testSameSong() {
  const std::string k = key(kBless);
  const char* same[] = {
      "DRUMS_86BPM_GMinor_VBV3_bless_ya_life.wav",
      "BASS_86BPM_GMinor_VBV3_bless_ya_life.wav",
      "bless_ya_life_Bass_86bpm_Gm.wav",
      "Bless Ya Life (Acapella) 86 BPM.wav",
      "bless-ya-life--vocals--98.00bpm-Bm.aif",
      "BLESS_YA_LIFE_VOX_DRY.wav",
      "bless ya life master__98.00bpm_bm__clearance_required_.wav",
      "Bless Ya Life chorus vocal take 2.wav",
      "bless_ya_life_key_F#m_hook.wav",
      "bless_ya_life_120_bpm_C_sharp_minor.wav",
      "VBV3 bless ya life drums stem.flac",
      "Bless Ya Life Instrumental copy.wav",
  };
  for (const char* n : same) expect(key(n) == k, (std::string("names: same song with other noise: ") + n).c_str());
}

void testDifferentSongs() {
  // False-positive guards: a word of the title differs, so the songs stay apart.
  const std::pair<const char*, const char*> apart[] = {
      {"VOCALS_bless_ya_life.wav", "VOCALS_bless_ya_wife.wav"},
      {"Bless Ya Life 2 vocals.wav", "Bless Ya Life vocals.wav"},  // a number in the title stays
      {"Some Song Remix vocals.wav", "Some Song vocals.wav"},      // a remix is another recording
      {"A Day In Life vocals.wav", "Day In Life vocals.wav"},
      {"Am I Dreaming vocals.wav", "I Dreaming vocals.wav"},      // "Am" is a word here, not a key
      {"Night Garden drums.wav", "Night Gardens drums.wav"},
      {"01 Morning Light drums.wav", "02 Morning Light drums.wav"},  // numbered files are not merged
      {"Golden Hour vocals.wav", "Golden Hours vocals.wav"},
      {"Paper Lanterns vocals.wav", "Lanterns Paper vocals.wav"},  // word order counts
  };
  for (const auto& [x, y] : apart)
    expect(key(x).empty() || key(x) != key(y), (std::string("names: different songs stay apart: ") + x + " / " + y).c_str());
}

void testNoIdentity() {
  // Too little left to tell songs apart: no identity (the file stays on its own, as before).
  for (const char* n : {"Kick 01.wav", "Royalty free kick.wav", "Love.wav", "Deep_Bass.wav", "808 bass 120bpm.wav", "Snare_2.wav", "FX_riser_01.wav",
                        "u2_one.wav", "vocals.wav", "Am.wav", "120bpm.wav", ""})
    expect(key(n).empty(), (std::string("names: no identity for '") + n + "'").c_str());
  expect(key("Wanderlust_vocals.wav") == "wanderlust", "names: one word of five letters or more is enough");
  expect(songIdentity("Royalty free kick clearance required.wav").clearance, "names: the store note is read without an identity too");
}

void testWords() {
  expect(key("Bad Habits vocals.wav") == "bad habits", "names: a word starting like a note is kept");
  expect(key("Ebony Eyes vocals.wav") == "ebony eyes", "names: 'Ebony' is not E flat");
  expect(key("Gmaj Heaven Strings.wav") == "heaven", "names: 'Gmaj' is a key");
  expect(key("Night Song_Ebmin_vocals.wav") == "night song", "names: a flat key with its quality");
  expect(key("Night Song G minor vocals.wav") == "night song", "names: a key in two words");
  expect(key("Song.Name.v2_vocals.wav") == "song name", "names: dots split words, 'v2' is a version");
  expect(key("Night Song 98.5bpm vocals.wav") == "night song", "names: a tempo with decimals");
  expect(key("Night Song bpm 98 vocals.wav") == "night song", "names: 'bpm 98' is a tempo");
  expect(key("Caf\xC3\xA9 Noir_vocals.wav") == "caf\xC3\xA9 noir", "names: letters outside ASCII stay in the song");
  const SongIdentity p = songIdentity("Long Song Part 2 vocals.wav");
  expect(p.key == "long song" && p.part == 2, "names: 'Part 2' is a part of the song, kept as its part");
  expect(songIdentity("Long Song pt1 drums.wav").part == 1 && key("Long Song pt1 drums.wav") == "long song", "names: 'pt1' is part 1");
  expect(songIdentity(kBless).title == "bless ya life", "names: the title keeps the words as written");
  expect(songIdentity("BLESS YA LIFE vox.wav").title == "BLESS YA LIFE", "names: the title keeps their case");
}

// ---- the report

Measurement threeFiles() {
  Measurement m;
  m.mode = "song";
  m.files.push_back(FileIn{std::string("/s/") + kBless + ".wav", 30, true, false, {}});
  m.files.push_back(FileIn{"/s/DRUMS_86BPM_GMinor_VBV3_bless_ya_life.wav", 30, true, false, {}});
  m.files.push_back(FileIn{std::string("/s/") + kMike + ".wav", 30, true, false, {}});
  Use u;
  u.measured = true;
  u.file = m.files[0].path, u.where = "vocals", u.ranges = {{0, 4}};
  m.uses.push_back(u);
  u.file = m.files[1].path, u.where = "drums", u.ranges = {{2, 6}};  // overlaps the vocals: one song, one timeline
  m.uses.push_back(u);
  u.file = m.files[2].path, u.where = "mike", u.ranges = {{10, 13}};
  m.uses.push_back(u);
  return m;
}

const SongRow* song(const Report& r, const std::string& k) {
  for (const SongRow& s : r.songs)
    if (s.key == k) return &s;
  return nullptr;
}

Saved tickAll(const Measurement& m) {
  Saved sv;
  for (const FileIn& f : m.files) sv.files[f.path].ticked = 1;
  return sv;
}

void testGrouping() {
  const Measurement m = threeFiles();
  {
    const Report r = build(m, {}, "t");
    expect(!r.files[0].ticked && !r.files[1].ticked && r.files[2].ticked && r.files[2].autoTicked,
           "report: a name that says clearance required is ticked for you, the others are not");
  }
  const Report r = build(m, tickAll(m), "t");
  expect(r.songs.size() == 2, "report: three files of two songs make two songs");
  const SongRow* b = song(r, "bless ya life");
  const SongRow* k = song(r, "mike ransom world wide");
  expect(b && k, "report: each song by the words of its name");
  if (!b || !k) return;
  expect(b->files.size() == 2 && b->grouped && !b->merged, "report: the two files of 'bless ya life' are grouped by name");
  expect(near(b->seconds, 6.0) && b->timelines.size() == 1, "report: stems of one song share a timeline (0-4 and 2-6: 6.0 s)");
  expect(b->title == "bless ya life" && b->artist.empty(), "report: the song is shown by its name's words");
  expect(k->files.size() == 1 && !k->grouped && near(k->seconds, 3.0), "report: the other song stays apart (3.0 s)");
  for (const FileRow& f : r.files) expect(!f.songTitle.empty(), ("report: every file names its song: " + f.name).c_str());
}

void testMerges() {
  const Measurement m = threeFiles();
  Saved sv = tickAll(m);
  mergeSong(sv, "mike ransom world wide", "bless ya life");
  Report r = build(m, sv, "t");
  expect(r.songs.size() == 1 && r.songs[0].key == "bless ya life" && r.songs[0].merged && r.songs[0].files.size() == 3,
         "merge: the user counts one song with another");
  expect(near(r.songs[0].seconds, 9.0), "merge: the merged song adds its parts (6.0 + 3.0 s)");
  expect(r.files[2].song == "bless ya life" && r.files[2].autoSong == "mike ransom world wide", "merge: a file keeps its own song beside the merge");
  mergeSong(sv, "bless ya life", "mike ransom world wide");  // the other way round: never a loop
  expect(sv.merges.size() == 1 && sv.merges.at("bless ya life") == "mike ransom world wide", "merge: merging back replaces the merge");
  r = build(m, sv, "t");
  expect(r.songs.size() == 1 && r.songs[0].key == "mike ransom world wide", "merge: merging back counts with the other song");
  unmergeInto(sv, "mike ransom world wide");
  expect(sv.merges.empty() && build(m, sv, "t").songs.size() == 2, "merge: undone, the songs are apart again");
  // A loop written by hand (an older or edited project) stops where it would repeat.
  sv.merges["bless ya life"] = "mike ransom world wide";
  sv.merges["mike ransom world wide"] = "bless ya life";
  expect(!mergedSong(sv, "bless ya life").empty() && build(m, sv, "t").songs.size() >= 1, "merge: a merge loop does not hang the report");
  sv.merges.clear();
  // Keep apart: each file is its own song.
  keepApart(sv, "bless ya life", {m.files[0].path, m.files[1].path});
  r = build(m, sv, "t");
  expect(r.songs.size() == 3, "keep apart: the grouped files become songs of their own");
  expect(song(r, ownSongKey(m.files[0].path)) && song(r, ownSongKey(m.files[0].path))->title == kBless,
         "keep apart: a file on its own is shown by its whole name");
  expect(r.files[0].ownSong, "keep apart: the file says it is kept on its own");
  // The file's menu: back to automatic.
  sv.files[m.files[0].path].song.clear();
  sv.files[m.files[1].path].song.clear();
  expect(build(m, sv, "t").songs.size() == 2, "keep apart: automatic again groups them again");
}

void testLegacy() {
  const Measurement m = threeFiles();
  Saved sv = tickAll(m);
  // 0.7.8 kept each such file as a song of its own, by its whole name.
  sv.songs[kBless].category = "B";
  sv.songs[kBless].holders = "Example Holder";
  Report r = build(m, sv, "t");
  const SongRow* b = song(r, "bless ya life");
  expect(b && b->fields.category == "B" && b->fields.holders == "Example Holder", "legacy: fields typed under a file's whole name are kept");
  // A file linked to another file's song by its whole name.
  Measurement m2 = m;
  m2.files.push_back(FileIn{"/s/Something else entirely.wav", 10, true, false, {}});
  Use u;
  u.measured = true, u.file = m2.files[3].path, u.where = "x", u.ranges = {{20, 21}};
  m2.uses.push_back(u);
  Saved sv2 = tickAll(m2);
  sv2.files[m2.files[3].path].song = kBless;
  r = build(m2, sv2, "t");
  b = song(r, "bless ya life");
  expect(b && b->files.size() == 3 && r.songs.size() == 2, "legacy: a file linked to a song's old key joins that song");
}

void testSaved() {
  Saved sv;
  sv.merges["a song"] = "b song";
  sv.files["/x.wav"].song = ownSongKey("/x.wav");
  expect(!sv.empty(), "saved: merges are something to save");
  const std::string j = savedToJson(sv);
  expect(j.find("\"merges\"") != std::string::npos, "saved: merges are written");
  const Saved back = savedFromJson(j);
  expect(back == sv && back.unknown.empty(), "saved: merges and files kept apart read back the same");
  expect(savedToJson(back) == j, "saved: written again, the same text");
  // A newer version's keys inside merges survive; a merge onto itself is dropped.
  const Saved odd = savedFromJson("{\"merges\":{\"a\":\"a\",\"b\":\"c\",\"d\":{\"x\":1}},\"v\":1}");
  expect(odd.merges.size() == 1 && odd.merges.at("b") == "c", "saved: a merge onto itself is not read");
  expect(savedToJson(odd).find("\"d\":{\"x\":1}") != std::string::npos, "saved: unknown entries in merges are kept");
}

}  // namespace

int main() {
  testUserExamples();
  testSameSong();
  testDifferentSongs();
  testNoIdentity();
  testWords();
  testGrouping();
  testMerges();
  testLegacy();
  testSaved();
  return finish();
}
