# PH-09: Gridline as a plugin. Builds <build>/plugins/Gridline.clap and
# <build>/plugins/Gridline.vst3 (each holds the instrument "Gridline" and the
# effect "Gridline FX"), with the app icon and the assets folder inside the
# bundle. Nothing is installed by the build or the tests:
#   cmake --build <build> --target install-plugins   copies both bundles into the
#       user's plugin folders (~/Library/Audio/Plug-Ins/{CLAP,VST3} on macOS,
#       ~/.clap and ~/.vst3 on Linux)
#   cmake --install <build> --component plugins      installs them under the prefix
#       (lib/clap, lib/vst3 on Linux; Library/Audio/Plug-Ins/... on macOS) for packages
# Included from CMakeLists.txt inside the SDL2 / OpenGL block (the interface
# sources need the SDL2 headers for their GL declarations; nothing links SDL2).
set(GRIDLINE_PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins)

add_library(gridline_plugin_common OBJECT src/plugin/PluginCore.cpp src/plugin/PluginFrame.cpp src/plugin/PluginWindowStub.cpp)
target_link_libraries(gridline_plugin_common PRIVATE gridline_core imgui_core rtmidi gridline_sdl_headers)
target_compile_options(gridline_plugin_common PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(gridline_plugin_common PRIVATE GRIDLINE_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets")
set_target_properties(gridline_plugin_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The editor: Cocoa on macOS, X11 (Xlib + GLX) where Xlib is installed,
# otherwise the plugins run without an editor.
if(APPLE)
  set(GRIDLINE_PLUGIN_VIEW_SRC src/plugin/PluginViewMac.mm)
else()
  find_package(X11 QUIET)
  if(X11_FOUND)
    set(GRIDLINE_PLUGIN_VIEW_SRC src/plugin/PluginViewX11.cpp)
  else()
    message(STATUS "Xlib headers not found: Gridline.clap / Gridline.vst3 are built without an editor (install libx11-dev)")
    set(GRIDLINE_PLUGIN_VIEW_SRC src/plugin/PluginViewNone.cpp)
  endif()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(GRIDLINE_LINUX_ARCH aarch64-linux)
else()
  set(GRIDLINE_LINUX_ARCH ${CMAKE_SYSTEM_PROCESSOR}-linux)
endif()

# gridline_plugin(<target> <format: clap|vst3> <entry source> <Objective-C view class>)
function(gridline_plugin target format entry viewClass)
  add_library(${target} MODULE ${entry} ${GRIDLINE_PLUGIN_VIEW_SRC} $<TARGET_OBJECTS:gridline_plugin_common> $<TARGET_OBJECTS:gridline_ui>)
  target_link_libraries(${target} PRIVATE gridline_core imgui_core rtmidi ${CMAKE_DL_LIBS})
  if(NOT APPLE AND X11_FOUND)
    target_link_libraries(${target} PRIVATE X11::X11)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(${target} PRIVATE GRIDLINE_VERSION_STRING="${PROJECT_VERSION}" GRIDLINE_PLUGIN_VIEW_CLASS=${viewClass})
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME Gridline)
  if(format STREQUAL "clap")
    set(bundle ${GRIDLINE_PLUGIN_DIR}/Gridline.clap)
    set(exports clap_entry)
  else()
    set(bundle ${GRIDLINE_PLUGIN_DIR}/Gridline.vst3)
    if(APPLE)
      set(exports GetPluginFactory bundleEntry bundleExit)
    else()
      set(exports GetPluginFactory ModuleEntry ModuleExit)
    endif()
  endif()
  if(APPLE)
    target_link_libraries(${target} PRIVATE objc "-framework Cocoa" "-framework OpenGL")
    set_target_properties(${target} PROPERTIES SUFFIX "" LIBRARY_OUTPUT_DIRECTORY ${bundle}/Contents/MacOS)
    if(format STREQUAL "clap")
      set(id org.gridline.Gridline.clap)
    else()
      set(id org.gridline.Gridline.vst3)
    endif()
    file(WRITE ${bundle}/Contents/Info.plist
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n<key>CFBundleDevelopmentRegion</key><string>English</string>\n<key>CFBundleName</key><string>Gridline</string>\n<key>CFBundleDisplayName</key><string>Gridline</string>\n<key>CFBundleExecutable</key><string>Gridline</string>\n<key>CFBundleIdentifier</key><string>${id}</string>\n<key>CFBundleIconFile</key><string>Gridline</string>\n<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n<key>CFBundlePackageType</key><string>BNDL</string>\n<key>CFBundleSignature</key><string>????</string>\n<key>CFBundleShortVersionString</key><string>${PROJECT_VERSION}</string>\n<key>CFBundleVersion</key><string>${PROJECT_VERSION}</string>\n<key>LSMinimumSystemVersion</key><string>10.15</string>\n<key>NSHighResolutionCapable</key><true/>\n</dict></plist>\n")
    file(WRITE ${bundle}/Contents/PkgInfo "BNDL????")
    set(list "")
    foreach(sym ${exports})
      string(APPEND list "_${sym}\n")
    endforeach()
    file(WRITE ${CMAKE_BINARY_DIR}/generated/${target}.exports ${list})
    target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_BINARY_DIR}/generated/${target}.exports")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${bundle}/Contents/Resources
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/assets/icons/Gridline.icns ${bundle}/Contents/Resources/Gridline.icns
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${bundle}/Contents/Resources/assets
      COMMAND touch ${bundle}
      COMMENT "Assembling ${bundle}")
  else()
    if(format STREQUAL "clap")  # a single file; its assets sit beside it in Gridline-assets
      set_target_properties(${target} PROPERTIES SUFFIX ".clap" LIBRARY_OUTPUT_DIRECTORY ${GRIDLINE_PLUGIN_DIR})
      set(assetsDir ${GRIDLINE_PLUGIN_DIR}/Gridline-assets)
    else()
      set_target_properties(${target} PROPERTIES SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY ${bundle}/Contents/${GRIDLINE_LINUX_ARCH})
      set(assetsDir ${bundle}/Contents/Resources/assets)
    endif()
    set(list "{\n  global:\n")
    foreach(sym ${exports})
      string(APPEND list "    ${sym};\n")
    endforeach()
    string(APPEND list "  local: *;\n};\n")
    file(WRITE ${CMAKE_BINARY_DIR}/generated/${target}.map ${list})
    target_link_options(${target} PRIVATE "LINKER:--version-script=${CMAKE_BINARY_DIR}/generated/${target}.map" "LINKER:-Bsymbolic")
    target_link_libraries(${target} PRIVATE OpenGL::GL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${assetsDir}
      COMMENT "Assembling the ${format} plugin")
  endif()
endfunction()

gridline_plugin(gridline_clap clap src/plugin/ClapEntry.cpp GridlineClapGLView)
gridline_plugin(gridline_vst3 vst3 src/plugin/Vst3Entry.cpp GridlineVst3GLView)
set(GRIDLINE_CLAP_BUNDLE ${GRIDLINE_PLUGIN_DIR}/Gridline.clap)
set(GRIDLINE_VST3_BUNDLE ${GRIDLINE_PLUGIN_DIR}/Gridline.vst3)

# ---- installing (never done by the build or the tests) ----
if(APPLE)
  set(GRIDLINE_USER_CLAP "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
  set(GRIDLINE_USER_VST3 "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
  install(DIRECTORY ${GRIDLINE_CLAP_BUNDLE} DESTINATION Library/Audio/Plug-Ins/CLAP COMPONENT plugins EXCLUDE_FROM_ALL USE_SOURCE_PERMISSIONS)
  install(DIRECTORY ${GRIDLINE_VST3_BUNDLE} DESTINATION Library/Audio/Plug-Ins/VST3 COMPONENT plugins EXCLUDE_FROM_ALL USE_SOURCE_PERMISSIONS)
  add_custom_target(install-plugins
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GRIDLINE_USER_CLAP} ${GRIDLINE_USER_VST3}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GRIDLINE_CLAP_BUNDLE} ${GRIDLINE_USER_CLAP}/Gridline.clap
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GRIDLINE_VST3_BUNDLE} ${GRIDLINE_USER_VST3}/Gridline.vst3
    DEPENDS gridline_clap gridline_vst3
    COMMENT "Installing Gridline.clap and Gridline.vst3 into ${GRIDLINE_USER_CLAP} and ${GRIDLINE_USER_VST3}"
    VERBATIM)
else()
  set(GRIDLINE_USER_CLAP "$ENV{HOME}/.clap")
  set(GRIDLINE_USER_VST3 "$ENV{HOME}/.vst3")
  install(FILES ${GRIDLINE_CLAP_BUNDLE} DESTINATION lib/clap COMPONENT plugins EXCLUDE_FROM_ALL)
  install(DIRECTORY ${GRIDLINE_PLUGIN_DIR}/Gridline-assets DESTINATION lib/clap COMPONENT plugins EXCLUDE_FROM_ALL)
  install(DIRECTORY ${GRIDLINE_VST3_BUNDLE} DESTINATION lib/vst3 COMPONENT plugins EXCLUDE_FROM_ALL USE_SOURCE_PERMISSIONS)
  add_custom_target(install-plugins
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GRIDLINE_USER_CLAP} ${GRIDLINE_USER_VST3}
    COMMAND ${CMAKE_COMMAND} -E copy ${GRIDLINE_CLAP_BUNDLE} ${GRIDLINE_USER_CLAP}/Gridline.clap
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GRIDLINE_PLUGIN_DIR}/Gridline-assets ${GRIDLINE_USER_CLAP}/Gridline-assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GRIDLINE_VST3_BUNDLE} ${GRIDLINE_USER_VST3}/Gridline.vst3
    DEPENDS gridline_clap gridline_vst3
    COMMENT "Installing Gridline.clap and Gridline.vst3 into ${GRIDLINE_USER_CLAP} and ${GRIDLINE_USER_VST3}"
    VERBATIM)
endif()

# ---- tests: the bundles loaded by Gridline's own plugin host ----
add_executable(gridline_tests_plugin_build tests/test_plugin_build.cpp)
target_link_libraries(gridline_tests_plugin_build PRIVATE gridline_core)
target_include_directories(gridline_tests_plugin_build PRIVATE tests)
target_compile_options(gridline_tests_plugin_build PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(gridline_tests_plugin_build PRIVATE GRIDLINE_PLUGIN_CLAP="${GRIDLINE_CLAP_BUNDLE}"
                           GRIDLINE_PLUGIN_VST3="${GRIDLINE_VST3_BUNDLE}" GRIDLINE_CLI_EXE="$<TARGET_FILE:gridline>")
add_dependencies(gridline_tests_plugin_build gridline_clap gridline_vst3 gridline)
add_test(NAME plugin_build COMMAND gridline_tests_plugin_build WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
