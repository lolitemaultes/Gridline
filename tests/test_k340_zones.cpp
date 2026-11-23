// Gridline - zone keys for the Studio layout (core/ZoneKeys.h): Tab alone, Tab + digit, Tab + digit +
// digit, key repeat ignored, a cancel by focus loss, back toggling two zones, the zones' text.
#include <string>
#include "Check.h"
#include "core/ZoneKeys.h"

using namespace gl;
using zones::Action;

int main() {
  // ---- the layer ----
  {
    zones::Layer l;
    expect(l.digit(1).kind == Action::None && !l.digit(1).swallow, "a digit without Tab is left alone");
    const Action d = l.tabDown();
    expect(d.kind == Action::None && d.swallow && l.held(), "Tab down holds the layer (and is taken)");
    const Action up = l.tabUp();
    expect(up.kind == Action::Back && up.swallow && !l.held(), "Tab pressed and let go alone goes back");
  }
  {
    zones::Layer l;
    l.tabDown();
    const Action a = l.digit(3);
    expect(a.kind == Action::Zone && a.zone == 3 && a.swallow, "Tab + 3 jumps to zone 3, the digit taken");
    expect(l.tabUp().kind == Action::None, "after a digit, letting go of Tab does not go back");
  }
  {
    zones::Layer l;
    l.tabDown();
    const Action a = l.digit(1), b = l.digit(2);
    expect(a.zone == 1 && b.kind == Action::Zone && b.zone == 2 && b.swallow, "Tab + 1 + 2: two jumps while Tab is held");
    expect(l.tabUp().kind == Action::None, "and no back when Tab is let go");
  }
  {
    zones::Layer l;
    l.tabDown();
    for (int i = 0; i < 10; ++i) l.tabDown(true);
    expect(l.tabUp().kind == Action::Back, "key repeats of Tab are ignored (Tab alone still goes back)");
    l.tabDown();
    l.tabDown(true);
    expect(l.digit(2).zone == 2, "a digit after repeats still jumps");
  }
  {
    zones::Layer l;
    l.tabDown();
    l.cancel();
    expect(!l.held() && l.tabUp().kind == Action::None, "a cancel (focus loss) ends the layer: no back, nothing");
    expect(l.digit(1).kind == Action::None, "after a cancel digits are left alone");
    l.tabDown();
    expect(l.digit(0).kind == Action::None && l.digit(12).kind == Action::None, "0 and other numbers are not zones");
    expect(l.tabUp().kind == Action::Back, "Tab alone after a non-zone key still goes back");
  }
  // ---- history ----
  {
    zones::History h;
    expect(h.back() == 0, "no history: nowhere to go back to");
    h.visit(1);
    h.visit(3);
    expect(h.back() == 1 && h.current() == 1, "back goes to the zone before");
    expect(h.back() == 3 && h.current() == 3, "back again toggles to the other zone");
    h.visit(3);
    expect(h.previous() == 1, "visiting the zone you are in changes nothing");
    h.visit(2);
    expect(h.back() == 3, "the two most recent zones");
  }
  // ---- zones ----
  {
    const zones::Zones z = zones::defaults();
    expect(z[0].windows[0] == "Playlist" && z[1].windows[0] == "Piano roll" && z[2].windows[0] == "Mixer" && z[2].windows[1] == "Channel rack",
           "defaults: 1 playlist, 2 piano roll, 3 mixer and channel rack");
    bool rest = true;
    for (int i = 3; i < zones::kZones; ++i) rest = rest && z[static_cast<size_t>(i)].empty();
    expect(rest, "4 to 9 are empty");
    expect(zones::toText(z) == "1=Playlist;2=Piano roll;3=Mixer|Channel rack", "the zones as settings text");
    expect(zones::fromText(zones::toText(z)) == z, "and back");
    expect(zones::fromText("") == z, "empty text: the defaults");
    const zones::Zones c = zones::fromText("2=Mixer;x=Playlist;0=Mixer;10=Stage;5=Browser|Browser;7=|Loupe;3=Bad;Name");
    expect(c[1].windows[0] == "Mixer" && c[0].empty() && c[4].windows[0] == "Browser" && c[4].windows[1].empty(), "broken parts are skipped, a window kept once");
    expect(c[6].windows[0] == "Loupe" && c[2].windows[0] == "Bad", "a zone with only a second window takes it as its first");
    expect(zones::pick(z[2], false, 0) == 0 && zones::pick(z[2], true, 0) == 1 && zones::pick(z[2], true, 1) == 0, "zone 3 again swaps the mixer and the channel rack");
    expect(zones::pick(z[0], true, 0) == 0, "a one-window zone always shows it");
    expect(zones::pick(z[5], false, 0) == -1, "an empty zone shows nothing");
  }
  return finish();
}
