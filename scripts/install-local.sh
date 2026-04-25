#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$ROOT_DIR/build/local}"

cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DALT_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    "$ROOT_DIR"

cmake --build "$BUILD_DIR" --parallel

sudo cmake --install "$BUILD_DIR"

echo "alt installed to /usr/local"
