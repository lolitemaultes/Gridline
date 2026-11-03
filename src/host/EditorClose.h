// Gridline — taking a bridged plugin's interface down without blocking the UI thread (K-37).
//
// A plugin bridged from Windows with yabridge can hang its own process while its interface closes
// (Dexed on Wine, in every version tested). The call then waits inside the bridge for good, so on
// the UI thread Gridline would stop answering. Plugin editor calls normally belong on the UI thread
// and a helper thread would crash other plugins, so this is used for bridged plugins only
// (Backend::bridged): the bridge makes the real call in the other process, on that process's own
// UI thread, and here it is a socket exchange. Every other plugin closes as it always did.
//
// The thread keeps what it uses alive: when the plugin never comes back, the caller just stops
// waiting, and the editor stays in memory with the thread still waiting in it.
#pragma once
#include <memory>

namespace gl::host {

class Editor;

class EditorClose {
 public:
  // Starts Editor::detachOffThread on a thread of its own. `e` has to stay alive until done(), or
  // for good when the caller gives up.
  explicit EditorClose(Editor* e);
  // Waits up to `ms` for it to finish (UI thread). True when it finished.
  bool waitFor(int ms);
  bool done() const;
  // It finished by throwing: the bridge's other process is gone.
  bool failed() const;

 private:
  struct Shared;
  std::shared_ptr<Shared> s_;
};

}  // namespace gl::host
