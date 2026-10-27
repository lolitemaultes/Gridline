# J-135: writes the demo song JSON into a C++ source as a byte array (gl::demoSongJson()).
# A byte array, not a string literal: MSVC limits the length of string literals, not of array initialisers.
function(gridline_embed_demo in out)
  file(READ "${in}" hex HEX)
  string(LENGTH "${hex}" n)
  math(EXPR size "${n} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # 16 bytes per line
  set(b "0x[0-9a-f][0-9a-f],")
  set(quad "${b}${b}${b}${b}")
  string(REGEX REPLACE "(${quad}${quad}${quad}${quad})" "\\1\n" bytes "${bytes}")
  set(src "// Generated from assets/demo/Gridline.gridline.json by cmake/EmbedDemo.cmake: do not edit.\n#include <string_view>\n\nnamespace gl {\nnamespace {\nconst unsigned char kDemoJson[] = {\n${bytes}0x00};\n}  // namespace\nstd::string_view demoSongJson();\nstd::string_view demoSongJson() { return std::string_view(reinterpret_cast<const char*>(kDemoJson), ${size}); }\n}  // namespace gl\n")
  if(EXISTS "${out}")
    file(READ "${out}" old)
    if(old STREQUAL src)
      return()
    endif()
  endif()
  file(WRITE "${out}" "${src}")
endfunction()

# Script mode (build.sh): cmake -DIN=<json> -DOUT=<cpp> -P cmake/EmbedDemo.cmake
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED IN AND DEFINED OUT)
  gridline_embed_demo("${IN}" "${OUT}")
endif()
