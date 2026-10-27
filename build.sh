#!/bin/sh
# Gridline dev build + test + demo render (works on macOS and Linux).
set -e
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
FLAGS="-std=c++17 -O2 -Wall -Wextra -pedantic -Isrc -isystem third_party"
CORE="$(ls src/core/*.cpp src/core/flp/*.cpp src/engine/*.cpp src/packs/Registry.cpp src/packs/*/*_info.cpp src/packs/*/*_dsp.cpp)"

mkdir -p build
# The demo song is compiled in (gl::demoSongJson()).
cmake -DIN=assets/demo/Gridline.gridline.json -DOUT=build/DemoSongData.cpp -P cmake/EmbedDemo.cmake
CORE="$CORE build/DemoSongData.cpp"
echo "== building =="
"$CXX" $FLAGS $CORE src/cli/main.cpp -o build/gridline
"$CXX" $FLAGS $CORE tests/test_engine.cpp -o build/gridline_tests
"$CXX" $FLAGS $CORE tests/test_m1.cpp -o build/gridline_tests_m1

echo "== tests =="
./build/gridline_tests
./build/gridline_tests_m1

echo "== demo render =="
./build/gridline demo build/demo.gridline.json
./build/gridline render build/demo.gridline.json -o build/demo.wav --sr 44100 --bits 16 --normalize
./build/gridline info build/demo.gridline.json
echo "done: build/demo.wav"
