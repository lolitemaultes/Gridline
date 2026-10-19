# Lua 5.4.7 (vendored)

Source: https://www.lua.org/ftp/lua-5.4.7.tar.gz
(SHA-256 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30).
Licence: MIT (see LICENSE, also at the end of src/lua.h).

The files in src/ are unmodified. Left out on purpose (Gridline's script
sandbox never offers them): lua.c and luac.c (stand-alone programs), linit.c,
liolib.c (io), loslib.c (os), loadlib.c (package / require) and ldblib.c (debug).

Gridline compiles the sources as C++ (CMake LANGUAGE CXX), so Lua errors unwind
with C++ exceptions instead of longjmp and the C++ glue's destructors run.
The sandbox itself lives in src/core/Script.cpp.
