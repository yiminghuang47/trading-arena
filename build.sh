#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
FLAGS=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude)

echo "Building arena..."
g++ "${FLAGS[@]}" src/arena_main.cpp -o build/arena
echo "Building tests..."
g++ "${FLAGS[@]}" tests/test_book.cpp -o build/test_book
echo "Build OK -> build/"

if [[ "${1:-}" == "--run" ]]; then
  echo; echo "=== tests ==="
  ./build/test_book
  echo; echo "=== running arena ==="
  ./build/arena
fi
