// Gridline — <SDL_syswm.h> without the window system's macros.
// On X11 it includes <X11/Xlib.h>, whose macros (None, Bool, Status, Success, Always, ...)
// would replace Gridline's own names (enum values like Kind::None); on Windows it includes
// <windows.h>. Include this instead of <SDL_syswm.h>, after the other headers.
#pragma once
#include <SDL_syswm.h>
#ifdef None
#undef None
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef Status
#undef Status
#endif
#ifdef Success
#undef Success
#endif
#ifdef Always
#undef Always
#endif
#ifdef True
#undef True
#endif
#ifdef False
#undef False
#endif
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef Expose
#undef Expose
#endif
#ifdef CurrentTime
#undef CurrentTime
#endif
#ifdef Above
#undef Above
#endif
#ifdef Below
#undef Below
#endif
#ifdef DestroyAll
#undef DestroyAll
#endif
#ifdef Complex
#undef Complex
#endif
#ifdef Convex
#undef Convex
#endif
#ifdef GrayScale
#undef GrayScale
#endif
#ifdef Unsorted
#undef Unsorted
#endif
#ifdef ControlMask
#undef ControlMask
#endif
#ifdef ShiftMask
#undef ShiftMask
#endif
#ifdef LockMask
#undef LockMask
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef Font
#undef Font
#endif
#ifdef Window
#undef Window
#endif
#ifdef Display
#undef Display
#endif
#ifdef Cursor
#undef Cursor
#endif
#ifdef Time
#undef Time
#endif
#ifdef Region
#undef Region
#endif
#ifdef Pixmap
#undef Pixmap
#endif
#ifdef Atom
#undef Atom
#endif
#ifdef Colormap
#undef Colormap
#endif
// <windows.h> (Windows): names Gridline uses for its own things.
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef DrawText
#undef DrawText
#endif
#ifdef GetObject
#undef GetObject
#endif
#ifdef LoadImage
#undef LoadImage
#endif
#ifdef SendMessage
#undef SendMessage
#endif
#ifdef PostMessage
#undef PostMessage
#endif
#ifdef GetMessage
#undef GetMessage
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef CopyFile
#undef CopyFile
#endif
#ifdef MoveFile
#undef MoveFile
#endif
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef TRANSPARENT
#undef TRANSPARENT
#endif
#ifdef OPAQUE
#undef OPAQUE
#endif
#ifdef RGB
#undef RGB
#endif
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef interface
#undef interface
#endif
