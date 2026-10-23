// Gridline — J-92 parameter audit: does every parameter of every built-in
// plugin actually do something?
//
// For each parameter the audit renders a fixed, deterministic stimulus
// (effects: noise, a drum loop and a tone; instruments: notes at several
// pitches / velocities with a glide; note processors: a note sequence whose
// output events are compared) with the parameter at its extremes, middle and
// every choice, and compares the result with the default render (sample
// difference relative to the default's level, plus level / brightness change).
// Parameters with no effect are searched for a condition that enables them
// (another switch, choice or knob moved), so truly dead parameters stand out.
// Working parameters are then changed while the plugin runs (the way
// automation does it: no re-prepare) to catch parameters that only apply at
// prepare time. A song-level check covers set/get through ParamIds, the
// automation value mapping and the project save/load round trip.
//
// `gridline param-audit [--type gl.x] [--json out] [--md out]` runs the full
// sweep; the plugin_param_audit test runs a fast check against the baseline
// (tests/param_audit_baseline.tsv): a parameter recorded as working must
// still change the output.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../core/Json.h"
#include "../core/PluginInfo.h"

namespace gl::paramaudit {

enum class Status {
  Working,      // changes the output at the default settings
  Combination,  // changes the output once another parameter is moved (see condition)
  NoEffect,     // no measurable change in any tested setting
  Routing,      // send / side-input routing: resolved by the engine graph, not the DSP
  Broken,       // non-finite or exploding output at some value
  Inert,        // no effect by design, documented (kInert in ParamAudit.cpp): display, legacy, reserved, ...
  Verified,     // works with settings the automated search did not reach; verified in the engine source (kInert "VERIFIED:")
};
const char* statusName(Status s);

struct Probe {
  double value = 0;
  double diffDb = -300;     // RMS of (out - default) relative to the default's RMS, dB
  double levelDb = 0;       // output level change vs default, dB
  double brightDb = 0;      // brightness (first-difference RMS / RMS) change, dB
  bool finite = true;
  double peak = 0;
};

struct ParamReport {
  int index = 0;
  std::string name;
  Status status = Status::NoEffect;
  std::vector<Probe> probes;  // at the default base
  double bestDiffDb = -300;
  double bestValue = 0;       // value with the largest change (baseline / fast check)
  // Combination: the base settings that enable the parameter, e.g. {{3, 1.0}}.
  std::vector<std::pair<int, double>> condition;
  std::string conditionText;  // "Filter = On"
  double conditionDiffDb = -300;
  // Live change (automation path, no re-prepare): -300 when not tested.
  bool liveTested = false, live = true;
  double liveDiffDb = -300, preparedDiffDb = -300;
  // Song-level: ParamId set/get, automation mapping, project save/load.
  bool roundTrip = true;
  std::string note;
  std::string inertReason;  // Status::Inert: why
};

// Why a parameter that changes nothing is fine (nullptr = it is not).
const char* inertReason(const std::string& type, const std::string& paramName);

struct PluginReport {
  std::string type, name, kind;  // kind: generator / effect / notefx / controller
  double refLevelDb = -300;      // default render level (dBFS RMS)
  double noiseFloorDb = -300;    // difference between two default renders
  bool created = true;
  std::vector<ParamReport> params;
  double seconds = 0;
  int renders = 0;
};

struct Options {
  bool combos = true;    // search conditions for parameters without effect
  bool liveness = true;  // live-change test of working parameters
  double thresholdDb = -80;  // relative difference counted as "changes the output"
  int maxBases = 400;        // condition search budget per plugin (bases)
  std::vector<std::pair<int, double>> base;  // parameter values on top of the defaults (triage)
  bool probeOnly = false;                     // render the defaults (with base) once, no sweep
};

// One plugin type (any built-in kind).
PluginReport auditPlugin(const PluginInfo& info, const Options& opt);
// Every built-in plugin (or the ones `filter` accepts), on `threads` workers.
std::vector<PluginReport> auditAll(const Options& opt, const std::function<bool(const PluginInfo&)>& filter, int threads,
                                   const std::function<void(const PluginReport&)>& done = {});

// Plugin types being audited right now (async-signal-safe: crash handlers).
void writeActive(int fd);

// Reports.
Json toJson(const std::vector<PluginReport>& r);
std::string toMarkdown(const std::vector<PluginReport>& r);
// Baseline TSV: type, index, name, status, best value, condition ("k=v;k=v").
std::string toBaseline(const std::vector<PluginReport>& r);

// Fast regression check of one baseline row: renders the plugin with the
// row's condition as the base and the parameter at `value`; returns the
// relative difference in dB (-300 = none).
struct BaselineRow {
  std::string type;
  int index = 0;
  std::string name;
  std::string status;
  double value = 0;
  std::vector<std::pair<int, double>> condition;
};
std::vector<BaselineRow> parseBaseline(const std::string& text);
// Checks all rows of one plugin type; returns the difference per row (same order).
std::vector<double> checkRows(const PluginInfo& info, const std::vector<const BaselineRow*>& rows);

}  // namespace gl::paramaudit
