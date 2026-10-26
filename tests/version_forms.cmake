# Gridline - J-125: the forms of the version agree everywhere. The version is set at the top of
# CMakeLists.txt (project(VERSION x.y.z) + GRIDLINE_VERSION_PRE); cmake/GridlineVersion.cmake and
# tools/release/version.sh derive the full version (0.7.0-beta.1), the numbers (0.7.0), the Debian
# version (0.7.0~beta.1) and the rpm Release (0.1.beta.1). Checked: both derivations for a
# prerelease and a plain release, the generated GridlineVersion.h, `gridline --version`, and the
# numbers-only versions of the macOS bundles and the Windows version resources of this build.
#   cmake -DSRC=<source dir> -DBIN=<build dir> -DCLI=<gridline program> -P tests/version_forms.cmake
cmake_minimum_required(VERSION 3.16)
include(${SRC}/cmake/GridlineVersion.cmake)

set(failures 0)
# `text` (a variable name) contains `needle` literally.
macro(has text needle what)
  string(FIND "${${text}}" "${needle}" at)
  if(at GREATER -1)
    message(STATUS "ok: ${what}")
  else()
    message(SEND_ERROR "FAIL: ${what}: no '${needle}'")
    math(EXPR failures "${failures} + 1")
  endif()
endmacro()
macro(same a b what)
  if("${a}" STREQUAL "${b}")
    message(STATUS "ok: ${what} (${a})")
  else()
    message(SEND_ERROR "FAIL: ${what}: '${a}' != '${b}'")
    math(EXPR failures "${failures} + 1")
  endif()
endmacro()

# ---- the derivation ----
gridline_version_forms(0.7.0 "beta.1" V)
same("${V_FULL}" "0.7.0-beta.1" "beta: full version")
same("${V_NUMERIC}" "0.7.0" "beta: numbers")
same("${V_DEB}" "0.7.0~beta.1" "beta: Debian version")
same("${V_RPM_RELEASE}" "0.1.beta.1" "beta: rpm Release")
gridline_version_forms(0.7.0 "" V)
same("${V_FULL}" "0.7.0" "release: full version")
same("${V_NUMERIC}" "0.7.0" "release: numbers")
same("${V_DEB}" "0.7.0" "release: Debian version")
same("${V_RPM_RELEASE}" "1" "release: rpm Release")
gridline_version_forms(1.10.3 "rc.2" V)
same("${V_FULL}" "1.10.3-rc.2" "rc: full version")
same("${V_DEB}" "1.10.3~rc.2" "rc: Debian version")

# ---- this tree ----
file(STRINGS ${SRC}/CMakeLists.txt projLine REGEX "^project\\(gridline VERSION [0-9.]+ ")
string(REGEX REPLACE "^project\\(gridline VERSION ([0-9.]+) .*" "\\1" numeric "${projLine}")
file(STRINGS ${SRC}/CMakeLists.txt preLine REGEX "^set\\(GRIDLINE_VERSION_PRE \"[0-9A-Za-z.]*\"\\)")
list(LENGTH preLine preCount)
same("${preCount}" "1" "one set(GRIDLINE_VERSION_PRE \"...\") line in CMakeLists.txt")
string(REGEX REPLACE "^set\\(GRIDLINE_VERSION_PRE \"([0-9A-Za-z.]*)\"\\).*" "\\1" pre "${preLine}")
gridline_version_forms(${numeric} "${pre}" CUR)
message(STATUS "this tree: ${CUR_FULL} (numbers ${CUR_NUMERIC}, label '${pre}')")

file(READ ${BIN}/generated/GridlineVersion.h header)
has(header "#define GRIDLINE_VERSION \"${CUR_FULL}\"\n" "GridlineVersion.h: GRIDLINE_VERSION is the full version")
has(header "#define GRIDLINE_VERSION_NUMERIC \"${CUR_NUMERIC}\"\n" "GridlineVersion.h: the numbers")
has(header "#define GRIDLINE_VERSION_PRE \"${pre}\"\n" "GridlineVersion.h: the label")

execute_process(COMMAND ${CLI} --version OUTPUT_VARIABLE out RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\r" "" out "${out}")
same("${rc}" "0" "gridline --version exits 0")
same("${out}" "Gridline ${CUR_FULL}" "gridline --version prints the full version")

# tools/release/version.sh (release.yml, check-version.sh) gives the same forms, for this tree
# and for the same tree as a plain release (label emptied).
find_program(BASH bash)
if(BASH AND NOT CMAKE_HOST_WIN32)
  file(READ ${SRC}/CMakeLists.txt lists)
  set(tmp ${BIN}/version_forms)
  file(MAKE_DIRECTORY ${tmp})
  string(REGEX REPLACE "\nset\\(GRIDLINE_VERSION_PRE \"[0-9A-Za-z.]*\"\\)" "\nset(GRIDLINE_VERSION_PRE \"\")" plain "${lists}")
  file(WRITE ${tmp}/CMakeLists-plain.txt "${plain}")
  string(REGEX REPLACE "\nset\\(GRIDLINE_VERSION_PRE \"[0-9A-Za-z.]*\"\\)" "\nset(GRIDLINE_VERSION_PRE \"beta.7\")" beta "${lists}")
  file(WRITE ${tmp}/CMakeLists-beta.txt "${beta}")
  foreach(case "${SRC}/CMakeLists.txt|${pre}" "${tmp}/CMakeLists-plain.txt|" "${tmp}/CMakeLists-beta.txt|beta.7")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 file)
    list(LENGTH case n)
    set(label "")
    if(n GREATER 1)
      list(GET case 1 label)
    endif()
    gridline_version_forms(${numeric} "${label}" W)
    execute_process(COMMAND ${BASH} ${SRC}/tools/release/version.sh ${file} OUTPUT_VARIABLE got RESULT_VARIABLE rc)
    same("${rc}" "0" "version.sh runs (label '${label}')")
    same("${got}" "numeric=${W_NUMERIC}\npre=${label}\nfull=${W_FULL}\ndeb=${W_DEB}\nrpm_release=${W_RPM_RELEASE}\n"
         "version.sh agrees with GridlineVersion.cmake (label '${label}')")
  endforeach()
endif()

# macOS: every bundle's Info.plist carries the numbers only (CFBundleShortVersionString and
# CFBundleVersion must be numbers); bundles that are not built in this configuration are skipped.
foreach(plist ${BIN}/Gridline.app/Contents/Info.plist ${BIN}/plugins/Gridline.vst3/Contents/Info.plist
        ${BIN}/plugins/Gridline.clap/Contents/Info.plist ${BIN}/plugins/Gridline.component/Contents/Info.plist)
  if(EXISTS ${plist})
    file(READ ${plist} p)
    has(p "<key>CFBundleShortVersionString</key><string>${CUR_NUMERIC}</string>" "${plist}: CFBundleShortVersionString ${CUR_NUMERIC}")
    has(p "<key>CFBundleVersion</key><string>${CUR_NUMERIC}</string>" "${plist}: CFBundleVersion ${CUR_NUMERIC}")
    if(plist MATCHES "component")
      # AUv2 version: 0xMMMMmmbb as an integer.
      string(REPLACE "." ";" parts "${CUR_NUMERIC}")
      list(GET parts 0 ma)
      list(GET parts 1 mi)
      list(GET parts 2 pa)
      math(EXPR au "${ma} * 65536 + ${mi} * 256 + ${pa}")
      has(p "<key>version</key><integer>${au}</integer>" "${plist}: AU version ${au}")
    endif()
  endif()
endforeach()

# Windows: the binary version is numbers only, the strings the full version.
file(GLOB rcs ${BIN}/generated/*.rc)
foreach(rcFile ${rcs})
  file(READ ${rcFile} r)
  string(REPLACE "." "," commas "${CUR_NUMERIC}")
  has(r "FILEVERSION ${commas},0" "${rcFile}: FILEVERSION ${commas},0")
  has(r "\"ProductVersion\", \"${CUR_FULL}\"" "${rcFile}: ProductVersion ${CUR_FULL}")
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} version check(s) failed")
endif()
