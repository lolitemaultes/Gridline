// Gridline - window bodies any host can draw (K-112). See HostedBodies.h.
#include "HostedBodies.h"
#include <cstdio>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "Widgets.h"
#include "../packs/PacksUi.h"

namespace gl {

void drawLoupeBody(App& a, float w, float h) { packs::drawLoupeBody(a, w, h); }

namespace hostbody {

namespace {
constexpr int kCount = static_cast<int>(Body::Count);
const char* gNow[kCount] = {};   // claimed this frame (by host)
const char* gLast[kCount] = {};  // claimed last frame
struct Draw {
  int body = 0;
  ObjectId owner = kNoId;
  int slot = -1, count = 0;
  char host[48] = {};  // the window it was drawn in (the first draw's)
};
constexpr int kDraws = 32;
Draw gDrawNow[kDraws], gDrawLast[kDraws];
int gDrawNowN = 0, gDrawLastN = 0;
}  // namespace

void drawn(Body b, ObjectId owner, int slot) {
  for (int i = 0; i < gDrawNowN; ++i)
    if (gDrawNow[i].body == static_cast<int>(b) && gDrawNow[i].owner == owner && gDrawNow[i].slot == slot) {
      ++gDrawNow[i].count;
      return;
    }
  if (gDrawNowN >= kDraws) return;
  Draw& d = gDrawNow[gDrawNowN++];
  d = Draw{static_cast<int>(b), owner, slot, 1, {}};
  const ImGuiWindow* w = ImGui::GetCurrentContext() ? ImGui::GetCurrentWindowRead() : nullptr;
  if (w) w = w->RootWindow;  // a child (the chain's body) reports its window
  std::snprintf(d.host, sizeof d.host, "%s", w ? ui::stableWindowName(w->Name) : "-");
}

int drawnNow(Body b, ObjectId owner, int slot, std::string* host) {
  for (int i = 0; i < gDrawNowN; ++i)
    if (gDrawNow[i].body == static_cast<int>(b) && gDrawNow[i].owner == owner && gDrawNow[i].slot == slot) {
      if (host) *host = gDrawNow[i].host;
      return gDrawNow[i].count;
    }
  if (host) host->clear();
  return 0;
}

std::string drawnReport() {
  std::string out;
  for (int i = 0; i < gDrawLastN; ++i) {
    const Draw& d = gDrawLast[i];
    out += std::string(out.empty() ? "" : "; ") + name(static_cast<Body>(d.body)) + " " + std::to_string(static_cast<unsigned long long>(d.owner)) +
           "/" + std::to_string(d.slot) + " x" + std::to_string(d.count) + " @" + d.host;
  }
  return out;
}

const char* name(Body b) {
  switch (b) {
    case Body::ChannelSettings: return "channel settings";
    case Body::EffectEditor: return "effect editor";
    case Body::MasterChain: return "master chain";
    case Body::Loupe: return "Loupe";
    default: return "";
  }
}

void claim(Body b, const char* host) {
  const int k = static_cast<int>(b);
  if (k >= 0 && k < kCount) gNow[k] = host && *host ? host : "host";
}

bool claimed(Body b) {
  const int k = static_cast<int>(b);
  return k >= 0 && k < kCount && (gLast[k] != nullptr || gNow[k] != nullptr);
}

const char* claimedBy(Body b) {
  const int k = static_cast<int>(b);
  if (k < 0 || k >= kCount) return "";
  return gNow[k] ? gNow[k] : gLast[k] ? gLast[k] : "";
}

void frameBegin() {
  for (int k = 0; k < kCount; ++k) {
    gLast[k] = gNow[k];
    gNow[k] = nullptr;
  }
  for (int i = 0; i < gDrawNowN; ++i) gDrawLast[i] = gDrawNow[i];
  gDrawLastN = gDrawNowN;
  gDrawNowN = 0;
}

}  // namespace hostbody
}  // namespace gl
