// Gridline - K-105: Interactive non-blocking Guided Tour inside a real project.
// Dynamically tracks live window/panel rectangles, draws a spotlight cutout ring,
// and positions a floating coachmark card that adapts to every layout.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::guidedtour {

// Start the guided tour immediately at step 0 (`loadDemoFirst` loads the demo song first).
void start(App& a, bool loadDemoFirst = false);
// Triggered from Help > Guided tour...: if the current song is empty, offers a quick choice
// to load the demo song or tour the current project; otherwise starts the tour right away.
void openFromMenu(App& a);
// Stop the tour (or empty-project prompt).
void stop();
// Step navigation.
void next(App& a);
void back(App& a);
void setStep(App& a, int stepIndex);

bool active();
int currentStep();

// Draw spotlight scrim, glowing target ring, and floating coachmark card (call once per frame
// after windows and before ImGui::Render).
void draw(App& a);

// Scripted test / screenshot hook (--dev-input "tour start [demo]|next|back|step <1..N>|stop|state").
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::guidedtour
