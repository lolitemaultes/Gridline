// Gridline — the one translation unit that compiles nanosvg + nanosvgrast
// (third_party/nanosvg, zlib licence, vendored unmodified). Its target builds
// it with warnings off (vendored code), like the other third-party sources.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define NANOSVG_IMPLEMENTATION
#include "../../../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../../../third_party/nanosvg/nanosvgrast.h"
