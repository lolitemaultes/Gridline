// Gridline - K-112: semantic anchors (core/SemanticAnchor.h): text round trip, the target behind
// each cursor anchor kind, and existence and names in a song (what a collaborator in another
// layout is on).
#include "core/SemanticAnchor.h"
#include "core/Document.h"
#include "Check.h"

using namespace gl;
using anchor::Kind;
using anchor::Target;

int main() {
  // ---- text round trip ----
  for (const Target& t : {Target{Kind::Channel, 12, 0}, Target{Kind::MixerTrack, 3, 2}, Target{Kind::Pattern, 4, 7}, Target{Kind::Clip, 99, 0},
                          Target{Kind::PlaylistTrack, 0, 0}, Target{Kind::Marker, 1920, 0}, Target{}}) {
    Target back;
    expect(anchor::fromText(anchor::toText(t), back) && back == t, ("round trip " + anchor::toText(t)).c_str());
  }
  expect(anchor::toText(Target{Kind::Pattern, 4, 7}) == "pattern:4/7", "pattern text");
  expect(anchor::toText(Target{}) == "none", "none text");
  Target bad;
  for (const char* s : {"", "channel", "channel:", "channel:x", "pattern:4/", "pattern:4/x", "nope:3", "none:3", "channel:3z", "channel:99999999999"})
    expect(!anchor::fromText(s, bad) && bad == Target(), (std::string("rejects \"") + s + "\"").c_str());
  for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
    expect(anchor::kindByName(anchor::kindName(static_cast<Kind>(k))) == static_cast<Kind>(k), "kind name round trip");
  expect(anchor::kindByName("bogus") == Kind::Count, "unknown kind");

  // ---- cursor anchors: the same object whatever window shows it ----
  expect(anchor::fromCursor(2, 5, 0, 0, -1) == Target{Kind::PlaylistTrack, 5, 0}, "playlist track");
  expect(anchor::fromCursor(3, 5, 0, 0, -1) == Target{Kind::PlaylistTrack, 5, 0}, "playlist header");
  expect(anchor::fromCursor(5, 40, 41, 0, -1) == Target{Kind::Pattern, 40, 41}, "piano grid");
  expect(anchor::fromCursor(8, 40, 41, 0, -1) == Target{Kind::Pattern, 40, 41}, "piano velocity");
  expect(anchor::fromCursor(9, 21, 0, 0, -1) == Target{Kind::Channel, 21, 0}, "rack row");
  expect(anchor::fromCursor(10, 3, 0, 0, -1) == Target{Kind::MixerTrack, 3, 0}, "mixer strip");
  expect(anchor::fromCursor(11, 3, 0, 0, -1) == Target{Kind::MixerTrack, 3, 0}, "stage track is its mixer track");
  expect(anchor::fromCursor(0, 0, 0, 0, -1) == Target(), "a window fraction names nothing");
  expect(anchor::fromCursor(1, 0, 0, 21, -1) == Target{Kind::Channel, 21, 0}, "instrument editor: its channel");
  expect(anchor::fromCursor(1, 0, 0, 3, 2) == Target{Kind::MixerTrack, 3, 3}, "effect editor: its track and slot");

  // ---- in a song ----
  Song s;
  Document doc(s);
  const ObjectId ch = doc.addChannel("Kick", "gl.synth");
  const ObjectId mx = doc.addMixerTrack();
  const ObjectId pat = doc.addPattern("Verse");
  const ObjectId clip = doc.addClip(pat, 0, 0);
  expect(anchor::exists(doc.song(), {Kind::Channel, ch, 0}) && anchor::describe(doc.song(), {Kind::Channel, ch, 0}) == "Kick", "channel");
  expect(anchor::exists(doc.song(), {Kind::MixerTrack, mx, 0}) && !anchor::describe(doc.song(), {Kind::MixerTrack, mx, 0}).empty(), "mixer track");
  expect(!doc.song().mixer.empty() && anchor::describe(doc.song(), {Kind::MixerTrack, doc.song().mixer[0].id, 0}) == "Master", "master");
  expect(anchor::exists(doc.song(), {Kind::Pattern, pat, ch}) && anchor::describe(doc.song(), {Kind::Pattern, pat, 0}) == "Verse", "pattern");
  expect(anchor::exists(doc.song(), {Kind::Clip, clip, 0}) && anchor::describe(doc.song(), {Kind::Clip, clip, 0}) == "Verse", "clip");
  expect(anchor::exists(doc.song(), {Kind::PlaylistTrack, 0, 0}) && !anchor::describe(doc.song(), {Kind::PlaylistTrack, 0, 0}).empty(), "track");
  expect(!anchor::exists(doc.song(), {Kind::PlaylistTrack, static_cast<uint32_t>(doc.song().tracks.size()), 0}), "track past the end");
  expect(!anchor::exists(doc.song(), {}) && anchor::describe(doc.song(), {}).empty(), "none");
  // A removed object is gone: going to it does nothing (a collaborator deleted it meanwhile).
  doc.removeChannel(ch);
  expect(!anchor::exists(doc.song(), {Kind::Channel, ch, 0}) && anchor::describe(doc.song(), {Kind::Channel, ch, 0}).empty(), "removed channel");
  return finish();
}
