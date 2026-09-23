#!/usr/bin/env bash
# Build the obs-zcamera macOS plugin locally and package it into a .plugin
# bundle. Downloads the OBS dependencies (libobs/obs-frontend-api/Qt6) from the
# official obs-deps release if they are not cached, then configures, builds and
# installs the bundle.
#
# Just run:  bash build_mac.sh
#
# Result:  dist-mac/obs-zcamera.plugin  ->  drop into OBS's plugins dir, e.g.
#     open /Applications/obs-studio.app/Contents/Resources/
#   or the user plugin dir  ~/Library/Application\ Support/obs-studio/plugins/
#
# Notes:
#   * The plugin links OBS's patched Qt6 (the -qt6- deps) so it ABI-matches the
#     OBS it runs in.
#   * On Apple Silicon the ssp-connector target is pinned to x86_64 by CMake, so
#     the bundled connector needs Rosetta 2 installed to run.
#   * Requires cmake and a working C++ toolchain (Xcode Command Line Tools).

set -euo pipefail
cd "$(dirname "$0")"

DEPS_TAG="2024-09-12"
CACHE="${ZC_DEPS_CACHE:-$HOME/.cache/obs-zcamera-deps}"
DEPS="$CACHE/prefix"
mkdir -p "$CACHE" "$DEPS"

ASSETS=(
  "macos-deps-${DEPS_TAG}-universal.tar.xz"
  "macos-deps-qt6-${DEPS_TAG}-universal.tar.xz"
)

need_dl=0
for a in "${ASSETS[@]}"; do
  [ -f "$CACHE/$a" ] || need_dl=1
done
if [ "$need_dl" = "1" ]; then
  echo "== downloading OBS deps ($DEPS_TAG) =="
  url_base="https://github.com/obsproject/obs-deps/releases/download/${DEPS_TAG}"
  for a in "${ASSETS[@]}"; do
    if [ ! -f "$CACHE/$a" ]; then
      echo "   $a"
      curl -fL "$url_base/$a" -o "$CACHE/$a"
    fi
  done
fi

echo "== extracting deps =="
for a in "${ASSETS[@]}"; do
  tar -xf "$CACHE/$a" -C "$DEPS"
done

echo "== configure =="
cmake -S . -B build-mac -G "Unix Makefiles" \
  -DCMAKE_PREFIX_PATH="$DEPS" \
  -DENABLE_ZERO_COPY=ON -DENABLE_SW_DECODE=ON -DENABLE_CAMERA_CONTROL=ON

echo "== build =="
cmake --build build-mac -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "== package (install the .plugin bundle) =="
rm -rf dist-mac
cmake --install build-mac --prefix "$PWD/dist-mac"

echo ""
echo "Plugin bundle: $PWD/dist-mac/obs-zcamera.plugin"
echo "Install into OBS, then restart OBS Studio."