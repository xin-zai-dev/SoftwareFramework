#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
qt_prefix="${QT_ROOT:-$HOME/Qt/6.11.2/macos}"
cmake_bin="$(command -v cmake || true)"
if [[ -z "$cmake_bin" ]]; then
    cmake_bin="$HOME/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
fi
ninja_bin="${NINJA_BIN:-$HOME/Qt/Tools/Ninja/ninja}"
"$cmake_bin" -S "$project_dir" -B "$project_dir/build/macos" -G Ninja \
    -DCMAKE_PREFIX_PATH="$qt_prefix" -DCMAKE_MAKE_PROGRAM="$ninja_bin" -DCMAKE_BUILD_TYPE=RelWithDebInfo
"$cmake_bin" --build "$project_dir/build/macos" --parallel 4
open "$project_dir/build/macos/IndustrialVisionStudio.app"
