/* Gridline's Lua build settings (included by lua.h through LUA_USER_H; not
   part of the Lua distribution). A fixed string-hash seed instead of
   time + address: table iteration order is the same on every run, so
   scripts seeded with the same number give the same result. */
#ifndef GRIDLINE_LUA_USER_H
#define GRIDLINE_LUA_USER_H
#define luai_makeseed(L) (0x4c756121u)
#endif
