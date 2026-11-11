cmake_minimum_required(VERSION 3.16)
project(gridline VERSION 0.7.4 LANGUAGES C CXX)
# Prerelease label: "beta.1" makes this Gridline 0.7.0-beta.1 (published as a GitHub pre-release);
# empty ("") for a release. The version is set only here (cmake/GridlineVersion.cmake, docs/releasing.md).
set(GRIDLINE_VERSION_PRE "")
include(cmake/GridlineVersion.cmake)
gridline_version_forms(${PROJECT_VERSION} "${GRIDLINE_VERSION_PRE}" GRIDLINE_VERSION)

# Gridline — clean-room DAW. Native Linux target (also builds on macOS for development).
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#   ctest --test-dir build
#   ./build/gridline-app
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- compilers and platforms ----
# Warning sets: GL_WARN (strict: -Wall -Wextra -Wpedantic), GL_WARN_LOOSE (no -Wpedantic; the
# interface code), GL_NOWARN (vendored code). GRIDLINE_WERROR=ON turns warnings into errors (CI).
option(GRIDLINE_WERROR "Treat compiler warnings as errors" OFF)
if(MSVC)
  # /W3 is MSVC's counterpart of -Wall -Wextra; the implicit narrowing conversions (C4244, C4267,
  # C4305) are what GCC/Clang leave to -Wconversion, which Gridline does not enable either.
  set(GL_WARN /W3 /wd4244 /wd4267 /wd4305 /wd4996 /wd4146 /wd4018 /wd4389)
  set(GL_WARN_LOOSE ${GL_WARN})
  set(GL_NOWARN /W0)
  if(GRIDLINE_WERROR)
    list(APPEND GL_WARN /WX)
    list(APPEND GL_WARN_LOOSE /WX)
  endif()
  # UTF-8 sources and narrow strings, standard conformance, large objects, <cmath> constants,
  # no min/max macros from <windows.h>.
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc)
  # <math.h> first everywhere: its M_PI (_USE_MATH_DEFINES) and SDL's fallback definition would
  # otherwise collide (C4005) depending on the include order.
  add_compile_options(/FImath.h)
  # Headers of vendored libraries (SYSTEM include folders) are not Gridline's warnings.
  add_compile_options(/external:W0)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                          _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # The main thread gets 8 MB of stack like on Linux and macOS (Windows default: 1 MB); some
  # tests and the offline renderer keep large buffers on the stack.
  string(APPEND CMAKE_EXE_LINKER_FLAGS " /STACK:8388608")
else()
  set(GL_WARN -Wall -Wextra -Wpedantic)
  # Identical audio on every CPU: GCC fuses a*b+c into one rounding (FMA) wherever the CPU has it
  # (aarch64, and x86 with -march), unlike Clang and MSVC; the reference captures expect
  # separate roundings.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-ffp-contract=off)
  endif()
  # musl gives the main thread of a program 8 MB but other threads 128 KB unless the program
  # asks for more; this sets the default (glibc ignores it and uses the stack limit).
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-z,stack-size=8388608")
  endif()
  set(GL_WARN_LOOSE -Wall -Wextra)
  set(GL_NOWARN -w)
  if(GRIDLINE_WERROR)
    list(APPEND GL_WARN -Werror)
    list(APPEND GL_WARN_LOOSE -Werror)
  endif()
  if(WIN32)  # MinGW / clang (GNU driver) on Windows
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  endif()
endif()
if(WIN32)
  # gridline-app has its own WinMain (src/app/main.cpp); Windows 10 APIs (GetDpiForWindow, ...).
  add_compile_definitions(SDL_MAIN_HANDLED _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
  # The BSDs use the Linux plugin conventions (VST3 bundle layout, X11 editors). The vendored
  # VST3 SDK headers (kept unmodified) only know Windows, Linux and macOS: let them take their
  # Linux branch. Nothing else in the tree or the system headers tests __gnu_linux__.
  add_compile_definitions(__gnu_linux__=1)
  if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    # The full NetBSD API with strict -std=c++17 (LLONG_MAX in <limits.h>, ...).
    add_compile_definitions(_NETBSD_SOURCE)
  endif()
  # Ports / pkgsrc / X11 install headers and libraries outside the compiler's default paths.
  foreach(_pfx /usr/local /usr/pkg /usr/X11R6 /usr/X11R7)
    if(EXISTS ${_pfx}/include)
      include_directories(SYSTEM ${_pfx}/include)
      link_directories(${_pfx}/lib)
      list(APPEND CMAKE_PREFIX_PATH ${_pfx})
    endif()
  endforeach()
endif()
# VST3 bundle architecture folder (Contents/<folder>/): <arch>-win on Windows, <arch>-linux on
# Linux and the BSDs (which use the Linux layout). Matches vst3::bundleArchFolder().
if(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(GRIDLINE_VST3_ARCH arm64-win)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(GRIDLINE_VST3_ARCH x86-win)
  else()
    set(GRIDLINE_VST3_ARCH x86_64-win)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(GRIDLINE_VST3_ARCH aarch64-linux)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64|AMD64|x86_64")
  set(GRIDLINE_VST3_ARCH x86_64-linux)
else()
  set(GRIDLINE_VST3_ARCH ${CMAKE_SYSTEM_PROCESSOR}-linux)
endif()
# Oldest macOS the bundles declare (Info.plist): the deployment target when one is set
# (universal builds: 11.0, the first with Apple Silicon), else 10.15.
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(GL_MACOS_MIN ${CMAKE_OSX_DEPLOYMENT_TARGET})
else()
  set(GL_MACOS_MIN 10.15)
endif()
# Builds run from the build tree find assets/ in the source tree (a compiled-in fallback path);
# release packages turn this off so no build machine path ends up in the programs.
option(GRIDLINE_DEV_ASSETS "Fall back to the source tree's assets/ folder (development builds)" ON)
# GUI tests (the off-screen app runs) need a display; headless CI runners turn them off.
option(GRIDLINE_GUI_TESTS "Register the tests that open (hidden) application windows" ON)

# ---- core + engine (no UI dependencies) ----
# Plugin packs (src/packs/<pack>/): *_info/*_dsp go into the core library, *_ui into the app.
# K-39: crash reports walk the frame chain (Linux and the BSDs drop it otherwise; macOS and Windows keep or
# do not need it). Release builds add symbol files with GRIDLINE_RELEASE_SYMBOLS: line tables only, the code
# is the same, and tools/crash_symbolize.py turns a crash code into functions and lines with them.
option(GRIDLINE_RELEASE_SYMBOLS "Debug line tables in a release build, for the per-release symbol files" OFF)
if(NOT MSVC AND UNIX AND NOT APPLE)
  add_compile_options(-fno-omit-frame-pointer)
endif()
if(GRIDLINE_RELEASE_SYMBOLS)
  if(MSVC)
    add_compile_options(/Z7)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-gline-tables-only)
  else()
    add_compile_options(-g1)
  endif()
endif()
file(GLOB GRIDLINE_CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp src/core/flp/*.cpp src/engine/*.cpp src/engine/pitch/*.cpp src/engine/stems/*.cpp src/packs/Registry.cpp
     src/packs/*/*_info.cpp src/packs/*/*_dsp.cpp src/host/*.cpp src/host/*/*.cpp)
# Plugin hosting on macOS: Audio Units, Cocoa plugin views, sandbox windows (Objective-C++).
if(APPLE)
  # OBJC too: with only OBJCXX enabled, CMake would build plain Objective-C files (.m, e.g. the
  # bundled SDL2's) as Objective-C++ with the C++ standard flag.
  enable_language(OBJC)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  # Sanitizer builds (-DCMAKE_CXX_FLAGS="-fsanitize=…"): instrument the
  # Objective-C++ sources too. Mixing instrumented and uninstrumented code
  # that share std::vector makes ASan report false container overflows.
  if(CMAKE_CXX_FLAGS MATCHES "-fsanitize" AND NOT CMAKE_OBJCXX_FLAGS MATCHES "-fsanitize")
    string(APPEND CMAKE_OBJCXX_FLAGS " ${CMAKE_CXX_FLAGS}")
  endif()
  file(GLOB GRIDLINE_CORE_MM CONFIGURE_DEPENDS src/host/*/*.mm)
  list(APPEND GRIDLINE_CORE_SRC ${GRIDLINE_CORE_MM})
endif()
# Steinberg VST 3 SDK interfaces (MIT, vendored unmodified): the two sources that
# define FUnknown helpers and the core interface ids. Built without our warnings.
add_library(vst3_pluginterfaces STATIC third_party/vst3/pluginterfaces/base/funknown.cpp
                                       third_party/vst3/pluginterfaces/base/coreiids.cpp)
target_include_directories(vst3_pluginterfaces SYSTEM PUBLIC third_party/vst3)
target_compile_options(vst3_pluginterfaces PRIVATE ${GL_NOWARN})
set_target_properties(vst3_pluginterfaces PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  target_link_libraries(vst3_pluginterfaces PUBLIC "-framework CoreFoundation")
endif()
# Lua 5.4 (MIT, vendored; one patch in lstrlib.c, see third_party/lua/README.gridline.md): the script
# engine (J-24). Compiled as C++ so script errors unwind with exceptions; no
# io / os / package / debug libraries (left out of the tree). A fixed hash seed
# keeps table iteration order the same on every run (deterministic scripts).
file(GLOB GRIDLINE_LUA_SRC CONFIGURE_DEPENDS third_party/lua/src/*.c)
set_source_files_properties(${GRIDLINE_LUA_SRC} PROPERTIES LANGUAGE CXX)
add_library(gridline_lua STATIC ${GRIDLINE_LUA_SRC})
target_include_directories(gridline_lua SYSTEM PUBLIC third_party)
target_compile_definitions(gridline_lua PRIVATE LUA_USER_H="gridline_lua_user.h")
target_include_directories(gridline_lua PRIVATE third_party/lua)
target_compile_options(gridline_lua PRIVATE ${GL_NOWARN})
set_target_properties(gridline_lua PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE CXX)
# SC-07: the MCP server serves docs/scripting.md as a resource; it is compiled
# in (regenerated when the document changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/scripting.md)
file(READ ${CMAKE_SOURCE_DIR}/docs/scripting.md GRIDLINE_SCRIPTING_DOC_RAW)
# Adjacent raw-string pieces of at most 8000 bytes: MSVC limits a single literal piece to 16 KB.
string(LENGTH "${GRIDLINE_SCRIPTING_DOC_RAW}" _docLen)
set(GRIDLINE_SCRIPTING_DOC "")
set(_docPos 0)
while(_docPos LESS _docLen)
  string(SUBSTRING "${GRIDLINE_SCRIPTING_DOC_RAW}" ${_docPos} 8000 _docPiece)
  string(APPEND GRIDLINE_SCRIPTING_DOC "R\"GLDOC(${_docPiece})GLDOC\"\n      ")
  math(EXPR _docPos "${_docPos} + 8000")
endwhile()
file(WRITE ${CMAKE_BINARY_DIR}/generated/McpDocs.cpp.in
     "// Generated from docs/scripting.md by CMakeLists.txt (SC-07). Do not edit.\nnamespace gl::mcp {\nconst char* scriptingReferenceText() {\n  return @GRIDLINE_SCRIPTING_DOC@;\n}\n}  // namespace gl::mcp\n")
configure_file(${CMAKE_BINARY_DIR}/generated/McpDocs.cpp.in ${CMAKE_BINARY_DIR}/generated/McpDocs.cpp @ONLY)
if(NOT MSVC)
  set_source_files_properties(${CMAKE_BINARY_DIR}/generated/McpDocs.cpp PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings")
endif()
list(APPEND GRIDLINE_CORE_SRC ${CMAKE_BINARY_DIR}/generated/McpDocs.cpp)
# J-135: the demo song "Gridline" (assets/demo/Gridline.gridline.json, made by tools/make_demo_song.py)
# is compiled in as bytes, so every program and package opens it without looking for files.
set(GRIDLINE_DEMO_JSON ${CMAKE_SOURCE_DIR}/assets/demo/Gridline.gridline.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GRIDLINE_DEMO_JSON} ${CMAKE_SOURCE_DIR}/cmake/EmbedDemo.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/EmbedDemo.cmake)
gridline_embed_demo(${GRIDLINE_DEMO_JSON} ${CMAKE_BINARY_DIR}/generated/DemoSongData.cpp)
list(APPEND GRIDLINE_CORE_SRC ${CMAKE_BINARY_DIR}/generated/DemoSongData.cpp)
# ---- J-111: collaborative projects (src/collab) ----
# The replicated document, op log, profiles and the protocol are plain C++ in the core library.
# Networking: libjuice 1.7.4 (MPL-2.0, ICE / STUN / TURN, vendored unmodified), libplum 0.6.0
# (MPL-2.0, router port mapping) and Monocypher 4.0.2
# (BSD-2-Clause / CC0, X25519, EdDSA, BLAKE2b, XChaCha20-Poly1305, vendored unmodified).
# GRIDLINE_COLLAB=OFF leaves libjuice out (collaboration then works only between peers in one
# process, i.e. the tests; the app reports that networking is not built in).
option(GRIDLINE_COLLAB "Collaborative projects over the network (libjuice)" ON)
file(GLOB GRIDLINE_COLLAB_SRC CONFIGURE_DEPENDS src/collab/*.cpp)
list(APPEND GRIDLINE_CORE_SRC ${GRIDLINE_COLLAB_SRC})
add_library(gridline_monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(gridline_monocypher SYSTEM PUBLIC third_party/monocypher)
target_compile_options(gridline_monocypher PRIVATE ${GL_NOWARN})
set_target_properties(gridline_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(gridline_stbimage STATIC third_party/stb/gridline_stb_image.c third_party/stb/gridline_stb_pictures.c)  # J-184: + Pixel Tone pictures
target_compile_options(gridline_stbimage PRIVATE ${GL_NOWARN})
set_target_properties(gridline_stbimage PROPERTIES POSITION_INDEPENDENT_CODE ON)
# J-157: Rubber Band Library 4.0.0 (GPL-2.0-or-later, vendored unmodified; see
# third_party/rubberband/README.gridline.md), the Rubber Band stretch choice. Upstream's
# single-file build: built-in FFT and resampler, no threads, no system libraries (vDSP from
# Accelerate on macOS).
add_library(gridline_rubberband STATIC third_party/rubberband/single/RubberBandSingle.cpp)
target_include_directories(gridline_rubberband SYSTEM PUBLIC third_party/rubberband)
target_compile_options(gridline_rubberband PRIVATE ${GL_NOWARN})
set_target_properties(gridline_rubberband PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  target_link_libraries(gridline_rubberband PUBLIC "-framework Accelerate")
endif()
find_package(Threads REQUIRED)
if(GRIDLINE_COLLAB)
  file(GLOB GRIDLINE_JUICE_SRC CONFIGURE_DEPENDS third_party/libjuice/src/*.c)
  add_library(gridline_juice STATIC ${GRIDLINE_JUICE_SRC})
  target_include_directories(gridline_juice SYSTEM PUBLIC third_party/libjuice/include)
  target_include_directories(gridline_juice PRIVATE third_party/libjuice/include/juice third_party/libjuice/src)
  target_compile_definitions(gridline_juice PUBLIC JUICE_STATIC PRIVATE JUICE_EXPORTS USE_NETTLE=0 JUICE_ENABLE_LOCALHOST_ADDRESS=1 $<$<CONFIG:Release>:RELEASE=1>)
  target_compile_options(gridline_juice PRIVATE ${GL_NOWARN})
  set_target_properties(gridline_juice PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
  target_link_libraries(gridline_juice PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(gridline_juice PUBLIC ws2_32 bcrypt)
  endif()
  # libplum 0.6.0 (MPL-2.0, vendored unmodified): router port mapping (PCP / NAT-PMP / UPnP-IGD).
  file(GLOB GRIDLINE_PLUM_SRC CONFIGURE_DEPENDS third_party/libplum/src/*.c)
  add_library(gridline_plum STATIC ${GRIDLINE_PLUM_SRC})
  target_include_directories(gridline_plum SYSTEM PUBLIC third_party/libplum/include)
  target_include_directories(gridline_plum PRIVATE third_party/libplum/include/plum third_party/libplum/src)
  target_compile_definitions(gridline_plum PUBLIC PLUM_STATIC PRIVATE PLUM_EXPORTS $<$<CONFIG:Release>:RELEASE=1>)
  # libjuice and libplum share internal helper names (addr.c, udp.c, timestamp.c): the plum copies
  # are renamed at compile time so both link into one program (sources unmodified).
  target_compile_definitions(gridline_plum PRIVATE
    addr_get_len=gl_plum_addr_get_len
    addr_get_port=gl_plum_addr_get_port
    addr_is_any=gl_plum_addr_is_any
    addr_is_equal=gl_plum_addr_is_equal
    addr_is_local=gl_plum_addr_is_local
    addr_map_inet6_v4mapped=gl_plum_addr_map_inet6_v4mapped
    addr_record_is_equal=gl_plum_addr_record_is_equal
    addr_record_to_string=gl_plum_addr_record_to_string
    addr_resolve=gl_plum_addr_resolve
    addr_set_port=gl_plum_addr_set_port
    addr_to_string=gl_plum_addr_to_string
    addr_unmap_inet6_v4mapped=gl_plum_addr_unmap_inet6_v4mapped
    current_timestamp=gl_plum_current_timestamp
    udp_create_socket=gl_plum_udp_create_socket
    udp_get_bound_addr=gl_plum_udp_get_bound_addr
    udp_get_local_addr=gl_plum_udp_get_local_addr
    udp_get_port=gl_plum_udp_get_port
    udp_recvfrom=gl_plum_udp_recvfrom
    udp_sendto=gl_plum_udp_sendto
    udp_sendto_self=gl_plum_udp_sendto_self
    udp_set_diffserv=gl_plum_udp_set_diffserv)
  target_compile_options(gridline_plum PRIVATE ${GL_NOWARN})
  set_target_properties(gridline_plum PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
  target_link_libraries(gridline_plum PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(gridline_plum PUBLIC ws2_32 iphlpapi bcrypt)
  endif()
endif()
add_library(gridline_core STATIC ${GRIDLINE_CORE_SRC})
set_target_properties(gridline_core PROPERTIES POSITION_INDEPENDENT_CODE ON)  # also linked into the plugin bundles (PH-09)
target_include_directories(gridline_core PUBLIC src)
# J-111: build id = the commit (collaborators must run the same build); GRIDLINE_BUILD_ID overrides it.
if(NOT GRIDLINE_BUILD_ID)
  execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE GRIDLINE_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _glGitRes)
  if(NOT _glGitRes EQUAL 0 OR NOT GRIDLINE_BUILD_ID)
    # K-1: a source archive (no .git) has the commit in cmake/build-id.txt (export-subst).
    file(STRINGS cmake/build-id.txt _glArchiveId LIMIT_COUNT 1)
    if(_glArchiveId MATCHES "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]")
      string(SUBSTRING "${_glArchiveId}" 0 12 GRIDLINE_BUILD_ID)
    else()
      set(GRIDLINE_BUILD_ID "unknown")
    endif()
  endif()
endif()
configure_file(cmake/Version.h.in ${CMAKE_BINARY_DIR}/generated/GridlineVersion.h @ONLY)
target_include_directories(gridline_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_include_directories(gridline_core SYSTEM PUBLIC third_party)
# CLAP interface headers (MIT, vendored unmodified; see third_party/clap/LICENSE).
target_include_directories(gridline_core SYSTEM PUBLIC third_party/clap/include)
# LV2 specification headers (ISC, vendored unmodified; see third_party/lv2/COPYING).
target_include_directories(gridline_core SYSTEM PUBLIC third_party/lv2/include)
if(APPLE)
  target_link_libraries(gridline_core PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreAudioKit"
                        "-framework AppKit" "-framework Foundation" "-framework Security"
                        "-framework Accelerate")  # J-191: stem splitter matrix products
elseif(WIN32)
  # Winsock (MCP server), Credential Manager (advapi32), process / memory statistics, windows,
  # WinHTTP (J-89 update check), Media Foundation (K-51: M4A / AAC samples).
  target_link_libraries(gridline_core PUBLIC ws2_32 bcrypt advapi32 psapi user32 gdi32 shell32 ole32 uuid urlmon comctl32 winhttp
                        mfplat mfreadwrite mfuuid)
elseif(UNIX)
  find_library(GRIDLINE_RT_LIB rt)  # shm_open lives in librt on Linux / NetBSD; libc elsewhere
  if(GRIDLINE_RT_LIB)
    target_link_libraries(gridline_core PUBLIC ${GRIDLINE_RT_LIB})
  endif()
endif()
target_compile_options(gridline_core PRIVATE ${GL_WARN})
find_package(Threads REQUIRED)
target_link_libraries(gridline_core PUBLIC Threads::Threads vst3_pluginterfaces gridline_lua gridline_monocypher gridline_stbimage gridline_rubberband ${CMAKE_DL_LIBS})
if(GRIDLINE_COLLAB)
  target_link_libraries(gridline_core PUBLIC gridline_juice gridline_plum)
  target_compile_definitions(gridline_core PUBLIC GRIDLINE_COLLAB_NET=1)
endif()

add_executable(gridline src/cli/main.cpp src/cli/Bench.cpp src/cli/FlpTools.cpp src/cli/McpCli.cpp src/cli/CollabCli.cpp src/cli/SuiteCli.cpp
               src/cli/StemsCli.cpp)
target_link_libraries(gridline PRIVATE gridline_core)

add_executable(gridline_tests tests/test_engine.cpp)
target_link_libraries(gridline_tests PRIVATE gridline_core)
add_executable(gridline_tests_m1 tests/test_m1.cpp)
target_link_libraries(gridline_tests_m1 PRIVATE gridline_core)
add_executable(gridline_tests_parity tests/test_parity.cpp)
target_link_libraries(gridline_tests_parity PRIVATE gridline_core)
target_compile_options(gridline_tests_parity PRIVATE ${GL_WARN})
add_executable(gridline_tests_menus tests/test_menus.cpp)
target_link_libraries(gridline_tests_menus PRIVATE gridline_core)
target_compile_options(gridline_tests_menus PRIVATE ${GL_WARN})

enable_testing()
add_test(NAME engine COMMAND gridline_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME m1_foundations COMMAND gridline_tests_m1 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME plugin_parity COMMAND gridline_tests_parity WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME menus COMMAND gridline_tests_menus WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_command_palette tests/test_command_palette.cpp)  # K-93
target_link_libraries(gridline_tests_command_palette PRIVATE gridline_core)
target_include_directories(gridline_tests_command_palette PRIVATE src tests)
target_compile_options(gridline_tests_command_palette PRIVATE ${GL_WARN})
add_executable(gridline_tests_layout_presets tests/test_layout_presets.cpp)  # K-79
target_link_libraries(gridline_tests_layout_presets PRIVATE gridline_core)
target_include_directories(gridline_tests_layout_presets PRIVATE src tests)
target_compile_options(gridline_tests_layout_presets PRIVATE ${GL_WARN})
add_test(NAME k79_layout_presets COMMAND gridline_tests_layout_presets WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME k93_command_palette COMMAND gridline_tests_command_palette WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-1: the distribution channel files (packaging/: Flatpak, AUR, Homebrew, Windows package
# managers, FreeBSD port, COPR, Snap) render for this version and agree with the tree.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME packaging_manifests COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/packaging/check.py
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
# J-141: welcome launcher logic (start-up plan, state machine, recents, sketches + cache, layout audit).
add_executable(gridline_tests_launcher tests/test_launcher.cpp)
target_link_libraries(gridline_tests_launcher PRIVATE gridline_core)
target_include_directories(gridline_tests_launcher PRIVATE src tests)
target_compile_options(gridline_tests_launcher PRIVATE ${GL_WARN})
add_test(NAME j141_launcher COMMAND gridline_tests_launcher WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-147: plugin manager logic (one row per plugin across formats, categories, preferred format).
add_executable(gridline_tests_plugin_groups tests/test_plugin_groups.cpp)
target_link_libraries(gridline_tests_plugin_groups PRIVATE gridline_core)
target_include_directories(gridline_tests_plugin_groups PRIVATE src tests)
target_compile_options(gridline_tests_plugin_groups PRIVATE ${GL_WARN})
add_test(NAME j147_plugin_groups COMMAND gridline_tests_plugin_groups WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-78: a project's third-party plugin opens in another installed format (matcher, state wrappings, projects).
add_executable(gridline_tests_cross_format tests/test_cross_format.cpp)
target_link_libraries(gridline_tests_cross_format PRIVATE gridline_core)
target_include_directories(gridline_tests_cross_format PRIVATE src tests)
target_compile_options(gridline_tests_cross_format PRIVATE ${GL_WARN})
add_test(NAME k78_cross_format COMMAND gridline_tests_cross_format WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_editing_tools tests/test_editing_tools.cpp)
target_link_libraries(gridline_tests_editing_tools PRIVATE gridline_core)
target_compile_options(gridline_tests_editing_tools PRIVATE ${GL_WARN})
add_test(NAME editing_tools COMMAND gridline_tests_editing_tools WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_timeline tests/test_timeline.cpp)
target_link_libraries(gridline_tests_timeline PRIVATE gridline_core)
add_test(NAME timeline COMMAND gridline_tests_timeline WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_playlist_view tests/test_playlist_view.cpp)
target_link_libraries(gridline_tests_playlist_view PRIVATE gridline_core)
target_compile_options(gridline_tests_playlist_view PRIVATE ${GL_WARN})
add_test(NAME playlist_view COMMAND gridline_tests_playlist_view WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_mixer_parity tests/test_mixer_parity.cpp)
target_link_libraries(gridline_tests_mixer_parity PRIVATE gridline_core)
target_compile_options(gridline_tests_mixer_parity PRIVATE ${GL_WARN})
add_test(NAME mixer_parity COMMAND gridline_tests_mixer_parity WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_edison tests/test_edison.cpp)
target_link_libraries(gridline_tests_edison PRIVATE gridline_core)
target_compile_options(gridline_tests_edison PRIVATE ${GL_WARN})
add_test(NAME edison COMMAND gridline_tests_edison WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-191: stem splitter (tiny deterministic model, no real weights)
add_executable(gridline_tests_stems tests/test_stems.cpp)
target_link_libraries(gridline_tests_stems PRIVATE gridline_core)
target_compile_options(gridline_tests_stems PRIVATE ${GL_WARN})
add_test(NAME stems COMMAND gridline_tests_stems WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME j191_stems_cli
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:gridline> -DTESTBIN=$<TARGET_FILE:gridline_tests_stems>
                 -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/j191_stems_cli -P ${CMAKE_SOURCE_DIR}/tests/j191_stems_cli.cmake
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(j191_stems_cli PROPERTIES TIMEOUT 300)
add_executable(gridline_tests_midi_workflow tests/test_midi_workflow.cpp)
target_link_libraries(gridline_tests_midi_workflow PRIVATE gridline_core)
target_compile_options(gridline_tests_midi_workflow PRIVATE ${GL_WARN})
add_test(NAME midi_workflow COMMAND gridline_tests_midi_workflow WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_pdc tests/test_pdc.cpp)
target_link_libraries(gridline_tests_pdc PRIVATE gridline_core)
target_compile_options(gridline_tests_pdc PRIVATE ${GL_WARN})
add_test(NAME pdc COMMAND gridline_tests_pdc WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_video_driver tests/test_video_driver.cpp)
target_compile_options(gridline_tests_video_driver PRIVATE ${GL_WARN})
add_test(NAME video_driver COMMAND gridline_tests_video_driver)
add_executable(gridline_tests_program_retire tests/test_program_retire.cpp)
target_link_libraries(gridline_tests_program_retire PRIVATE gridline_core)
target_compile_options(gridline_tests_program_retire PRIVATE ${GL_WARN})
add_test(NAME program_retire COMMAND gridline_tests_program_retire WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(program_retire PROPERTIES TIMEOUT 300)
add_executable(gridline_tests_automation_edit tests/test_automation_edit.cpp)
target_link_libraries(gridline_tests_automation_edit PRIVATE gridline_core)
target_compile_options(gridline_tests_automation_edit PRIVATE ${GL_WARN})
add_test(NAME automation_edit COMMAND gridline_tests_automation_edit WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_j2 tests/test_j2.cpp)
target_link_libraries(gridline_tests_j2 PRIVATE gridline_core)
target_include_directories(gridline_tests_j2 PRIVATE tests)
target_compile_options(gridline_tests_j2 PRIVATE ${GL_WARN})
add_test(NAME j2_instruments COMMAND gridline_tests_j2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-32: level laws (Matrix FM pluck / waveshaper), Maximus decode, live-mode offline render.
add_executable(gridline_tests_j32 tests/test_j32.cpp)
target_link_libraries(gridline_tests_j32 PRIVATE gridline_core)
target_include_directories(gridline_tests_j32 PRIVATE tests)
target_compile_options(gridline_tests_j32 PRIVATE ${GL_WARN})
add_test(NAME j32_levels COMMAND gridline_tests_j32 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-48: interface sounds (loader, real-time mix without allocation, muting rules, settings).
add_executable(gridline_tests_ui_sounds tests/test_ui_sounds.cpp)
target_link_libraries(gridline_tests_ui_sounds PRIVATE gridline_core)
target_include_directories(gridline_tests_ui_sounds PRIVATE tests)
target_compile_options(gridline_tests_ui_sounds PRIVATE ${GL_WARN})
add_test(NAME ui_sounds COMMAND gridline_tests_ui_sounds WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-67: smooth scrolling and elastic edges (scroll model).
add_executable(gridline_tests_smooth_scroll tests/test_smooth_scroll.cpp)
target_link_libraries(gridline_tests_smooth_scroll PRIVATE gridline_core)
target_include_directories(gridline_tests_smooth_scroll PRIVATE tests)
target_compile_options(gridline_tests_smooth_scroll PRIVATE ${GL_WARN})
add_test(NAME smooth_scroll COMMAND gridline_tests_smooth_scroll WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-188: smooth zooming (zoom glide model, motion switch).
add_executable(gridline_tests_smooth_zoom tests/test_smooth_zoom.cpp)
target_link_libraries(gridline_tests_smooth_zoom PRIVATE gridline_core)
target_include_directories(gridline_tests_smooth_zoom PRIVATE tests)
target_compile_options(gridline_tests_smooth_zoom PRIVATE ${GL_WARN})
add_test(NAME smooth_zoom COMMAND gridline_tests_smooth_zoom WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-99: smooth freehand drawing (stroke fill, one-euro filter, edge-preserving pass, undo grouping).
add_executable(gridline_tests_draw_smooth tests/test_draw_smooth.cpp)
target_link_libraries(gridline_tests_draw_smooth PRIVATE gridline_core)
target_include_directories(gridline_tests_draw_smooth PRIVATE tests)
target_compile_options(gridline_tests_draw_smooth PRIVATE ${GL_WARN})
add_test(NAME draw_smooth COMMAND gridline_tests_draw_smooth WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-83: wheel routing over mixer controls (thin mixer scrolls, wide adjusts, momentum, setting).
add_executable(gridline_tests_wheel_route tests/test_wheel_route.cpp)
target_link_libraries(gridline_tests_wheel_route PRIVATE gridline_core)
target_include_directories(gridline_tests_wheel_route PRIVATE tests)
target_compile_options(gridline_tests_wheel_route PRIVATE ${GL_WARN})
add_test(NAME wheel_route COMMAND gridline_tests_wheel_route WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-84: channel rack layout (knobs at every width / length, name truncation, step scroll) and rack wheel routing.
add_executable(gridline_tests_rack_layout tests/test_rack_layout.cpp)
target_link_libraries(gridline_tests_rack_layout PRIVATE gridline_core)
target_include_directories(gridline_tests_rack_layout PRIVATE tests)
target_compile_options(gridline_tests_rack_layout PRIVATE ${GL_WARN})
add_test(NAME rack_layout COMMAND gridline_tests_rack_layout WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j1 tests/test_j1.cpp)
target_link_libraries(gridline_tests_j1 PRIVATE gridline_core)
target_compile_options(gridline_tests_j1 PRIVATE ${GL_WARN})
add_test(NAME j1_effects COMMAND gridline_tests_j1 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-92: every built-in plugin parameter recorded as working still changes the output
# (fast check against tests/param_audit_baseline.tsv; full sweep: gridline param-audit).
add_executable(gridline_tests_plugin_param_audit tests/test_plugin_param_audit.cpp)
target_link_libraries(gridline_tests_plugin_param_audit PRIVATE gridline_core)
target_compile_options(gridline_tests_plugin_param_audit PRIVATE ${GL_WARN})
add_test(NAME plugin_param_audit COMMAND gridline_tests_plugin_param_audit WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(plugin_param_audit PROPERTIES TIMEOUT 900)
add_executable(gridline_tests_bitcrush_range tests/test_bitcrush_range.cpp)
target_link_libraries(gridline_tests_bitcrush_range PRIVATE gridline_core)
target_compile_options(gridline_tests_bitcrush_range PRIVATE ${GL_WARN})
add_test(NAME bitcrush_range COMMAND gridline_tests_bitcrush_range WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-136: one reset-or-type rule for the double-click of every parameter control.
add_executable(gridline_tests_param_reset tests/test_param_reset.cpp)
target_link_libraries(gridline_tests_param_reset PRIVATE gridline_core)
target_include_directories(gridline_tests_param_reset PRIVATE tests)
target_compile_options(gridline_tests_param_reset PRIVATE ${GL_WARN})
add_test(NAME j136_param_reset COMMAND gridline_tests_param_reset WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j33 tests/test_j33.cpp)
target_link_libraries(gridline_tests_j33 PRIVATE gridline_core)
target_include_directories(gridline_tests_j33 PRIVATE tests)
target_compile_options(gridline_tests_j33 PRIVATE ${GL_WARN})
add_test(NAME j33_instruments COMMAND gridline_tests_j33 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j41 tests/test_j41.cpp)
target_link_libraries(gridline_tests_j41 PRIVATE gridline_core)
target_include_directories(gridline_tests_j41 PRIVATE tests)
target_compile_options(gridline_tests_j41 PRIVATE ${GL_WARN})
add_test(NAME j41_articulators COMMAND gridline_tests_j41 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j50 tests/test_j50.cpp)
target_link_libraries(gridline_tests_j50 PRIVATE gridline_core)
target_include_directories(gridline_tests_j50 PRIVATE tests)
target_compile_options(gridline_tests_j50 PRIVATE ${GL_WARN})
add_test(NAME j50_matrixfm_reference COMMAND gridline_tests_j50 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_kappa1 tests/test_kappa1.cpp)
target_link_libraries(gridline_tests_kappa1 PRIVATE gridline_core)
target_compile_options(gridline_tests_kappa1 PRIVATE ${GL_WARN})
add_test(NAME kappa1 COMMAND gridline_tests_kappa1 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j26 tests/test_j26.cpp)
target_link_libraries(gridline_tests_j26 PRIVATE gridline_core)
target_compile_options(gridline_tests_j26 PRIVATE ${GL_WARN})
add_test(NAME j26_effects COMMAND gridline_tests_j26 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j35 tests/test_j35.cpp)
target_link_libraries(gridline_tests_j35 PRIVATE gridline_core)
target_compile_options(gridline_tests_j35 PRIVATE ${GL_WARN})
add_test(NAME j35_effects COMMAND gridline_tests_j35 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_j42 tests/test_j42.cpp)
target_link_libraries(gridline_tests_j42 PRIVATE gridline_core)
target_compile_options(gridline_tests_j42 PRIVATE ${GL_WARN})
add_test(NAME j42_effects COMMAND gridline_tests_j42 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_kappa2 tests/test_kappa2.cpp)
target_link_libraries(gridline_tests_kappa2 PRIVATE gridline_core)
target_compile_options(gridline_tests_kappa2 PRIVATE ${GL_WARN})
add_test(NAME kappa2 COMMAND gridline_tests_kappa2 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_kappa3 tests/test_kappa3.cpp)
target_link_libraries(gridline_tests_kappa3 PRIVATE gridline_core)
target_compile_options(gridline_tests_kappa3 PRIVATE ${GL_WARN})
add_test(NAME kappa3 COMMAND gridline_tests_kappa3 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_wavpack tests/test_wavpack.cpp)
target_link_libraries(gridline_tests_wavpack PRIVATE gridline_core)
target_compile_options(gridline_tests_wavpack PRIVATE ${GL_WARN})
add_test(NAME wavpack COMMAND gridline_tests_wavpack WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_rawnorm tests/test_rawnorm.cpp)
target_link_libraries(gridline_tests_rawnorm PRIVATE gridline_core)
target_compile_options(gridline_tests_rawnorm PRIVATE ${GL_WARN})
add_test(NAME rawnorm COMMAND gridline_tests_rawnorm WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp tests/test_flp.cpp)
target_link_libraries(gridline_tests_flp PRIVATE gridline_core)
add_test(NAME flp_import COMMAND gridline_tests_flp WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_gen2 tests/test_flp_gen2.cpp)
target_link_libraries(gridline_tests_flp_gen2 PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_gen2 PRIVATE ${GL_WARN})
add_test(NAME flp_gen2 COMMAND gridline_tests_flp_gen2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_export tests/test_flp_export.cpp)
target_link_libraries(gridline_tests_flp_export PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_export PRIVATE ${GL_WARN})
add_test(NAME flp_export COMMAND gridline_tests_flp_export WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_export_perf tests/test_flp_export_perf.cpp)
target_link_libraries(gridline_tests_flp_export_perf PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_export_perf PRIVATE ${GL_WARN})
add_test(NAME flp_export_perf COMMAND gridline_tests_flp_export_perf WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_standin tests/test_flp_standin.cpp)
target_link_libraries(gridline_tests_flp_standin PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_standin PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_flp_standin PRIVATE GRIDLINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME flp_standin COMMAND gridline_tests_flp_standin WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_suite tests/test_flp_suite.cpp)  # K-110
target_link_libraries(gridline_tests_flp_suite PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_suite PRIVATE ${GL_WARN})
add_test(NAME flp_suite COMMAND gridline_tests_flp_suite WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_dec3 tests/test_flp_dec3.cpp)
target_link_libraries(gridline_tests_flp_dec3 PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_dec3 PRIVATE ${GL_WARN})
add_test(NAME flp_dec3 COMMAND gridline_tests_flp_dec3 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_mixer tests/test_flp_mixer.cpp)
target_link_libraries(gridline_tests_flp_mixer PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_mixer PRIVATE ${GL_WARN})
add_test(NAME flp_mixer COMMAND gridline_tests_flp_mixer WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_fx2 tests/test_flp_fx2.cpp)
target_link_libraries(gridline_tests_flp_fx2 PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_fx2 PRIVATE ${GL_WARN})
add_test(NAME flp_fx2 COMMAND gridline_tests_flp_fx2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_flp_generators tests/test_flp_generators.cpp)
target_link_libraries(gridline_tests_flp_generators PRIVATE gridline_core)
target_compile_options(gridline_tests_flp_generators PRIVATE ${GL_WARN})
add_test(NAME flp_generators COMMAND gridline_tests_flp_generators WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_arrangement tests/test_arrangement.cpp)
target_link_libraries(gridline_tests_arrangement PRIVATE gridline_core)
target_compile_options(gridline_tests_arrangement PRIVATE ${GL_WARN})
add_test(NAME arrangement COMMAND gridline_tests_arrangement WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_corpus tests/test_corpus.cpp)
target_link_libraries(gridline_tests_corpus PRIVATE gridline_core)
target_compile_options(gridline_tests_corpus PRIVATE ${GL_WARN})
add_test(NAME corpus COMMAND gridline_tests_corpus WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_browser tests/test_browser.cpp)
target_link_libraries(gridline_tests_browser PRIVATE gridline_core)
target_include_directories(gridline_tests_browser PRIVATE tests)
target_compile_options(gridline_tests_browser PRIVATE ${GL_WARN})
add_test(NAME browser COMMAND gridline_tests_browser WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-161: sliced loops (.rx2), tagged loops (.caf), AIFF / WAV loop chunks, loop files into a project.
add_executable(gridline_tests_loop_files tests/test_loop_files.cpp)
target_link_libraries(gridline_tests_loop_files PRIVATE gridline_core)
target_include_directories(gridline_tests_loop_files PRIVATE tests)
target_compile_options(gridline_tests_loop_files PRIVATE ${GL_WARN})
add_test(NAME loop_files COMMAND gridline_tests_loop_files WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-75: drags from other applications (planning, formats, headers; macOS: pasteboards + the window's drag destination).
add_executable(gridline_tests_external_drop tests/test_external_drop.cpp)
target_link_libraries(gridline_tests_external_drop PRIVATE gridline_core)
target_include_directories(gridline_tests_external_drop PRIVATE tests)
target_compile_options(gridline_tests_external_drop PRIVATE ${GL_WARN})
add_test(NAME external_drop COMMAND gridline_tests_external_drop WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-97: drop-to-replace on the channel rack (row zones, sample slots, name policy).
add_executable(gridline_tests_j97 tests/test_j97.cpp)
target_link_libraries(gridline_tests_j97 PRIVATE gridline_core)
target_include_directories(gridline_tests_j97 PRIVATE tests)
target_compile_options(gridline_tests_j97 PRIVATE ${GL_WARN})
add_test(NAME j97 COMMAND gridline_tests_j97 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-16 / K-18: browser file operations (rename, folders, moves, the trash, project references
# and undo) and favourite plugins (settings round trip, picker order).
add_executable(gridline_tests_k18 tests/test_k18.cpp)
target_link_libraries(gridline_tests_k18 PRIVATE gridline_core)
target_include_directories(gridline_tests_k18 PRIVATE tests)
target_compile_options(gridline_tests_k18 PRIVATE ${GL_WARN})
add_test(NAME k18_file_ops COMMAND gridline_tests_k18 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-6: ASIO backend: sample formats, buffer sizes, channel plan and the host on the simulated
# driver (every platform); the registry driver list (Windows only, passes with none installed).
add_executable(gridline_tests_asio tests/test_asio.cpp)
target_link_libraries(gridline_tests_asio PRIVATE gridline_core)
target_include_directories(gridline_tests_asio PRIVATE tests)
target_compile_options(gridline_tests_asio PRIVATE ${GL_WARN})
add_test(NAME k6_asio COMMAND gridline_tests_asio WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
if(WIN32)
  add_executable(gridline_tests_asio_win tests/test_asio_win.cpp)
  target_link_libraries(gridline_tests_asio_win PRIVATE gridline_core)
  target_include_directories(gridline_tests_asio_win PRIVATE tests)
  target_compile_options(gridline_tests_asio_win PRIVATE ${GL_WARN})
  add_test(NAME k6_asio_drivers COMMAND gridline_tests_asio_win WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
# J-143: the OUT meter (level meter ballistics, master bus peak) and the shipped templates.
add_executable(gridline_tests_j143 tests/test_j143.cpp)
target_link_libraries(gridline_tests_j143 PRIVATE gridline_core)
target_include_directories(gridline_tests_j143 PRIVATE tests)
target_compile_options(gridline_tests_j143 PRIVATE ${GL_WARN})
add_test(NAME j143_out_meter COMMAND gridline_tests_j143 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# J-89: update check (versions, release JSON, downloads per platform, SHA256SUMS, schedule; no network).
add_executable(gridline_tests_update tests/test_update.cpp)
target_link_libraries(gridline_tests_update PRIVATE gridline_core)
target_include_directories(gridline_tests_update PRIVATE tests)
target_compile_options(gridline_tests_update PRIVATE ${GL_WARN})
add_test(NAME update_check COMMAND gridline_tests_update WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-125: the forms of the version (0.7.0-beta.1, 0.7.0, 0.7.0~beta.1, rpm Release) agree in CMake,
# tools/release/version.sh, GridlineVersion.h, `gridline --version`, the Info.plists and .rc files.
add_test(NAME version_forms COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DBIN=${CMAKE_BINARY_DIR} -DCLI=$<TARGET_FILE:gridline>
                                    -P ${CMAKE_SOURCE_DIR}/tests/version_forms.cmake)
# J-111: collaboration core (replicated document, sessions, undo locality, op log, files, policy,
# relay, conflicts and admin review, signatures, profiles), all in memory with fixed seeds.
add_executable(gridline_tests_collab tests/test_collab.cpp)
target_link_libraries(gridline_tests_collab PRIVATE gridline_core)
target_include_directories(gridline_tests_collab PRIVATE tests)
target_compile_options(gridline_tests_collab PRIVATE ${GL_WARN})
add_test(NAME collab COMMAND gridline_tests_collab WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(collab PROPERTIES TIMEOUT 600)
# J-111: encrypted reliable link (loss, reordering, tampering), invites, reply / expiry / reuse, the
# version gate between peers, relaying through a collaborator, introductions, reconnects (in memory).
add_executable(gridline_tests_collab_net tests/test_collab_net.cpp)
target_link_libraries(gridline_tests_collab_net PRIVATE gridline_core)
target_include_directories(gridline_tests_collab_net PRIVATE tests)
target_compile_options(gridline_tests_collab_net PRIVATE ${GL_WARN})
add_test(NAME collab_net COMMAND gridline_tests_collab_net WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(collab_net PROPERTIES TIMEOUT 600)
# J-111: sound and playback stay on each computer (two collaborators with real engines, offline).
add_executable(gridline_tests_collab_local tests/test_collab_local.cpp)
target_link_libraries(gridline_tests_collab_local PRIVATE gridline_core)
target_include_directories(gridline_tests_collab_local PRIVATE tests)
target_compile_options(gridline_tests_collab_local PRIVATE ${GL_WARN})
add_test(NAME collab_local COMMAND gridline_tests_collab_local WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(collab_local PROPERTIES TIMEOUT 600)
# J-146: presence on the welcome screen (authenticated probes both ways, strangers, removed,
# hidden, other versions, no answer, transitive), project cards and the list model (in memory).
# K-141: undo and redo in a collaborative project: your own history, one of your steps per
# press, collaborators' later edits stay (with a message), redo, local settings, limits, reopening.
add_executable(gridline_tests_collab_undo tests/test_collab_undo.cpp)
target_link_libraries(gridline_tests_collab_undo PRIVATE gridline_core)
target_include_directories(gridline_tests_collab_undo PRIVATE tests)
target_compile_options(gridline_tests_collab_undo PRIVATE ${GL_WARN})
add_test(NAME k141_collab_undo COMMAND gridline_tests_collab_undo WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k141_collab_undo PROPERTIES TIMEOUT 900)
# K-141: the history: notes with each step, levels and memory budget, clearing / rebuilding it,
# stepping on a scratch song, the move callback, rollbacks that never take an unrelated step.
add_executable(gridline_tests_undo_history tests/test_undo_history.cpp)
target_link_libraries(gridline_tests_undo_history PRIVATE gridline_core)
target_include_directories(gridline_tests_undo_history PRIVATE tests)
target_compile_options(gridline_tests_undo_history PRIVATE ${GL_WARN})
add_test(NAME k141_undo_history COMMAND gridline_tests_undo_history WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_collab_presence tests/test_collab_presence.cpp)
target_link_libraries(gridline_tests_collab_presence PRIVATE gridline_core)
target_include_directories(gridline_tests_collab_presence PRIVATE tests)
target_compile_options(gridline_tests_collab_presence PRIVATE ${GL_WARN})
add_test(NAME j146_collab_presence COMMAND gridline_tests_collab_presence WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(j146_collab_presence PROPERTIES TIMEOUT 600)
# J-111: two gridline processes, separate configs, real ICE on this computer (no server).
if(GRIDLINE_COLLAB)
  add_test(NAME j111_collab_processes
           COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:gridline> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                   -DOUT=${CMAKE_BINARY_DIR}/j111_collab -P ${CMAKE_SOURCE_DIR}/tests/j111_collab.cmake
           WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(j111_collab_processes PROPERTIES TIMEOUT 400)
endif()
set_tests_properties(update_check PROPERTIES ENVIRONMENT "GRIDLINE_NO_UPDATE_CHECK=1")
if(APPLE)
  add_executable(gridline_tests_external_drop_mac tests/test_external_drop_mac.mm)
  target_link_libraries(gridline_tests_external_drop_mac PRIVATE gridline_core "-framework AppKit")
  target_include_directories(gridline_tests_external_drop_mac PRIVATE tests)
  target_compile_options(gridline_tests_external_drop_mac PRIVATE ${GL_WARN_LOOSE})
  add_test(NAME external_drop_mac COMMAND gridline_tests_external_drop_mac WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  # K-36: the Audio Unit listener and the backend's lifetime (Apple's AUDelay, create/listen/destroy stress).
  add_executable(gridline_tests_k36 tests/test_k36_au_listener.mm)
  target_link_libraries(gridline_tests_k36 PRIVATE gridline_core)
  target_include_directories(gridline_tests_k36 PRIVATE tests)
  target_compile_options(gridline_tests_k36 PRIVATE ${GL_WARN})
  add_test(NAME k36_au_listener COMMAND gridline_tests_k36 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(k36_au_listener PROPERTIES TIMEOUT 300)
endif()
add_executable(gridline_tests_channels tests/test_channels.cpp)
target_link_libraries(gridline_tests_channels PRIVATE gridline_core)
target_compile_options(gridline_tests_channels PRIVATE ${GL_WARN})
add_test(NAME channels COMMAND gridline_tests_channels WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-138: song swing (smooth knob, notes swing with steps, sample-accurate playback).
add_executable(gridline_tests_swing tests/test_swing.cpp)
target_link_libraries(gridline_tests_swing PRIVATE gridline_core)
target_compile_options(gridline_tests_swing PRIVATE ${GL_WARN})
add_test(NAME swing COMMAND gridline_tests_swing WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_presets tests/test_presets.cpp)
target_link_libraries(gridline_tests_presets PRIVATE gridline_core)
add_test(NAME presets COMMAND gridline_tests_presets WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_coverage tests/test_coverage.cpp)
target_link_libraries(gridline_tests_coverage PRIVATE gridline_core)
target_compile_options(gridline_tests_coverage PRIVATE ${GL_WARN})
add_test(NAME coverage COMMAND gridline_tests_coverage WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-27 menu parity sweep: the new menu operations (MenuParity, NoteFxBurn).
# J-55: keymap registry, translation layer and i18n coverage of the sources (runs in the source dir).
add_executable(gridline_tests_shortcuts_i18n tests/test_shortcuts_i18n.cpp)
target_link_libraries(gridline_tests_shortcuts_i18n PRIVATE gridline_core)
target_compile_options(gridline_tests_shortcuts_i18n PRIVATE ${GL_WARN})
add_test(NAME shortcuts_i18n COMMAND gridline_tests_shortcuts_i18n WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_menu_parity tests/test_menu_parity.cpp)
target_link_libraries(gridline_tests_menu_parity PRIVATE gridline_core)
target_compile_options(gridline_tests_menu_parity PRIVATE ${GL_WARN})
add_test(NAME menu_parity COMMAND gridline_tests_menu_parity WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# VST3 hosting: a tiny in-tree test plugin bundle (instrument + effect) and the host tests.
add_library(gridline_vst3_testplug MODULE tests/vst3/TestPlugin.cpp)
target_link_libraries(gridline_vst3_testplug PRIVATE vst3_pluginterfaces)
target_compile_options(gridline_vst3_testplug PRIVATE ${GL_WARN})
set(GRIDLINE_TESTPLUG_BUNDLE ${CMAKE_BINARY_DIR}/test-vst3/GridlineTest.vst3)
if(APPLE)
  set(GRIDLINE_TESTPLUG_DIR ${GRIDLINE_TESTPLUG_BUNDLE}/Contents/MacOS)
  set_target_properties(gridline_vst3_testplug PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME GridlineTest
                        LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_TESTPLUG_DIR})
  file(WRITE ${GRIDLINE_TESTPLUG_BUNDLE}/Contents/Info.plist
       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n<key>CFBundleExecutable</key><string>GridlineTest</string>\n<key>CFBundleIdentifier</key><string>org.gridline.vst3.test</string>\n<key>CFBundlePackageType</key><string>BNDL</string>\n</dict></plist>\n")
else()
  if(WIN32)
    set(_vst3suffix ".vst3")
  else()
    set(_vst3suffix ".so")
  endif()
  set_target_properties(gridline_vst3_testplug PROPERTIES PREFIX "" SUFFIX "${_vst3suffix}" OUTPUT_NAME GridlineTest
                        LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_TESTPLUG_BUNDLE}/Contents/${GRIDLINE_VST3_ARCH})
  # Multi-config generators (Visual Studio) add a per-configuration folder unless told otherwise.
  foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set_target_properties(gridline_vst3_testplug PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_CFG} ${GRIDLINE_TESTPLUG_BUNDLE}/Contents/${GRIDLINE_VST3_ARCH})
  endforeach()
endif()
add_executable(gridline_tests_vst3 tests/test_vst3.cpp)
target_link_libraries(gridline_tests_vst3 PRIVATE gridline_core)
target_compile_options(gridline_tests_vst3 PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_vst3 PRIVATE GRIDLINE_TESTPLUG_BUNDLE="${GRIDLINE_TESTPLUG_BUNDLE}"
                           GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_vst3 gridline_vst3_testplug gridline)
add_test(NAME vst3_host COMMAND gridline_tests_vst3 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# CLAP / VST2 / AU hosting, the plugin sandbox and FL "Fruity Wrapper" import:
# in-tree test plugins (never installed) and the host tests.
add_library(gridline_clap_testplug MODULE tests/clap/TestPlugin.cpp)
target_include_directories(gridline_clap_testplug SYSTEM PRIVATE third_party/clap/include)
target_compile_options(gridline_clap_testplug PRIVATE ${GL_WARN})
add_library(gridline_vst2_testplug MODULE tests/vst2/TestPlugin.cpp)
target_include_directories(gridline_vst2_testplug PRIVATE src)
target_compile_options(gridline_vst2_testplug PRIVATE ${GL_WARN})
# The VST3 and CLAP test synths have their own interface on macOS (a plain
# Cocoa view, one class name per binary).
if(APPLE)
  target_sources(gridline_vst3_testplug PRIVATE tests/common/TestViewMac.mm)
  target_sources(gridline_clap_testplug PRIVATE tests/common/TestViewMac.mm)
  target_compile_definitions(gridline_vst3_testplug PRIVATE GRIDLINE_TEST_VIEW_CLASS=GridlineVst3TestView)
  target_compile_definitions(gridline_clap_testplug PRIVATE GRIDLINE_TEST_VIEW_CLASS=GridlineClapTestView)
  target_link_libraries(gridline_vst3_testplug PRIVATE "-framework AppKit")
  target_link_libraries(gridline_clap_testplug PRIVATE "-framework AppKit")
endif()
set(GRIDLINE_CLAP_TESTPLUG ${CMAKE_BINARY_DIR}/test-clap/GridlineTest.clap)
if(APPLE)
  set(GRIDLINE_VST2_TESTPLUG ${CMAKE_BINARY_DIR}/test-vst2/GridlineTestVst2.vst)
  set_target_properties(gridline_clap_testplug PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME GridlineTest
                        LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_CLAP_TESTPLUG}/Contents/MacOS)
  set_target_properties(gridline_vst2_testplug PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME GridlineTestVst2
                        LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_VST2_TESTPLUG}/Contents/MacOS)
  foreach(b clap vst2)
    if(b STREQUAL "clap")
      set(_dir ${GRIDLINE_CLAP_TESTPLUG})
      set(_exe GridlineTest)
    else()
      set(_dir ${GRIDLINE_VST2_TESTPLUG})
      set(_exe GridlineTestVst2)
    endif()
    file(WRITE ${_dir}/Contents/Info.plist
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n<key>CFBundleExecutable</key><string>${_exe}</string>\n<key>CFBundleIdentifier</key><string>org.gridline.${b}.test</string>\n<key>CFBundlePackageType</key><string>BNDL</string>\n</dict></plist>\n")
  endforeach()
else()
  set(GRIDLINE_VST2_TESTPLUG ${CMAKE_BINARY_DIR}/test-vst2/GridlineTestVst2${CMAKE_SHARED_MODULE_SUFFIX})
  set_target_properties(gridline_clap_testplug PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME GridlineTest
                        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-clap)
  set_target_properties(gridline_vst2_testplug PROPERTIES PREFIX "" SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}" OUTPUT_NAME GridlineTestVst2
                        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-vst2)
endif()
add_executable(gridline_tests_hosting tests/test_hosting.cpp)
target_link_libraries(gridline_tests_hosting PRIVATE gridline_core)
target_compile_options(gridline_tests_hosting PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_hosting PRIVATE GRIDLINE_CLAP_TESTPLUG="${GRIDLINE_CLAP_TESTPLUG}"
                           GRIDLINE_VST2_TESTPLUG="${GRIDLINE_VST2_TESTPLUG}" GRIDLINE_TESTPLUG_BUNDLE="${GRIDLINE_TESTPLUG_BUNDLE}"
                           GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_hosting gridline_clap_testplug gridline_vst2_testplug gridline_vst3_testplug gridline)
add_test(NAME plugin_hosting COMMAND gridline_tests_hosting WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-13: plugin search folders per format (fixture CLAP plugin in temp folders only).
add_executable(gridline_tests_plugin_folders tests/test_plugin_folders.cpp)
target_link_libraries(gridline_tests_plugin_folders PRIVATE gridline_core)
target_include_directories(gridline_tests_plugin_folders PRIVATE src tests)
target_compile_options(gridline_tests_plugin_folders PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_plugin_folders PRIVATE GRIDLINE_CLAP_TESTPLUG="${GRIDLINE_CLAP_TESTPLUG}"
                           GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_plugin_folders gridline_clap_testplug gridline)
add_test(NAME k13_plugin_folders COMMAND gridline_tests_plugin_folders WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-71: a plugin scan never holds up the interface (fake scanner children, fake plugin folders).
add_executable(gridline_tests_scan_hang tests/test_scan_hang.cpp)
target_link_libraries(gridline_tests_scan_hang PRIVATE gridline_core)
target_include_directories(gridline_tests_scan_hang PRIVATE src tests)
target_compile_options(gridline_tests_scan_hang PRIVATE ${GL_WARN})
add_test(NAME k71_scan_hang COMMAND gridline_tests_scan_hang WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k71_scan_hang PROPERTIES TIMEOUT 240)
# K-73: a stand-in plugin scanner child (fake plugins that hang, crash, are slow or hold many plugins).
# K-154: a plugin scan never runs the computer out of memory (the test is its own fake scanner child).
add_executable(gridline_tests_scan_memory tests/test_scan_memory.cpp)
target_link_libraries(gridline_tests_scan_memory PRIVATE gridline_core)
target_include_directories(gridline_tests_scan_memory PRIVATE src tests)
target_compile_options(gridline_tests_scan_memory PRIVATE ${GL_WARN})
add_test(NAME k154_scan_memory COMMAND gridline_tests_scan_memory WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k154_scan_memory PROPERTIES TIMEOUT 300)
add_executable(gridline_tests_demo_memory tests/test_demo_memory.cpp)
target_link_libraries(gridline_tests_demo_memory PRIVATE gridline_core)
target_include_directories(gridline_tests_demo_memory PRIVATE src tests)
target_compile_options(gridline_tests_demo_memory PRIVATE ${GL_WARN})
add_test(NAME k154_demo_memory COMMAND gridline_tests_demo_memory WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k154_demo_memory PROPERTIES TIMEOUT 600)

add_executable(gridline_fake_plugin_scanner tests/fake_plugin_scanner.cpp)
target_compile_options(gridline_fake_plugin_scanner PRIVATE ${GL_WARN})
# K-73: scans at a big collection's scale (1836 fake plugins), shells, hung plugins, VST3 search, Windows DLL check.
add_executable(gridline_tests_scan_stress tests/test_scan_stress.cpp)
target_link_libraries(gridline_tests_scan_stress PRIVATE gridline_core)
target_include_directories(gridline_tests_scan_stress PRIVATE src tests)
target_compile_options(gridline_tests_scan_stress PRIVATE ${GL_WARN})
add_dependencies(gridline_tests_scan_stress gridline_fake_plugin_scanner)
add_test(NAME k73_scan_stress COMMAND gridline_tests_scan_stress $<TARGET_FILE:gridline_fake_plugin_scanner> WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k73_scan_stress PROPERTIES TIMEOUT 600)
# J-66: keys from plugin interfaces to Gridline's shortcuts.
add_executable(gridline_tests_j66 tests/test_j66.cpp)
target_link_libraries(gridline_tests_j66 PRIVATE gridline_core)
target_compile_options(gridline_tests_j66 PRIVATE ${GL_WARN})
add_test(NAME j66_plugin_keys COMMAND gridline_tests_j66 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Q-4: piano roll event lane painting (segment sweep) and line tool.
add_executable(gridline_tests_q4 tests/test_q4.cpp)
target_link_libraries(gridline_tests_q4 PRIVATE gridline_core)
target_compile_options(gridline_tests_q4 PRIVATE ${GL_WARN})
add_test(NAME q4_event_lane COMMAND gridline_tests_q4 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-73: eye toggle, shared minimum inset, effect reorder by wheel, reverb room drag, lane bar glides.
add_executable(gridline_tests_j73 tests/test_j73.cpp)
target_link_libraries(gridline_tests_j73 PRIVATE gridline_core)
target_compile_options(gridline_tests_j73 PRIVATE ${GL_WARN})
add_test(NAME j73_ui_polish COMMAND gridline_tests_j73 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# J-128: layout alignment checker (rows, labels, gaps, baselines, overlaps, clipping) and its regression gate.
add_executable(gridline_tests_align_check tests/test_align_check.cpp)
target_compile_options(gridline_tests_align_check PRIVATE ${GL_WARN})
add_test(NAME j128_align_check COMMAND gridline_tests_align_check WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# LV2 hosting (PH-04): an in-tree test bundle (three plugins, their interfaces,
# presets) plus a presets-only bundle, built into test-lv2/ (never installed).
set(GRIDLINE_LV2_DIR ${CMAKE_BINARY_DIR}/test-lv2)
set(GRIDLINE_LV2_BUNDLE ${GRIDLINE_LV2_DIR}/GridlineTest.lv2)
set(GL_LV2_EXT ${CMAKE_SHARED_LIBRARY_SUFFIX})
set(GRIDLINE_LV2_UI_SRC tests/lv2/TestUi.cpp)
if(APPLE)
  list(APPEND GRIDLINE_LV2_UI_SRC tests/lv2/TestUiCocoa.mm)
  set(GL_LV2_GAIN_UI "ui:ui <urn:gridline:lv2test:gain#ui> ;")
  set(GL_LV2_COCOA_MANIFEST "<urn:gridline:lv2test:gain#ui> a ui:CocoaUI ; ui:binary <GridlineTestLv2UI${GL_LV2_EXT}> ; rdfs:seeAlso <ui.ttl> .")
  set(GL_LV2_COCOA_UI "<urn:gridline:lv2test:gain#ui> a ui:CocoaUI ; ui:binary <GridlineTestLv2UI${GL_LV2_EXT}> ; lv2:optionalFeature ui:resize .")
else()
  set(GL_LV2_GAIN_UI "")
  set(GL_LV2_COCOA_MANIFEST "")
  set(GL_LV2_COCOA_UI "")
endif()
add_library(gridline_lv2_testplug MODULE tests/lv2/TestPlugins.cpp)
add_library(gridline_lv2_testui MODULE ${GRIDLINE_LV2_UI_SRC})
foreach(t gridline_lv2_testplug gridline_lv2_testui)
  target_include_directories(${t} SYSTEM PRIVATE third_party/lv2/include)
  target_compile_options(${t} PRIVATE ${GL_WARN})
  set_target_properties(${t} PROPERTIES PREFIX "" SUFFIX "${GL_LV2_EXT}" LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_LV2_BUNDLE})
endforeach()
set_target_properties(gridline_lv2_testplug PROPERTIES OUTPUT_NAME GridlineTestLv2)
set_target_properties(gridline_lv2_testui PROPERTIES OUTPUT_NAME GridlineTestLv2UI)
if(APPLE)
  target_link_libraries(gridline_lv2_testui PRIVATE "-framework AppKit")
endif()
foreach(f manifest.ttl gain.ttl ui.ttl)
  configure_file(tests/lv2/bundle/${f}.in ${GRIDLINE_LV2_BUNDLE}/${f} @ONLY)
endforeach()
foreach(f synth.ttl state.ttl presets.ttl)
  configure_file(tests/lv2/bundle/${f} ${GRIDLINE_LV2_BUNDLE}/${f} COPYONLY)
endforeach()
foreach(f manifest.ttl extra.ttl)
  configure_file(tests/lv2/presets-bundle/${f} ${GRIDLINE_LV2_DIR}/GridlineTestPresets.lv2/${f} COPYONLY)
endforeach()
add_executable(gridline_tests_lv2 tests/test_lv2.cpp)
target_link_libraries(gridline_tests_lv2 PRIVATE gridline_core)
target_compile_options(gridline_tests_lv2 PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_lv2 PRIVATE GRIDLINE_LV2_DIR="${GRIDLINE_LV2_DIR}" GRIDLINE_LV2_BUNDLE="${GRIDLINE_LV2_BUNDLE}"
                           GRIDLINE_LV2_EXT="${GL_LV2_EXT}" GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_lv2 gridline_lv2_testplug gridline_lv2_testui gridline)
add_test(NAME lv2_host COMMAND gridline_tests_lv2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# The plugin-check sweep runs the test plugins against real-time block deadlines: run it alone
# so a busy parallel ctest on a small CI runner cannot turn load into a failure.
set_tests_properties(lv2_host PROPERTIES RUN_SERIAL TRUE)
# K-180: a bundle of hundreds of LV2 plugins scans in little memory. The test writes its bundles
# around this binary of 256 plugins (each instance holds 24 MB).
add_library(gridline_lv2_manytestplug MODULE tests/lv2/ManyPlugins.cpp)
target_include_directories(gridline_lv2_manytestplug SYSTEM PRIVATE third_party/lv2/include)
target_compile_options(gridline_lv2_manytestplug PRIVATE ${GL_WARN})
set_target_properties(gridline_lv2_manytestplug PROPERTIES PREFIX "" OUTPUT_NAME GridlineManyLv2 SUFFIX "${GL_LV2_EXT}"
                      LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_LV2_DIR}/many-bin)
add_executable(gridline_tests_lv2_many tests/test_lv2_many.cpp)
target_link_libraries(gridline_tests_lv2_many PRIVATE gridline_core)
target_include_directories(gridline_tests_lv2_many PRIVATE src tests)
target_compile_options(gridline_tests_lv2_many PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_lv2_many PRIVATE GRIDLINE_LV2_MANY_BINARY="$<TARGET_FILE:gridline_lv2_manytestplug>")
add_dependencies(gridline_tests_lv2_many gridline_lv2_manytestplug)
add_test(NAME k180_lv2_many COMMAND gridline_tests_lv2_many WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k180_lv2_many PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

add_executable(gridline_tests_scripting tests/test_scripting.cpp)
target_link_libraries(gridline_tests_scripting PRIVATE gridline_core)
target_compile_options(gridline_tests_scripting PRIVATE ${GL_WARN})
add_test(NAME scripting COMMAND gridline_tests_scripting WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# SC-07: MCP server (protocol, tools, loopback HTTP transport, stdio, attach bridge).
add_executable(gridline_tests_mcp tests/test_mcp.cpp)
target_link_libraries(gridline_tests_mcp PRIVATE gridline_core)
target_compile_options(gridline_tests_mcp PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_mcp PRIVATE GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_mcp gridline)
add_test(NAME mcp COMMAND gridline_tests_mcp WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# J-57: full-model project scripting API (SC-02).
add_executable(gridline_tests_project_script tests/test_project_script.cpp)
target_link_libraries(gridline_tests_project_script PRIVATE gridline_core)
target_compile_options(gridline_tests_project_script PRIVATE ${GL_WARN})
add_test(NAME project_script COMMAND gridline_tests_project_script WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# J-59: loop starter (GEN-01) and render to sampled instrument (RN-08).
add_executable(gridline_tests_j59 tests/test_j59.cpp)
target_link_libraries(gridline_tests_j59 PRIVATE gridline_core)
target_compile_options(gridline_tests_j59 PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_j59 PRIVATE GRIDLINE_TESTPLUG_BUNDLE="${GRIDLINE_TESTPLUG_BUNDLE}")
add_dependencies(gridline_tests_j59 gridline_vst3_testplug)
add_test(NAME j59_loopstarter_sampler COMMAND gridline_tests_j59 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# J-61: audio load meter, secret store (MCP token) and its migration.
add_executable(gridline_tests_j61 tests/test_j61.cpp)
target_link_libraries(gridline_tests_j61 PRIVATE gridline_core)
target_compile_options(gridline_tests_j61 PRIVATE ${GL_WARN})
add_test(NAME j61_followups COMMAND gridline_tests_j61 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# J-86: the operating-system layer (folders, processes, numbers, sockets, shared objects, keys).
add_executable(gridline_tests_portability tests/test_portability.cpp)
target_link_libraries(gridline_tests_portability PRIVATE gridline_core)
target_include_directories(gridline_tests_portability PRIVATE tests)
target_compile_options(gridline_tests_portability PRIVATE ${GL_WARN})
target_compile_definitions(gridline_tests_portability PRIVATE GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_portability gridline)
add_test(NAME portability COMMAND gridline_tests_portability WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(gridline_tests_recording tests/test_recording.cpp)
target_link_libraries(gridline_tests_recording PRIVATE gridline_core)
target_compile_options(gridline_tests_recording PRIVATE ${GL_WARN})
add_test(NAME recording COMMAND gridline_tests_recording WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-20: export & interop (render scopes, tails, dither, stems, MIDI, zipped projects, relinking).
add_executable(gridline_tests_export tests/test_export.cpp)
target_link_libraries(gridline_tests_export PRIVATE gridline_core)
target_include_directories(gridline_tests_export PRIVATE tests)
target_compile_options(gridline_tests_export PRIVATE ${GL_WARN})
add_test(NAME export COMMAND gridline_tests_export WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-52: project files — open vs import routing, dialog types, FL Studio zipped loop package.
add_executable(gridline_tests_project_files tests/test_project_files.cpp)
target_link_libraries(gridline_tests_project_files PRIVATE gridline_core)
target_include_directories(gridline_tests_project_files PRIVATE tests)
target_compile_options(gridline_tests_project_files PRIVATE ${GL_WARN})
add_test(NAME project_files COMMAND gridline_tests_project_files WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-51: M4A (AAC, Apple Lossless) and ADTS AAC samples: system decoder and FFmpeg routes, FL import.
add_executable(gridline_tests_k51 tests/test_k51.cpp)
target_link_libraries(gridline_tests_k51 PRIVATE gridline_core)
target_include_directories(gridline_tests_k51 PRIVATE tests src)
target_compile_options(gridline_tests_k51 PRIVATE ${GL_WARN})
add_test(NAME k51_m4a COMMAND gridline_tests_k51 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-121: single project extensions (.gridline, .gridlinez; the older names still open) and one Gridline per user.
add_executable(gridline_tests_k121 tests/test_k121.cpp)
target_link_libraries(gridline_tests_k121 PRIVATE gridline_core)
target_include_directories(gridline_tests_k121 PRIVATE tests src)
target_compile_options(gridline_tests_k121 PRIVATE ${GL_WARN})
add_test(NAME k121 COMMAND gridline_tests_k121 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-78: FLP export hunt, FLP file writing, save types (Gridline zipped project), notification copy text.
add_executable(gridline_tests_j78 tests/test_j78.cpp src/app/ToastText.cpp)
target_link_libraries(gridline_tests_j78 PRIVATE gridline_core)
target_include_directories(gridline_tests_j78 PRIVATE tests src)
target_compile_options(gridline_tests_j78 PRIVATE ${GL_WARN})
add_test(NAME j78 COMMAND gridline_tests_j78 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-135: the "Gridline" demo song loads, has its section markers and renders on target (loudness, true peak).
add_executable(gridline_tests_demo_song tests/test_demo_song.cpp)
target_link_libraries(gridline_tests_demo_song PRIVATE gridline_core)
target_include_directories(gridline_tests_demo_song PRIVATE tests src)
target_compile_options(gridline_tests_demo_song PRIVATE ${GL_WARN})
add_test(NAME j135_demo_song COMMAND gridline_tests_demo_song WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(j135_demo_song PROPERTIES TIMEOUT 900)
# Stand-ins for missing Serum / Kickstart (Plasma / Tideline) and Plasma's one Open path for preset files.
add_executable(gridline_tests_standin tests/test_standin.cpp)
target_link_libraries(gridline_tests_standin PRIVATE gridline_core)
target_include_directories(gridline_tests_standin PRIVATE tests src)
target_compile_options(gridline_tests_standin PRIVATE ${GL_WARN})
add_test(NAME standin COMMAND gridline_tests_standin WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-31: chord progression editor core, groups & loops, recording extras, key markers, quantized jumps.
add_executable(gridline_tests_j31 tests/test_j31.cpp)
target_link_libraries(gridline_tests_j31 PRIVATE gridline_core)
target_include_directories(gridline_tests_j31 PRIVATE tests)
target_compile_options(gridline_tests_j31 PRIVATE ${GL_WARN})
add_test(NAME j31 COMMAND gridline_tests_j31 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-169: split and layer instrument (zones, modes, macros, layer presets, FLP rules).
add_executable(gridline_tests_layer_rack tests/test_layer_rack.cpp)
target_link_libraries(gridline_tests_layer_rack PRIVATE gridline_core)
target_include_directories(gridline_tests_layer_rack PRIVATE tests src)
target_compile_options(gridline_tests_layer_rack PRIVATE ${GL_WARN})
add_test(NAME k169_layer_rack COMMAND gridline_tests_layer_rack WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-77: engine note state (lit keys, pad flashes), Keep / Revert flow, PAT shows the rack.
add_executable(gridline_tests_j77 tests/test_j77.cpp src/app/DisplayConfirm.cpp)
target_link_libraries(gridline_tests_j77 PRIVATE gridline_core)
target_include_directories(gridline_tests_j77 PRIVATE tests src)
target_compile_options(gridline_tests_j77 PRIVATE ${GL_WARN})
add_test(NAME j77 COMMAND gridline_tests_j77 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-217: the incremental waveform summary of takes being recorded; live placement equals the committed clips.
add_executable(gridline_tests_j217 tests/test_j217.cpp)
target_link_libraries(gridline_tests_j217 PRIVATE gridline_core)
target_include_directories(gridline_tests_j217 PRIVATE tests src)
target_compile_options(gridline_tests_j217 PRIVATE ${GL_WARN})
add_test(NAME j217_live_take COMMAND gridline_tests_j217 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-186: play lights: channel activity (note starts, held notes, output peak), the rack light model, Kerf's sounding regions.
add_executable(gridline_tests_j186 tests/test_j186.cpp)
target_link_libraries(gridline_tests_j186 PRIVATE gridline_core)
target_include_directories(gridline_tests_j186 PRIVATE tests src)
target_compile_options(gridline_tests_j186 PRIVATE ${GL_WARN})
add_test(NAME j186 COMMAND gridline_tests_j186 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-79: topmost-only delete sweeps, the audio clip volume line, zero crossings, stretch mode, toggles.
add_executable(gridline_tests_j79 tests/test_j79.cpp)
target_link_libraries(gridline_tests_j79 PRIVATE gridline_core)
target_include_directories(gridline_tests_j79 PRIVATE tests src)
target_compile_options(gridline_tests_j79 PRIVATE ${GL_WARN})
add_test(NAME j79 COMMAND gridline_tests_j79 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_clip_multi_edit tests/test_clip_multi_edit.cpp)
target_link_libraries(gridline_tests_clip_multi_edit PRIVATE gridline_core)
target_include_directories(gridline_tests_clip_multi_edit PRIVATE tests src)
target_compile_options(gridline_tests_clip_multi_edit PRIVATE ${GL_WARN})
add_test(NAME clip_multi_edit COMMAND gridline_tests_clip_multi_edit WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-196: clip level drags patch the playing program; pitch drags defer renders and keep voices.
add_executable(gridline_tests_j196 tests/test_j196.cpp)
target_link_libraries(gridline_tests_j196 PRIVATE gridline_core)
target_include_directories(gridline_tests_j196 PRIVATE tests src)
target_compile_options(gridline_tests_j196 PRIVATE ${GL_WARN})
add_test(NAME j196 COMMAND gridline_tests_j196 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-208: the playlist picker's "+" (New pattern naming) and instruments dropped on playlist tracks.
add_executable(gridline_tests_j208 tests/test_j208.cpp)
target_link_libraries(gridline_tests_j208 PRIVATE gridline_core)
target_include_directories(gridline_tests_j208 PRIVATE tests src)
target_compile_options(gridline_tests_j208 PRIVATE ${GL_WARN})
add_test(NAME j208 COMMAND gridline_tests_j208 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-12: the playlist track "+" edits (an instrument for a track, a pattern started on it; one undo step each).
add_executable(gridline_tests_k12 tests/test_k12.cpp)
target_link_libraries(gridline_tests_k12 PRIVATE gridline_core)
target_include_directories(gridline_tests_k12 PRIVATE tests src)
target_compile_options(gridline_tests_k12 PRIVATE ${GL_WARN})
add_test(NAME k12 COMMAND gridline_tests_k12 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-213: replacing a channel's instrument renames an automatically named channel (one undo step).
add_executable(gridline_tests_j213 tests/test_j213.cpp)
target_link_libraries(gridline_tests_j213 PRIVATE gridline_core)
target_include_directories(gridline_tests_j213 PRIVATE tests src)
target_compile_options(gridline_tests_j213 PRIVATE ${GL_WARN})
add_test(NAME j213 COMMAND gridline_tests_j213 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-126: the channel rack's solo (right-click on a channel's LED) and its unsolo.
add_executable(gridline_tests_k126 tests/test_k126.cpp)
target_link_libraries(gridline_tests_k126 PRIVATE gridline_core)
target_include_directories(gridline_tests_k126 PRIVATE tests src)
target_compile_options(gridline_tests_k126 PRIVATE ${GL_WARN})
add_test(NAME k126_channel_solo COMMAND gridline_tests_k126 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-85: one-step song loop markers; marker actions in the audio engine (sample-exact).
add_executable(gridline_tests_j85 tests/test_j85.cpp)
target_link_libraries(gridline_tests_j85 PRIVATE gridline_core)
target_include_directories(gridline_tests_j85 PRIVATE tests src)
target_compile_options(gridline_tests_j85 PRIVATE ${GL_WARN})
add_test(NAME j85 COMMAND gridline_tests_j85 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-87: time stretching / pitch shifting (engine/TimeStretch, Signalsmith Stretch).
add_executable(gridline_tests_j87 tests/test_j87.cpp)
target_link_libraries(gridline_tests_j87 PRIVATE gridline_core)
target_include_directories(gridline_tests_j87 PRIVATE tests src)
target_compile_options(gridline_tests_j87 PRIVATE ${GL_WARN})
add_test(NAME j87 COMMAND gridline_tests_j87 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-164: an audio clip's pitch never changes its length or how its audio fills it.
add_executable(gridline_tests_j164 tests/test_j164.cpp)
target_link_libraries(gridline_tests_j164 PRIVATE gridline_core)
target_include_directories(gridline_tests_j164 PRIVATE tests src)
target_compile_options(gridline_tests_j164 PRIVATE ${GL_WARN})
add_test(NAME j164 COMMAND gridline_tests_j164 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-168: FLP playlist records of every size; an unknown layout is read or reported, never dropped.
add_executable(gridline_tests_j168 tests/test_j168.cpp)
target_link_libraries(gridline_tests_j168 PRIVATE gridline_core)
target_include_directories(gridline_tests_j168 PRIVATE tests src)
target_compile_options(gridline_tests_j168 PRIVATE ${GL_WARN})
add_test(NAME j168 COMMAND gridline_tests_j168 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-177: FL channel stretch "time": imported clips play on the beat grid, round trip.
add_executable(gridline_tests_j177 tests/test_j177.cpp)
target_link_libraries(gridline_tests_j177 PRIVATE gridline_core)
target_include_directories(gridline_tests_j177 PRIVATE tests src)
target_compile_options(gridline_tests_j177 PRIVATE ${GL_WARN})
add_test(NAME j177_flp_clip_time COMMAND gridline_tests_j177 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-167: the project loader's progress / cancel model (reading, samples, plugins, cancel at each phase).
# K-42: opening projects from web and network drives (zip streaming, the local cache, stalled and
# failing reads, cancel, no folder walks).
add_executable(gridline_tests_k42 tests/test_k42.cpp)
target_link_libraries(gridline_tests_k42 PRIVATE gridline_core)
target_include_directories(gridline_tests_k42 PRIVATE tests src)
target_compile_options(gridline_tests_k42 PRIVATE ${GL_WARN})
add_test(NAME k42_web_drives COMMAND gridline_tests_k42 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k42_web_drives PROPERTIES TIMEOUT 120)
add_executable(gridline_tests_k44 tests/test_k44.cpp)
target_link_libraries(gridline_tests_k44 PRIVATE gridline_core)
target_include_directories(gridline_tests_k44 PRIVATE tests src)
target_compile_options(gridline_tests_k44 PRIVATE ${GL_WARN})
add_test(NAME k44_zip_holds_everything COMMAND gridline_tests_k44 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k44_zip_holds_everything PROPERTIES TIMEOUT 300)

# K-45: the unpacked project cache: local archives, close, sweep, limit, Clear cache.
add_executable(gridline_tests_k45 tests/test_k45.cpp)
target_link_libraries(gridline_tests_k45 PRIVATE gridline_core)
target_include_directories(gridline_tests_k45 PRIVATE tests src)
target_compile_options(gridline_tests_k45 PRIVATE ${GL_WARN})
add_test(NAME k45_unpacked_cache COMMAND gridline_tests_k45 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(gridline_tests_q18 tests/test_q18.cpp)
target_link_libraries(gridline_tests_q18 PRIVATE gridline_core)
target_include_directories(gridline_tests_q18 PRIVATE tests src)
target_compile_options(gridline_tests_q18 PRIVATE ${GL_WARN})
add_test(NAME q18_first_block COMMAND gridline_tests_q18 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k45_unpacked_cache PROPERTIES TIMEOUT 180)

add_executable(gridline_tests_j167 tests/test_j167.cpp)
target_link_libraries(gridline_tests_j167 PRIVATE gridline_core)
target_include_directories(gridline_tests_j167 PRIVATE tests src)
target_compile_options(gridline_tests_j167 PRIVATE ${GL_WARN})
add_test(NAME j167_project_load COMMAND gridline_tests_j167 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-166: imported Slicex chops (order, keys, names, ends) play and export exactly in Kerf.
add_executable(gridline_tests_j166 tests/test_j166.cpp)
target_link_libraries(gridline_tests_j166 PRIVATE gridline_core)
target_include_directories(gridline_tests_j166 PRIVATE tests src)
target_compile_options(gridline_tests_j166 PRIVATE ${GL_WARN})
add_test(NAME j166_slicex_chops COMMAND gridline_tests_j166 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-176: Slicex states with long sample paths (0xFF + u32 length) and hundreds of regions reach Kerf.
add_executable(gridline_tests_j176 tests/test_j176.cpp)
target_link_libraries(gridline_tests_j176 PRIVATE gridline_core)
target_include_directories(gridline_tests_j176 PRIVATE tests src)
target_compile_options(gridline_tests_j176 PRIVATE ${GL_WARN})
add_test(NAME j176_slicex_long_path COMMAND gridline_tests_j176 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-180: a note's fine pitch (and channel / master pitch) tunes key-mapped instruments without moving the key.
add_executable(gridline_tests_j180 tests/test_j180.cpp)
target_link_libraries(gridline_tests_j180 PRIVATE gridline_core)
target_include_directories(gridline_tests_j180 PRIVATE tests src)
target_compile_options(gridline_tests_j180 PRIVATE ${GL_WARN})
add_test(NAME j180_note_fine_pitch COMMAND gridline_tests_j180 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-182: Kerf's time and marker settings: snap sizes, nudging, bars:beats ruler, names, state round trips.
add_executable(gridline_tests_j182 tests/test_j182.cpp)
target_link_libraries(gridline_tests_j182 PRIVATE gridline_core)
target_include_directories(gridline_tests_j182 PRIVATE tests src)
target_compile_options(gridline_tests_j182 PRIVATE ${GL_WARN})
add_test(NAME j182_kerf_time_markers COMMAND gridline_tests_j182 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-190: Kerf slices on the beat grid (synthetic loops: tempos, pickup, drift, 3/4, triplets, fallback, state, FL export).
add_executable(gridline_tests_j190 tests/test_j190.cpp)
target_link_libraries(gridline_tests_j190 PRIVATE gridline_core)
target_include_directories(gridline_tests_j190 PRIVATE tests src)
target_compile_options(gridline_tests_j190 PRIVATE ${GL_WARN})
add_test(NAME j190_kerf_beat_grid COMMAND gridline_tests_j190 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(j190_kerf_beat_grid PROPERTIES TIMEOUT 600)

# J-193: stretched clips follow the tempo; unstretched clips keep their audio (their border follows).
add_executable(gridline_tests_j193 tests/test_j193.cpp)
target_link_libraries(gridline_tests_j193 PRIVATE gridline_core)
target_include_directories(gridline_tests_j193 PRIVATE tests src)
target_compile_options(gridline_tests_j193 PRIVATE ${GL_WARN})
add_test(NAME j193_tempo_follow COMMAND gridline_tests_j193 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(j193_tempo_follow PROPERTIES TIMEOUT 600)
# J-192: Import from URL (core/UrlImport.h, core/MediaTools.h) with a fake yt-dlp / ffmpeg: never the network.
add_executable(gridline_fake_media_tool tests/fake_media_tool.cpp)
target_compile_options(gridline_fake_media_tool PRIVATE ${GL_WARN})
add_executable(gridline_tests_urlimport tests/test_urlimport.cpp)
target_link_libraries(gridline_tests_urlimport PRIVATE gridline_core)
target_include_directories(gridline_tests_urlimport PRIVATE tests src)
target_compile_options(gridline_tests_urlimport PRIVATE ${GL_WARN})
add_dependencies(gridline_tests_urlimport gridline_fake_media_tool)
add_test(NAME j192_import_url COMMAND gridline_tests_urlimport $<TARGET_FILE:gridline_fake_media_tool> WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(j192_import_url PROPERTIES TIMEOUT 300)
# J-109: Stop releases every sound outside the song; Stop twice (panic) silences tails too.
add_executable(gridline_tests_j109 tests/test_j109.cpp)
target_link_libraries(gridline_tests_j109 PRIVATE gridline_core)
# backtrace() for the allocation-stack aid lives in libexecinfo on the BSDs (and musl with it installed).
include(CheckSymbolExists)
find_library(GL_EXECINFO_LIB execinfo)
if(GL_EXECINFO_LIB)
  set(CMAKE_REQUIRED_LIBRARIES ${GL_EXECINFO_LIB})
endif()
check_symbol_exists(backtrace execinfo.h GL_HAVE_BACKTRACE)
unset(CMAKE_REQUIRED_LIBRARIES)
if(GL_HAVE_BACKTRACE)
  target_compile_definitions(gridline_tests_j109 PRIVATE GL_HAVE_BACKTRACE=1)
  if(GL_EXECINFO_LIB)
    target_link_libraries(gridline_tests_j109 PRIVATE ${GL_EXECINFO_LIB})
  endif()
endif()
target_include_directories(gridline_tests_j109 PRIVATE tests src)
target_compile_options(gridline_tests_j109 PRIVATE ${GL_WARN})
add_test(NAME j109_stop_sound COMMAND gridline_tests_j109 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-110: plugins' own play / run states (Seltmann's pattern, one-shots, Shuttle, Loupe) follow Stop.
add_executable(gridline_tests_j110 tests/test_j110.cpp)
target_link_libraries(gridline_tests_j110 PRIVATE gridline_core)
target_include_directories(gridline_tests_j110 PRIVATE tests src)
target_compile_options(gridline_tests_j110 PRIVATE ${GL_WARN})
add_test(NAME j110_play_state COMMAND gridline_tests_j110 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-112: Play while the transport is stopped and a plugin plays on its own stops that playback first.
add_executable(gridline_tests_j112 tests/test_j112.cpp)
target_link_libraries(gridline_tests_j112 PRIVATE gridline_core)
target_include_directories(gridline_tests_j112 PRIVATE tests src)
target_compile_options(gridline_tests_j112 PRIVATE ${GL_WARN})
add_test(NAME j112_play_stops_plugins COMMAND gridline_tests_j112 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-113: detachable windows: saved positions recovered onto the monitors, re-anchored, title bars kept reachable.
add_executable(gridline_tests_j113 tests/test_j113.cpp)
target_include_directories(gridline_tests_j113 PRIVATE tests src)
target_compile_options(gridline_tests_j113 PRIVATE ${GL_WARN})
add_test(NAME j113_window_placement COMMAND gridline_tests_j113 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-117: the elastic boundary of floating windows inside the main window (core/WindowRubber.h).
add_executable(gridline_tests_j117 tests/test_j117.cpp src/core/SmoothScroll.cpp)
target_include_directories(gridline_tests_j117 PRIVATE tests src)
target_compile_options(gridline_tests_j117 PRIVATE ${GL_WARN})
add_test(NAME j117_window_rubber COMMAND gridline_tests_j117 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-118: the playlist's clip brush: a clicked clip paints as it is (engine/PlaylistTools.h).
add_executable(gridline_tests_j118 tests/test_j118.cpp)
target_link_libraries(gridline_tests_j118 PRIVATE gridline_core)
target_include_directories(gridline_tests_j118 PRIVATE tests src)
target_compile_options(gridline_tests_j118 PRIVATE ${GL_WARN})
add_test(NAME j118_paint_brush COMMAND gridline_tests_j118 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-91 (GitHub issue #1): system interface scale, plugin keys on X11 / Windows, Auto Pan pad, motion switches.
add_executable(gridline_tests_j91 tests/test_j91.cpp)
target_link_libraries(gridline_tests_j91 PRIVATE gridline_core)
target_include_directories(gridline_tests_j91 PRIVATE tests src)
target_compile_options(gridline_tests_j91 PRIVATE ${GL_WARN})
add_test(NAME j91 COMMAND gridline_tests_j91 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-19 / K-15: plugin editor sizes (zoom rules, remembered per type) and Loupe's layout per dock spot.
add_executable(gridline_tests_k19 tests/test_k19.cpp)
target_link_libraries(gridline_tests_k19 PRIVATE gridline_core)
target_include_directories(gridline_tests_k19 PRIVATE tests src)
target_compile_options(gridline_tests_k19 PRIVATE ${GL_WARN})
add_test(NAME k19_editor_size COMMAND gridline_tests_k19 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-147: magnetic knob detents (soft magnets at the divisions of a synced time knob) and the
# plugin window fit rules.
add_executable(gridline_tests_k147 tests/test_k147.cpp)
target_link_libraries(gridline_tests_k147 PRIVATE gridline_core)
target_include_directories(gridline_tests_k147 PRIVATE tests src)
target_compile_options(gridline_tests_k147 PRIVATE ${GL_WARN})
add_test(NAME k147_magnets COMMAND gridline_tests_k147 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-25: per-note pitch bend lines: shape, save / load, undo, note edits, playback (measured pitch,
# per voice, block sizes, MIDI Out), MIDI file export / import, FL export report.
add_executable(gridline_tests_k25 tests/test_k25.cpp)
target_link_libraries(gridline_tests_k25 PRIVATE gridline_core)
target_include_directories(gridline_tests_k25 PRIVATE tests src)
target_compile_options(gridline_tests_k25 PRIVATE ${GL_WARN})
add_test(NAME k25_pitch_bend COMMAND gridline_tests_k25 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-28: bend curve shapes and amounts, FL export as slide notes (pitch tracks compared by ear).
add_executable(gridline_tests_k28 tests/test_k28.cpp)
target_link_libraries(gridline_tests_k28 PRIVATE gridline_core)
target_include_directories(gridline_tests_k28 PRIVATE tests src)
target_compile_options(gridline_tests_k28 PRIVATE ${GL_WARN})
add_test(NAME k28_bend_curves_fl COMMAND gridline_tests_k28 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-22: channel rack steps in the piano roll: the view, the first edit's conversion (one undo step),
# playback identical before / after (rendered audio), save / load, FL export / import round trip.
add_executable(gridline_tests_k22 tests/test_k22.cpp)
target_link_libraries(gridline_tests_k22 PRIVATE gridline_core)
target_include_directories(gridline_tests_k22 PRIVATE tests src)
target_compile_options(gridline_tests_k22 PRIVATE ${GL_WARN})
add_test(NAME k22_steps_in_piano_roll COMMAND gridline_tests_k22 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# K-47: exports with progress and Cancel, no partial target; a clipboard that holds a closed project's copy.
add_executable(gridline_tests_k49 tests/test_k49.cpp)
target_link_libraries(gridline_tests_k49 PRIVATE gridline_core)
target_include_directories(gridline_tests_k49 PRIVATE tests src)
target_compile_options(gridline_tests_k49 PRIVATE ${GL_WARN})
add_test(NAME k49_fl_tempo_clips_and_stretched_clips COMMAND gridline_tests_k49 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k49_fl_tempo_clips_and_stretched_clips PROPERTIES TIMEOUT 300)

# K-58: FL audio clips on a channel that resamples hold their rate and pitch under tempo automation.
add_executable(gridline_tests_k58 tests/test_k58.cpp)
target_link_libraries(gridline_tests_k58 PRIVATE gridline_core)
target_include_directories(gridline_tests_k58 PRIVATE tests src)
target_compile_options(gridline_tests_k58 PRIVATE ${GL_WARN})
add_test(NAME k58_fl_resampled_clips_under_tempo_automation COMMAND gridline_tests_k58 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k58_fl_resampled_clips_under_tempo_automation PROPERTIES TIMEOUT 300)

add_executable(gridline_tests_k62 tests/test_k62.cpp)
target_link_libraries(gridline_tests_k62 PRIVATE gridline_core)
target_include_directories(gridline_tests_k62 PRIVATE tests src)
target_compile_options(gridline_tests_k62 PRIVATE ${GL_WARN})
add_test(NAME k62_convolver_against_fl COMMAND gridline_tests_k62 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k62_convolver_against_fl PROPERTIES TIMEOUT 300)

add_executable(gridline_tests_k63 tests/test_k63.cpp)
target_link_libraries(gridline_tests_k63 PRIVATE gridline_core)
target_include_directories(gridline_tests_k63 PRIVATE tests src)
target_compile_options(gridline_tests_k63 PRIVATE ${GL_WARN})
add_test(NAME k63_fl_sampler_note_level COMMAND gridline_tests_k63 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k63_fl_sampler_note_level PROPERTIES TIMEOUT 300)

# K-87: freeze and hibernate: null tests (frozen = live), plugins unloaded, exact unfreeze,
# out-of-date detection, save / load / zip, hibernate and wake, undo.
add_executable(gridline_tests_freeze tests/test_freeze.cpp)
target_link_libraries(gridline_tests_freeze PRIVATE gridline_core)
target_include_directories(gridline_tests_freeze PRIVATE tests src)
target_compile_options(gridline_tests_freeze PRIVATE ${GL_WARN})
add_test(NAME k87_freeze COMMAND gridline_tests_freeze WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k87_freeze PROPERTIES TIMEOUT 600)

add_executable(gridline_tests_k64 tests/test_k64.cpp)
target_link_libraries(gridline_tests_k64 PRIVATE gridline_core)
target_include_directories(gridline_tests_k64 PRIVATE tests src)
target_compile_options(gridline_tests_k64 PRIVATE ${GL_WARN})
add_test(NAME k64_smooth_playback_and_solo COMMAND gridline_tests_k64 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k64_smooth_playback_and_solo PROPERTIES TIMEOUT 300)

# K-50: the pitch of FL audio clips: times beyond the sampler's range, sources far above the engine's rate.
add_executable(gridline_tests_k50 tests/test_k50.cpp)
target_link_libraries(gridline_tests_k50 PRIVATE gridline_core)
target_include_directories(gridline_tests_k50 PRIVATE tests src)
target_compile_options(gridline_tests_k50 PRIVATE ${GL_WARN})
add_test(NAME k50_fl_clip_pitch COMMAND gridline_tests_k50 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k50_fl_clip_pitch PROPERTIES TIMEOUT 300)

add_executable(gridline_tests_k47 tests/test_k47.cpp)
target_link_libraries(gridline_tests_k47 PRIVATE gridline_core)
target_include_directories(gridline_tests_k47 PRIVATE tests src)
target_compile_options(gridline_tests_k47 PRIVATE ${GL_WARN})
add_test(NAME k47_exports_and_clipboard COMMAND gridline_tests_k47 ${CMAKE_BINARY_DIR}/k47_units WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k47_exports_and_clipboard PROPERTIES TIMEOUT 180)
# K-133: project bones over an earlier export replace nothing until all is written; the render
# worker's sign of life; the sound fixes measured against the reference renders.
add_executable(gridline_tests_k133 tests/test_k133.cpp)
target_link_libraries(gridline_tests_k133 PRIVATE gridline_core)
target_include_directories(gridline_tests_k133 PRIVATE tests src)
target_compile_options(gridline_tests_k133 PRIVATE ${GL_WARN})
add_test(NAME k133_recovery_and_sound COMMAND gridline_tests_k133 ${CMAKE_BINARY_DIR}/k133_units WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(k133_recovery_and_sound PROPERTIES TIMEOUT 300)
# K-75: Chromebook / Android terminal Linux environments (detection, audio defaults), GLSL for OpenGL 2.x.
add_executable(gridline_tests_k75 tests/test_k75.cpp)
target_link_libraries(gridline_tests_k75 PRIVATE gridline_core)
target_include_directories(gridline_tests_k75 PRIVATE tests src)
target_compile_options(gridline_tests_k75 PRIVATE ${GL_WARN})
add_test(NAME k75_chromeos COMMAND gridline_tests_k75 ${CMAKE_BINARY_DIR}/k75_units WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# K-39: crash reports. The probe has debug information of its own: the symbol test needs no debug build of the app.
add_executable(gridline_tests_k39 tests/test_k39.cpp)
target_link_libraries(gridline_tests_k39 PRIVATE gridline_core)
target_include_directories(gridline_tests_k39 PRIVATE tests src)
target_compile_options(gridline_tests_k39 PRIVATE ${GL_WARN})
add_test(NAME k39_crash_units COMMAND gridline_tests_k39 ${CMAKE_BINARY_DIR}/k39_units WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_crash_probe tests/crash_probe.cpp)
target_link_libraries(gridline_crash_probe PRIVATE gridline_core)
target_include_directories(gridline_crash_probe PRIVATE tests src)
target_compile_options(gridline_crash_probe PRIVATE ${GL_WARN} $<IF:$<CXX_COMPILER_ID:MSVC>,/Z7,-g>)
if(MSVC)
  target_link_options(gridline_crash_probe PRIVATE /DEBUG)
endif()
find_program(GRIDLINE_PYTHON3 NAMES python3 python)
if(GRIDLINE_PYTHON3)
  add_test(NAME k39_crash_symbols_and_false_alarms
           COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:gridline_crash_probe> -DTOOL=${CMAKE_SOURCE_DIR}/tools/crash_symbolize.py -DPYTHON=${GRIDLINE_PYTHON3}
                   -DOUT=${CMAKE_BINARY_DIR}/k39_probe -P ${CMAKE_SOURCE_DIR}/tests/k39_crash_probe.cmake
           WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(k39_crash_symbols_and_false_alarms PROPERTIES TIMEOUT 200)
endif()
add_executable(gridline_tests_k29 tests/test_k29.cpp)
target_link_libraries(gridline_tests_k29 PRIVATE gridline_core)
target_include_directories(gridline_tests_k29 PRIVATE tests src)
target_compile_options(gridline_tests_k29 PRIVATE ${GL_WARN})
add_test(NAME k29_bend_shapes_played COMMAND gridline_tests_k29 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_clip_sources tests/test_clip_sources.cpp)
target_link_libraries(gridline_tests_clip_sources PRIVATE gridline_core)
target_include_directories(gridline_tests_clip_sources PRIVATE tests src)
target_compile_options(gridline_tests_clip_sources PRIVATE ${GL_WARN})
add_test(NAME clip_sources COMMAND gridline_tests_clip_sources WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-173: automation clips inside patterns (model, playback, undo, files, FL / MIDI, collaboration).
add_executable(gridline_tests_pattern_automation_clips tests/test_pattern_automation_clips.cpp)
target_link_libraries(gridline_tests_pattern_automation_clips PRIVATE gridline_core)
target_include_directories(gridline_tests_pattern_automation_clips PRIVATE tests src)
target_compile_options(gridline_tests_pattern_automation_clips PRIVATE ${GL_WARN})
add_test(NAME pattern_automation_clips COMMAND gridline_tests_pattern_automation_clips WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# J-56: playlist track modes (PL-08), MTC / clock receive and chase (MI-02), master chain + loudness (RN-09).
add_executable(gridline_tests_chop tests/test_chop.cpp)
target_link_libraries(gridline_tests_chop PRIVATE gridline_core)
target_include_directories(gridline_tests_chop PRIVATE tests)
target_compile_options(gridline_tests_chop PRIVATE ${GL_WARN})
add_test(NAME chop COMMAND gridline_tests_chop WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Q-1 / Q-3: looping pattern clips, Shift+drag copies (core side).
add_executable(gridline_tests_q1 tests/test_q1_loops.cpp)
target_link_libraries(gridline_tests_q1 PRIVATE gridline_core)
target_include_directories(gridline_tests_q1 PRIVATE tests)
target_compile_options(gridline_tests_q1 PRIVATE ${GL_WARN})
add_test(NAME q1_loops COMMAND gridline_tests_q1 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-114: clips loop only when asked (per clip: pattern, audio, automation); old projects and FL keep playing as before.
add_executable(gridline_tests_k114 tests/test_k114.cpp)
target_link_libraries(gridline_tests_k114 PRIVATE gridline_core)
target_include_directories(gridline_tests_k114 PRIVATE tests)
target_compile_options(gridline_tests_k114 PRIVATE ${GL_WARN})
add_test(NAME k114_clip_loops COMMAND gridline_tests_k114 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-140: a cut clip loops from its cut point (the part it showed repeats).
add_executable(gridline_tests_k140 tests/test_k140.cpp)
target_link_libraries(gridline_tests_k140 PRIVATE gridline_core)
target_include_directories(gridline_tests_k140 PRIVATE tests)
target_compile_options(gridline_tests_k140 PRIVATE ${GL_WARN})
add_test(NAME k140_cut_loops COMMAND gridline_tests_k140 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# K-85: sampler start / end handles trim audio clips; warp markers.
add_executable(gridline_tests_k85 tests/test_k85.cpp)
target_link_libraries(gridline_tests_k85 PRIVATE gridline_core)
target_include_directories(gridline_tests_k85 PRIVATE tests src)
target_compile_options(gridline_tests_k85 PRIVATE ${GL_WARN})
add_test(NAME k85_clip_trim_warp COMMAND gridline_tests_k85 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_j56 tests/test_j56.cpp)
target_link_libraries(gridline_tests_j56 PRIVATE gridline_core)
target_include_directories(gridline_tests_j56 PRIVATE tests)
target_compile_options(gridline_tests_j56 PRIVATE ${GL_WARN})
add_test(NAME j56 COMMAND gridline_tests_j56 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# J-21 QA: mutation fuzzer over the file readers (tests/fuzz/fuzz_main.cpp; long runs by hand,
# a short smoke run per reader under ctest) and the cross-feature regression tests.
add_executable(gridline_fuzz tests/fuzz/fuzz_main.cpp)
target_link_libraries(gridline_fuzz PRIVATE gridline_core)
target_compile_options(gridline_fuzz PRIVATE ${GL_WARN})
add_executable(gridline_tests_qa tests/test_qa.cpp)
target_link_libraries(gridline_tests_qa PRIVATE gridline_core)
target_include_directories(gridline_tests_qa PRIVATE tests)
target_compile_options(gridline_tests_qa PRIVATE ${GL_WARN})
add_test(NAME qa COMMAND gridline_tests_qa WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(gridline_tests_qa_cross tests/test_qa_cross.cpp)
target_link_libraries(gridline_tests_qa_cross PRIVATE gridline_core)
target_include_directories(gridline_tests_qa_cross PRIVATE tests)
target_compile_options(gridline_tests_qa_cross PRIVATE ${GL_WARN})
add_test(NAME qa_cross COMMAND gridline_tests_qa_cross WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
foreach(ft flp flp-export fst project sample midi browser-index zip lua lua-project lua-audio song-flp fx-preset)
  add_test(NAME fuzz_${ft} COMMAND gridline_fuzz ${ft} 3 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()

# One test executable per pack: tests/packs/test_<pack>.cpp
file(GLOB GRIDLINE_PACK_TESTS CONFIGURE_DEPENDS tests/packs/test_*.cpp)
foreach(t ${GRIDLINE_PACK_TESTS})
  get_filename_component(tn ${t} NAME_WE)
  add_executable(gridline_${tn} ${t})
  target_link_libraries(gridline_${tn} PRIVATE gridline_core)
  add_test(NAME ${tn} COMMAND gridline_${tn} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# ---- desktop application (SDL2 + OpenGL 3 + Dear ImGui) ----
# GRIDLINE_BUNDLED_SDL2=ON builds SDL2 from source as a static library (Windows, the macOS
# universal2 release, any system without SDL2 development files). Otherwise the system SDL2.
option(GRIDLINE_BUNDLED_SDL2 "Build SDL2 from source (static) instead of using the system SDL2" OFF)
if(GRIDLINE_BUNDLED_SDL2)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
  if(MSVC)
    set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4)
  FetchContent_MakeAvailable(SDL2)
  set(SDL2_FOUND TRUE)
  if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2-static)
  endif()
endif()
if(NOT SDL2_FOUND)
  find_package(SDL2 QUIET)
endif()
if(NOT SDL2_FOUND)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
  endif()
endif()
find_package(OpenGL QUIET)

if((SDL2_FOUND OR SDL2PC_FOUND) AND OPENGL_FOUND)
  # Dear ImGui core + OpenGL 3 renderer (no window system: the plugin build, PH-09),
  # and the SDL2 platform backend on top for the application.
  add_library(imgui_core STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_demo.cpp third_party/imgui/imgui_impl_opengl3.cpp
    src/app/imgui_hooks/ImGuiItemHook.cpp)
  # J-92: ImGui's item hooks (off unless the editor audit turns them on: one branch per item).
  target_compile_definitions(imgui_core PUBLIC IMGUI_ENABLE_TEST_ENGINE)
  # K-24: 32-bit draw indices. A busy window (the playlist with many clips and automation lanes) can
  # pass 65535 vertices; with 16-bit indices that relies on draw-command vertex offsets, which the
  # OpenGL renderer only honours on GL 3.2+. Some Windows drivers give a 3.0/3.1 context, and the
  # indices then wrapped (blank ruler and track headers, streaks across the playlist).
  target_compile_definitions(imgui_core PUBLIC "ImDrawIdx=unsigned int")
  target_include_directories(imgui_core SYSTEM PUBLIC third_party/imgui)
  target_link_libraries(imgui_core PUBLIC ${CMAKE_DL_LIBS})
  set_target_properties(imgui_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # SDL2 headers only (the interface sources include SDL_opengl.h for GL declarations).
  add_library(gridline_sdl_headers INTERFACE)
  if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
      target_include_directories(gridline_sdl_headers SYSTEM INTERFACE $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
    else()
      target_include_directories(gridline_sdl_headers SYSTEM INTERFACE ${SDL2_INCLUDE_DIRS})
    endif()
  else()
    target_include_directories(gridline_sdl_headers SYSTEM INTERFACE $<TARGET_PROPERTY:PkgConfig::SDL2PC,INTERFACE_INCLUDE_DIRECTORIES>)
  endif()
  add_library(imgui STATIC third_party/imgui/imgui_impl_sdl2.cpp)
  target_link_libraries(imgui PUBLIC imgui_core)
  if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
      target_link_libraries(imgui PUBLIC SDL2::SDL2)
    else()
      target_include_directories(imgui SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})
      target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES})
    endif()
  else()
    target_link_libraries(imgui PUBLIC PkgConfig::SDL2PC)
  endif()
  target_link_libraries(imgui PUBLIC OpenGL::GL ${CMAKE_DL_LIBS})

  # MIDI I/O (RtMidi): CoreMIDI on macOS, ALSA (+JACK if present) on Linux.
  add_library(rtmidi STATIC third_party/rtmidi/RtMidi.cpp)
  set_target_properties(rtmidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(rtmidi SYSTEM PUBLIC third_party/rtmidi)
  target_compile_options(rtmidi PRIVATE ${GL_NOWARN})  # vendored code
  if(APPLE)
    target_compile_definitions(rtmidi PUBLIC __MACOSX_CORE__)
    target_link_libraries(rtmidi PUBLIC "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
  elseif(WIN32)
    target_compile_definitions(rtmidi PUBLIC __WINDOWS_MM__)
    target_link_libraries(rtmidi PUBLIC winmm)
  elseif(CMAKE_SYSTEM_NAME MATCHES "BSD|DragonFly")
    # RtMidi has no BSD back end: sndio (src/app/MidiBackend.cpp) behind RtMidi's dummy API.
    find_path(GRIDLINE_SNDIO_INCLUDE sndio.h PATHS /usr/local/include /usr/pkg/include)
    find_library(GRIDLINE_SNDIO_LIB sndio PATHS /usr/local/lib /usr/pkg/lib)
    target_compile_definitions(rtmidi PUBLIC __RTMIDI_DUMMY__)
    if(GRIDLINE_SNDIO_INCLUDE AND GRIDLINE_SNDIO_LIB)
      target_compile_definitions(rtmidi PUBLIC GRIDLINE_MIDI_SNDIO)
      target_include_directories(rtmidi SYSTEM PUBLIC ${GRIDLINE_SNDIO_INCLUDE})
      target_link_libraries(rtmidi PUBLIC ${GRIDLINE_SNDIO_LIB} Threads::Threads)
      message(STATUS "MIDI devices: sndio")
    else()
      message(WARNING "sndio not found: MIDI devices disabled (install the sndio package)")
    endif()
  elseif(UNIX)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
      pkg_check_modules(JACK QUIET IMPORTED_TARGET jack)
    endif()
    if(ALSA_FOUND)
      target_compile_definitions(rtmidi PUBLIC __LINUX_ALSA__)
      target_link_libraries(rtmidi PUBLIC PkgConfig::ALSA Threads::Threads)
    endif()
    if(JACK_FOUND)
      target_compile_definitions(rtmidi PUBLIC __UNIX_JACK__)
      target_link_libraries(rtmidi PUBLIC PkgConfig::JACK)
    endif()
    if(NOT ALSA_FOUND AND NOT JACK_FOUND)
      message(WARNING "Neither ALSA nor JACK development files found: MIDI devices disabled (install libasound2-dev)")
      target_compile_definitions(rtmidi PUBLIC __RTMIDI_DUMMY__)
    endif()
  endif()

  # Q-13: interface icons. assets/icons/ui/*.svg (Lucide ISC / Tabler MIT geometry) are embedded at
  # build time and rasterised by nanosvg (zlib, vendored unmodified in third_party/nanosvg) into an
  # alpha atlas at the exact device size (src/app/icons).
  file(GLOB GRIDLINE_UI_SVGS CONFIGURE_DEPENDS assets/icons/ui/*.svg)
  set(GRIDLINE_ICON_DATA ${CMAKE_BINARY_DIR}/generated/IconData.cpp)
  add_custom_command(OUTPUT ${GRIDLINE_ICON_DATA}
    COMMAND ${CMAKE_COMMAND} -DICON_DIR=${CMAKE_SOURCE_DIR}/assets/icons/ui -DOUT=${GRIDLINE_ICON_DATA}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedIcons.cmake
    DEPENDS ${GRIDLINE_UI_SVGS} ${CMAKE_SOURCE_DIR}/cmake/EmbedIcons.cmake
    COMMENT "Embedding interface icons")
  # J-130: plugin wordmarks (assets/wordmarks/*.svg, made by tools/wordmarks/make_wordmarks.py),
  # embedded the same way and rasterised per colour role at the exact device height.
  file(GLOB GRIDLINE_WORDMARK_SVGS CONFIGURE_DEPENDS assets/wordmarks/*.svg)
  set(GRIDLINE_WORDMARK_DATA ${CMAKE_BINARY_DIR}/generated/WordmarkData.cpp)
  add_custom_command(OUTPUT ${GRIDLINE_WORDMARK_DATA}
    COMMAND ${CMAKE_COMMAND} -DWORDMARK_DIR=${CMAKE_SOURCE_DIR}/assets/wordmarks -DOUT=${GRIDLINE_WORDMARK_DATA}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedWordmarks.cmake
    DEPENDS ${GRIDLINE_WORDMARK_SVGS} ${CMAKE_SOURCE_DIR}/cmake/EmbedWordmarks.cmake
    COMMENT "Embedding plugin wordmarks")
  add_library(gridline_nanosvg OBJECT src/app/icons/NanoSvgImpl.cpp)
  target_compile_options(gridline_nanosvg PRIVATE ${GL_NOWARN})  # vendored code
  set_target_properties(gridline_nanosvg PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(gridline_icons OBJECT src/app/icons/IconAtlas.cpp src/app/icons/IconTexture.cpp src/app/icons/IconNames.cpp ${GRIDLINE_ICON_DATA}
              src/app/icons/Wordmark.cpp ${GRIDLINE_WORDMARK_DATA})
  target_include_directories(gridline_icons PRIVATE src/app/icons)
  target_link_libraries(gridline_icons PRIVATE imgui_core)
  target_compile_options(gridline_icons PRIVATE ${GL_WARN})
  set_target_properties(gridline_icons PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(GRIDLINE_ICON_OBJS $<TARGET_OBJECTS:gridline_icons> $<TARGET_OBJECTS:gridline_nanosvg>)
  # Q-13 tests: the atlas builds, every icon has its SVG, re-rasterisation on a scale change only.
  add_executable(gridline_tests_icons tests/test_icons.cpp ${GRIDLINE_ICON_OBJS})
  target_link_libraries(gridline_tests_icons PRIVATE imgui_core)
  target_include_directories(gridline_tests_icons PRIVATE src tests)
  target_compile_options(gridline_tests_icons PRIVATE ${GL_WARN})
  add_test(NAME q13_icons COMMAND gridline_tests_icons WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # K-24: busy windows past 65535 vertices draw intact without renderer vertex offsets (GL 3.0/3.1).
  add_executable(gridline_tests_draw_index tests/test_draw_index.cpp)
  target_link_libraries(gridline_tests_draw_index PRIVATE imgui_core)
  target_include_directories(gridline_tests_draw_index PRIVATE src tests)
  target_compile_options(gridline_tests_draw_index PRIVATE ${GL_WARN})
  add_test(NAME k24_draw_index COMMAND gridline_tests_draw_index)
  # J-130: every catalogue plugin has a wordmark that parses, rasterises and carries its name.
  add_executable(gridline_tests_wordmarks tests/test_wordmarks.cpp ${GRIDLINE_ICON_OBJS})
  target_link_libraries(gridline_tests_wordmarks PRIVATE imgui_core gridline_core)
  target_include_directories(gridline_tests_wordmarks PRIVATE src tests)
  target_compile_options(gridline_tests_wordmarks PRIVATE ${GL_WARN})
  add_test(NAME j130_wordmarks COMMAND gridline_tests_wordmarks WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # J-54: dialog layout checks, headless Dear ImGui (no window, no renderer).
  add_executable(gridline_tests_dialog_layout tests/test_dialog_layout.cpp src/app/DialogLayout.cpp src/app/Theme.cpp src/core/SmoothScroll.cpp)
  target_link_libraries(gridline_tests_dialog_layout PRIVATE imgui)
  target_include_directories(gridline_tests_dialog_layout PRIVATE src tests)
  target_compile_options(gridline_tests_dialog_layout PRIVATE ${GL_WARN})
  add_test(NAME dialog_layout COMMAND gridline_tests_dialog_layout WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # J-55: theme page settings (mode, accent / role colours, contrast, brightness), headless.
  add_executable(gridline_tests_theme_settings tests/test_theme_settings.cpp src/app/Theme.cpp src/app/ThemeSettings.cpp)
  target_link_libraries(gridline_tests_theme_settings PRIVATE imgui gridline_core)
  target_include_directories(gridline_tests_theme_settings PRIVATE src tests)
  target_compile_options(gridline_tests_theme_settings PRIVATE ${GL_WARN})
  add_test(NAME theme_settings COMMAND gridline_tests_theme_settings WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  # J-137: the Theme page's control style cards fit, wrap and keep their previews whole, headless.
  add_executable(gridline_tests_theme_layout tests/test_theme_layout.cpp)
  target_include_directories(gridline_tests_theme_layout PRIVATE src tests)
  target_compile_options(gridline_tests_theme_layout PRIVATE ${GL_WARN})
  add_test(NAME j137_theme_layout COMMAND gridline_tests_theme_layout WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  # J-139: faders and sliders centred on their track in every control style, headless.
  add_executable(gridline_tests_fader_centre tests/test_fader_centre.cpp)
  target_include_directories(gridline_tests_fader_centre PRIVATE src tests)
  target_compile_options(gridline_tests_fader_centre PRIVATE ${GL_WARN})
  add_test(NAME j139_fader_centre COMMAND gridline_tests_fader_centre WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  # The interface (every window and editor) is compiled once and used by the
  # application and by the plugin build (PH-09). main.cpp and PluginWindow.cpp
  # (SDL windows), Viewports.cpp (J-113: detached windows) and LiveResize.cpp (J-116) belong to the application only.
  file(GLOB GRIDLINE_APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/packs/RegistryUi.cpp src/packs/*/*_ui.cpp)
  list(REMOVE_ITEM GRIDLINE_APP_SRC ${CMAKE_SOURCE_DIR}/src/app/main.cpp ${CMAKE_SOURCE_DIR}/src/app/PluginWindow.cpp
       ${CMAKE_SOURCE_DIR}/src/app/Viewports.cpp ${CMAKE_SOURCE_DIR}/src/app/LiveResize.cpp
       ${CMAKE_SOURCE_DIR}/src/app/AppIcon.cpp ${CMAKE_SOURCE_DIR}/src/app/AppIconImage.cpp  # K-14: the program only
       ${CMAKE_SOURCE_DIR}/src/app/GlRecovery.cpp ${CMAKE_SOURCE_DIR}/src/app/VideoDriver.cpp  # K-47: the program's own OpenGL context
       ${CMAKE_SOURCE_DIR}/src/app/OpenDocuments.cpp)  # K-119: the program's SDL events
  add_library(gridline_ui OBJECT ${GRIDLINE_APP_SRC})
  target_link_libraries(gridline_ui PRIVATE gridline_core imgui_core rtmidi gridline_sdl_headers)
  target_compile_options(gridline_ui PRIVATE ${GL_WARN_LOOSE})
  set_target_properties(gridline_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # K-14: the application icon built into the program (every window's icon, AppIcon.h).
  set(GRIDLINE_APP_ICON_DATA ${CMAKE_BINARY_DIR}/generated/AppIconData.cpp)
  add_custom_command(OUTPUT ${GRIDLINE_APP_ICON_DATA}
    COMMAND ${CMAKE_COMMAND} -DICON_DIR=${CMAKE_SOURCE_DIR}/assets/icons -DSIZES=256,512 -DOUT=${GRIDLINE_APP_ICON_DATA}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedAppIcon.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/icons/gridline-app-256.png ${CMAKE_SOURCE_DIR}/assets/icons/gridline-app-512.png
            ${CMAKE_SOURCE_DIR}/cmake/EmbedAppIcon.cmake
    COMMENT "Embedding the application icon")
  add_library(gridline_appicon OBJECT src/app/AppIconImage.cpp ${GRIDLINE_APP_ICON_DATA})
  target_include_directories(gridline_appicon PRIVATE src/app)
  target_compile_options(gridline_appicon PRIVATE ${GL_WARN})
  add_executable(gridline_tests_app_icon tests/test_app_icon.cpp $<TARGET_OBJECTS:gridline_appicon>)
  target_link_libraries(gridline_tests_app_icon PRIVATE gridline_stbimage)
  target_include_directories(gridline_tests_app_icon PRIVATE src tests)
  target_compile_options(gridline_tests_app_icon PRIVATE ${GL_WARN})
  add_test(NAME k14_app_icon COMMAND gridline_tests_app_icon WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_executable(gridline-app src/app/main.cpp src/app/PluginWindow.cpp src/app/Viewports.cpp src/app/LiveResize.cpp src/app/GlRecovery.cpp src/app/VideoDriver.cpp src/app/OpenDocuments.cpp $<TARGET_OBJECTS:gridline_ui> ${GRIDLINE_ICON_OBJS}
                              src/app/AppIcon.cpp $<TARGET_OBJECTS:gridline_appicon>)
  target_link_libraries(gridline-app PRIVATE gridline_core imgui rtmidi)
  if(APPLE)
    target_link_libraries(gridline-app PRIVATE objc)  # plugin editor windows (NSView of an SDL window)
  endif()
  target_compile_options(gridline-app PRIVATE ${GL_WARN_LOOSE})
  # Run from the build tree: assets are found next to the source tree.
  if(GRIDLINE_DEV_ASSETS)
    target_compile_definitions(gridline-app PRIVATE GRIDLINE_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets")
  endif()

  # App icon (tools/make_app_icon.py): Gridline.icns for the macOS bundle, gridline-app-<size>.png
  # for Linux, gridline-app-256.bmp for the SDL window / dock icon (loaded at startup).
  if(APPLE)
    # build/Gridline.app: a launchable bundle (Finder / Dock icon) whose executable is a hard
    # link to build/gridline-app, refreshed after every link. Assets resolve to the source tree.
    set(GRIDLINE_APP_BUNDLE ${CMAKE_BINARY_DIR}/Gridline.app)
    file(MAKE_DIRECTORY ${GRIDLINE_APP_BUNDLE}/Contents/MacOS ${GRIDLINE_APP_BUNDLE}/Contents/Resources)
    # K-119: the plist holds the file types Gridline opens (cmake/macos/Info.plist.in).
    configure_file(${CMAKE_SOURCE_DIR}/cmake/macos/Info.plist.in ${GRIDLINE_APP_BUNDLE}/Contents/Info.plist @ONLY)
    add_custom_command(TARGET gridline-app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/icons/Gridline.icns
              ${GRIDLINE_APP_BUNDLE}/Contents/Resources/Gridline.icns
      COMMAND ${CMAKE_COMMAND} -E rm -f ${GRIDLINE_APP_BUNDLE}/Contents/MacOS/gridline-app
      COMMAND ${CMAKE_COMMAND} -E create_hardlink $<TARGET_FILE:gridline-app> ${GRIDLINE_APP_BUNDLE}/Contents/MacOS/gridline-app
      COMMAND touch ${GRIDLINE_APP_BUNDLE}
      COMMENT "Assembling Gridline.app")
  endif()

  if(GRIDLINE_GUI_TESTS)
    # K-157 (GitHub issue #33): the main window is an ordinary top-level window. On X11 the test
    # runs its own Xvfb and a minimal window manager that decides floating as tiling managers do.
    add_executable(gridline_tests_k157 tests/test_k157.cpp)
    target_link_libraries(gridline_tests_k157 PRIVATE imgui Threads::Threads)
    target_include_directories(gridline_tests_k157 PRIVATE src tests)
    target_compile_options(gridline_tests_k157 PRIVATE ${GL_WARN})
    if(NOT APPLE AND NOT WIN32)
      find_package(X11 QUIET)
      if(X11_FOUND)
        target_compile_definitions(gridline_tests_k157 PRIVATE GRIDLINE_K157_X11=1)
        target_link_libraries(gridline_tests_k157 PRIVATE X11::X11)
      endif()
    endif()
    add_test(NAME k157_window_hints COMMAND gridline_tests_k157)
    set_tests_properties(k157_window_hints PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

    # Q-7: every built-in editor at widths x1 / x1.5 / x2, no control within
    # inset::kMinInset px of its panel's edge (off-screen run; isolated config).
    # Hidden window: the desktop cursor cannot hover a control and show its readout.
    add_test(NAME editor_layout_audit
             COMMAND gridline-app --hidden --layout-audit ${CMAKE_BINARY_DIR}/layout_audit --size 2400x1600
                     --screenshot ${CMAKE_BINARY_DIR}/layout_audit/frame.ppm --frames 100000 --no-ui-sounds
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # J-103: a failing exit fails the test (ctest checks the code); an abort message on the way out does too.
    # J-128: also fails on alignment findings above tests/alignment_baseline.tsv that
    # tests/alignment_allowlist.tsv does not justify (AlignCheck.h gate()).
    set_tests_properties(editor_layout_audit PROPERTIES ENVIRONMENT
                         "GRIDLINE_CONFIG_HOME=${CMAKE_BINARY_DIR}/layout_audit_cfg;GRIDLINE_ALIGN_BASELINE=${CMAKE_SOURCE_DIR}/tests/alignment_baseline.tsv;GRIDLINE_ALIGN_ALLOW=${CMAKE_SOURCE_DIR}/tests/alignment_allowlist.tsv"
                         TIMEOUT 1800  # about 630 s on an x86_64 CI runner; software-rendered arm64 takes longer
                         FAIL_REGULAR_EXPRESSION "terminating due to|mutex lock failed|libc\\+\\+abi")

    # J-129: every built-in editor renders in every control style (thumbnail run, hidden, isolated config).
    foreach(style classic slate graphite hairline lumen)
      add_test(NAME j129_style_${style}
               COMMAND gridline-app --hidden --no-ui-sounds --no-update-check --look ${style} --make-thumbs --size 1600x1100
                       --screenshot ${CMAKE_BINARY_DIR}/j129_style_${style}/frame.ppm --frames 100000
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j129_style_${style} PROPERTIES
                           ENVIRONMENT "GRIDLINE_CONFIG_HOME=${CMAKE_BINARY_DIR}/j129_style_${style};GRIDLINE_NO_PLUGIN_SCAN=1;GRIDLINE_HIDE_PLUGINS=all"
                           TIMEOUT 600 PASS_REGULAR_EXPRESSION "thumbnails: [1-9][0-9]* written"
                           FAIL_REGULAR_EXPRESSION "Assertion|assert|terminating due to|libc\\+\\+abi")
    endforeach()

    # J-78: clicks on a notification card never reach the playlist clip below it (hidden off-screen run).
    add_test(NAME j78_toast_click
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j78_toast_click -P ${CMAKE_SOURCE_DIR}/tests/j78_toast_click.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j78_toast_click PROPERTIES TIMEOUT 300)

    # J-91: the transport buttons answer real (SDL) clicks at 100 / 150 / 200 % and the metronome swings.
    add_test(NAME j91_transport_click
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j91_transport_click
                     -P ${CMAKE_SOURCE_DIR}/tests/j91_transport_click.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j91_transport_click PROPERTIES TIMEOUT 300)
    # J-216: automated and dev-layout runs never write the user's saved layout.
    add_test(NAME j216_layout_untouched
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j216_layout_untouched
                     -P ${CMAKE_SOURCE_DIR}/tests/j216_layout_untouched.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j216_layout_untouched PROPERTIES TIMEOUT 300)
    # J-212: Stop clicked with the transport stopped (or a double-click) goes back to the beginning.
    add_test(NAME j212_stop_to_start
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j212_stop_to_start
                     -P ${CMAKE_SOURCE_DIR}/tests/j212_stop_to_start.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j212_stop_to_start PROPERTIES TIMEOUT 480)
    # J-213: replacing the instrument of an open channel renames it and resets the plugin view's preset.
    add_test(NAME j213_replace_instrument_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j213_replace_instrument
                     -P ${CMAKE_SOURCE_DIR}/tests/j213_replace_instrument.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j213_replace_instrument_app PROPERTIES TIMEOUT 240)
    # K-124 to K-127: Esc back to the playlist, a new instrument opens its editor, the rack LED's
    # right-click solo and the activity light before the steps (geometry at 100% and 200%).
    add_test(NAME k124_rack_and_keys_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k124_rack_and_keys
                     -P ${CMAKE_SOURCE_DIR}/tests/k124_rack_and_keys.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k124_rack_and_keys_app PROPERTIES TIMEOUT 600)

    # J-109: Stop releases preview keys, typed keys (also on focus loss) and the browser preview; Stop twice = panic.
    add_test(NAME j109_stop_sound_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DCLI=$<TARGET_FILE:gridline>
                     -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json -DOUT=${CMAKE_BINARY_DIR}/j109_stop_sound
                     -P ${CMAKE_SOURCE_DIR}/tests/j109_stop_sound.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j109_stop_sound_app PROPERTIES TIMEOUT 300)

    # J-83: wheel over a mixer gain fader: thin mixer scrolls, wide adjusts, Ctrl / click / momentum / live resize.
    add_test(NAME j83_mixer_wheel
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j83_mixer_wheel -P ${CMAKE_SOURCE_DIR}/tests/j83_mixer_wheel.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j83_mixer_wheel PROPERTIES TIMEOUT 300)

    # J-84: channel rack with long patterns / narrow racks: knobs drawn, steps scroll, Q-15 wheel routing.
    add_test(NAME j84_rack_scroll
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j84_rack_scroll -P ${CMAKE_SOURCE_DIR}/tests/j84_rack_scroll.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j84_rack_scroll PROPERTIES TIMEOUT 300)

    # K-4: auto scrolling switches: the playlist and the pattern editors (piano roll, channel rack) follow
    # playback only while theirs is on; manual scrolling while playing still works.
    add_test(NAME k4_follow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k4_follow -P ${CMAKE_SOURCE_DIR}/tests/k4_follow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k4_follow PROPERTIES TIMEOUT 300)

    # K-24: the playlist zoom anchored at the pointer on both axes: Alt+wheel keeps the track under the
    # pointer (top, middle, bottom, mixed heights, folded group), the scrollers' ends zoom with the other end
    # fixed, Ctrl+wheel keeps its tick, the piano roll's Alt+wheel keeps its row, the vertical zoom is remembered.
    add_test(NAME k24_zoom_anchor
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${GRIDLINE_DEMO_JSON}
                     -DSONG=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k24_zoom_anchor -P ${CMAKE_SOURCE_DIR}/tests/k24_zoom_anchor.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k24_zoom_anchor PROPERTIES TIMEOUT 600)

    # K-12: the playlist track "+": a click opens its menu; an existing channel, a new instrument (a favourite)
    # and a new pattern on the track (clip place, piano roll on the track's channel), one undo step each; the
    # View switch hides it and is remembered; at 24 px tracks and 200 % it overlaps none of the header's parts.
    add_test(NAME k12_track_plus
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k12_track_plus -P ${CMAKE_SOURCE_DIR}/tests/k12_track_plus.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k12_track_plus PROPERTIES TIMEOUT 600)

    # K-26: overlapping playlist clips: the clip beneath shows through the one on top, dimmed (pixels),
    # no shadow on it, grid lines one brightness through overlaps; a right-click there picks the top clip.
    add_test(NAME k26_clip_overlap
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k26_clip_overlap -P ${CMAKE_SOURCE_DIR}/tests/k26_clip_overlap.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k26_clip_overlap PROPERTIES TIMEOUT 600)

    # J-98: Tideline's Split slider keeps its handle under the pointer at 100 / 150 / 200 % (SDL drags).
    add_test(NAME j98_slider_follow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j98_slider_follow
                     -P ${CMAKE_SOURCE_DIR}/tests/j98_slider_follow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j98_slider_follow PROPERTIES TIMEOUT 300)

    # J-165: Tideline's trigger Source list: pick a channel (notes) and a mixer track (audio), undo.
    add_test(NAME j165_tideline_source
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j165_tideline_source
                     -P ${CMAKE_SOURCE_DIR}/tests/j165_tideline_source.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j165_tideline_source PROPERTIES TIMEOUT 300)

    # J-178: Plasma's preset library: folder clicks (any depth) filter the list, presets select / open.
    add_test(NAME j178_plasma_library
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j178_plasma_library
                     -P ${CMAKE_SOURCE_DIR}/tests/j178_plasma_library.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j178_plasma_library PROPERTIES TIMEOUT 300)

    # J-202: every page and view of Plasma's editor renders at 100 % and 150 % (no assert, no
    # overlapping text in Plasma's own layout check).
    add_test(NAME j202_plasma_pages
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j202_plasma_pages
                     -P ${CMAKE_SOURCE_DIR}/tests/j202_plasma_pages.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j202_plasma_pages PROPERTIES TIMEOUT 600)
    # K-170: Voltage In / Voltage Out editors (inset and overlap audit) and the calibration window.
    add_test(NAME k170_cv_editors_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/k170_cv_editors
                     -P ${CMAKE_SOURCE_DIR}/tests/k170_cv_editors_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k170_cv_editors_app PROPERTIES TIMEOUT 600)

    # K-42: a zipped project on a "web drive" in the real app: frames keep coming while it reads,
    # nothing lands on the drive, the second open needs no read, offline opens the last copy, the
    # recent list survives a drive that has gone.
    add_test(NAME k42_web_drive_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DSOUNDS=${CMAKE_SOURCE_DIR}/assets/sounds -DOUT=${CMAKE_BINARY_DIR}/k42_web_drive
                     -P ${CMAKE_SOURCE_DIR}/tests/k42_web_drive_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k42_web_drive_app PROPERTIES TIMEOUT 300)

    # K-45: every zip unpacks into the cache, a closed local project's copy goes, a drive's stays, the
    # startup sweep, and the Clear cache control in Settings > Files.
    add_test(NAME k45_cache_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DSOUNDS=${CMAKE_SOURCE_DIR}/assets/sounds -DOUT=${CMAKE_BINARY_DIR}/k45_cache
                     -P ${CMAKE_SOURCE_DIR}/tests/k45_cache_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k45_cache_app PROPERTIES TIMEOUT 300)

    # J-167: opening a slow project through File > Open: the dialog closes at once, frames keep
    # coming, the loading window shows progress and the log, input is blocked, Esc cancels back to
    # the open project exactly; a finished load keeps its warnings on a card; a corrupt file says why.
    add_test(NAME j167_project_load_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DSOUNDS=${CMAKE_SOURCE_DIR}/assets/sounds -DOUT=${CMAKE_BINARY_DIR}/j167_project_load
                     -P ${CMAKE_SOURCE_DIR}/tests/j167_project_load.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j167_project_load_app PROPERTIES TIMEOUT 300)

    # J-105: Convolver EQ / IR envelope point editing at 100 / 200 % (SDL drags): click adds one point, no painting, undo per gesture, save + load.
    add_test(NAME j105_conv_points
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j105_conv_points
                     -P ${CMAKE_SOURCE_DIR}/tests/j105_conv_points.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j105_conv_points PROPERTIES TIMEOUT 600)

    # J-97: browser / external drops onto channel rack rows replace their content (routing, steps, notes kept), insert between rows, undo.
    add_test(NAME j97_rack_drop
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j97_rack_drop -P ${CMAKE_SOURCE_DIR}/tests/j97_rack_drop.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j97_rack_drop PROPERTIES TIMEOUT 300)

    # J-173: automation clips in a pattern: the curves on the pattern's playlist clips (a
    # double-click opens one in the editor), the channel rack's clip rows, the view setting.
    add_test(NAME j173_pattern_auto_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j173_pattern_auto_app -P ${CMAKE_SOURCE_DIR}/tests/j173_pattern_auto_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j173_pattern_auto_app PROPERTIES TIMEOUT 300)
    # K-173: the separator between the rack's channel rows and its automation rows: place, gap,
    # caption, hits, a channel dropped on it, display groups, graph editor, themes, scale, looks, layouts.
    add_test(NAME k173_rack_separator_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k173_rack_separator -P ${CMAKE_SOURCE_DIR}/tests/k173_rack_separator_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k173_rack_separator_app PROPERTIES TIMEOUT 600)

    # K-172: the button language (primary / toggle on / pressed) in every control style and both
    # themes, read from the dev gallery's pixels.
    add_test(NAME k172_button_language_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/k172_button_language_app
                     -P ${CMAKE_SOURCE_DIR}/tests/k172_button_language_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k172_button_language_app PROPERTIES TIMEOUT 300)

    # K-171: the piano roll's automation lane: erase, box select + Delete, move, copy / paste, a linear
    # line, simplify, the resizable lane and its draw-curve chip.
    add_test(NAME k171_auto_lane_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k171_auto_lane_app -P ${CMAKE_SOURCE_DIR}/tests/k171_auto_lane_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k171_auto_lane_app PROPERTIES TIMEOUT 300)

    # K-0: "Make unique" from a playlist clip's menu on a pattern with piano roll notes keeps the app
    # running (a GitHub issue: the menu read the replaced song's pattern).
    add_test(NAME k0_make_unique_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k0_make_unique_app -P ${CMAKE_SOURCE_DIR}/tests/k0_make_unique_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k0_make_unique_app PROPERTIES TIMEOUT 300)

    # K-20: Shift+click in the piano roll draws a note and the drag sets its length.
    add_test(NAME k20_shift_draw
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k20_shift_draw -P ${CMAKE_SOURCE_DIR}/tests/k20_shift_draw.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k20_shift_draw PROPERTIES TIMEOUT 300)

    # K-21: a glissando on the piano roll's keyboard sounds and lights one key at a time.
    add_test(NAME k21_key_gliss
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k21_key_gliss -P ${CMAKE_SOURCE_DIR}/tests/k21_key_gliss.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k21_key_gliss PROPERTIES TIMEOUT 300)

    # K-25: the pitch bend tool: N turns it on, a drag bends one note, the line stays drawn (dimmed) on the pencil.
    add_test(NAME k25_pitch_bend_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k25_pitch_bend_app -P ${CMAKE_SOURCE_DIR}/tests/k25_pitch_bend_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k25_pitch_bend_app PROPERTIES TIMEOUT 300)
    # K-22: the hat's rack steps in the piano roll: looking changes nothing, a drag moves them into
    # the notes in one undo step, undo restores the rack row.
    add_test(NAME k22_steps_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k22_steps_app -P ${CMAKE_SOURCE_DIR}/tests/k22_steps_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k22_steps_app PROPERTIES TIMEOUT 300)
    # K-28: bend points follow the piano roll's snap (1/4 step, Line, None; Alt inverts), the curve ring, Alt+right-click.
    add_test(NAME k28_bend_snap_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k28_bend_snap_app -P ${CMAKE_SOURCE_DIR}/tests/k28_bend_snap_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k28_bend_snap_app PROPERTIES TIMEOUT 300)
    # K-39: every failure forced in the real app, judged by the report it wrote; recovery; the pop-up process.
    # (POSIX only: it expects signal names and the forked rescue copy. Windows has the units and the probe.)
    if(UNIX)
      add_test(NAME k39_crash_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                       -DOUT=${CMAKE_BINARY_DIR}/k39_crash_app -P ${CMAKE_SOURCE_DIR}/tests/k39_crash_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k39_crash_app PROPERTIES TIMEOUT 400)
    endif()
    # K-47: the exports off the interface thread, the clipboard and the cache, a lost OpenGL context.
    # K-133: on every platform (it needs no signals and no fork: the graphics report, the recovery
    # copy and the pop-up go the same way on Windows).
    add_test(NAME k47_exports_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DSOUNDS=${CMAKE_SOURCE_DIR}/assets/sounds -DOUT=${CMAKE_BINARY_DIR}/k47_exports -P ${CMAKE_SOURCE_DIR}/tests/k47_exports_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k47_exports_app PROPERTIES TIMEOUT 400)
    # K-133: a lost OpenGL context at the splash and in the crash pop-up; plugin interfaces opened
    # again after a reset; project bones over an earlier export; a render worker that stops moving on;
    # a Track Receipt that fails.
    add_test(NAME k133_recovery_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DCLAP=${GRIDLINE_CLAP_TESTPLUG} -DOUT=${CMAKE_BINARY_DIR}/k133_recovery_app -P ${CMAKE_SOURCE_DIR}/tests/k133_recovery_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k133_recovery_app PROPERTIES TIMEOUT 400)
    if(UNIX)
      # K-75: a Chromebook's Linux environment (audio defaults of a new installation, a saved choice
      # wins) and OpenGL capped at 2.1 / 3.0 under Mesa (virtual GPUs).
      add_test(NAME k75_chromeos_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/k75_chromeos_app
                       -P ${CMAKE_SOURCE_DIR}/tests/k75_chromeos_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k75_chromeos_app PROPERTIES TIMEOUT 300)
      # K-71: a Rescan of a slow folder over a large plugin cache never holds up a frame (POSIX: symlink loops).
      add_test(NAME k71_scan_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                       -DOUT=${CMAKE_BINARY_DIR}/k71_scan_app -P ${CMAKE_SOURCE_DIR}/tests/k71_scan_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k71_scan_app PROPERTIES TIMEOUT 300)
      # K-73: the first scan of 1836 fake plugins (VST3, VST2, CLAP; hung, crashed, slow, shells) with the plugin
      # manager and the browser's full list open: every frame stays short, the scan ends by itself.
      add_test(NAME k73_scan_stress_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DSCANNER=$<TARGET_FILE:gridline_fake_plugin_scanner>
                       -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json -DOUT=${CMAKE_BINARY_DIR}/k73_scan_stress_app
                       -P ${CMAKE_SOURCE_DIR}/tests/k73_scan_stress_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k73_scan_stress_app PROPERTIES TIMEOUT 900)
    endif()
    # K-48: the top bar's tap tempo button: real clicks, tempo follows, one undo step a run, a pause starts over, one tapper, every width.
    add_test(NAME k48_tap_tempo_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k48_tap_tempo_app -P ${CMAKE_SOURCE_DIR}/tests/k48_tap_tempo_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k48_tap_tempo_app PROPERTIES TIMEOUT 400)
    # K-29: double-click a point removes only it, a glide or its ring straightens, the note resets its line; one undo step each, chords too.
    add_test(NAME k29_bend_dblclick_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k29_bend_dblclick_app -P ${CMAKE_SOURCE_DIR}/tests/k29_bend_dblclick_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k29_bend_dblclick_app PROPERTIES TIMEOUT 300)
    # Script window: a slow drag on an integer knob moves it; a double-click goes back to the script's value.
    add_test(NAME script_knob_drag_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/script_knob_drag_app -P ${CMAKE_SOURCE_DIR}/tests/script_knob_drag_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(script_knob_drag_app PROPERTIES TIMEOUT 300)
    # Every knob of the piano roll tools and of one window of each other kind follows a slow drag, a fine drag and a double-click
    # (three parts that run side by side: the note tools, the step tools with the wheel / type-in / undo checks and the
    # channel pages, the other windows).
    foreach(part "tools|quantize,chop,strum,arp,flam,articulate,randomize,lfo,levels,limit,riff,props"
                 "pages|fill,randsteps,extra,chanmisc,chanenv" "windows|mixer,autolfo,plugin,rack")
      string(REPLACE "|" ";" part "${part}")
      list(GET part 0 tag)
      list(GET part 1 only)
      add_test(NAME knob_drag_${tag}_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                       -DOUT=${CMAKE_BINARY_DIR}/knob_drag_${tag}_app "-DONLY=${only}" -P ${CMAKE_SOURCE_DIR}/tests/knob_drag_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(knob_drag_${tag}_app PROPERTIES TIMEOUT 1800)
    endforeach()

    # The typing keyboard goes by where keys sit: other layouts play the same notes.
    add_test(NAME typing_key_positions_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/typing_key_positions_app -P ${CMAKE_SOURCE_DIR}/tests/typing_key_positions_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(typing_key_positions_app PROPERTIES TIMEOUT 120)
    # K-113: the typing keyboard's layout choice: Standard by default, names per layout, kept after a quit.
    add_test(NAME typing_layout_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/typing_layout_app -P ${CMAKE_SOURCE_DIR}/tests/typing_layout_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(typing_layout_app PROPERTIES TIMEOUT 300)

    # A click on an off-grid clip or note changes nothing; a drag moves it by whole grid steps, keeping its offset.
    add_test(NAME drag_keeps_offset_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/drag_keeps_offset_app -P ${CMAKE_SOURCE_DIR}/tests/drag_keeps_offset_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(drag_keeps_offset_app PROPERTIES TIMEOUT 240)

    # K-114 / K-82: a plain edge drag extends a clip, Ctrl (Cmd) + drag loops it (read live); a clip
    # dragged past the song's start lands on the beat.
    add_test(NAME k114_clip_loop_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k114_clip_loop_app -P ${CMAKE_SOURCE_DIR}/tests/k114_clip_loop_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k114_clip_loop_app PROPERTIES TIMEOUT 300)

    # K-140: Ctrl (Cmd) + edge drag of a cut clip loops the part it showed; back at the cut point it is
    # the clip it was; the loop badge shows only where a loop-key drag loops.
    add_test(NAME k140_cut_loop_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k140_cut_loop_app -P ${CMAKE_SOURCE_DIR}/tests/k140_cut_loop_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k140_cut_loop_app PROPERTIES TIMEOUT 300)

    # A right-click on overlapping notes deletes only the top one, held or with a slight slip.
    add_test(NAME piano_erase_overlap
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/piano_erase_overlap -P ${CMAKE_SOURCE_DIR}/tests/piano_erase_overlap.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(piano_erase_overlap PROPERTIES TIMEOUT 300)

    # The hovered key on the piano roll's keyboard has exactly the accent chosen in Settings.
    add_test(NAME piano_key_accent_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/piano_key_accent_app -P ${CMAKE_SOURCE_DIR}/tests/piano_key_accent_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(piano_key_accent_app PROPERTIES TIMEOUT 300)

    # J-113: detached windows kept across runs and recovered, Space and the typing keyboard from a
    # detached window, focus moving between Gridline's windows keeping held keys, J-97 drops into a
    # detached channel rack, dialogs over it (J-116: detaching, docking back, live resize: j116_detach_app).
    add_test(NAME j113_detach_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j113_detach -P ${CMAKE_SOURCE_DIR}/tests/j113_detach.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j113_detach_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # J-116 (replaces J-114's detach on drop): only the pop-out button detaches (a panel dragged past
    # the edge and released outside stays inside); moved / resized by the OS, never docked by being
    # moved (OS title bar or its own); docked back by the dock-back button, its OS close button and
    # "Bring all windows back"; live resize draws one frame per size step at the new size (SDL's
    # event watch from a blocked pump); fixed-size editors; the setting off.
    add_test(NAME j116_detach_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j116_detach -P ${CMAKE_SOURCE_DIR}/tests/j116_detach.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j116_detach_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # Plugin editors: Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y / Cmd+Z undo and redo knob drags, wheel turns and
    # drawn shapes (plugin data) as one step per gesture, in the main window and in a detached plugin window.
    # K-7: Junction's map in the real app: wiring, moving, deleting, zooming, undo per edit, both
    # themes, interface scales 100 % and 200 %.
    add_test(NAME k7_junction_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k7_junction_app -P ${CMAKE_SOURCE_DIR}/tests/k7_junction_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k7_junction_app PROPERTIES TIMEOUT 300)

    # K-7: an installed plugin inside a Junction: its own interface opens, an edit there comes
    # back as an edit of the song, undo restores it in the plugin (the in-tree test plugin).
    add_test(NAME k7_junction_hosted_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DCLAP=${GRIDLINE_CLAP_TESTPLUG} -DOUT=${CMAKE_BINARY_DIR}/k7_junction_hosted_app -P ${CMAKE_SOURCE_DIR}/tests/k7_junction_hosted_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k7_junction_hosted_app PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # An export of a project with an installed plugin leaves the window alive, and Cancel and quit work.
    add_test(NAME k38_export_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DCLAP=${GRIDLINE_CLAP_TESTPLUG} -DOUT=${CMAKE_BINARY_DIR}/k38_export_app -P ${CMAKE_SOURCE_DIR}/tests/k38_export_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k38_export_app PROPERTIES TIMEOUT 420)

    # A seeded stress run (src/app/DevStress.h): rapid edits, undo, add and remove, play, render while editing.
    add_test(NAME k38_stress_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k38_stress_app -P ${CMAKE_SOURCE_DIR}/tests/k38_stress_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k38_stress_app PROPERTIES TIMEOUT 300)

    # K-37: a bridged plugin's interface that never closes does not stop the app.
    add_test(NAME k37_bridged_close_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DCLAP=${GRIDLINE_CLAP_TESTPLUG} -DOUT=${CMAKE_BINARY_DIR}/k37_bridged_close_app -P ${CMAKE_SOURCE_DIR}/tests/k37_bridged_close_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k37_bridged_close_app PROPERTIES TIMEOUT 200 SKIP_REGULAR_EXPRESSION "SKIP: ")

    add_test(NAME plugin_undo_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/plugin_undo_app -P ${CMAKE_SOURCE_DIR}/tests/plugin_undo_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(plugin_undo_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # K-40: Crest's editor as drawn: no overlaps, knobs and controls on their rows and columns, no
    # clipped text, in every view, band, mode and engine, at interface scales 100 and 200 %, in both themes.
    add_test(NAME k40_crest_layout_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app>
                     -DOUT=${CMAKE_BINARY_DIR}/k40_crest_layout_app -P ${CMAKE_SOURCE_DIR}/tests/k40_crest_layout_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k40_crest_layout_app PROPERTIES TIMEOUT 1200)

    # J-117: floating windows inside the main window have an elastic boundary (dragged past an
    # edge they follow with damped overshoot, released they spring back inside; a shrunk main
    # window pulls them back), the pop-out button stays beside the close button whatever the
    # window's position, and docking by dragging still works.
    add_test(NAME j117_rubber_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j117_rubber -P ${CMAKE_SOURCE_DIR}/tests/j117_rubber.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j117_rubber_app PROPERTIES TIMEOUT 300)

    # J-158: big plugin editors (Platter, Seltmann) fit inside the main window: never popped out
    # by themselves (also when saved on a larger main window), the main window keeps its size,
    # capped to the work area with their content scrolling, natural size on big screens.
    add_test(NAME j158_window_fit_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j158_window_fit -P ${CMAKE_SOURCE_DIR}/tests/j158_window_fit_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j158_window_fit_app PROPERTIES TIMEOUT 300)

    # K-72 (issue 15): the main window's menus, popups and hints stay inside it (a docked rack's
    # "+" menu at the right edge used to open in OS windows made and destroyed per submenu, and
    # the app flickered); a popped-out window's menus may still stick out of it.
    add_test(NAME k72_popups_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k72_popups -P ${CMAKE_SOURCE_DIR}/tests/k72_popups_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k72_popups_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")
    # K-77: the main menus and the context menus in their planned order (open, create, edit,
    # properties, arrange, removals last), with no empty sections and no item listed twice.
    add_test(NAME k77_menu_order_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k77_menu_order -P ${CMAKE_SOURCE_DIR}/tests/k77_menu_order_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k77_menu_order_app PROPERTIES TIMEOUT 600)
    # K-87: freeze and hibernate in the running app (renders on disk, plugins unloaded and loaded
    # again, a note by hand plays live, a hibernating track wakes, saved with the project).
    add_test(NAME k87_freeze_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k87_freeze -P ${CMAKE_SOURCE_DIR}/tests/k87_freeze_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k87_freeze_app PROPERTIES TIMEOUT 300)
    # K-79: the window layout never reaches the project: the same project saved under every
    # layout (chosen in settings or switched while running) is byte-identical, and opening it changes nothing.
    add_test(NAME k79_layout_project_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k79_layout_project -P ${CMAKE_SOURCE_DIR}/tests/k79_layout_project_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k79_layout_project_app PROPERTIES TIMEOUT 600)
    # K-130: the layout chooser: the welcome page (keyboard and mouse), Settings > Interface (a
    # card and the Matching shortcuts option) and View > Window layout each pick and save a layout.
    add_test(NAME k130_layout_chooser_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k130_layout_chooser -P ${CMAKE_SOURCE_DIR}/tests/k130_layout_chooser_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k130_layout_chooser_app PROPERTIES TIMEOUT 600)

    # K-83: on X11 a popped-out window's menus, lists and hints stay inside it where they fit (an
    # OS window made per submenu or hint made the screen flicker on GNOME); macOS / Windows keep
    # letting them stick out.
    add_test(NAME k83_popups_detached_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k83_popups -P ${CMAKE_SOURCE_DIR}/tests/k83_popups_detached_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k83_popups_detached_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # K-19: plugin editors have their own size on top of the interface scale: automatic on a
    # large workspace, a picked size drawn, remembered and capped to the work area, controls
    # working where they show.
    add_test(NAME k19_editor_zoom_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k19_editor_zoom -P ${CMAKE_SOURCE_DIR}/tests/k19_editor_zoom_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k19_editor_zoom_app PROPERTIES TIMEOUT 300)
    # K-147: every built-in plugin window fits its editor (nothing clipped, no spare room) at the
    # default size, 75, 100 and 150 %, at interface scale 1 and 2.
    add_test(NAME k147_plugin_fit_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k147_plugin_fit -P ${CMAKE_SOURCE_DIR}/tests/k147_plugin_fit_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k147_plugin_fit_app PROPERTIES TIMEOUT 600)  # five sweeps of 15 to 60 s (up to 2 min on a loaded, software-rendered machine); each is cut at 150 s
    # K-147: Tracer's synced Time knob holds on divisions, is free between them, one undo per
    # drag; Shift drags past them; the wheel steps from division to division.
    add_test(NAME k147_magnet_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k147_magnet -P ${CMAKE_SOURCE_DIR}/tests/k147_magnet_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k147_magnet_app PROPERTIES TIMEOUT 400)
    # K-136: one size for every built-in plugin window (K-147: 75 % by default, 60 to 150 %), a plugin's
    # own size remembered, controls working at 75 %; the channel rack as tall as its content.
    add_test(NAME k136_plugin_size_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k136_plugin_size -P ${CMAKE_SOURCE_DIR}/tests/k136_plugin_size_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k136_plugin_size_app PROPERTIES TIMEOUT 400)
    add_test(NAME k136_rack_fit_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k136_rack_fit -P ${CMAKE_SOURCE_DIR}/tests/k136_rack_fit_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k136_rack_fit_app PROPERTIES TIMEOUT 300)
    # K-146: a cut-off rack with a long pattern: one sideways scroll control (the step slider), no
    # frame-to-frame size or scroll changes, the wheel and the slider scroll by the expected amounts.
    add_test(NAME k146_rack_scroll_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k146_rack_scroll -P ${CMAKE_SOURCE_DIR}/tests/k146_rack_scroll_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k146_rack_scroll_app PROPERTIES TIMEOUT 400)
    # K-15: Loupe pinned into the main layout (a tab next to the playlist, a split below it, a
    # side column), its geometry in each spot, unpinned again, the pin kept with the layout.
    add_test(NAME k15_loupe_dock_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k15_loupe_dock -P ${CMAKE_SOURCE_DIR}/tests/k15_loupe_dock_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k15_loupe_dock_app PROPERTIES TIMEOUT 300)

    # J-118: the clip brush: click a trimmed audio clip, paint copies of it as it is (length,
    # audio start, fades, gain, pitch, reverse); a sample picked in the picker paints at its
    # natural length; one undo step per paint. Crossfade screenshots at 1x / 2x, light / dark.
    add_test(NAME j118_paint_brush_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j118_paint_brush -P ${CMAKE_SOURCE_DIR}/tests/j118_paint_brush.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j118_paint_brush_app PROPERTIES TIMEOUT 300)

    # K-37: the Sweep Kick channel's envelope page at 100 % and 200 %, dark and light.
    add_test(NAME k37_sweepkick_env_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/k37_sweepkick_env
                     -P ${CMAKE_SOURCE_DIR}/tests/k37_sweepkick_env.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k37_sweepkick_env_app PROPERTIES TIMEOUT 300)

    # J-142: relative volume / fade drags across a selection of audio clips (one undo step each),
    # a single clip edited alone, and right-clicks on a clip's title strip: the name box and the
    # menu arrow open the clip menu, the rest of the strip deletes like the body (right-drag too).
    add_test(NAME j142_clip_edits_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j142_clip_edits -P ${CMAKE_SOURCE_DIR}/tests/j142_clip_edits.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j142_clip_edits_app PROPERTIES TIMEOUT 300)

    # J-187: the slip tool on a selection slips every selected clip (one undo step); alone, only the clip.
    add_test(NAME j187_multi_slip_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j187_multi_slip -P ${CMAKE_SOURCE_DIR}/tests/j187_multi_slip.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j187_multi_slip_app PROPERTIES TIMEOUT 300)

    # J-194: the browser search field's clear button empties the field, also while it is being edited.
    add_test(NAME j194_browser_search_clear_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j194_browser_search_clear
                     -P ${CMAKE_SOURCE_DIR}/tests/j194_browser_search_clear.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j194_browser_search_clear_app PROPERTIES TIMEOUT 300)

    # J-198: the tempo display's whole-number and decimal drag zones.
    add_test(NAME j198_tempo_drag_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j198_tempo_drag
                     -P ${CMAKE_SOURCE_DIR}/tests/j198_tempo_drag.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j198_tempo_drag_app PROPERTIES TIMEOUT 300)

    # J-195: the channel window's "Route to a free mixer track" button and list item (audio clip
    # channel, instrument, installed plugin channel): free insert, track named, one undo step.
    add_test(NAME j195_route_free_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j195_route_free -P ${CMAKE_SOURCE_DIR}/tests/j195_route_free.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j195_route_free_app PROPERTIES TIMEOUT 300)

    # J-193: a stretch-resized clip keeps its bars on a tempo change; an unstretched one keeps its audio.
    add_test(NAME j193_tempo_follow_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j193_tempo_follow -P ${CMAKE_SOURCE_DIR}/tests/j193_tempo_follow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j193_tempo_follow_app PROPERTIES TIMEOUT 300)

    # J-188: smooth snapping glides a dragged clip over several frames (off: jumps; the switch applies
    # live, also mid-drag); a Ctrl+wheel notch zooms smoothly with the point under the pointer fixed.
    add_test(NAME j188_smooth_motion_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j188_smooth_motion -P ${CMAKE_SOURCE_DIR}/tests/j188_smooth_motion.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j188_smooth_motion_app PROPERTIES TIMEOUT 300)

    # J-192: File > Import > Import from URL with a fake yt-dlp / ffmpeg (no network): open, paste,
    # fetch, import -> an audio clip on a new track (one undo step), playlist items, errors, the
    # tools set up on first use (a fake GitHub); screenshots of each state.
    add_test(NAME j192_import_url_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTOOL=$<TARGET_FILE:gridline_fake_media_tool>
                     -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j192_import_url -P ${CMAKE_SOURCE_DIR}/tests/j192_import_url.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j192_import_url_app PROPERTIES TIMEOUT 300)

    # The plugin window's channel strip (volume, pan, pitch, preview keyboard): hidden by default,
    # the header's eye slides it in and out, the window's size is stable after the slide, the
    # keyboard and knobs work while shown, and the choice survives a restart.
    add_test(NAME plugin_strip_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/plugin_strip -P ${CMAKE_SOURCE_DIR}/tests/plugin_strip.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(plugin_strip_app PROPERTIES TIMEOUT 300)

    # J-107: Seltmann's one-step flow: drop a file on the editor -> analysed -> click a layout -> playing.
    add_test(NAME seltmann_flow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/seltmann_flow -P ${CMAKE_SOURCE_DIR}/tests/seltmann_flow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(seltmann_flow PROPERTIES TIMEOUT 300)

    # J-185: Seltmann: drag a pad's pitch readout, right-click a block > Start the sequence here (undo), drag a cut to resize.
    add_test(NAME j185_seltmann_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j185_seltmann -P ${CMAKE_SOURCE_DIR}/tests/j185_seltmann_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j185_seltmann_app PROPERTIES TIMEOUT 300)

    # J-208: Seltmann: the wheel over a sequence block pitches its region (Shift: finer), one undo step a run.
    add_test(NAME j208_seltmann_wheel
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j208_seltmann_wheel -P ${CMAKE_SOURCE_DIR}/tests/j208_seltmann_wheel.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j208_seltmann_wheel PROPERTIES TIMEOUT 300)

    # J-214: Seltmann: a sequence block's right edge sets that step's length (not its region), one undo step a drag.
    add_test(NAME j214_seltmann_lengths
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j214_seltmann_lengths -P ${CMAKE_SOURCE_DIR}/tests/j214_seltmann_lengths.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j214_seltmann_lengths PROPERTIES TIMEOUT 300)

    # J-218: Seltmann records from an input on its start screen (a synthetic input signal, never a microphone).
    add_test(NAME j218_seltmann_record
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DRECBIN=$<TARGET_FILE:gridline_test_seltmann_rec>
                     -DOUT=${CMAKE_BINARY_DIR}/j218_seltmann_record -P ${CMAKE_SOURCE_DIR}/tests/j218_seltmann_record.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j218_seltmann_record PROPERTIES TIMEOUT 400)

    # K-145: a recording's waveform drawn live and fitted to the width, in Loupe and Seltmann's recorder (simulated inputs).
    add_test(NAME k145_live_wave
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/k145_live_wave -P ${CMAKE_SOURCE_DIR}/tests/k145_live_wave.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k145_live_wave PROPERTIES TIMEOUT 600)

    # J-208: the playlist picker's "+" (new pattern) and plugins dropped on playlist tracks (instrument tracks).
    add_test(NAME j208_playlist_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j208_playlist -P ${CMAKE_SOURCE_DIR}/tests/j208_playlist_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j208_playlist_app PROPERTIES TIMEOUT 400)

    # K-17: audio dragged out of Loupe onto playlist tracks (whole file / selection), edited audio in the PROJECT tab.
    add_test(NAME k17_loupe_drag
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k17_loupe_drag -P ${CMAKE_SOURCE_DIR}/tests/k17_loupe_drag.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k17_loupe_drag PROPERTIES TIMEOUT 400)

    # Loupe workflow: selection edges, playback follows the selection and clicks, scrubbing, auto-scroll setting,
    # the drag handle from a channel's sample, a file opened on its own.
    add_test(NAME k166_loupe_workflow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k166_loupe_workflow -P ${CMAKE_SOURCE_DIR}/tests/k166_loupe_workflow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k166_loupe_workflow PROPERTIES TIMEOUT 600)

    # Layer editor: every built-in layer drawn without overlapping text (both themes, two sizes), a zone
    # edge drag as one undo step, a layer preset file saved and loaded back.
    add_test(NAME k169_layer_editor
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k169_layer_editor -P ${CMAKE_SOURCE_DIR}/tests/k169_layer_editor.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k169_layer_editor PROPERTIES TIMEOUT 600)

    # K-18: rename, new folder, moves and the trash in the browser; the project's references and undo follow.
    add_test(NAME k18_browser_files
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k18_browser_files -P ${CMAKE_SOURCE_DIR}/tests/k18_browser_files.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k18_browser_files PROPERTIES TIMEOUT 400)

    # K-160: browse and preview samples in the browser with the keyboard.
    add_test(NAME k160_browser_keys
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/k160_browser_keys -P ${CMAKE_SOURCE_DIR}/tests/k160_browser_keys.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(k160_browser_keys PROPERTIES TIMEOUT 400)

    # J-191: Split stems from the clip menu (a tiny model): four aligned clips on new tracks, one undo step.
    add_test(NAME j191_stems_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_tests_stems>
                     -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j191_stems_app -P ${CMAKE_SOURCE_DIR}/tests/j191_stems_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j191_stems_app PROPERTIES TIMEOUT 400)

    # J-151: Platter's two decks: the controller question, both decks playing, the crossfader.
    add_test(NAME platter_flow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_platter>
                     -DOUT=${CMAKE_BINARY_DIR}/platter_flow -P ${CMAKE_SOURCE_DIR}/tests/platter_flow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(platter_flow PROPERTIES TIMEOUT 300)

    # J-160: audio dragged onto Platter loads on the deck it is dropped on (A or B, lane or panel).
    add_test(NAME platter_drop
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_platter>
                     -DOUT=${CMAKE_BINARY_DIR}/platter_drop -P ${CMAKE_SOURCE_DIR}/tests/platter_drop.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(platter_drop PROPERTIES TIMEOUT 300)

    # J-159: deck passes recorded as automation and notes (pattern and song mode, a new pattern
    # per pass, one undo step each), shown in the piano roll and the playlist.
    add_test(NAME platter_perf_flow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_platter>
                     -DOUT=${CMAKE_BINARY_DIR}/platter_perf_flow -P ${CMAKE_SOURCE_DIR}/tests/platter_perf_flow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(platter_perf_flow PROPERTIES TIMEOUT 300)

    # J-204: Platter's hot cue pads tapped as fast as the input allows (playing and stopped,
    # alternating pads, Shift, right-click, hot cue keys): every tap lands, nothing sticks.
    add_test(NAME j204_hotcue_taps
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_platter>
                     -DOUT=${CMAKE_BINARY_DIR}/j204_hotcue_taps -P ${CMAKE_SOURCE_DIR}/tests/j204_hotcue_taps.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j204_hotcue_taps PROPERTIES TIMEOUT 400)

    # J-110: Seltmann's pattern Play button follows every Stop (button, Space, panic, Tools menu,
    # MIDI / remote, focus loss) and plays again with one click.
    add_test(NAME j110_play_state_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j110_play_state -P ${CMAKE_SOURCE_DIR}/tests/j110_play_state.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j110_play_state_app PROPERTIES TIMEOUT 300)

    # J-217: audio recorded into the playlist (synthetic input) draws its clip and waveform live; the
    # committed clip equals the last live frame (geometry, peaks), loop passes stack, at 100 % and 200 %.
    add_test(NAME j217_live_take_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j217_live_take
                     -P ${CMAKE_SOURCE_DIR}/tests/j217_live_take_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j217_live_take_app PROPERTIES TIMEOUT 300)

    # J-181: Kerf's editor: drop zone, loading, tabs and display modes respond, fits at 1280 x 800, layout audit clean.
    add_test(NAME j181_kerf_ui_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j181_kerf_ui -P ${CMAKE_SOURCE_DIR}/tests/j181_kerf_ui.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j181_kerf_ui_app PROPERTIES TIMEOUT 600)

    # J-182: Kerf's snap, ruler, marker drag / add on the grid, rename and typed position in the real app.
    add_test(NAME j182_kerf_time_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j182_kerf_time -P ${CMAKE_SOURCE_DIR}/tests/j182_kerf_time.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j182_kerf_time_app PROPERTIES TIMEOUT 600)

    # J-190: Kerf slices on the beat grid: the grid menu, region counts and starts on the detected grid, layout audit.
    add_test(NAME j190_kerf_grid_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j190_kerf_grid -P ${CMAKE_SOURCE_DIR}/tests/j190_kerf_grid.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j190_kerf_grid_app PROPERTIES TIMEOUT 600)

    # J-184: Pixel Tone pictures: file dialog + undo, drop on the picture (highlight), rack rows, corrupt file.
    add_test(NAME j184_pixeltone_pictures_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_pixeltone>
                     -DOUT=${CMAKE_BINARY_DIR}/j184_pixeltone_pictures -P ${CMAKE_SOURCE_DIR}/tests/j184_pixeltone_pictures.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j184_pixeltone_pictures_app PROPERTIES TIMEOUT 600)

    # J-183: Kerf's layout: no overlapping controls, rows aligned, no clipped or tiny text (1440x960, 1280x800, UI scale 1.5).
    add_test(NAME j183_kerf_layout_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j183_kerf_layout -P ${CMAKE_SOURCE_DIR}/tests/j183_kerf_layout.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j183_kerf_layout_app PROPERTIES TIMEOUT 900)

    # J-186: play lights in the real app: channel rack rows light while their channel plays (steps on two
    # channels, a silent and a muted row stay dark); Kerf lights the region a note plays.
    add_test(NAME j186_play_lights_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j186_play_lights -P ${CMAKE_SOURCE_DIR}/tests/j186_play_lights.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j186_play_lights_app PROPERTIES TIMEOUT 600)

    # J-112: Space / the Play button / the remote Play while Seltmann's pattern plays (transport stopped) stop the
    # pattern and leave the transport stopped; the next Play plays the song. Window focused or not.
    add_test(NAME j112_play_stops_plugins_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j112_play_stops_plugins -P ${CMAKE_SOURCE_DIR}/tests/j112_play_stops_plugins.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j112_play_stops_plugins_app PROPERTIES TIMEOUT 300)

    # J-111: two app windows (hidden, own configs, the app's network settings) collaborate through
    # dev-input "collab" commands: join after a delay, edits converge, both quit and reopen
    # (project file / folder) and reconnect by themselves; a join whose ICE connects late still works.
    if(GRIDLINE_COLLAB)
      add_test(NAME j111_collab_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111_collab_app
                       -P ${CMAKE_SOURCE_DIR}/tests/j111_collab_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111_collab_app PROPERTIES TIMEOUT 900)
      # K-141: undo / redo through the real shortcuts in two app windows, in each window layout:
      # each person's undo takes back only their own step, redo puts it back.
      add_test(NAME k141_collab_undo_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/k141_collab_undo_app
                       -P ${CMAKE_SOURCE_DIR}/tests/k141_collab_undo_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k141_collab_undo_app PROPERTIES TIMEOUT 1500)
      # J-111e: an admin removes someone whose project is open; private copy; leave > keep / delete.
      add_test(NAME j111e_collab_remove_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111e_collab_remove_app
                       -P ${CMAKE_SOURCE_DIR}/tests/j111e_collab_remove_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111e_collab_remove_app PROPERTIES TIMEOUT 1800)
      # J-111d: collaborators' cursors: a simulated collaborator's cursor drawn on top of a floating
      # window (pixel read back) and faded where its spot is not shown; two app windows of other
      # sizes and interface scales map a cursor to the same playlist spot and the same button.
      add_test(NAME j111d_collab_cursors
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111d_collab_cursors
                       -P ${CMAKE_SOURCE_DIR}/tests/j111d_collab_cursors.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111d_collab_cursors PROPERTIES TIMEOUT 600)
      # J-111f: a live rename reaches the other window (name, title, file, Recent collaborative
      # projects); a rename made while the other Gridline was closed arrives when both are open again.
      add_test(NAME j111_rename_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111_rename_app
                       -P ${CMAKE_SOURCE_DIR}/tests/j111_rename_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111_rename_app PROPERTIES TIMEOUT 900)
      # J-111h: no window changes size after it opened (Share this project, renaming in place,
      # Project info, a window sizing itself to a full-width section), at a large and a small size.
      add_test(NAME j111h_dialog_stable
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111h_dialog_stable
                       -P ${CMAKE_SOURCE_DIR}/tests/j111h_dialog_stable.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111h_dialog_stable PROPERTIES TIMEOUT 600)
    endif()

    # J-103: a session (demo copy, playing, an editor open) quit by the user, and the layout audit
    # without --screenshot, exit with code 0: no background thread runs into static destruction.
    add_test(NAME j103_clean_exit
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j103_clean_exit -P ${CMAKE_SOURCE_DIR}/tests/j103_clean_exit.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j103_clean_exit PROPERTIES TIMEOUT 900)
    # J-141: the welcome launcher: logo shows it, Esc returns, tiles open projects, unsaved changes ask first.
    add_test(NAME j141_launcher_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j141_launcher_app -P ${CMAKE_SOURCE_DIR}/tests/j141_launcher_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j141_launcher_app PROPERTIES TIMEOUT 600)
    # K-119: files the system asks to open (macOS open requests while starting and while running, the
    # argument opened once), projects and tracker modules, unsaved changes asked first.
    if(Python3_Interpreter_FOUND)
      add_test(NAME k119_os_open
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                       -DPY=${Python3_EXECUTABLE} -DOUT=${CMAKE_BINARY_DIR}/k119_os_open -P ${CMAKE_SOURCE_DIR}/tests/k119_os_open.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k119_os_open PROPERTIES TIMEOUT 600)
      # K-121: a later launch hands its files to the running Gridline (two hidden instances on a
      # private test endpoint): opened with the unsaved-changes prompt, opt-out, stale socket, hidden runs.
      add_test(NAME k121_single_instance
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/k121_single_instance.py $<TARGET_FILE:gridline-app>
                       ${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json ${CMAKE_BINARY_DIR}/k121_single_instance
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k121_single_instance PROPERTIES TIMEOUT 600)
      # K-93: the command palette in each layout and translated: every menu item once, every keymap
      # command once, the shortcut, typing + Enter runs commands, settings, plugins; recent rows.
      add_test(NAME k93_palette_app
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/k93_palette_app.py $<TARGET_FILE:gridline-app>
                       ${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json ${CMAKE_BINARY_DIR}/k93_palette_app
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(k93_palette_app PROPERTIES TIMEOUT 900)
    endif()
    # J-146: the welcome screen's Collaborative view: rail, tiles, Join, live presence (simulated collaborators).
    add_test(NAME j146_launcher_collab_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j146_launcher_collab_app
                     -P ${CMAKE_SOURCE_DIR}/tests/j146_launcher_collab_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j146_launcher_collab_app PROPERTIES TIMEOUT 600)
  endif()

  include(cmake/GridlinePlugins.cmake)  # PH-09: Gridline.clap / Gridline.vst3

  if(WIN32)
    # A window program (no console window); --version / --help print to the console it was
    # started from (main.cpp).
    set_target_properties(gridline-app PROPERTIES WIN32_EXECUTABLE TRUE)
    # Windows layout: the programs with assets\ beside them (main.cpp finds <exe dir>/assets).
    install(TARGETS gridline-app gridline RUNTIME DESTINATION .)
    install(DIRECTORY assets/ DESTINATION assets PATTERN "sounds/src" EXCLUDE)
  else()
    install(TARGETS gridline-app gridline RUNTIME DESTINATION bin)
    install(DIRECTORY assets/ DESTINATION share/gridline/assets PATTERN "sounds/src" EXCLUDE)  # J-48: sound sources stay in the tree
  endif()
  # J-192: the release packages carry yt-dlp and ffmpeg for Import from URL (a folder made by
  # tools/release/fetch-media-tools.py; empty in normal builds, which install them on first use).
  set(GRIDLINE_MEDIA_TOOLS_DIR "" CACHE PATH "Folder with yt-dlp and ffmpeg to install beside the programs (release packages)")
  if(GRIDLINE_MEDIA_TOOLS_DIR)
    if(WIN32)
      install(DIRECTORY "${GRIDLINE_MEDIA_TOOLS_DIR}/" DESTINATION tools USE_SOURCE_PERMISSIONS)
    elseif(NOT APPLE)
      install(DIRECTORY "${GRIDLINE_MEDIA_TOOLS_DIR}/" DESTINATION share/gridline/tools USE_SOURCE_PERMISSIONS)
    endif()
  endif()
  if(UNIX AND NOT APPLE)
    install(FILES assets/linux/gridline.desktop DESTINATION share/applications)
    install(FILES assets/linux/gridline.xml DESTINATION share/mime/packages)
    install(FILES assets/linux/com.lolitemaultes.Gridline.metainfo.xml DESTINATION share/metainfo)  # J-134: software centres
    # K-14: the icon theme sizes plus the scalable one (tools/ci/check-linux-icons.sh checks them).
    foreach(sz 16 24 32 48 64 96 128 256 512)
      install(FILES assets/icons/gridline-app-${sz}.png DESTINATION share/icons/hicolor/${sz}x${sz}/apps RENAME gridline.png)
    endforeach()
    install(FILES assets/icons/gridline-app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME gridline.svg)
  endif()
else()
  message(STATUS "SDL2/OpenGL not found (SDL2: ${SDL2_FOUND}${SDL2PC_FOUND}, OpenGL: ${OPENGL_FOUND}): building the engine, CLI and tests only (no gridline-app)")
endif()

# ---- Windows: version resources, icons and the application manifest ----
# Every executable gets cmake/windows/gridline.manifest (UTF-8 process code page, per-monitor DPI,
# long paths); the programs and the plugins get version information, the programs the icon.
if(WIN32)
  function(gridline_windows_resources target description filename filetype icon)
    set(GL_RC_DESCRIPTION "${description}")
    set(GL_RC_NAME "${target}")
    set(GL_RC_FILENAME "${filename}")
    set(GL_RC_FILETYPE "${filetype}")
    if(icon)
      file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/icons/Gridline.ico" _ico)
      set(GL_RC_ICON "1 ICON \"${_ico}\"")
    else()
      set(GL_RC_ICON "")
    endif()
    configure_file(${CMAKE_SOURCE_DIR}/cmake/windows/gridline.rc.in ${CMAKE_BINARY_DIR}/generated/${target}.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated/${target}.rc)
  endfunction()
  gridline_windows_resources(gridline "Gridline command line" "gridline.exe" "VFT_APP" ON)
  if(TARGET gridline-app)
    gridline_windows_resources(gridline-app "Gridline" "gridline-app.exe" "VFT_APP" ON)
  endif()
  if(TARGET gridline_vst3)
    gridline_windows_resources(gridline_vst3 "Gridline (VST3 plugin)" "Gridline.vst3" "VFT_DLL" OFF)
    gridline_windows_resources(gridline_clap "Gridline (CLAP plugin)" "Gridline.clap" "VFT_DLL" OFF)
  endif()
  if(TARGET gridline_suite_gridline_vst3)  # J-131
    gridline_windows_resources(gridline_suite_gridline_vst3 "Gridline Suite (VST3 plugins)" "Gridline Suite.vst3" "VFT_DLL" OFF)
    gridline_windows_resources(gridline_suite_gridline_clap "Gridline Suite (CLAP plugins)" "Gridline Suite.clap" "VFT_DLL" OFF)
  endif()
  if(TARGET gridline_suite_mini_vst3)  # J-132
    gridline_windows_resources(gridline_suite_mini_vst3 "Gridline Suite Mini (VST3 plugins)" "Gridline Suite Mini.vst3" "VFT_DLL" OFF)
    gridline_windows_resources(gridline_suite_mini_clap "Gridline Suite Mini (CLAP plugins)" "Gridline Suite Mini.clap" "VFT_DLL" OFF)
  endif()
  get_property(_gl_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t ${_gl_targets})
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "EXECUTABLE")
      target_sources(${_t} PRIVATE ${CMAKE_SOURCE_DIR}/cmake/windows/gridline.manifest)
    elseif(_type STREQUAL "MODULE_LIBRARY")
      # Plugins share output names (Gridline, GridlineTest): their import libraries and export
      # files (.lib / .exp) go to a folder of their own.
      set_target_properties(${_t} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/implib/${_t})
    endif()
  endforeach()
endif()

# ---- Linux packages (cpack in the build folder): .tar.gz, .deb, .rpm ----
# Installed under /usr: programs in bin, assets in share/gridline, the desktop entry, icons and
# MIME type; with GRIDLINE_INSTALL_PLUGINS=ON also lib/vst3/Gridline.vst3 and lib/clap/Gridline.clap.
if(UNIX AND NOT APPLE)
  set(CPACK_PACKAGE_NAME gridline)
  set(CPACK_PACKAGE_VENDOR "Gridline")
  # Package versions: numbers for CPack, 0.7.0~beta.1 for Debian (sorts before 0.7.0), Version
  # 0.7.0 + Release 0.1.beta.1 for rpm; the file names carry the full version.
  set(CPACK_PACKAGE_VERSION ${GRIDLINE_VERSION_NUMERIC})
  set(CPACK_DEBIAN_PACKAGE_VERSION ${GRIDLINE_VERSION_DEB})
  set(CPACK_RPM_PACKAGE_VERSION ${GRIDLINE_VERSION_NUMERIC})
  set(CPACK_RPM_PACKAGE_RELEASE ${GRIDLINE_VERSION_RPM_RELEASE})
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Pattern-based music workstation")
  set(CPACK_PACKAGE_DESCRIPTION "Gridline is a pattern-based digital audio workstation: channel rack, piano roll, playlist, mixer, built-in instruments and effects, VST3 / CLAP / VST2 / LV2 plugin hosting, and FL Studio project import and export.")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/lolitemaultes/Gridline")
  set(CPACK_PACKAGE_CONTACT "Gridline <188165912+lolitemaultes@users.noreply.github.com>")
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
  set(CPACK_STRIP_FILES ON)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_gl_deb_arch arm64)
    set(_gl_rpm_arch aarch64)
  else()
    set(_gl_deb_arch amd64)
    set(_gl_rpm_arch x86_64)
  endif()
  set(CPACK_PACKAGE_FILE_NAME "Gridline-${GRIDLINE_VERSION_FULL}-linux-${_gl_rpm_arch}")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${_gl_deb_arch})
  set(CPACK_DEBIAN_FILE_NAME "Gridline-${GRIDLINE_VERSION_FULL}-linux-${_gl_deb_arch}.deb")
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_RPM_PACKAGE_ARCHITECTURE ${_gl_rpm_arch})
  set(CPACK_RPM_FILE_NAME "Gridline-${GRIDLINE_VERSION_FULL}-linux-${_gl_rpm_arch}.rpm")
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
  set(CPACK_RPM_PACKAGE_URL "https://github.com/lolitemaultes/Gridline")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/mime
      /usr/share/mime/packages /usr/share/metainfo /usr/lib/vst3 /usr/lib/clap)
  # K-119: the file type and desktop entry caches. The .deb needs no script: dpkg's triggers of
  # shared-mime-info and desktop-file-utils refresh them. The .rpm runs one where no trigger does.
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/cmake/linux/rpm-refresh-caches.sh)
  set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/cmake/linux/rpm-refresh-caches.sh)
  if(NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
  endif()
  include(CPack)
endif()


# K-172: no button keeps the toggle-on look switched on for good (a main action is a primary button).
add_test(NAME k172_primary_audit COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/src -P ${CMAKE_SOURCE_DIR}/tests/k172_primary_audit.cmake)

# K-136: built-in plugin windows open at 85 % by default (Settings > Interface). The tests that
# drive editors by their design coordinates run them at 100 %; the plugin window size tests
# (k136_plugin_size_app, k19_editor_zoom_app) clear this to test the real setting.
get_property(GL_ALL_TESTS DIRECTORY PROPERTY TESTS)
if(GL_ALL_TESTS)
  set_property(TEST ${GL_ALL_TESTS} APPEND PROPERTY ENVIRONMENT "GRIDLINE_PLUGIN_WINDOW_SIZE=1")
endif()

# Slow test machines (software-rendered frames on small ARM runners, as in the Chromebook container
# on CI): -DGRIDLINE_TEST_TIMEOUT_SCALE=<n> multiplies every test's own time limit by n.
set(GRIDLINE_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiply each test's own TIMEOUT by this whole number (slow test machines)")
if(GL_ALL_TESTS AND NOT GRIDLINE_TEST_TIMEOUT_SCALE EQUAL 1)
  foreach(t IN LISTS GL_ALL_TESTS)
    get_test_property(${t} TIMEOUT limit)
    if(limit)
      math(EXPR limit "${limit} * ${GRIDLINE_TEST_TIMEOUT_SCALE}")
      set_tests_properties(${t} PROPERTIES TIMEOUT ${limit})
    endif()
  endforeach()
endif()
