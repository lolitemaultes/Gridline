// Gridline tests — a plain native view for the in-tree test plugins'
// interfaces (macOS: an NSView that fills itself with a colour and draws its
// label; TestViewMac.mm). Each test plugin compiles it with its own class
// name (GRIDLINE_TEST_VIEW_CLASS) so several can load into one process.
#pragma once

// Adds a w x h view to `parent` (NSView*); returns it (nullptr on failure).
void* gridlineTestViewAttach(void* parent, int w, int h, const char* label, float r, float g, float b);
void gridlineTestViewResize(void* view, int w, int h);
void gridlineTestViewRemove(void* view);
