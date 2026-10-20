// Gridline — the thirteen FL-style point shapes (J-41).
//
// One segment of an FL-style point list (automation clips, envelope
// controller envelopes / LFO amplitude envelopes / maps, Slicex
// articulators) runs from the previous point's value to its own value; the
// point's shape and tension bend the progress y(x), x and y in [0, 1].
// Shape numbers follow FL's stored mode byte:
//   0 single curve      1 double curve      2 hold          3 stairs
//   4 smooth stairs     5 pulse             6 wave          7 single curve 2
//   8 double curve 2    9 half sine        10 smooth       11 single curve 3
//  12 double curve 3
// Tension uses FL's sign: t > 0 bends the segment to a fast start, t < 0 to a
// slow start (Gridline's automation points store the opposite sign).
// Behavioural reimplementation from analysis/rea/agents/j33/envc_spec.md §2.4.
#pragma once

namespace gl::flcurve {

inline constexpr int kShapeCount = 13;
inline constexpr int kSmooth = 10;

// Single-curve kernel C(t, x) (exponential, 10^(6|t|) dynamic range).
double singleCurve(double t, double x);
// Kernel with an explicit curvature c (LFO tension): k = ln(1 + |c|).
double curveKernel(double c, double x);
// Step count n(t) of the stairs / pulse / wave shapes.
int stepCount(double t);
// Progress y(x) of a segment ending at a point of `shape` with tension t.
// The smooth shape (10) uses the neutral tangents here; see smooth().
double shape(int shape, double t, double x);
// Smooth shape with start / end tangents ps / pe (in units of the segment's
// own rise per length; 0 = flat).
double smooth(double t, double x, double ps, double pe);
// Tangent at the junction with a neighbouring smooth segment: sigma = the
// neighbour's slope relative to this segment's, r = its length relative to
// this segment's (weighted harmonic mean, monotone: 0 when sigma <= 0).
double smoothTangent(double sigma, double r);

}  // namespace gl::flcurve
