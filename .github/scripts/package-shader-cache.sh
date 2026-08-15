#!/usr/bin/env bash
# Zip a prepared ./ShaderCache directory into a distributable .7z. Includes an
# empty SKSE/ folder alongside it so MO2/VFS installers root-detect the archive
# correctly -- "ShaderCache" alone isn't a folder name their heuristics
# recognize, so without an anchor like SKSE the mod can install one directory
# too deep and never reach Data/ShaderCache/.
#
# Usage: package-shader-cache.sh <output.7z>
set -euo pipefail

output="$1"
mkdir -p SKSE
7z a -t7z -mx=7 "$output" ./ShaderCache ./SKSE
