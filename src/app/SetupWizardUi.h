// Gridline - K-105: First-run setup wizard (reopenable anytime from Help > Setup wizard...).
// 4-step wizard: Workspace layout, Look & theme (live preview), Audio & plugins (with test tone),
// and Ready to make music (demo + guided tour, empty project, or finish).
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::setupwizard {

// Open the setup wizard (step 0 = Workspace). `firstRun` chooses welcoming copy vs reopen copy.
void open(App& a, bool firstRun = false);
// Close the wizard without triggering a launch action; marks ui.setupDone & ui.layoutChosen.
void close(App& a, bool markDone = true);
// True while the wizard is open.
bool visible();
// Current 0-based step (0..3).
int currentStep();
// Jump directly to a 0-based step (0..3), opening the wizard if needed.
void setStep(App& a, int stepIndex);

// Draw the wizard overlay (call once per frame inside dialogs pass).
void draw(App& a);

// Scripted test / screenshot hook (--dev-input "wizard open|step <1..4>|next|back|close|state").
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::setupwizard
