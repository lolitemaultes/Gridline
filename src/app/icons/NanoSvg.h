// Gridline — nanosvg / nanosvgrast declarations (third_party/nanosvg, zlib licence).
// The implementation is compiled once, warnings off, in NanoSvgImpl.cpp.
#pragma once
#include <cstdio>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "../../../third_party/nanosvg/nanosvg.h"
#include "../../../third_party/nanosvg/nanosvgrast.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
