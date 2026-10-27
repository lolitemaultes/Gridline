cmake_minimum_required(VERSION 3.16)
project(gridline VERSION 0.7.0 LANGUAGES C CXX)
# Prerelease label: "beta.1" makes this Gridline 0.7.0-beta.1 (published as a GitHub pre-release);
# empty ("") for a release. The version is set only here (cmake/GridlineVersion.cmake, docs/releasing.md).
set(GRIDLINE_VERSION_PRE "beta.2")
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
file(GLOB GRIDLINE_CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp src/core/flp/*.cpp src/engine/*.cpp src/engine/pitch/*.cpp src/packs/Registry.cpp
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
add_library(gridline_stbimage STATIC third_party/stb/gridline_stb_image.c)
target_compile_options(gridline_stbimage PRIVATE ${GL_NOWARN})
set_target_properties(gridline_stbimage PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
    set(GRIDLINE_BUILD_ID "unknown")
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
                        "-framework AppKit" "-framework Foundation" "-framework Security")
elseif(WIN32)
  # Winsock (MCP server), Credential Manager (advapi32), process / memory statistics, windows,
  # WinHTTP (J-89 update check).
  target_link_libraries(gridline_core PUBLIC ws2_32 bcrypt advapi32 psapi user32 gdi32 shell32 ole32 uuid urlmon comctl32 winhttp)
elseif(UNIX)
  find_library(GRIDLINE_RT_LIB rt)  # shm_open lives in librt on Linux / NetBSD; libc elsewhere
  if(GRIDLINE_RT_LIB)
    target_link_libraries(gridline_core PUBLIC ${GRIDLINE_RT_LIB})
  endif()
endif()
target_compile_options(gridline_core PRIVATE ${GL_WARN})
find_package(Threads REQUIRED)
target_link_libraries(gridline_core PUBLIC Threads::Threads vst3_pluginterfaces gridline_lua gridline_monocypher gridline_stbimage ${CMAKE_DL_LIBS})
if(GRIDLINE_COLLAB)
  target_link_libraries(gridline_core PUBLIC gridline_juice gridline_plum)
  target_compile_definitions(gridline_core PUBLIC GRIDLINE_COLLAB_NET=1)
endif()

add_executable(gridline src/cli/main.cpp src/cli/Bench.cpp src/cli/FlpTools.cpp src/cli/McpCli.cpp src/cli/CollabCli.cpp src/cli/SuiteCli.cpp)
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
# J-141: welcome launcher logic (start-up plan, state machine, recents, sketches + cache, layout audit).
add_executable(gridline_tests_launcher tests/test_launcher.cpp)
target_link_libraries(gridline_tests_launcher PRIVATE gridline_core)
target_include_directories(gridline_tests_launcher PRIVATE src tests)
target_compile_options(gridline_tests_launcher PRIVATE ${GL_WARN})
add_test(NAME j141_launcher COMMAND gridline_tests_launcher WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
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
add_executable(gridline_tests_midi_workflow tests/test_midi_workflow.cpp)
target_link_libraries(gridline_tests_midi_workflow PRIVATE gridline_core)
target_compile_options(gridline_tests_midi_workflow PRIVATE ${GL_WARN})
add_test(NAME midi_workflow COMMAND gridline_tests_midi_workflow WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_executable(gridline_tests_pdc tests/test_pdc.cpp)
target_link_libraries(gridline_tests_pdc PRIVATE gridline_core)
target_compile_options(gridline_tests_pdc PRIVATE ${GL_WARN})
add_test(NAME pdc COMMAND gridline_tests_pdc WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
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
# J-77: engine note state (lit keys, pad flashes), Keep / Revert flow, PAT shows the rack.
add_executable(gridline_tests_j77 tests/test_j77.cpp src/app/DisplayConfirm.cpp)
target_link_libraries(gridline_tests_j77 PRIVATE gridline_core)
target_include_directories(gridline_tests_j77 PRIVATE tests src)
target_compile_options(gridline_tests_j77 PRIVATE ${GL_WARN})
add_test(NAME j77 COMMAND gridline_tests_j77 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
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
add_executable(gridline_tests_clip_sources tests/test_clip_sources.cpp)
target_link_libraries(gridline_tests_clip_sources PRIVATE gridline_core)
target_include_directories(gridline_tests_clip_sources PRIVATE tests src)
target_compile_options(gridline_tests_clip_sources PRIVATE ${GL_WARN})
add_test(NAME clip_sources COMMAND gridline_tests_clip_sources WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
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
       ${CMAKE_SOURCE_DIR}/src/app/Viewports.cpp ${CMAKE_SOURCE_DIR}/src/app/LiveResize.cpp)
  add_library(gridline_ui OBJECT ${GRIDLINE_APP_SRC})
  target_link_libraries(gridline_ui PRIVATE gridline_core imgui_core rtmidi gridline_sdl_headers)
  target_compile_options(gridline_ui PRIVATE ${GL_WARN_LOOSE})
  set_target_properties(gridline_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_executable(gridline-app src/app/main.cpp src/app/PluginWindow.cpp src/app/Viewports.cpp src/app/LiveResize.cpp $<TARGET_OBJECTS:gridline_ui> ${GRIDLINE_ICON_OBJS})
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
    file(WRITE ${GRIDLINE_APP_BUNDLE}/Contents/Info.plist
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n<key>CFBundleName</key><string>Gridline</string>\n<key>CFBundleDisplayName</key><string>Gridline</string>\n<key>CFBundleExecutable</key><string>gridline-app</string>\n<key>CFBundleIdentifier</key><string>org.gridline.Gridline</string>\n<key>CFBundleIconFile</key><string>Gridline</string>\n<key>CFBundlePackageType</key><string>APPL</string>\n<key>CFBundleShortVersionString</key><string>${GRIDLINE_VERSION_NUMERIC}</string>\n<key>CFBundleVersion</key><string>${GRIDLINE_VERSION_NUMERIC}</string>\n<key>LSApplicationCategoryType</key><string>public.app-category.music</string>\n<key>LSMinimumSystemVersion</key><string>${GL_MACOS_MIN}</string>\n<key>NSHighResolutionCapable</key><true/>\n<key>NSMicrophoneUsageDescription</key><string>Gridline records audio from your input devices.</string>\n</dict></plist>\n")
    add_custom_command(TARGET gridline-app POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/icons/Gridline.icns
              ${GRIDLINE_APP_BUNDLE}/Contents/Resources/Gridline.icns
      COMMAND ${CMAKE_COMMAND} -E rm -f ${GRIDLINE_APP_BUNDLE}/Contents/MacOS/gridline-app
      COMMAND ${CMAKE_COMMAND} -E create_hardlink $<TARGET_FILE:gridline-app> ${GRIDLINE_APP_BUNDLE}/Contents/MacOS/gridline-app
      COMMAND touch ${GRIDLINE_APP_BUNDLE}
      COMMENT "Assembling Gridline.app")
  endif()

  if(GRIDLINE_GUI_TESTS)
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
                         TIMEOUT 900
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

    # J-98: Tideline's Split slider keeps its handle under the pointer at 100 / 150 / 200 % (SDL drags).
    add_test(NAME j98_slider_follow
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j98_slider_follow
                     -P ${CMAKE_SOURCE_DIR}/tests/j98_slider_follow.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j98_slider_follow PROPERTIES TIMEOUT 300)

    # J-105: Convolver EQ / IR envelope point editing at 100 / 200 % (SDL drags): click adds one point, no painting, undo per gesture, save + load.
    add_test(NAME j105_conv_points
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j105_conv_points
                     -P ${CMAKE_SOURCE_DIR}/tests/j105_conv_points.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j105_conv_points PROPERTIES TIMEOUT 300)

    # J-97: browser / external drops onto channel rack rows replace their content (routing, steps, notes kept), insert between rows, undo.
    add_test(NAME j97_rack_drop
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j97_rack_drop -P ${CMAKE_SOURCE_DIR}/tests/j97_rack_drop.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j97_rack_drop PROPERTIES TIMEOUT 300)

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
    add_test(NAME plugin_undo_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/plugin_undo_app -P ${CMAKE_SOURCE_DIR}/tests/plugin_undo_app.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(plugin_undo_app PROPERTIES TIMEOUT 300 SKIP_REGULAR_EXPRESSION "SKIP: ")

    # J-117: floating windows inside the main window have an elastic boundary (dragged past an
    # edge they follow with damped overshoot, released they spring back inside; a shrunk main
    # window pulls them back), the pop-out button stays beside the close button whatever the
    # window's position, and docking by dragging still works.
    add_test(NAME j117_rubber_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j117_rubber -P ${CMAKE_SOURCE_DIR}/tests/j117_rubber.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j117_rubber_app PROPERTIES TIMEOUT 300)

    # J-118: the clip brush: click a trimmed audio clip, paint copies of it as it is (length,
    # audio start, fades, gain, pitch, reverse); a sample picked in the picker paints at its
    # natural length; one undo step per paint. Crossfade screenshots at 1x / 2x, light / dark.
    add_test(NAME j118_paint_brush_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j118_paint_brush -P ${CMAKE_SOURCE_DIR}/tests/j118_paint_brush.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j118_paint_brush_app PROPERTIES TIMEOUT 300)

    # J-142: relative volume / fade drags across a selection of audio clips (one undo step each),
    # a single clip edited alone, and right-clicks on a clip's title strip: the name box and the
    # menu arrow open the clip menu, the rest of the strip deletes like the body (right-drag too).
    add_test(NAME j142_clip_edits_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/tests/fixtures/test_song.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j142_clip_edits -P ${CMAKE_SOURCE_DIR}/tests/j142_clip_edits.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j142_clip_edits_app PROPERTIES TIMEOUT 300)

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

    # J-110: Seltmann's pattern Play button follows every Stop (button, Space, panic, Tools menu,
    # MIDI / remote, focus loss) and plays again with one click.
    add_test(NAME j110_play_state_app
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DTESTBIN=$<TARGET_FILE:gridline_test_seltmann>
                     -DOUT=${CMAKE_BINARY_DIR}/j110_play_state -P ${CMAKE_SOURCE_DIR}/tests/j110_play_state.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j110_play_state_app PROPERTIES TIMEOUT 300)

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
      # J-111e: an admin removes someone whose project is open; private copy; leave > keep / delete.
      add_test(NAME j111e_collab_remove_app
               COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DOUT=${CMAKE_BINARY_DIR}/j111e_collab_remove_app
                       -P ${CMAKE_SOURCE_DIR}/tests/j111e_collab_remove_app.cmake
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
      set_tests_properties(j111e_collab_remove_app PROPERTIES TIMEOUT 900)
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
  if(UNIX AND NOT APPLE)
    install(FILES assets/linux/gridline.desktop DESTINATION share/applications)
    install(FILES assets/linux/gridline.xml DESTINATION share/mime/packages)
    install(FILES assets/linux/org.gridline.Gridline.metainfo.xml DESTINATION share/metainfo)  # J-134: software centres
    foreach(sz 16 24 32 48 64 96 128 256 512)
      install(FILES assets/icons/gridline-app-${sz}.png DESTINATION share/icons/hicolor/${sz}x${sz}/apps RENAME gridline.png)
    endforeach()
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
  if(NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
  endif()
  include(CPack)
endif()

