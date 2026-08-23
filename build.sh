#!/bin/bash
# Build ftlsim. Uses system g++ if present, else the portable zig toolchain in ~/tools.
set -e
cd "$(dirname "$0")"
if command -v g++ >/dev/null; then CXX="g++"; else CXX="$HOME/tools/zig-linux-x86_64-0.13.0/zig c++"; fi
$CXX -O2 -std=c++17 -Wall -Wextra ftlsim/ftlsim.cpp -o ftlsim/ftlsim
echo "built ftlsim/ftlsim with: $CXX"
