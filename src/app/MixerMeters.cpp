#include "MixerMeters.h"
#include <map>
#include "../../third_party/imgui/imgui.h"
#include "App.h"

namespace gl::mixmeters {

Reading get(App& a, ObjectId track) {
  struct Entry {
    int frame = -1;
    Reading r;
  };
  static std::map<ObjectId, Entry> cache;
  if (cache.size() > 4096) cache.clear();  // deleted tracks never pile up
  const int frame = ImGui::GetFrameCount();
  Entry& e = cache[track];
  if (e.frame != frame) {
    e.frame = frame;
    e.r = Reading{};
    if (auto rt = a.builder.mixerRuntime(track)) {
      e.r.peakL = rt->peakL.exchange(0);
      e.r.peakR = rt->peakR.exchange(0);
      e.r.rmsL = rt->rmsL.exchange(0);
      e.r.rmsR = rt->rmsR.exchange(0);
    }
  }
  return e.r;
}

}  // namespace gl::mixmeters
