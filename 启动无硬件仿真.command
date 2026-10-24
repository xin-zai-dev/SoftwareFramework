#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "$0")" && pwd)"
app_bundle="$project_dir/build/macos/IndustrialVisionStudio.app"
if [[ ! -x "$app_bundle/Contents/MacOS/IndustrialVisionStudio" ]]; then
    cmake_bin="$(command -v cmake || true)"
    if [[ -z "$cmake_bin" ]]; then
        cmake_bin="$HOME/Qt/Tools/CMake/CMake.app/Contents/bin/cmake"
    fi
    "$cmake_bin" --preset macos-local -S "$project_dir"
    "$cmake_bin" --build "$project_dir/build/macos" --target IndustrialVisionStudio --parallel 4
fi
open -n "$app_bundle" --args --project "$project_dir/examples/simulation-only.ivproj" --workspace "$project_dir/simulation-lab" --simulation-console
