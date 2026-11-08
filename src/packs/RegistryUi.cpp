// Gridline — plugin pack registry (editor side).
#include "PacksUi.h"

GL_DECLARE_PACK_UI(synths)
GL_DECLARE_PACK_UI(samplers)
GL_DECLARE_PACK_UI(spacefx)
GL_DECLARE_PACK_UI(spectral)
GL_DECLARE_PACK_UI(dynamics)
GL_DECLARE_PACK_UI(modulation)
GL_DECLARE_PACK_UI(models)
GL_DECLARE_PACK_UI(utility)
GL_DECLARE_PACK_UI(creative)
GL_DECLARE_PACK_UI(edison)
GL_DECLARE_PACK_UI(flexpack)
GL_DECLARE_PACK_UI(tideline)
GL_DECLARE_PACK_UI(seltmann)
GL_DECLARE_PACK_UI(plasma)
GL_DECLARE_PACK_UI(mini)
GL_DECLARE_PACK_UI(platter)
GL_DECLARE_PACK_UI(trackerpack)
namespace gl::packs::edison {
void drawWindows(App& a);
bool openAudioEditor(App& a, const std::string& path, ObjectId channel);
int armTransportRecording(App& a, ObjectId track);
bool claimEffectEditor(App& a, const PluginTarget& t);  // K-15
bool dockDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);
}  // namespace gl::packs::edison
namespace gl::packs::modulation {
bool junctionDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);  // K-7
}  // namespace gl::packs::modulation
namespace gl::packs::synths {
void tickPictureLoads(App& a);                                              // J-184
bool loadPicture(App& a, const PluginTarget& t, const std::string& path);  // J-184
}  // namespace gl::packs::synths
namespace gl::packs::seltmann {
void tick(App& a);  // J-218: the start screen recorder drains the input tap
}  // namespace gl::packs::seltmann
namespace gl::packs::platter {
void tick(App& a);
bool claimMidi(App& a, const std::string& device);
bool recordsWithTransport(App& a);
}  // namespace gl::packs::platter

#define GL_PACK_LIST X(synths) X(samplers) X(spacefx) X(spectral) X(dynamics) X(modulation) X(models) X(utility) X(creative) X(edison) X(flexpack) X(tideline) X(seltmann) X(plasma) X(mini) X(platter) X(trackerpack)

namespace gl::packs {

bool drawEditor(App& a, const PluginTarget& t, float width) {
#define X(P) \
  if (P::drawEditor(a, t, width)) return true;
  GL_PACK_LIST
#undef X
  return false;
}

void drawWindows(App& a) {
  edison::drawWindows(a);
  platter::tick(a);  // J-150: decks (controller values, cues, takes, LEDs)
  synths::tickPictureLoads(a);  // J-184: Pixel Tone pictures decoded in the background
  seltmann::tick(a);  // J-218: recording into Seltmann (also while its editor is hidden)
}

bool loadPicture(App& a, const PluginTarget& t, const std::string& path) { return synths::loadPicture(a, t, path); }

bool claimMidi(App& a, const std::string& device) { return platter::claimMidi(a, device); }

bool recordsWithTransport(App& a) { return platter::recordsWithTransport(a); }

bool openAudioEditor(App& a, const std::string& path, ObjectId channel) { return edison::openAudioEditor(a, path, channel); }

bool claimEffectEditor(App& a, const PluginTarget& t) { return edison::claimEffectEditor(a, t); }

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) { return edison::dockDevCommand(a, cmd, args) || modulation::junctionDevCommand(a, cmd, args); }

int armAudioEditorRecording(App& a, ObjectId track) { return edison::armTransportRecording(a, track); }

}  // namespace gl::packs
