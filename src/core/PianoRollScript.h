// Gridline — piano roll scripts (SC-04): Lua scripts that read and rewrite the
// notes of the piano roll's target (current pattern, selected channel), with a
// declarative parameter form the piano roll shows as a tool window (knobs,
// combos, check boxes, text fields; live preview, Accept / Cancel, one undo
// step).
//
// Script layout (Gridline's own API):
//
//   -- name: Euclidean rhythm
//   -- category: Rhythm
//   -- description: Spreads hits evenly over a number of steps.
//   function form(f)
//     f:knobInt("Hits", 5, 1, 32, "How many notes")
//     f:combo("Grid", {"1/16", "1/8"}, 1)
//     f:check("Replace notes", true)
//   end
//   function apply(score, p)            -- p["Hits"], p["Grid"] (1-based), …
//     for i, n in score:notes() do n.velocity = 90 end
//     score:add{time = 0, length = score.ppq, pitch = 60}
//   end
//
// Notes are Lua tables {time, length, pitch, velocity 0..127, pan -64..64,
// fine (cents), release, modx, mody 0..255, color 0..15, slide, porta, muted,
// selected}; score:get(i) returns the live table (1-based). A script without
// `apply` does its work at the top level. Every run starts a fresh VM, and
// math.random is seeded with score.seed, so a run is deterministic.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "Script.h"
#include "Song.h"

namespace gl::script {

// ------------------------------------------------------------------ forms ----
enum class FormKind { Knob, KnobInt, Combo, Check, Text };
struct FormParam {
  FormKind kind = FormKind::Knob;
  std::string name, hint, group;
  double value = 0;      // Knob / KnobInt; Combo: 1-based item index; Check: 0 / 1
  double min = 0, max = 1;
  std::vector<std::string> items;  // Combo
  std::string text;                // Text
  bool operator==(const FormParam& o) const {
    return kind == o.kind && name == o.name && hint == o.hint && group == o.group && value == o.value && min == o.min &&
           max == o.max && items == o.items && text == o.text;
  }
};
struct Form {
  std::string description;
  std::vector<FormParam> params;
  int find(const std::string& name) const;
};

// ------------------------------------------------------------- run context ----
struct PianoRollContext {
  std::vector<Note> notes;
  std::vector<bool> sel;  // same size as notes (empty = nothing selected)
  std::vector<Marker> markers;
  int ppq = kDefaultPpq;
  int tsNum = 4, tsDen = 4;
  Tick length = 0;        // pattern length in ticks
  int root = 0;           // 0..11
  int scale = 0;          // tools::scales() index (0 = none)
  Tick selStart = 0, selEnd = 0;  // time selection (selEnd <= selStart: none)
  uint32_t seed = 1;
  std::string channel, pattern;
};

struct PianoRollResult {
  bool ok = false;
  std::string error;
  std::vector<Note> notes;  // sorted by start, pitch (sel follows)
  std::vector<bool> sel;
  std::vector<Marker> markers;
  bool markersChanged = false;
  std::vector<std::string> log;
};

// ------------------------------------------------------------- script list ----
struct ScriptInfo {
  std::string path, id, name, category, description, author;
  bool user = false;  // from the user folder
};
// *.lua files of each folder (non-recursive), sorted by category then name.
// A later folder's script with the same id (file stem) replaces an earlier one.
std::vector<ScriptInfo> scanScripts(const std::vector<std::pair<std::string, bool>>& dirs);

class PianoRollScript {
 public:
  static constexpr size_t kMaxNotes = 200000;

  bool loadFile(const std::string& path, std::string* err = nullptr);
  // Parses the header and builds the form (runs the script once on an empty score).
  bool loadSource(const std::string& source, const std::string& name, std::string* err = nullptr);

  const std::string& name() const { return name_; }
  const Header& header() const { return header_; }
  const Form& form() const { return form_; }
  const std::string& path() const { return path_; }
  bool loaded() const { return !source_.empty(); }

  // Runs the script on `ctx` with the form values `values` (defaults when empty).
  // `fileRoot` = folder for the files API ("" = none, read-only).
  PianoRollResult run(const PianoRollContext& ctx, const std::vector<FormParam>& values = {}, const Limits& limits = {},
                      const std::string& fileRoot = "") const;

 private:
  std::string source_, name_, path_;
  Header header_;
  Form form_;
};

}  // namespace gl::script
