// Gridline — J-128 tests: the layout alignment checker (AlignCheck.h) finds
// rows off their centre line, labels off their knob, uneven gaps, baselines,
// overlaps and clipping at >= 1 physical px (UI scale 100 / 150 / 200 %), stays
// quiet on a clean layout, and the regression gate (baseline + justified
// allowlist) only fails on new findings. Also the row geometry (LayoutGrid.h).
#include <string>
#include <vector>
#include "../src/app/AlignCheck.h"
#include "../src/app/LayoutGrid.h"
#include "Check.h"

using namespace gl;
using align::Box;

namespace {
align::Ctl knob(float x, float y, float s) { return align::Ctl{Box{x, y, x + s, y + s}, Box{}, align::kKnob, "knob"}; }
// A label of font size 10 centred on cx with its line top at y (ink = layout box).
align::Text label(float cx, float y, float w, const char* s, float size = 10) {
  align::Text t;
  t.r = Box{cx - w * 0.5f, y, cx + w * 0.5f, y + size};
  t.ink = t.r;
  t.baseline = y + size * 0.8f;
  t.size = size;
  t.str = s;
  return t;
}
int count(const std::vector<align::Finding>& fs, const char* rule) {
  int n = 0;
  for (const auto& f : fs) n += f.rule == rule;
  return n;
}
// Four 40 px knobs 10 px apart on one centre line, labels centred under them.
align::Scene cleanRow() {
  align::Scene sc;
  sc.frames.push_back(align::Frame{Box{0, 0, 300, 120}, "panel"});
  for (int i = 0; i < 4; ++i) {
    const float x = 20 + 50.0f * static_cast<float>(i);
    sc.ctls.push_back(knob(x, 20, 40));
    sc.texts.push_back(label(x + 20, 62, 30, "LBL"));
  }
  return sc;
}
}  // namespace

int main() {
  {
    const auto fs = align::check(cleanRow());
    expect(fs.empty(), "clean row of knobs: no findings");
  }
  {
    align::Scene sc = cleanRow();
    sc.ctls[2].r.y0 += 1, sc.ctls[2].r.y1 += 1;  // 1 logical px low
    sc.texts[2].r.y0 += 1, sc.texts[2].r.y1 += 1, sc.texts[2].ink = sc.texts[2].r, sc.texts[2].baseline += 1;
    const auto fs = align::check(sc);
    expect(count(fs, "row") == 1, "a knob 1 px off the row's centre line is found");
    expect(count(fs, "baseline") == 1, "its label 1 px off the label baseline is found");
  }
  {
    align::Scene sc = cleanRow();
    sc.ctls[1].r.y0 += 0.4f, sc.ctls[1].r.y1 += 0.4f;  // 0.4 logical = 0.8 physical px at 200 %
    expect(count(align::check(sc), "row") == 0, "below 1 physical px at 200 %: not a finding");
  }
  {
    align::Scene sc = cleanRow();
    sc.texts[3].r.x0 += 2, sc.texts[3].r.x1 += 2, sc.texts[3].ink = sc.texts[3].r;
    const auto fs = align::check(sc);
    expect(count(fs, "label") == 1, "a label 2 px off its knob's centre is found");
    bool phys = false;
    for (const auto& f : fs)
      if (f.rule == "label") phys = f.detail.find("2.0/3.0/4.0 px") != std::string::npos;
    expect(phys, "the label finding reports physical px at 100 / 150 / 200 %");
  }
  {
    align::Scene sc = cleanRow();
    sc.ctls[3].r.x0 += 2, sc.ctls[3].r.x1 += 2;  // gap 10 -> 12
    sc.texts[3].r.x0 += 2, sc.texts[3].r.x1 += 2, sc.texts[3].ink = sc.texts[3].r;
    expect(count(align::check(sc), "gap") == 1, "an uneven gap in a row of equal knobs is found");
    align::Options o;
    o.gaps = false;
    expect(count(align::check(sc, o), "gap") == 0, "the gap rule can be switched off");
  }
  {
    align::Scene sc = cleanRow();
    sc.ctls[3].r.x0 += 15, sc.ctls[3].r.x1 += 15;  // deliberate group spacing (> 12 % of a knob)
    sc.texts[3].r.x0 += 15, sc.texts[3].r.x1 += 15, sc.texts[3].ink = sc.texts[3].r;
    expect(count(align::check(sc), "gap") == 0, "a wide deliberate group gap is not a finding");
  }
  {
    // Mixed sizes: a 30 px knob centred on the 40 px knobs' centre line is fine.
    align::Scene sc = cleanRow();
    sc.ctls[1] = knob(75, 25, 30);
    expect(count(align::check(sc), "row") == 0, "a smaller knob on the shared centre line is fine");
    sc.ctls[1] = knob(75, 20, 30);  // top-aligned instead: centre 5 px high
    expect(count(align::check(sc), "row") == 1, "a smaller knob aligned by its top edge is found");
  }
  {
    align::Scene sc = cleanRow();
    sc.texts.push_back(label(45, 62, 30, "X"));  // on top of the first label
    expect(count(align::check(sc), "overlap") >= 1, "text over text is an overlap");
    align::Scene s2 = cleanRow();
    s2.texts[0].clipped = true;
    expect(count(align::check(s2), "clip") == 1, "clipped text is found");
    align::Scene s3 = cleanRow();
    s3.ctls[0].clip = Box{0, 0, 300, 50};  // the knob ends at 60
    expect(count(align::check(s3), "clip") == 1, "a control outside its clip rectangle is found");
  }
  {
    // Two vertical lists side by side with different item pitches: their items
    // are not one text row; the same texts as free labels are.
    align::Scene sc;
    sc.frames.push_back(align::Frame{Box{0, 0, 300, 120}, "panel"});
    sc.ctls.push_back(align::Ctl{Box{10, 10, 50, 70}, Box{}, align::kButton, "list"});
    sc.ctls.push_back(align::Ctl{Box{80, 10, 120, 70}, Box{}, align::kButton, "list"});
    sc.texts.push_back(label(30, 20, 20, "A"));
    sc.texts.push_back(label(100, 23, 20, "B"));
    expect(count(align::check(sc), "baseline") == 0, "items of two different lists are not one row");
    sc.ctls.clear();
    expect(count(align::check(sc), "baseline") == 2, "the same texts as free labels are one row 3 px apart");
  }
  {
    // The gate: counts within the baseline pass, more fail, allowlisted scopes pass.
    std::vector<align::Count> counts;
    std::vector<align::Finding> fs(3);
    for (auto& f : fs) f.rule = "row";
    align::tally(counts, "gl.a", fs);
    expect(counts.size() == 1 && counts[0].n == 3, "tally counts per scope and rule");
    expect(align::gate(counts, "gl.a\trow\t3\n", "").empty(), "a count within its baseline passes");
    expect(align::gate(counts, "# comment\ngl.a\trow\t2\n", "").size() == 1, "a count above its baseline fails");
    expect(align::gate(counts, "", "gl.a\t*\tXY pad: free-form handles\n").empty(), "an allowlisted scope passes");
    expect(align::gate(counts, "", "gl.a\trow\t\n").size() == 2, "an allowlist entry needs a justification");
    expect(align::gate(counts, align::countsTsv(counts), "").empty(), "the written counts are a valid baseline");
  }
  {
    // Row geometry: mixed sizes share a centre line and a label line.
    const grid::Row r = grid::row(10, 40);
    expect(r.cy == 30 && r.top(40) == 10 && r.top(20) == 20, "row: every control centred on one line");
    expect(r.labelY == 51.5f, "row: labels under the tallest control");
    float gaps[3];
    for (int i = 0; i < 3; ++i) gaps[i] = grid::spread(0, 200, 4, 30, i + 1) - grid::spread(0, 200, 4, 30, i);
    expect(gaps[0] == gaps[1] && gaps[1] == gaps[2], "spread: equal steps");
    expect(grid::snap(10.3f, 2) == 10.5f && grid::snap(10.3f, 1) == 10, "snap to the device pixel grid");
  }
  return finish();
}
