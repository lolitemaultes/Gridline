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
namespace gl::packs::edison {
void drawWindows(App& a);
bool openAudioEditor(App& a, const std::string& path, ObjectId channel);
int armTransportRecording(App& a, ObjectId track);
}  // namespace gl::packs::edison
namespace gl::packs::platter {
void tick(App& a);
bool claimMidi(App& a, const std::string& device);
}  // namespace gl::packs::platter

#define GL_PACK_LIST X(synths) X(samplers) X(spacefx) X(spectral) X(dynamics) X(modulation) X(models) X(utility) X(creative) X(edison) X(flexpack) X(tideline) X(seltmann) X(plasma) X(mini) X(platter)

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
}

bool claimMidi(App& a, const std::string& device) { return platter::claimMidi(a, device); }

bool openAudioEditor(App& a, const std::string& path, ObjectId channel) { return edison::openAudioEditor(a, path, channel); }

int armAudioEditorRecording(App& a, ObjectId track) { return edison::armTransportRecording(a, track); }

}  // namespace gl::packs
