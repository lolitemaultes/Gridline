// Gridline — Document: a Song plus its undo history. This is the only way to
// edit a project; every method below records one undoable Command (UN-01).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Params.h"
#include "Song.h"
#include "Undo.h"

namespace gl {

class Document {
 public:
  Document() = default;
  explicit Document(Song song) : song_(std::move(song)) {}

  const Song& song() const { return song_; }
  // Replaces the whole song (open/new project): history is cleared, state is "saved".
  void reset(Song song);
  // Increments on every change of the song (edit, undo, redo, reset).
  uint64_t revision() const { return revision_; }

  // --- generic object edits ------------------------------------------------
  // `fn` mutates a copy of the object; the change is recorded with `label`.
  // A non-zero mergeKey folds consecutive edits with the same key (drags).
  bool editChannel(ObjectId id, const std::string& label, const std::function<void(Channel&)>& fn, uint64_t mergeKey = 0);
  bool editPattern(ObjectId id, const std::string& label, const std::function<void(Pattern&)>& fn, uint64_t mergeKey = 0);
  bool editClip(ObjectId id, const std::string& label, const std::function<void(Clip&)>& fn, uint64_t mergeKey = 0);
  bool editMixerTrack(ObjectId id, const std::string& label, const std::function<void(MixerTrack&)>& fn, uint64_t mergeKey = 0);
  // Automation clip contents (Song::automations), e.g. point edits (J-5).
  bool editAutomation(ObjectId id, const std::string& label, const std::function<void(AutomationClip&)>& fn,
                      uint64_t mergeKey = 0);
  bool editPlaylistTrack(int index, const std::string& label, const std::function<void(PlaylistTrack&)>& fn);
  // Whole-song edit (snapshot) for rare global operations.
  void editSong(const std::string& label, const std::function<void(Song&)>& fn);

  // --- channels -----------------------------------------------------------
  ObjectId addChannel(const std::string& name, const std::string& pluginType, int index = -1);
  bool removeChannel(ObjectId id);
  bool moveChannel(ObjectId id, int newIndex);
  bool renameChannel(ObjectId id, const std::string& name);
  bool setChannelPlugin(ObjectId id, const std::string& pluginType);
  ObjectId cloneChannel(ObjectId id);  // copy placed after the original, with its pattern data
  bool routeChannel(ObjectId id, ObjectId mixerTrack);

  // --- parameters (AU-01) ---------------------------------------------------
  // Consecutive changes to the same parameter merge into one undo step until
  // endGesture() (e.g. mouse-up) unless merge is false.
  bool setParam(ParamId id, double value, bool merge = true);
  void endGesture() { undo_.breakMerge(); }
  // One pointer gesture (mouse press .. release) = one undo step, whatever the editor under
  // the pointer does: the first edit made during it starts a step and every later edit joins
  // that step (a knob drag, a drawn shape, a drag that changes several parameters or the
  // plugin's data each frame). An undo / redo during the gesture ends the joining. The UI
  // opens it at the frame the press is seen and closes it after the frame of the release.
  void beginPointerGesture();
  void endPointerGesture();
  bool inPointerGesture() const { return pointerGesture_; }
  // Folds the edits `fn` makes into the newest history step when that step is still the one
  // whose state serial is `serial` (nothing was edited, undone or redone since; serial 0 never
  // matches); otherwise they are recorded as usual. True when folded. (A hosted plugin's state
  // captured after its own edit gesture: one undo step for both.)
  bool amendStep(uint64_t serial, const std::function<void()>& fn);
  // The state serial right after the latest undo / redo / jump (0 before any): equal to
  // history().stateSerial() while nothing was edited since.
  uint64_t moveSerial() const { return moveSerial_; }
  // Called after every successful setParam (automation recording listens here).
  std::function<void(ParamId, double)> paramObserver;
  // The parameter most recently changed through setParam and a counter that
  // increases with every such change (MIDI multilink: "move a control, then a
  // hardware knob").
  ParamId lastTouchedParam() const { return lastTouched_; }
  // The parameter changed before the last touched one (a different one; owner kNoId = none).
  ParamId previousTouchedParam() const { return prevTouched_; }
  uint64_t touchSerial() const { return touchSerial_; }

  // --- patterns -----------------------------------------------------------
  ObjectId addPattern(const std::string& name, int bars = 1, int stepsPerBar = 16, int index = -1);
  bool removePattern(ObjectId id);  // also removes its playlist clips
  ObjectId clonePattern(ObjectId id);
  bool renamePattern(ObjectId id, const std::string& name);
  bool setPatternLength(ObjectId id, int bars, int stepsPerBar);
  bool setSteps(ObjectId pattern, ObjectId channel, std::vector<uint8_t> row);
  bool setStep(ObjectId pattern, ObjectId channel, int step, uint8_t velocity);
  bool setNotes(ObjectId pattern, ObjectId channel, std::vector<Note> notes, const std::string& label = "edit notes",
                uint64_t mergeKey = 0);
  bool addNote(ObjectId pattern, ObjectId channel, const Note& note);

  // --- playlist -----------------------------------------------------------
  ObjectId addClip(ObjectId pattern, int track, Tick start, Tick length = 0);
  bool removeClip(ObjectId id);
  bool moveClip(ObjectId id, int track, Tick start);
  bool setClipLength(ObjectId id, Tick length);
  bool setPlaylistTrackCount(int count);
  // Edits clip positions/lengths/tracks in one (mergeable) step. `fn` must not
  // add or remove clips or change their ids.
  bool editClips(const std::string& label, const std::function<void(std::vector<Clip>&)>& fn, uint64_t mergeKey = 0);

  // --- mixer --------------------------------------------------------------
  ObjectId addMixerTrack();
  bool removeMixerTrack(ObjectId id);  // reroutes its channels and senders to the master
  // Sets (or adds) a send; refuses cycles. level <= 0 removes the send.
  bool setSend(ObjectId from, ObjectId to, float level);
  bool setSlotPlugin(ObjectId track, int slot, const std::string& pluginType);  // "" clears the slot
  bool moveSlot(ObjectId track, int from, int to);
  // Marks an existing send as sidechain-only (feeds the destination's
  // sidechain input instead of its mix) or back to a normal send.
  bool setSendSidechain(ObjectId from, ObjectId to, bool sidechainOnly);

  // --- internal controller links --------------------------------------------
  // Adds the link, replacing any link that already drives the same target.
  // Refuses sources without controller outputs and unresolved targets.
  bool setModLink(const ModLink& link);
  bool removeModLink(ParamId target);
  // Edits the link driving `target` (mapping, range, smoothing…); mergeable.
  bool editModLink(ParamId target, const std::string& label, const std::function<void(ModLink&)>& fn,
                   uint64_t mergeKey = 0);

  // --- channel note processors (Channel::noteFx) -----------------------------
  // Inserts a PluginKind::NoteFx plugin at `index` (-1 = end); false when full.
  bool addNoteFx(ObjectId channel, const std::string& type, int index = -1);
  bool removeNoteFx(ObjectId channel, int index);
  bool moveNoteFx(ObjectId channel, int from, int to);
  // Replaces a note processor's whole state (data edits, presets) as one step.
  bool setNoteFxState(ObjectId channel, int index, const PluginState& state, const std::string& label,
                      uint64_t mergeKey = 0);

  // --- song ---------------------------------------------------------------
  // Returns false for a PPQ outside kPpqChoices. `roundedOut` receives how many
  // tick values did not fit the new grid exactly.
  bool setPpq(int ppq, int* roundedOut = nullptr);
  bool setTimeSignature(int num, int den);
  void setInfo(const SongInfo& info);

  // --- grouping -----------------------------------------------------------
  // Edits between begin/end become one undo step. Groups nest; the outermost
  // label wins. Empty groups record nothing.
  void beginGroup(const std::string& label);
  // `joinGesture` false: never folded into a pointer gesture's step (a recorded pass, J-159).
  void endGroup(bool joinGesture = true);
  // Drops the innermost open group: its edits are reverted and nothing is
  // recorded (a script that failed halfway, SC-02).
  void cancelGroup();
  // Number of open groups.
  size_t groupDepth() const { return groups_.size(); }

  // --- history (UN-02) ----------------------------------------------------
  bool undo();
  bool redo();
  void jumpTo(size_t position);
  const UndoStack& history() const { return undo_; }
  void setMaxUndoLevels(size_t n) { undo_.setMaxLevels(n); }

  // --- collaboration (J-111) -------------------------------------------------
  // While a collaboration session is attached: `historyHook(-1 / +1)` replaces
  // undo / redo (the session moves the history position with stepHistory() and
  // changes the song itself, so only the user's own changes are undone), and
  // `beforeChange` runs before an edit that starts a new history step (so the
  // session can file the changes made so far under the previous step).
  std::function<bool(int dir)> historyHook;
  std::function<void()> beforeChange;
  // Moves the history position without applying commands (see historyHook).
  bool stepHistory(int dir);
  // Replaces the song with a merged state from collaborators: not recorded,
  // the history is kept (its steps are undone through historyHook).
  void replaceSong(Song song);
  // A history step that changes nothing in the song, for a change the
  // session's historyHook undoes itself (J-111f: the collaborative project's
  // shared name). Without a session, its undo / redo do nothing.
  void recordMark(const std::string& label);
  // Changes the song in place without recording (session housekeeping).
  void patchSong(const std::function<void(Song&)>& fn) {
    fn(song_);
    ++revision_;
  }
  // Number of edits made outside groups that started a new history step.
  uint64_t pushCount() const { return undo_.pushCount(); }

  // --- saved state --------------------------------------------------------
  bool modified() const { return undo_.stateSerial() != savedSerial_; }
  void markSaved() { savedSerial_ = undo_.stateSerial(); }

 private:
  void exec(CommandPtr cmd, bool mayJoin = true);
  // Records a change of song.modLinks (no-op when equal).
  void setModLinks(std::vector<ModLink> after, const std::string& label);
  Song song_;
  UndoStack undo_;
  std::vector<std::unique_ptr<GroupCommand>> groups_;
  uint64_t savedSerial_ = 0;
  uint64_t revision_ = 0;
  ParamId lastTouched_;
  ParamId prevTouched_;
  uint64_t touchSerial_ = 0;
  bool pointerGesture_ = false;
  uint64_t gestureSerial_ = 0;  // state serial of the step the current pointer gesture made (0: none yet)
  uint64_t moveSerial_ = 0;
};

}  // namespace gl
