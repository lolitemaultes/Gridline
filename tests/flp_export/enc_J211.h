// Gridline — J-211: the track icon set and FL icon numbers (mixer inserts,
// channels, playlist tracks): table, project save / load, undo, FL import,
// export and round trips. The corpus check reads FL Studio's shipped
// templates when FL is installed (skipped otherwise).
#pragma once
#include <filesystem>
#include <set>
#include <string>
#include "core.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/MixerOps.h"
#include "core/Project.h"
#include "core/TrackIconSet.h"

namespace enctest {

inline void testTrackIconsJ211() {
  using namespace gl;
  namespace ti = gl::trackicon;
  // ---- the table ----
  {
    int n = 0;
    const ti::Info* set = ti::all(&n);
    std::set<int> ids, fls;
    std::set<std::string> svgs;
    bool unique = true, files = true, cats = true, back = true, names = true;
    for (int k = 0; k < n; ++k) {
      const ti::Info& i = set[k];
      unique = unique && ids.insert(i.id).second && svgs.insert(i.svg).second && (i.fl == 0 || fls.insert(i.fl).second);
      files = files && std::filesystem::exists(std::filesystem::path(__FILE__).parent_path() / "../../assets/icons/ui" /
                                                (std::string(i.svg) + ".svg"));
      cats = cats && i.category >= 0 && i.category < ti::CategoryCount && ti::categoryName(i.category)[0];
      back = back && ti::info(i.id) == &i && ti::fromFl(ti::toFl(i.id)) != 0 && (i.fl == 0 || ti::fromFl(i.fl) == i.id);
      names = names && i.name && i.name[0];
    }
    expect(n >= 130 && unique, "J-211: well over a hundred icons, ids / drawings / FL numbers unique");
    expect(files, "J-211: every icon has its drawing (assets/icons/ui)");
    expect(cats && names, "J-211: every icon has a name and a category");
    expect(back, "J-211: every icon exports as an FL number that imports as an icon");
    expect(ti::maxId() == static_cast<int>(*ids.rbegin()) && ids.size() == static_cast<size_t>(n) && *ids.begin() == 1,
           "J-211: ids run 1..maxId");
    for (int id = 1; id <= 16; ++id) expect(ti::info(id) != nullptr, "J-211: the first sixteen icons keep their ids");
    // FL's icon list: 1..134, 136 (zero), 237 (keys), 239 (hi-hat) all have an icon; 135 is FL's empty glyph.
    bool all = true;
    for (int f = 1; f <= 136; ++f)
      if (f != 135) all = all && ti::fromFl(f) != 0;
    expect(all && ti::fromFl(237) && ti::fromFl(239) && !ti::fromFl(135) && !ti::fromFl(0) && !ti::fromFl(300) && !ti::fromFl(-4),
           "J-211: every FL icon number shows a Gridline icon");
    expect(ti::fromFl(5) == 1 && ti::fromFl(4) == 2 && ti::fromFl(239) == 3 && ti::fromFl(14) == 5 && ti::fromFl(2) == 9 &&
               ti::fromFl(30) == 10 && ti::fromFl(40) == 14 && ti::fromFl(34) == 16,
           "J-211: FL's kick, snare, hi-hat, bass, guitar, microphone, bus, output numbers show the matching icons");
    expect(ti::toFl(13) == 18 && ti::toFl(15) == 93, "J-211: delay and sidechain export as FL's closest numbers");
    // Export rule.
    expect(ti::exportFl(0, 0) == 0 && ti::exportFl(0, 135) == 135 && ti::exportFl(0, 300) == 300, "J-211: an FL number with no icon stays");
    expect(ti::exportFl(ti::fromFl(75), 75) == 75 && ti::exportFl(1, 0) == 5 && ti::exportFl(1, 75) == 5 && ti::exportFl(13, 18) == 18,
           "J-211: a kept number while it shows the icon, else the icon's own");
    expect(ti::exportFl(999, 300) == 300 && ti::exportFl(999, 7) == 7 && ti::info(0) == nullptr && ti::info(ti::maxId() + 1) == nullptr, "J-211: unknown ids are no icon");
  }
  // ---- project save / load (and projects from before J-211) ----
  {
    Song s = makeEmptySong();
    MixerTrack m;
    m.id = s.allocateId();
    m.icon = 45, m.flIcon = 8;  // trumpet
    s.mixer.push_back(m);
    MixerTrack old;
    old.id = s.allocateId();
    old.flIcon = 5;  // an FL import saved before J-211: number only
    s.mixer.push_back(old);
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Lead";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    ch.icon = 33, ch.flIcon = 17;
    s.channels.push_back(ch);
    if (s.tracks.size() < 3) s.tracks.resize(3);
    s.tracks[0].icon = 61, s.tracks[0].flIcon = 13;
    s.tracks[1].flIcon = 20;   // number only
    s.tracks[2].flIcon = 135;  // a number Gridline cannot show
    Json j = songToJson(s, false);
    // Projects saved before J-211 carry the FL number only (no "icon" key).
    for (const char* list : {"mixer", "tracks"})
      for (Json& e : j.object[list].array) {
        auto it = e.object.find("icon");
        if (it != e.object.end() && it->second.number == 0) e.object.erase(it);
      }
    Song back;
    expect(songFromJson(j, back), "J-211: project with icons loads");
    const MixerTrack *bm = back.mixerTrack(m.id), *bo = back.mixerTrack(old.id);
    expect(bm && bm->icon == 45 && bm->flIcon == 8, "J-211: mixer icon saved and loaded");
    expect(bo && bo->icon == 1 && bo->flIcon == 5, "J-211: an older project's FL number shows its icon");
    expect(!back.channels.empty() && back.channels[0].icon == 33 && back.channels[0].flIcon == 17, "J-211: channel icon saved and loaded");
    expect(back.tracks.size() > 2 && back.tracks[0].icon == 61 && back.tracks[0].flIcon == 13, "J-211: playlist track icon saved and loaded");
    expect(back.tracks.size() > 2 && back.tracks[1].icon == ti::fromFl(20) && back.tracks[2].icon == 0 && back.tracks[2].flIcon == 135,
           "J-211: playlist FL numbers show their icons (or none)");
    Json bad = songToJson(s, false);
    if (Json& jt = bad.object["tracks"]; !jt.array.empty()) jt.array[0].object["icon"] = Json::makeNumber(9999);
    Song b2;
    expect(songFromJson(bad, b2) && !b2.tracks.empty() && b2.tracks[0].icon == 0, "J-211: an unknown icon id loads as none");
  }
  // ---- editing: undo, the FL number follows ----
  {
    Song s = makeEmptySong();
    MixerTrack m;
    m.id = s.allocateId();
    s.mixer.push_back(m);
    if (s.tracks.empty()) s.tracks.resize(1);
    Document d(s);
    expect(mixops::setIcon(d, {m.id}, 47) && d.song().mixerTrack(m.id)->icon == 47 && d.song().mixerTrack(m.id)->flIcon == 91,
           "J-211: mixer icon set, FL number follows");
    d.undo();
    expect(d.song().mixerTrack(m.id)->icon == 0 && d.song().mixerTrack(m.id)->flIcon == 0, "J-211: mixer icon change undoes");
    mixops::setIcon(d, {m.id}, 47);
    expect(mixops::setIcon(d, {m.id}, 777) && d.song().mixerTrack(m.id)->icon == 0, "J-211: an unknown icon id sets none");
    d.editPlaylistTrack(0, "change track icon", [](PlaylistTrack& t) { t.icon = 63, t.flIcon = gl::trackicon::toFl(63); });
    expect(d.song().tracks[0].icon == 63 && d.song().tracks[0].flIcon == 70, "J-211: playlist track icon set");
    d.undo();
    expect(d.song().tracks[0].icon == 0 && d.song().tracks[0].flIcon == 0, "J-211: playlist track icon change undoes");
  }
  // ---- FL export / import (no origin): Gridline icons go out as FL numbers ----
  {
    Song s = makeEmptySong();
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Keys";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    ch.icon = 29;  // grand piano, chosen in Gridline (no FL number yet)
    s.channels.push_back(ch);
    Pattern p;
    p.id = s.allocateId();
    p.name = "P";
    Note n;
    n.start = 0, n.length = s.ppq, n.pitch = 60;
    p.notes[ch.id].push_back(n);
    s.patterns.push_back(p);
    Clip c;
    c.id = s.allocateId();
    c.pattern = p.id, c.track = 2, c.start = 0;
    s.playlist.push_back(c);
    if (s.tracks.size() < 4) s.tracks.resize(4);
    s.tracks[0].icon = 1;                          // kick, Gridline only
    s.tracks[1].icon = 13;                         // delay: FL's closest number
    s.tracks[2].icon = 0, s.tracks[2].flIcon = 300;  // an FL number Gridline cannot show
    s.tracks[3].icon = ti::fromFl(75), s.tracks[3].flIcon = 75;
    const char* names[] = {"Drums", "Vox", "Bus", "Old"};
    const int icons[] = {2, 10, 14, 0};
    const int fls[] = {0, 0, 0, 239};
    for (int k = 0; k < 4; ++k) {
      MixerTrack t;
      t.id = s.allocateId();
      t.name = names[k];
      t.icon = icons[k], t.flIcon = fls[k];
      if (k == 3) t.icon = ti::fromFl(239);
      s.mixer.push_back(t);
    }
    Song back;
    flp::ExportReport rep;
    expect(coretest::exportImport(s, back, rep), "J-211: icons: export / import");
    auto mix = [&](const char* nm) -> const MixerTrack* {
      for (const auto& t : back.mixer)
        if (t.name == nm) return &t;
      return nullptr;
    };
    const MixerTrack *a = mix("Drums"), *v = mix("Vox"), *b = mix("Bus"), *o = mix("Old");
    expect(a && a->flIcon == 4 && a->icon == 2, "J-211: mixer snare icon goes out as FL 4 and comes back");
    expect(v && v->flIcon == 30 && v->icon == 10, "J-211: mixer microphone icon goes out as FL 30 and comes back");
    expect(b && b->flIcon == 40 && b->icon == 14, "J-211: mixer bus icon goes out as FL 40 and comes back");
    expect(o && o->flIcon == 239 && o->icon == 3, "J-211: an imported FL number round-trips exactly");
    expect(!back.channels.empty() && back.channels[0].flIcon == 12 && back.channels[0].icon == 29, "J-211: channel icon goes out as FL 12 and comes back");
    expect(back.tracks.size() > 3 && back.tracks[0].flIcon == 5 && back.tracks[0].icon == 1, "J-211: playlist kick icon goes out as FL 5");
    expect(back.tracks.size() > 3 && back.tracks[1].flIcon == 18 && back.tracks[1].icon == ti::fromFl(18), "J-211: delay goes out as FL 18");
    expect(back.tracks.size() > 3 && back.tracks[2].flIcon == 300 && back.tracks[2].icon == 0, "J-211: an FL number with no icon round-trips");
    expect(back.tracks.size() > 3 && back.tracks[3].flIcon == 75 && back.tracks[3].icon == ti::fromFl(75), "J-211: playlist audio icon round-trips");
    // Second trip: nothing changes.
    Song again;
    flp::ExportReport rep2;
    expect(coretest::exportImport(back, again, rep2), "J-211: icons: second export / import");
    bool same = again.mixer.size() == back.mixer.size() && again.tracks.size() >= 4 && !again.channels.empty();
    for (size_t k = 0; same && k < back.mixer.size(); ++k) same = again.mixer[k].icon == back.mixer[k].icon && again.mixer[k].flIcon == back.mixer[k].flIcon;
    for (size_t k = 0; same && k < 4; ++k) same = again.tracks[k].icon == back.tracks[k].icon && again.tracks[k].flIcon == back.tracks[k].flIcon;
    expect(same && again.channels[0].icon == back.channels[0].icon && again.channels[0].flIcon == back.channels[0].flIcon,
           "J-211: icons are stable over a second round trip");
  }
  // ---- FL's shipped templates (when installed): icons show, and survive an export with the origin ----
  {
    const std::string path = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data/Templates/Utility/Drum mix/Drum mix.flp";
    if (std::filesystem::exists(path)) {
      Song s;
      flp::Report r;
      std::string err;
      const bool ok = importFlp(path, s, r, &err);
      int shownMix = 0, numbered = 0;
      for (const auto& t : s.mixer) {
        numbered += t.flIcon != 0;
        shownMix += t.flIcon != 0 && t.icon == ti::fromFl(t.flIcon) && t.icon != 0;
      }
      expect(ok && numbered > 0 && shownMix == numbered, "J-211: an FL template's mixer icons all show");
      // Change one icon in Gridline, export with the origin, import again.
      Document d(s);
      ObjectId changed = kNoId;
      for (const auto& t : d.song().mixer)
        if (t.flIcon) {
          changed = t.id;
          break;
        }
      if (changed != kNoId) mixops::setIcon(d, {changed}, 45);  // trumpet
      flp::Bytes out;
      flp::ExportReport rep;
      flp::ExportOptions o;
      Song back;
      flp::Report r2;
      const bool ok2 = flp::exportFlp(d.song(), out, rep, o) && importFlpBytes(out, back, r2, &err, "", "", false);
      bool kept = ok2 && back.mixer.size() == d.song().mixer.size();
      for (size_t k = 0; kept && k < back.mixer.size(); ++k) {
        const MixerTrack& was = d.song().mixer[k];
        kept = back.mixer[k].flIcon == ti::exportFl(was.icon, was.flIcon) && back.mixer[k].icon == was.icon;
      }
      expect(kept, "J-211: template icons export with the origin unchanged, the changed one as its new FL number");
      bool tracksKept = ok2 && back.tracks.size() >= d.song().tracks.size();
      for (size_t k = 0; tracksKept && k < d.song().tracks.size(); ++k) tracksKept = back.tracks[k].flIcon == d.song().tracks[k].flIcon;
      expect(tracksKept, "J-211: template playlist track icon numbers come back");
    } else {
      std::printf("  (FL Studio not installed: J-211 template check skipped)\n");
    }
  }
}

}  // namespace enctest
