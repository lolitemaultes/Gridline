// Gridline — project scripts (SC-02): the whole project model for Lua.
//
// One global table `project` holds the modules (see docs/scripting.md for the
// full reference):
//   project.song        tempo, time signature, swing, title, length
//   project.channels    channel rack: names, colours, strip, instrument,
//                       plugin parameters, routing, add / remove / clone
//   project.patterns    patterns: names, lengths, steps, notes
//   project.playlist    tracks and clips (pattern, audio, automation)
//   project.automation  automation clips and their points
//   project.mixer       mixer tracks, sends, effect slots, slot parameters
//   project.params      any parameter by reference (get / set / name / range)
//   project.markers     song markers
//   project.selection   what is selected in the editors
//   project.transport   play state and position (read-only unless allowed)
//
// Indices are 1-based (channels, patterns, playlist tracks, automation clips,
// markers, effect slots, parameters) except mixer tracks: 0 = master, 1.. =
// inserts, as on screen. Clips are named by their id (stable while the script
// runs). Every change goes through Document, so the UI, undo and the engine
// see it; runProject() wraps a run in one undo group (one undo step), and a
// failed run leaves the project as it was.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Document.h"
#include "PianoRollScript.h"
#include "Script.h"

namespace gl::script {

// Editor state a project script can see (and partly set). Selection is UI
// state, not part of the document: changing it is not an undo step.
class ProjectHost {
 public:
  explicit ProjectHost(Document& d) : doc(d) {}
  virtual ~ProjectHost() = default;
  Document& doc;

  // Transport. Scripts may only start / stop / move it when allowTransport.
  bool allowTransport = false;
  virtual bool playing() const = 0;
  virtual bool recording() const = 0;
  virtual bool songMode() const = 0;
  virtual Tick position() const = 0;
  virtual void play(bool on) = 0;
  virtual void setPosition(Tick t) = 0;

  // Selection (object ids / indices of the Song containers).
  virtual std::vector<ObjectId> selectedChannels() const = 0;
  virtual void selectChannels(const std::vector<ObjectId>& ids) = 0;
  virtual std::vector<ObjectId> selectedClips() const = 0;
  virtual void selectClips(const std::vector<ObjectId>& ids) = 0;
  virtual ObjectId currentPattern() const = 0;
  virtual void setCurrentPattern(ObjectId id) = 0;
  virtual ObjectId selectedMixer() const = 0;
  virtual void selectMixer(ObjectId id) = 0;
  // Playlist time selection (end <= start: none).
  virtual std::pair<Tick, Tick> timeSelection() const = 0;
  virtual void setTimeSelection(Tick start, Tick end) = 0;
};

// Keeps its own transport and selection: tests, batch runs, the console.
class SimpleProjectHost : public ProjectHost {
 public:
  using ProjectHost::ProjectHost;
  bool playing() const override { return isPlaying; }
  bool recording() const override { return false; }
  bool songMode() const override { return isSongMode; }
  Tick position() const override { return pos; }
  void play(bool on) override { isPlaying = on; }
  void setPosition(Tick t) override { pos = t; }
  std::vector<ObjectId> selectedChannels() const override { return channels; }
  void selectChannels(const std::vector<ObjectId>& ids) override { channels = ids; }
  std::vector<ObjectId> selectedClips() const override { return clips; }
  void selectClips(const std::vector<ObjectId>& ids) override { clips = ids; }
  ObjectId currentPattern() const override { return pattern; }
  void setCurrentPattern(ObjectId id) override { pattern = id; }
  ObjectId selectedMixer() const override { return mixer; }
  void selectMixer(ObjectId id) override { mixer = id; }
  std::pair<Tick, Tick> timeSelection() const override { return {selStart, selEnd}; }
  void setTimeSelection(Tick s, Tick e) override { selStart = s, selEnd = e; }

  bool isPlaying = false, isSongMode = true;
  Tick pos = 0, selStart = 0, selEnd = 0;
  std::vector<ObjectId> channels, clips;
  ObjectId pattern = kNoId, mixer = kNoId;
};

// Registers `project` in `vm` (vm.user must stay free: the host is kept in
// the registry). Safe to open next to the controller API.
void openProjectApi(Vm& vm, ProjectHost& host);

struct ProjectRunResult {
  bool ok = false;
  bool changed = false;   // the run recorded an undo step
  bool limitHit = false;  // stopped by the instruction / time limit
  std::string error;
  std::vector<std::string> log;
};

// Runs `code` in `vm` (project API already open) as one undo step labelled
// `label`; on error the edits made so far are reverted.
ProjectRunResult runProject(Vm& vm, ProjectHost& host, const std::string& code, const std::string& chunk,
                            const std::string& label);

// A project script file: header, optional form(f), apply(p) or top-level code.
class ProjectScript {
 public:
  bool loadFile(const std::string& path, std::string* err = nullptr);
  bool loadSource(const std::string& source, const std::string& name, std::string* err = nullptr);
  const std::string& name() const { return name_; }
  const std::string& path() const { return path_; }
  const Header& header() const { return header_; }
  const Form& form() const { return form_; }
  bool loaded() const { return !source_.empty(); }

  // One run = one undo step ("script: <name>"). `fileRoot` = folder for the
  // read-only files API ("" = none). math.random is seeded with `seed`.
  ProjectRunResult run(ProjectHost& host, const std::vector<FormParam>& values = {}, const Limits& limits = {},
                       const std::string& fileRoot = "", uint32_t seed = 1) const;

 private:
  std::string source_, name_, path_;
  Header header_;
  Form form_;
};

}  // namespace gl::script
