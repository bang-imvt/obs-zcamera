#!/usr/bin/env bash
# Build the obs-zcamera macOS plugin locally and package it into a .plugin
# bundle. Caches the OBS/Qt6 archives, stages them for the project's dependency
# bootstrap, then configures, builds and installs the bundle.
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
#   * Requires cmake and a full Xcode installation selected by xcode-select.

set -euo pipefail
cd "$(dirname "$0")"

DEPS_TAG="2024-09-12"
CACHE="${ZC_DEPS_CACHE:-$HOME/.cache/obs-zcamera-deps}"
DEPS_DIR="$PWD/.deps"
BUILD_DIR="build-mac"
BUILD_CONFIG="${ZC_BUILD_CONFIG:-Release}"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
mkdir -p "$CACHE" "$DEPS_DIR"

# OBS's Qt 6.6.3 still links AGL, but the macOS 26 SDK only ships the runtime
# framework without AGL.tbd. Reuse the newest legacy SDK that still has it.
AGL_FRAMEWORK_DIR=""
for sdk in /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk; do
  if [ -f "$sdk/System/Library/Frameworks/AGL.framework/AGL.tbd" ]; then
    AGL_FRAMEWORK_DIR="$sdk/System/Library/Frameworks"
  fi
done
LINKER_FLAGS=()
if [ -n "$AGL_FRAMEWORK_DIR" ]; then
  LINKER_FLAGS=(
    "-DCMAKE_MODULE_LINKER_FLAGS=-F$AGL_FRAMEWORK_DIR"
    "-DCMAKE_SHARED_LINKER_FLAGS=-F$AGL_FRAMEWORK_DIR"
  )
fi

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

echo "== staging cached dependency archives =="
for a in "${ASSETS[@]}"; do
  if [ ! -s "$DEPS_DIR/$a" ] || ! cmp -s "$CACHE/$a" "$DEPS_DIR/$a"; then
    echo "   $a"
    cp -p "$CACHE/$a" "$DEPS_DIR/$a"
  fi
done

echo "== configure =="
# OBS's macOS CMake configuration requires the Xcode generator.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cached_generator="$(awk -F= '/^CMAKE_GENERATOR:INTERNAL=/{print $2; exit}' \
    "$BUILD_DIR/CMakeCache.txt")"
  if [ -n "$cached_generator" ] && [ "$cached_generator" != "Xcode" ]; then
    echo "== removing stale $BUILD_DIR cache ($cached_generator) =="
    cmake -E remove_directory "$BUILD_DIR"
  fi
fi

cmake -S . -B "$BUILD_DIR" -G Xcode \
  -DCMAKE_FIND_FRAMEWORK=LAST \
  -DCMAKE_PREFIX_PATH= \
  -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  "${LINKER_FLAGS[@]}" \
  -DENABLE_ZERO_COPY=ON -DENABLE_SW_DECODE=ON -DENABLE_CAMERA_CONTROL=ON

echo "== build =="
cmake --build "$BUILD_DIR" --config "$BUILD_CONFIG" \
  --parallel "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "== package (install the .plugin bundle) =="
rm -rf dist-mac
cmake --install "$BUILD_DIR" --config "$BUILD_CONFIG" --prefix "$PWD/dist-mac"

echo ""
echo "Plugin bundle: $PWD/dist-mac/obs-zcamera.plugin"
echo "Install into OBS, then restart OBS Studio."
