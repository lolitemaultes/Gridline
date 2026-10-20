// Gridline — J-38 tests, included into test_playlist_view.cpp (inside its
// anonymous namespace): fade gain law, automatic crossfades (creation, follow
// on move / resize / trim, hand-set override, save / load, undo, render) and
// the thin mixer layout.

constexpr double kPiD = 3.14159265358979323846;

std::string writeWavOf(const char* name, size_t frames, const std::function<float(size_t)>& f) {
  const std::string path = (fs::temp_directory_path() / name).string();
  AudioBuffer b(2, frames);
  for (size_t i = 0; i < b.frames; ++i) b.at(0, i) = b.at(1, i) = f(i);
  writeWav(path, b, 44100, 24);
  return path;
}

ObjectId addSampleChannel(Song& s, const std::string& wav, const char* name) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = wav;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}

ObjectId addAudioClip(Song& s, ObjectId chan, int track, Tick start) {
  Clip a;
  a.id = s.allocateId(), a.kind = ClipKind::Audio, a.source = chan, a.track = track, a.start = start;
  a.length = audioClipNaturalLength(s, a);
  s.playlist.push_back(a);
  return a.id;
}

const ClipFades& fadesOf(const Song& s, const std::vector<ClipFades>& all, ObjectId id) {
  return all[static_cast<size_t>(s.clipIndex(id))];
}

double rmsOf(const AudioBuffer& b, size_t from, size_t n) {
  double e = 0;
  for (size_t i = from; i < from + n && i < b.frames; ++i) e += static_cast<double>(b.at(0, i)) * b.at(0, i);
  return std::sqrt(e / static_cast<double>(std::max<size_t>(1, n)));
}

float maxStep(const AudioBuffer& b, size_t from, size_t to) {
  float m = 0;
  for (size_t i = std::max<size_t>(1, from); i < to && i < b.frames; ++i) m = std::max(m, std::fabs(b.at(0, i) - b.at(0, i - 1)));
  return m;
}

void crossfadeTests() {
  // ---- gain law (J-43: FL's shapes and tension law), mirrored pairs ----
  {
    bool ends = true, mono = true, ampl = true;
    for (float t : {-2000.0f, -10.0f, -0.6f, 0.0f, 0.3f, 4.0f, 635.0f})
      for (FadeShape sh : {FadeShape::Smooth, FadeShape::Single}) {
        ends = ends && near(fadeGain(0, sh, t), 0, 1e-12) && near(fadeGain(1, sh, t), 1, 1e-12);
        double prev = -1;
        for (int k = 0; k <= 200; ++k) {
          const double p = k / 200.0, g = fadeGain(p, sh, t), m = fadeGain(1 - p, sh, -t);  // fade in vs. mirrored fade out
          mono = mono && g >= prev - 1e-12;
          prev = g;
          ampl = ampl && near(g + m, 1.0, 1e-9);
        }
      }
    expect(ends && mono, "fades: every shape / tension runs 0 -> 1 monotonically");
    expect(ampl, "fades: crossfades with mirrored tensions keep constant amplitude (both shapes)");
    expect(near(fadeGain(0.5, FadeShape::Single, 0), 0.5) && near(fadeGain(0.25, FadeShape::Smooth, 0), 0.5 - 0.5 * std::cos(kPiD / 4), 1e-12),
           "fades: plain straight / smooth (raised cosine) curves");
    expect(fadeGain(0.5, FadeShape::Single, 2.0f) > 0.5 && fadeGain(0.5, FadeShape::Single, -2.0f) < 0.5,
           "fades: positive tension bulges the curve up, negative bows it down");
  }

  // ---- automatic crossfades ----
  const std::string dc = writeWavOf("gridline_j38_dc.wav", 88200, [](size_t) { return 0.5f; });
  std::mt19937 rng(38);
  std::uniform_real_distribution<float> uni(-0.5f, 0.5f);
  std::vector<float> n1(88200), n2(88200);
  for (auto& x : n1) x = uni(rng);
  for (auto& x : n2) x = uni(rng);
  const std::string noiseA = writeWavOf("gridline_j38_n1.wav", n1.size(), [&](size_t i) { return n1[i]; });
  const std::string noiseB = writeWavOf("gridline_j38_n2.wav", n2.size(), [&](size_t i) { return n2[i]; });
  Song s = makeEmptySong();
  s.tempo = 120.0;  // 96 PPQ: 192 ticks = 1 s; the 2 s samples are 384 ticks long
  const ObjectId chDc = addSampleChannel(s, dc, "dc");
  const ObjectId A = addAudioClip(s, chDc, 0, 0), B = addAudioClip(s, chDc, 0, 288);
  TempoMap tm;
  tm.build(s);
  {
    const auto all = resolveAllFades(s, tm);
    const ClipFades &fa = fadesOf(s, all, A), &fb = fadesOf(s, all, B);
    expect(s.clip(A)->length == 384 && s.clip(B)->length == 384, "crossfade fixture: 2 s clips");
    expect(fa.out.automatic && fa.out.partner == B && near(fa.out.len, 0.5, 1e-9) && fa.out.shape == FadeShape::Smooth,
           "auto crossfade: the earlier clip fades out over the overlap (smooth curve)");
    expect(fb.in.automatic && fb.in.partner == A && near(fb.in.len, 0.5, 1e-9) && fa.in.len == 0 && fb.out.len == 0,
           "auto crossfade: the later clip fades in over the overlap, free edges stay unfaded");
    const ClipFades one = resolveFades(s, tm, *s.clip(B));
    expect(one.in.len == fb.in.len && one.in.partner == A && one.out.len == 0, "auto crossfade: single-clip and whole-song resolution agree");
  }
  {  // no crossfade: other track, disabled, containment, muted, pattern clips
    Song o = s;
    o.clip(B)->track = 1;
    expect(resolveFades(o, tm, *o.clip(A)).out.len == 0, "auto crossfade: clips on different tracks don't crossfade");
    o = s;
    o.autoCrossfades = false;
    const ClipFades f = resolveFades(o, tm, *o.clip(B));
    expect(f.in.len == 0 && f.in.crossfade && !f.in.automatic, "auto crossfade: off in the project settings (the overlap is still known)");
    o = s;
    o.clip(B)->start = 96, o.clip(B)->length = 96;  // inside A
    expect(resolveFades(o, tm, *o.clip(A)).out.len == 0 && resolveFades(o, tm, *o.clip(B)).in.len == 0,
           "auto crossfade: a clip completely inside another doesn't crossfade");
    o = s;
    o.clip(A)->muted = true;
    expect(resolveFades(o, tm, *o.clip(B)).in.len == 0, "auto crossfade: muted clips don't crossfade");
    Clip pc;
    pc.id = o.allocateId(), pc.kind = ClipKind::Pattern, pc.pattern = o.patterns[0].id, pc.track = 0, pc.start = 300;
    o.playlist.push_back(pc);
    const auto all = resolveAllFades(o, tm);
    expect(fadesOf(o, all, pc.id).in.len == 0 && fadesOf(o, all, pc.id).out.len == 0, "auto crossfade: pattern clips only overlap visually");
  }
  {
    Document d(s);
    const int tracks = static_cast<int>(d.song().tracks.size());
    auto fadesNow = [&](ObjectId id) {
      TempoMap m;
      m.build(d.song());
      return resolveFades(d.song(), m, *d.song().clip(id));
    };
    // Move the later clip 96 ticks left: the overlap (and the crossfade) grows to 1 s.
    d.editClips("move clips", [&](std::vector<Clip>& v) { v = pltools::moveClips(v, {B}, -96, 0, tracks); });
    expect(near(fadesNow(A).out.len, 1.0, 1e-9) && near(fadesNow(B).in.len, 1.0, 1e-9), "auto crossfade follows a move");
    // Shorten the earlier clip by 48 ticks: 0.75 s.
    const Song before = d.song();
    d.editClips("resize clips", [&](std::vector<Clip>& v) { v = pltools::resizeEnds(before, v, {A}, -48, false); });
    expect(near(fadesNow(A).out.len, 0.75, 1e-9) && near(fadesNow(B).in.len, 0.75, 1e-9), "auto crossfade follows a resize");
    // Trim the later clip's start 96 ticks right: 0.25 s.
    const Song before2 = d.song();
    d.editClips("trim clips", [&](std::vector<Clip>& v) { v = pltools::trimStarts(before2, tm, v, {B}, 96, false); });
    expect(near(fadesNow(B).in.len, 0.25, 1e-9) && near(fadesNow(A).out.len, 0.25, 1e-9), "auto crossfade follows a trim");
    // Hand-set fade on the later clip: stored, wins over the overlap, survives moves.
    d.editClip(B, "clip fade in", [](Clip& c) {
      c.fadeIn = 0.1, c.fadeInManual = true, c.fadeInShape = static_cast<uint8_t>(FadeShape::Single), c.fadeInTension = 0.4f;
    });
    d.editClip(A, "crossfade tension", [](Clip& c) { c.fadeOutTension = -0.4f; });
    d.editClips("move clips", [&](std::vector<Clip>& v) { v = pltools::moveClips(v, {B}, -48, 0, tracks); });
    ClipFades fb = fadesNow(B), fa = fadesNow(A);
    expect(near(fb.in.len, 0.1, 1e-9) && !fb.in.automatic && fb.in.shape == FadeShape::Single && fb.in.tension == 0.4f,
           "hand-set fade: length / shape / tension persist through a move");
    expect(fa.out.automatic && near(fa.out.len, 0.5, 1e-9) && fa.out.tension == -0.4f, "hand-set fade: the partner edge still follows the overlap");
    // A hand-set zero: no fade at all, even though the clips overlap.
    d.editClip(A, "clip fade out", [](Clip& c) { c.fadeOut = 0, c.fadeOutManual = true; });
    expect(fadesNow(A).out.len == 0 && !fadesNow(A).out.automatic, "hand-set fade: a hand-set zero removes the automatic crossfade");
    // Save / load keeps the hand-set fields.
    Song back;
    std::string err;
    const bool loaded = songFromJson(songToJson(d.song()), back, &err);
    const Clip *lb = loaded ? back.clip(B) : nullptr, *la = loaded ? back.clip(A) : nullptr;
    expect(lb && la && *lb == *d.song().clip(B) && *la == *d.song().clip(A) && back.autoCrossfades,
           "hand-set fade: shape / tension / manual flags survive save + load");
    Song off = d.song();
    off.autoCrossfades = false;
    Song offBack;
    expect(songFromJson(songToJson(off), offBack, &err) && !offBack.autoCrossfades, "project setting: automatic crossfades off survives save + load");
    // Undo back to the automatic fade.
    d.undo();  // zero fade out
    d.undo();  // move
    d.undo();  // tension
    d.undo();  // hand-set fade in
    fb = fadesNow(B);
    expect(fb.in.automatic && near(fb.in.len, 0.25, 1e-9) && d.song().clip(B)->fadeInShape == 0 && !d.song().clip(B)->fadeInManual,
           "undo: the hand-set fade goes, the automatic one is back");
  }

  // ---- render: the overlap plays click-free with a constant sum ----
  {
    Song r = s;  // A [0, 2 s), B [1.5 s, 3.5 s), both DC 0.5, linear crossfade (constant amplitude for equal signals)
    r.clip(A)->fadeOutShape = static_cast<uint8_t>(FadeShape::Single);
    r.clip(B)->fadeInShape = static_cast<uint8_t>(FadeShape::Single);
    RenderOptions o;
    AudioBuffer out = renderSong(r, o);
    const float level = out.at(0, 22050);  // A alone
    float dev = 0;
    for (size_t i = 22050; i < 150000; ++i) dev = std::max(dev, std::fabs(out.at(0, i) - level));
    expect(level > 0.05f && dev < level * 0.002f, "render: linear crossfade of equal signals is flat across the overlap");
    expect(maxStep(out, 22050, 150000) < 1e-3f, "render: no step (click) where the clips meet");
    // The default smooth crossfade keeps a constant amplitude too (FL's law): flat, no step.
    r = s;
    out = renderSong(r, o);
    dev = 0;
    for (size_t i = 22050; i < 150000; ++i) dev = std::max(dev, std::fabs(out.at(0, i) - level));
    expect(maxStep(out, 22050, 150000) < 1e-3f && dev < level * 0.002f, "render: smooth crossfade of equal signals is flat and click-free");
    // Without crossfades the second clip's start is a hard step.
    r.autoCrossfades = false;
    out = renderSong(r, o);
    expect(maxStep(out, 22050, 150000) > level * 0.9f, "render: crossfades off -> the overlap starts with a jump (the click crossfades remove)");
  }
  {  // independent noise on two channels: a constant-amplitude crossfade dips ~3 dB in the middle
    Song r = makeEmptySong();
    r.tempo = 120.0;
    const ObjectId c1 = addSampleChannel(r, noiseA, "n1"), c2 = addSampleChannel(r, noiseB, "n2");
    addAudioClip(r, c1, 0, 0);
    addAudioClip(r, c2, 0, 192);  // overlap: 1 s .. 2 s
    RenderOptions o;
    AudioBuffer out = renderSong(r, o);
    const double ref = rmsOf(out, 4410, 30000);
    double worst = 0;
    for (size_t w = 44100 + 2205; w + 4410 < 88200 - 2205; w += 2205) worst = std::max(worst, -20 * std::log10(rmsOf(out, w, 4410) / ref));
    expect(ref > 0.01 && worst > 2.4 && worst < 3.3, "render: smooth crossfade of uncorrelated audio dips about 3 dB mid-way (FL's constant-amplitude law)");
  }
  fs::remove(dc);
  fs::remove(noiseA);
  fs::remove(noiseB);
}

void mixerLayoutTests() {
  // Thin mixer: every pane width from 160 to 760 px at UI scale 1 and 1.5.
  bool noOverlap = true, inside = true, minW = true, tall = true, comfy = true, text = true, stacked = true, columnSeen = false;
  std::string bad;
  for (float scale : {1.0f, 1.5f})
    for (int px = 160; px <= 760; px += 20)
      for (int strips : {1, 5, 17, 64})
        for (float hpx : {420.0f, 760.0f, 1100.0f})
          for (float bandH : {0.0f, 14.0f}) {
            mixlayout::Input in;
            in.width = static_cast<float>(px) / scale;
            in.height = hpx / scale;
            in.strips = strips;
            in.bandH = bandH;
            const auto L = mixlayout::compute(in);
            if (!L.column) continue;
            columnSeen = true;
            const std::string where = " (" + std::to_string(px) + " px, x" + std::to_string(scale).substr(0, 3) + ", " + std::to_string(strips) +
                                      " strips, " + std::to_string(static_cast<int>(hpx)) + " px high)";
            for (size_t i = 0; i < L.cells.size(); ++i) {
              const auto& c = L.cells[i];
              if (c.x < -0.01f || c.x + c.w > L.contentW + 0.01f) inside = false, bad = where;
              if (c.w < std::min(mixlayout::kMinStripW, L.contentW) - 0.01f) minW = false, bad = where;
              for (size_t k = 0; k < i; ++k) {
                const auto& q = L.cells[k];
                if (c.row == q.row && c.x < q.x + q.w - 0.01f && q.x < c.x + c.w - 0.01f) noOverlap = false, bad = where;
              }
            }
            if (static_cast<int>(L.cells.size()) != strips) inside = false, bad = where;
            if (L.rowH < in.minStripH) tall = false, bad = where;
            const float area = L.stripsH - 2 * in.padding;
            if (area >= mixlayout::minRowHeight(in) + bandH && L.rowH < mixlayout::minRowHeight(in) - 0.5f) comfy = false, bad = where;
            if (L.stripW - 10 < mixlayout::kNameMinPx) text = false, bad = where;
            if (L.showInspector && (L.inspectorH < mixlayout::kInspectorMinH || L.stripsH + L.inspectorH > in.height + 0.5f)) stacked = false, bad = where;
          }
  expect(columnSeen, "thin mixer: thin panes use the stacked layout");
  expect(noOverlap, (std::string("thin mixer: strips never overlap") + bad).c_str());
  expect(inside, (std::string("thin mixer: every strip is placed inside the content width (none clipped)") + bad).c_str());
  expect(minW, (std::string("thin mixer: strips keep their minimum width") + bad).c_str());
  expect(tall, (std::string("thin mixer: strips never get shorter than the minimum strip") + bad).c_str());
  expect(comfy, (std::string("thin mixer: rows keep a comfortable fader height whenever the pane has room") + bad).c_str());
  expect(text, (std::string("thin mixer: strip names have room") + bad).c_str());
  expect(stacked, (std::string("thin mixer: the inspector stacks below with at least its minimum height") + bad).c_str());
  {
    mixlayout::Input in;
    in.width = 300, in.height = 760, in.strips = 17;
    const auto L = mixlayout::compute(in);
    expect(L.column && L.perRow >= 2 && L.cells[1].row == 0, "thin mixer: master and the first insert share the first row at 300 px");
    expect(L.showInspector && L.rowH >= mixlayout::minRowHeight(in), "thin mixer: 760 px high: inspector below, strips at a comfortable height");
    in.width = 900;
    expect(!mixlayout::compute(in).column, "thin mixer: wide panes keep the normal layout");
    in.width = 100, in.strips = 3;
    const auto T = mixlayout::compute(in);
    expect(!T.masterScale && T.cells[0].w <= T.contentW, "thin mixer: the master drops its dB scale first when the pane is very thin");
  }
  {  // J-46: thin toolbar. "Routing" was drawn (and clipped) in every thin pane:
     // its room was measured after "Hold" had ended the line. Groups drop in
     // order, never out of order, and what shows always fits.
    const float mw = 120, hw = 44, rw = 86;
    bool ordered = true, fitsRoom = true;
    for (float room = 0; room <= 400; room += 1) {
      const auto f = mixlayout::toolbarFit(room, mw, hw, rw);
      ordered = ordered && (!f.hold || f.meters) && (!f.routing || f.hold);
      const float used = (f.meters ? mw : 0) + (f.hold ? hw : 0) + (f.routing ? rw : 0);
      fitsRoom = fitsRoom && used <= room;
    }
    const auto at1440 = mixlayout::toolbarFit(200, mw, hw, rw);  // the default layout's pane at 1440 x 900
    expect(ordered && fitsRoom, "thin mixer toolbar: optional groups drop in order and always fit the room");
    expect(at1440.meters && at1440.hold && !at1440.routing, "thin mixer toolbar: routing drops when only meters + hold fit");
  }
}
