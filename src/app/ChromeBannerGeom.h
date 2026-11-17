// Gridline - K-315: the geometry and timing of the Tracker About banner (ChromeBanner.h draws it).
//
// Every shape is the same number K of quad facets (a triangle is a quad whose last two corners are
// one point), so any shape morphs into any other by moving each facet's corners: the extruded
// "gridline" letters, the extruded Gridline mark (the tile with its hash cut through), sine worms
// (each facet a ribbon piece) and particles (each facet a small chip). The letters and the mark are
// grids of cells, each full or a diagonal half, drawn by hand here (no font outlines).
//
// The banner loops through a fixed list of segments (spin in, hold, worms, logo, particles, twist,
// wireframe, shatter), 58.5 s: every state holds long enough to read, and every move (morphs,
// turns, sways, the spread of the particles) eases in and out, so nothing starts or stops with a
// jerk and the loop point joins seamlessly. evaluate(t) gives every facet's corners in world space
// at loop time t. screenMap() places a frame in a banner rectangle: one scale for the whole loop
// (fitted to the widest state, with a margin), each frame centred on its own bounding box.
// Pure math: no ImGui, no allocation after build.
#pragma once
#include <cstdint>
#include <vector>

namespace gl::banner {

struct V3 {
  float x = 0, y = 0, z = 0;
};
struct Facet {
  V3 v[4];
};

enum class Shape : uint8_t { Text, Logo, Worms, Particles };

// One step of the loop: `from` morphs into `to` over the segment (the same shape: it holds).
struct Segment {
  const char* name;
  float seconds;
  Shape from, to;
  float turns;       // whole turns about the vertical axis over the segment (eased in and out)
  float sway;        // a sway about the vertical axis (radians, zero at both ends)
  float tilt;        // a lean back about the horizontal axis (radians, zero at both ends)
  float twist;       // a twist wave through the letters (radians at its peak)
  float wire;        // the wireframe amount at its peak
  float spreadFrom, spreadTo;  // the particles' radius over the segment
  float stagger;     // how far the morph sweeps left to right (0: every facet together)
};

const std::vector<Segment>& segments();
double loopSeconds();

// The ease used for every move: 0 to 1 with zero speed and zero acceleration at both ends.
float ease(float u);

// Where loop time t is: the segment and how far through it (0..1).
struct Where {
  int segment = 0;
  float u = 0;
};
Where where(double t);
// The start of the segment after the one at t (loop time; may equal loopSeconds()).
double nextStart(double t);
// The wireframe amount at t (0: solid, 1: edges only).
float wireAt(double t);
// True while a solid shape holds (no morph under way): back faces can be culled.
bool settledAt(double t);

struct Model {
  int K = 0;                       // facets in every shape
  std::vector<Facet> text, logo;   // K each, ordered to correspond
  std::vector<float> delay;        // per facet: when it starts to morph (0..1, left to right)
  std::vector<V3> dir;             // per facet: its particle direction (unit) and chip axes
  std::vector<float> rnd;          // per facet: a fixed random in 0..1
  int textFacets = 0, logoFacets = 0;  // before padding
};
// Built once, then shared.
const Model& model();

// Fills out[0..K) with one shape at loop time t (worms wriggle with t; particles at `spread`).
void shape(const Model& m, Shape s, double t, float spread, Facet* out);
// Every facet at loop time t, in world space (y up, the viewer on +z), into out[0..K).
// `scratch` needs K facets too.
void evaluate(const Model& m, double t, Facet* out, Facet* scratch);

// The text's size in world units: width and height.
constexpr float kTextW = 34.0f, kTextH = 9.0f;
// The viewer on +z, this far from the centre (far enough that a turn barely swells the near end).
constexpr float kCamD = 100.0f;

// The largest half width and half height of any frame of the loop, each frame centred on its own
// bounding box, in view-plane units (x / (kCamD - z)). Worked out once by sampling the loop.
struct Extent {
  float halfW = 0, halfH = 0;
  double ms = 0;  // what working it out cost
};
const Extent& loopExtent();

// Where a frame lands in a banner of w x h px: screen x = cx + f * x / (kCamD - z),
// y = cy - f * y / (kCamD - z). f is the loop's scale (the widest state fits with a margin);
// cx, cy put the frame's bounding box centre on the banner's centre. `clamped`: this frame
// was wider than the loop's widest sample, so f shrank for it (a safety net; never in the loop).
struct ScreenMap {
  float f = 1, cx = 0, cy = 0;
  bool clamped = false;
};
// The margin kept clear on each side of the banner (px).
float marginX(float w);
float marginY(float h);
ScreenMap screenMap(const Facet* world, int K, float w, float h);

}  // namespace gl::banner
