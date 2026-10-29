// Gridline — J-167: the project loading window. While a project opens (App::openProject ->
// projload::Loader) a modal window blocks every input to the project behind from the first
// frame; it becomes visible after kShowAfter seconds (fast loads never flash it), dims the
// windows behind and shows the project name, a progress bar over the real work units, the
// current step, Cancel (Esc) and a log of what was loaded with times (warnings amber, slow
// plugins marked). A project that cannot open shows why in the same window, with Close.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::projloadui {

inline constexpr double kShowAfter = 0.3;  // seconds before the window shows (input is blocked at once)

// Per frame, at the root level after every other window and dialog (it replaces any open
// popup or menu when a load starts).
void draw(App& a);
// The log's filter: only warnings, errors and slow plugins (the footer's warning count toggles it).
void setWarningsOnly(bool on);
// Dev check (--dev-input "loadstate"): the load's phase, counts, the window and the popups to stdout.
void devReport(App& a, int frame);

}  // namespace gl::projloadui
