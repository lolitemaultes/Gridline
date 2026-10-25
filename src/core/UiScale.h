// Gridline - J-91: the interface scale the operating system asks for.
//
// Gridline lays its interface out in logical pixels and multiplies them by
// one scale factor. Part of that factor comes from the system:
//  * macOS: always 1 here; Retina screens are handled by the backing scale
//    (points to pixels), so nothing is added on top.
//  * Windows: the per-monitor DPI SDL reports for the window's display
//    (Settings > Display > Scale; 96 dpi = 100 %), unchanged since 0.5.
//  * Linux and the BSDs: the desktop's own scale setting, never the panel's
//    physical pixel density. A 15" 1920x1080 laptop panel has ~141 physical
//    dpi but its desktop runs at 100 %; 0.6.0 scaled such a panel to 150 %
//    (GitHub issue #1). In order:
//      1. GDK_SCALE / GDK_DPI_SCALE (set by the user or the session),
//      2. QT_SCALE_FACTOR,
//      3. a window that SDL already scales (Wayland output scale: drawable
//         larger than the window): 1, the compositor does it,
//      4. Xft.dpi from the X resource database (what GNOME, Zorin, KDE, Xfce
//         and Cinnamon write for their scale and text-scaling settings;
//         96 = 100 %),
//      5. otherwise 1.
// Settings > Interface "Follow the system's display scale" (ui.scaleAuto,
// on by default) multiplies the user's "Interface scale" by this; turned off,
// the user's scale is used as it is.
// Pure functions: the inputs are injected (unit tested in test_j91).
#pragma once
#include <string>

namespace gl::uiscale {

enum class Desktop { MacOS, Windows, X11, Wayland, Other };

struct Inputs {
  Desktop desktop = Desktop::Other;
  float displayDpi = 0;  // SDL_GetDisplayDPI of the window's display (Windows), 0 = unknown
  float xftDpi = 0;      // Xft.dpi from the X resource database, 0 = not set
  int windowW = 0;       // window size in SDL units
  int drawableW = 0;     // drawable (framebuffer) width in pixels
  // Environment (nullptr = not set).
  const char* gdkScale = nullptr;
  const char* gdkDpiScale = nullptr;
  const char* qtScaleFactor = nullptr;
};

// The system's scale factor for the interface (>= 1, in quarter steps).
float systemScale(const Inputs& in);
// Where it came from, for the settings page and logs ("Xft.dpi 96", ...).
std::string systemScaleSource(const Inputs& in);

// Xft.dpi in an X resource database string (XResourceManagerString), 0 when absent.
float parseXftDpi(const char* resources);
// J-111g: Xcursor.size in an X resource database string (8..512), 0 when absent.
int parseXcursorSize(const char* resources);

// J-111g: collaborators' cursors are drawn the size of this computer's own pointer.
// The height of the system's arrow (the tip to the end of its tail, outline
// included) in the window units the nominal size is in, for a nominal pointer size
// (macOS: the arrow image's height in points; Windows: SM_CYCURSOR; Linux and the
// BSDs: Xcursor.size / XCURSOR_SIZE / GNOME's cursor-size; 0 = the default) and
// the accessibility multiplier (macOS "Pointer size"; 0 or 1 = none).
enum class PointerSystem { MacOS, Windows, Unix };
double pointerArrowHeight(PointerSystem sys, double nominal, double multiplier);

// The interface scale to use: the user's setting (clamped 0.75 .. 2) times
// the system scale when `followSystem`; never more than fits a window of
// winW x winH logical pixels at the minimum layout size 960 x 600.
float effectiveScale(double userScale, bool followSystem, float system, int winW, int winH);

// The system scale found at startup (the application sets it once; plugin
// windows hand it to installed plugins as their content scale on X11).
void setDetected(float s);
float detected();

}  // namespace gl::uiscale
