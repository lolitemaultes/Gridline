cmake_minimum_required(VERSION 3.16)
project(gridline VERSION 0.5.1 LANGUAGES C CXX)

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
else()
  set(GL_WARN -Wall -Wextra -Wpedantic)
  # Identical audio on every CPU: GCC fuses a*b+c into one rounding (FMA) wherever the CPU has it
  # (aarch64, and x86 with -march), unlike Clang and MSVC; the reference captures expect
  # separate roundings.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-ffp-contract=off)
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
file(GLOB GRIDLINE_CORE_SRC CONFIGURE_DEPENDS src/core/*.cpp src/core/flp/*.cpp src/engine/*.cpp src/packs/Registry.cpp
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
add_library(gridline_core STATIC ${GRIDLINE_CORE_SRC})
set_target_properties(gridline_core PROPERTIES POSITION_INDEPENDENT_CODE ON)  # also linked into the plugin bundles (PH-09)
target_include_directories(gridline_core PUBLIC src)
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
  # Winsock (MCP server), Credential Manager (advapi32), process / memory statistics, windows.
  target_link_libraries(gridline_core PUBLIC ws2_32 advapi32 psapi user32 gdi32 shell32 ole32 uuid urlmon comctl32)
elseif(UNIX)
  find_library(GRIDLINE_RT_LIB rt)  # shm_open lives in librt on Linux / NetBSD; libc elsewhere
  if(GRIDLINE_RT_LIB)
    target_link_libraries(gridline_core PUBLIC ${GRIDLINE_RT_LIB})
  endif()
endif()
target_compile_options(gridline_core PRIVATE ${GL_WARN})
find_package(Threads REQUIRED)
target_link_libraries(gridline_core PUBLIC Threads::Threads vst3_pluginterfaces gridline_lua ${CMAKE_DL_LIBS})

add_executable(gridline src/cli/main.cpp src/cli/Bench.cpp src/cli/FlpTools.cpp src/cli/McpCli.cpp)
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
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_demo.cpp third_party/imgui/imgui_impl_opengl3.cpp)
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
  add_library(gridline_nanosvg OBJECT src/app/icons/NanoSvgImpl.cpp)
  target_compile_options(gridline_nanosvg PRIVATE ${GL_NOWARN})  # vendored code
  set_target_properties(gridline_nanosvg PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(gridline_icons OBJECT src/app/icons/IconAtlas.cpp src/app/icons/IconTexture.cpp src/app/icons/IconNames.cpp ${GRIDLINE_ICON_DATA})
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

  # J-54: dialog layout checks, headless Dear ImGui (no window, no renderer).
  add_executable(gridline_tests_dialog_layout tests/test_dialog_layout.cpp src/app/DialogLayout.cpp src/app/Theme.cpp)
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

  # The interface (every window and editor) is compiled once and used by the
  # application and by the plugin build (PH-09). main.cpp and PluginWindow.cpp
  # (SDL windows) belong to the application only.
  file(GLOB GRIDLINE_APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/packs/RegistryUi.cpp src/packs/*/*_ui.cpp)
  list(REMOVE_ITEM GRIDLINE_APP_SRC ${CMAKE_SOURCE_DIR}/src/app/main.cpp ${CMAKE_SOURCE_DIR}/src/app/PluginWindow.cpp)
  add_library(gridline_ui OBJECT ${GRIDLINE_APP_SRC})
  target_link_libraries(gridline_ui PRIVATE gridline_core imgui_core rtmidi gridline_sdl_headers)
  target_compile_options(gridline_ui PRIVATE ${GL_WARN_LOOSE})
  set_target_properties(gridline_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_executable(gridline-app src/app/main.cpp src/app/PluginWindow.cpp $<TARGET_OBJECTS:gridline_ui> ${GRIDLINE_ICON_OBJS})
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
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n<key>CFBundleName</key><string>Gridline</string>\n<key>CFBundleDisplayName</key><string>Gridline</string>\n<key>CFBundleExecutable</key><string>gridline-app</string>\n<key>CFBundleIdentifier</key><string>org.gridline.Gridline</string>\n<key>CFBundleIconFile</key><string>Gridline</string>\n<key>CFBundlePackageType</key><string>APPL</string>\n<key>CFBundleShortVersionString</key><string>${PROJECT_VERSION}</string>\n<key>CFBundleVersion</key><string>${PROJECT_VERSION}</string>\n<key>LSApplicationCategoryType</key><string>public.app-category.music</string>\n<key>LSMinimumSystemVersion</key><string>${GL_MACOS_MIN}</string>\n<key>NSHighResolutionCapable</key><true/>\n<key>NSMicrophoneUsageDescription</key><string>Gridline records audio from your input devices.</string>\n</dict></plist>\n")
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
    add_test(NAME editor_layout_audit
             COMMAND gridline-app --layout-audit ${CMAKE_BINARY_DIR}/layout_audit --size 2400x1600
                     --screenshot ${CMAKE_BINARY_DIR}/layout_audit/frame.ppm --frames 100000 --no-ui-sounds
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(editor_layout_audit PROPERTIES ENVIRONMENT "GRIDLINE_CONFIG_HOME=${CMAKE_BINARY_DIR}/layout_audit_cfg" TIMEOUT 900)

    # J-78: clicks on a notification card never reach the playlist clip below it (hidden off-screen run).
    add_test(NAME j78_toast_click
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/demo/demo.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j78_toast_click -P ${CMAKE_SOURCE_DIR}/tests/j78_toast_click.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j78_toast_click PROPERTIES TIMEOUT 300)

    # J-83: wheel over a mixer gain fader: thin mixer scrolls, wide adjusts, Ctrl / click / momentum / live resize.
    add_test(NAME j83_mixer_wheel
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/demo/demo.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j83_mixer_wheel -P ${CMAKE_SOURCE_DIR}/tests/j83_mixer_wheel.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j83_mixer_wheel PROPERTIES TIMEOUT 300)

    # J-84: channel rack with long patterns / narrow racks: knobs drawn, steps scroll, Q-15 wheel routing.
    add_test(NAME j84_rack_scroll
             COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:gridline-app> -DDEMO=${CMAKE_SOURCE_DIR}/demo/demo.gridline.json
                     -DOUT=${CMAKE_BINARY_DIR}/j84_rack_scroll -P ${CMAKE_SOURCE_DIR}/tests/j84_rack_scroll.cmake
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(j84_rack_scroll PROPERTIES TIMEOUT 300)
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
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
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
  set(CPACK_PACKAGE_FILE_NAME "Gridline-${PROJECT_VERSION}-linux-${_gl_rpm_arch}")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${_gl_deb_arch})
  set(CPACK_DEBIAN_FILE_NAME "Gridline-${PROJECT_VERSION}-linux-${_gl_deb_arch}.deb")
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_RPM_PACKAGE_ARCHITECTURE ${_gl_rpm_arch})
  set(CPACK_RPM_FILE_NAME "Gridline-${PROJECT_VERSION}-linux-${_gl_rpm_arch}.rpm")
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
  set(CPACK_RPM_PACKAGE_URL "https://github.com/lolitemaultes/Gridline")
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/mime
      /usr/share/mime/packages /usr/lib/vst3 /usr/lib/clap)
  if(NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
  endif()
  include(CPack)
endif()

