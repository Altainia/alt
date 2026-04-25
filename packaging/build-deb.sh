#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

VERSION=$(grep -m1 'project(Alt VERSION' "$ROOT_DIR/CMakeLists.txt" \
    | sed 's/.*VERSION \([0-9.]*\).*/\1/')

BUILD_DIR="$ROOT_DIR/build/deb"
STAGING_DIR="$SCRIPT_DIR/staging"
DEB_OUT="$ROOT_DIR/altlib_${VERSION}_amd64.deb"

echo "Building altlib ${VERSION}..."

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/DEBIAN"
mkdir -p "$STAGING_DIR/usr/local"

cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DALT_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    "$ROOT_DIR"

cmake --build "$BUILD_DIR" --parallel

DESTDIR="$STAGING_DIR" cmake --install "$BUILD_DIR"

sed "s/@ALT_VERSION@/${VERSION}/" "$SCRIPT_DIR/control.in" \
    > "$STAGING_DIR/DEBIAN/control"

dpkg-deb --root-owner-group --build "$STAGING_DIR" "$DEB_OUT"

rm -rf "$STAGING_DIR"

echo "Package created: $DEB_OUT"
