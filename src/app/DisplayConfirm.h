// Gridline - "Keep this change?" for settings that can make the interface hard
// to use or cut the audio (J-77): interface size, theme / contrast /
// brightness, language, window layout reset, audio device / rate / buffer.
//
// One change waits at a time. Two kinds:
//  * PreviewFirst - nothing changes until Keep (interface size, layout reset):
//    the dialog previews before / after; Revert or the countdown drops it.
//  * ApplyFirst - applied at once so it can be judged in place (theme,
//    language, audio, like an OS display / sound dialog); Revert or the
//    countdown restores the earlier settings.
// The countdown (default 15 s, Settings "ui.confirmAutoRevert") reverts
// a change nobody confirms, so a change that makes the interface unusable
// undoes itself.
//
// No ImGui here (unit tested); the dialog is drawn by DisplayConfirmUi.cpp.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gl {
class Settings;
}

namespace gl::dispconf {

enum class Mode { PreviewFirst, ApplyFirst };
enum class Result { None, Kept, Reverted, TimedOut };
// What the dialog draws between the title and the buttons.
enum class Preview { None, Scale, Theme, Language, Audio, Layout };

// ---- settings snapshots (ApplyFirst changes of plain settings)
struct Key {
  std::string key;
  char kind;  // 'b' bool, 'n' number, 's' string
};
struct Snapshot {
  struct Val {
    Key key;
    bool b = false;
    double n = 0;
    std::string s;
  };
  std::vector<Val> vals;
  bool operator==(const Snapshot& o) const;
  bool operator!=(const Snapshot& o) const { return !(*this == o); }
};
struct Change {
  std::string id;               // the setting (one pending change per id)
  std::string title;            // English, translated when drawn ("Use this interface size?")
  std::string label;            // English row caption ("Interface size")
  std::string before, after;    // values as shown ("100%" / "150%", already in display form)
  Mode mode = Mode::PreviewFirst;
  Preview preview = Preview::None;
  double beforeValue = 0, afterValue = 0;  // numeric values for the preview (scale factors)
  std::function<void()> apply;   // makes the change (PreviewFirst: on Keep; ApplyFirst: at once)
  std::function<void()> revert;  // ApplyFirst: restores what was there before
  std::function<void()> revertQuiet;  // the same for the stored settings only (quitting: no theme / device calls)
  // Texts in the language that was active before the change (language
  // changes show both, so the buttons stay readable in either language).
  std::string titleBefore, keepBefore, revertBefore;
  // ApplyFirst snapshot changes: the settings on either side (theme preview).
  Snapshot fromSnap, toSnap;
};

class Confirm {
 public:
  double timeout = 15.0;   // s until an unconfirmed change reverts
  bool autoRevert = true;

  // Starts confirming `c` at time `now` (s). A change to the value already
  // in place is ignored. While another change of the same setting waits, the
  // new value replaces it and Revert still goes back to the first one's
  // starting point; a different setting's change resolves the waiting one
  // first (applied changes are kept, previews dropped).
  void request(Change c, double now);
  bool pending() const { return pending_; }
  const Change* current() const { return pending_ ? &cur_ : nullptr; }
  void keep();
  void revert();
  // Quitting with a change still waiting: the earlier settings are what gets saved.
  void abandon();
  // Per frame: reverts on time-out. Returns the result of this call (TimedOut or None).
  Result tick(double now);
  // Seconds left on the countdown (timeout when it is off).
  double remaining(double now) const;
  double openedAt() const { return opened_; }
  Result last() const { return last_; }
  // Number of requests that opened (or re-opened) the dialog (for the UI's appear animation).
  int generation() const { return generation_; }

 private:
  void finish(Result r);
  Change cur_;
  bool pending_ = false;
  double opened_ = 0;
  Result last_ = Result::None;
  int generation_ = 0;
};

// The application's confirmation (one per process; drawn by drawDialogs).
Confirm& confirm();

Snapshot take(const Settings& st, const std::vector<Key>& keys);
void restore(Settings& st, const Snapshot& snap);

// The settings a theme change touches (mode, colours, contrast, brightness).
const std::vector<Key>& themeKeys();
const std::vector<Key>& audioKeys();  // sample rate + buffer

// An ApplyFirst change between two snapshots of the same keys; `onApplied`
// runs after either side is written (re-apply the theme, reopen the audio
// device, reload the language).
Change snapshotChange(Settings& st, std::string id, std::string title, std::string label, std::string before, std::string after,
                      Snapshot from, Snapshot to, Preview preview, std::function<void()> onApplied);

// Interface size: PreviewFirst; Keep writes "ui.scale".
Change scaleChange(Settings& st, double to);
std::string scaleText(double scale);  // "125%"

// A slider whose value only counts once it is let go (the interface size:
// nothing changes while dragging).
struct DeferredValue {
  bool editing = false;
  double value = 0;
  double shown(double committed) const { return editing ? value : committed; }
  void edit(double v) {
    editing = true;
    value = v;
  }
  // Released: true (and `*out`) when the value differs from `committed`.
  bool release(double committed, double* out);
};

}  // namespace gl::dispconf
