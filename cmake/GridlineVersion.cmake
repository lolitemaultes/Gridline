# J-125: the forms of Gridline's version. The version is set in one place, at the top of
# CMakeLists.txt: project(VERSION <major>.<minor>.<patch>) and GRIDLINE_VERSION_PRE, a prerelease
# label ("beta.1"; empty for a release). tools/release/version.sh reads the same two lines and
# must give the same forms (tests/version_forms.cmake checks both).
#
#   gridline_version_forms(<numeric> <pre> <prefix>) sets in the caller:
#     <prefix>_FULL         0.7.0-beta.1 / 0.7.0   shown everywhere, file names, the git tag
#     <prefix>_NUMERIC      0.7.0                  where a platform wants numbers only (macOS bundle
#                                                  versions, Windows version resources, AU, rpm Version)
#     <prefix>_DEB          0.7.0~beta.1 / 0.7.0   Debian version ('~' sorts before the release)
#     <prefix>_RPM_RELEASE  0.1.beta.1 / 1         rpm Release (Fedora's prerelease convention)
function(gridline_version_forms numeric pre prefix)
  if(NOT numeric MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Gridline version '${numeric}' is not <major>.<minor>.<patch>")
  endif()
  if(pre STREQUAL "")
    set(full "${numeric}")
    set(deb "${numeric}")
    set(rpmRelease "1")
  else()
    if(NOT pre MATCHES "^[0-9A-Za-z]+(\\.[0-9A-Za-z]+)*$")
      message(FATAL_ERROR "Gridline prerelease label '${pre}' must be dot-separated letters and digits (e.g. beta.1)")
    endif()
    set(full "${numeric}-${pre}")
    set(deb "${numeric}~${pre}")
    set(rpmRelease "0.1.${pre}")
  endif()
  set(${prefix}_FULL "${full}" PARENT_SCOPE)
  set(${prefix}_NUMERIC "${numeric}" PARENT_SCOPE)
  set(${prefix}_DEB "${deb}" PARENT_SCOPE)
  set(${prefix}_RPM_RELEASE "${rpmRelease}" PARENT_SCOPE)
endfunction()
