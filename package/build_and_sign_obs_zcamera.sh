#!/usr/bin/env bash
#
# Build, sign, package, and notarize the macOS OBS plugin bundle.
#
# Usage:
#   bash package/build_and_sign_obs_zcamera.sh
#   bash package/build_and_sign_obs_zcamera.sh --no-build
#   bash package/build_and_sign_obs_zcamera.sh --skip-notarize
#
# Result:
#   package/release/obs-zcamera-<version>.pkg
#
# The installer places:
#   /Library/Application Support/obs-studio/plugins/obs-zcamera.plugin

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT="$SCRIPT_DIR/build_release"
STAGE_ROOT="$BUILD_ROOT/root"
SCRIPTS_DIR="$BUILD_ROOT/scripts"
RELEASE_DIR="$SCRIPT_DIR/release"

PLUGIN_NAME="obs-zcamera"
BUNDLE_NAME="$PLUGIN_NAME.plugin"
PKG_ID="com.imvt.obs-zcamera"
INSTALL_PARENT="/Library/Application Support/obs-studio/plugins"
SOURCE_BUNDLE="$PROJECT_DIR/dist-mac/$BUNDLE_NAME"
STAGED_BUNDLE="$STAGE_ROOT$INSTALL_PARENT/$BUNDLE_NAME"
FINAL_PKG=""

APP_IDENTITY="${APP_IDENTITY:-Developer ID Application: ImagineVision Tech Co., Ltd (2P64WGQM23)}"
INSTALLER_IDENTITY="${INSTALLER_IDENTITY:-Developer ID Installer: ImagineVision Tech Co., Ltd (2P64WGQM23)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-zcam_app}"

DO_BUILD=1
DO_NOTARIZE=1

usage() {
  sed -n '2,18p' "$0"
}

for arg in "$@"; do
  case "$arg" in
    --no-build)
      DO_BUILD=0
      ;;
    --skip-notarize)
      DO_NOTARIZE=0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for tool in codesign pkgbuild productbuild security xcrun plutil ditto xattr; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool" >&2
    exit 1
  fi
done

VERSION="$(plutil -extract version raw -o - "$PROJECT_DIR/buildspec.json")"
BUNDLE_ID="$(plutil -extract platformConfig.macos.bundleId raw -o - "$PROJECT_DIR/buildspec.json")"
DISPLAY_NAME="$(plutil -extract displayName raw -o - "$PROJECT_DIR/buildspec.json")"
FINAL_PKG="$RELEASE_DIR/${PLUGIN_NAME}-${VERSION}.pkg"

echo "=========================================="
echo "obs-zcamera macOS plugin release packaging"
echo "=========================================="
echo "Project:             $PROJECT_DIR"
echo "Version:             $VERSION"
echo "Bundle ID:           $BUNDLE_ID"
echo "App identity:        $APP_IDENTITY"
echo "Installer identity:  $INSTALLER_IDENTITY"
echo "Notary profile:      $NOTARY_PROFILE"
echo "Output:              $FINAL_PKG"
echo ""

echo "== verifying signing identities =="
IDENTITIES="$(security find-identity -v -p basic)"
for identity in "$APP_IDENTITY" "$INSTALLER_IDENTITY"; do
  if ! grep -F "\"$identity\"" <<<"$IDENTITIES" >/dev/null; then
    echo "Missing signing identity: $identity" >&2
    echo "$IDENTITIES" >&2
    exit 1
  fi
done

if [ "$DO_BUILD" = "1" ]; then
  echo ""
  echo "== building plugin bundle =="
  (cd "$PROJECT_DIR" && bash build_mac.sh)
else
  echo ""
  echo "== using existing build output =="
fi

if [ ! -d "$SOURCE_BUNDLE" ]; then
  echo "Plugin bundle not found: $SOURCE_BUNDLE" >&2
  exit 1
fi

echo ""
echo "== staging signed plugin bundle =="
rm -rf "$BUILD_ROOT" "$RELEASE_DIR"
mkdir -p "$STAGED_BUNDLE/Contents" "$SCRIPTS_DIR" "$RELEASE_DIR"
ditto --norsrc --noextattr --noacl --noqtn "$SOURCE_BUNDLE" "$STAGED_BUNDLE"

INFO_PLIST="$STAGED_BUNDLE/Contents/Info.plist"
cat > "$INFO_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>$PLUGIN_NAME</string>
	<key>CFBundleDisplayName</key>
	<string>$DISPLAY_NAME</string>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CFBundleVersion</key>
	<string>$VERSION</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleExecutable</key>
	<string>$PLUGIN_NAME</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>MacOSX</string>
	</array>
</dict>
</plist>
EOF

cat > "$SCRIPTS_DIR/preinstall" <<'EOF'
#!/bin/sh
set -e

OLD_BUNDLE="/Library/Application Support/obs-studio/plugins/obs-zcamera.plugin"
if [ -e "$OLD_BUNDLE" ]; then
    rm -rf "$OLD_BUNDLE"
fi

exit 0
EOF
chmod 755 "$SCRIPTS_DIR/preinstall"

cat > "$SCRIPTS_DIR/postinstall" <<'EOF'
#!/bin/sh
set -e

# OBS on Apple Silicon does not scan /Library/Application Support; it only
# scans the current user's ~/Library/Application Support path. Intel OBS still
# uses the legacy system path, so keep the pkg payload there and mirror it into
# the console user's plugin directory for modern arm64 builds.
SYSTEM_BUNDLE="/Library/Application Support/obs-studio/plugins/obs-zcamera.plugin"
CONSOLE_USER="$(stat -f %Su /dev/console 2>/dev/null || true)"

case "$CONSOLE_USER" in
    ""|root|loginwindow)
        exit 0
        ;;
esac

USER_HOME="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
if [ -z "$USER_HOME" ] || [ ! -d "$USER_HOME" ]; then
    exit 0
fi

USER_PLUGIN_DIR="$USER_HOME/Library/Application Support/obs-studio/plugins"
USER_BUNDLE="$USER_PLUGIN_DIR/obs-zcamera.plugin"

mkdir -p "$USER_PLUGIN_DIR"
rm -rf "$USER_BUNDLE"
ditto --norsrc --noextattr --noacl --noqtn "$SYSTEM_BUNDLE" "$USER_BUNDLE"
chown -R "$CONSOLE_USER" "$USER_BUNDLE"

exit 0
EOF
chmod 755 "$SCRIPTS_DIR/postinstall"

# packagebuild records extended attributes as AppleDouble `._*` entries in the
# pkg payload. Strip them from the staging tree before the binaries are signed.
xattr -cr "$STAGE_ROOT"
find "$STAGE_ROOT" -name '._*' -delete

echo "== signing nested binaries and plugin bundle =="
sign_path() {
  echo "   signing $1"
  codesign --force --options runtime --timestamp \
    --sign "$APP_IDENTITY" "$1"
}

sign_path "$STAGED_BUNDLE/Contents/Frameworks/libssp.dylib"
sign_path "$STAGED_BUNDLE/Contents/MacOS/ssp-connector"
sign_path "$STAGED_BUNDLE/Contents/MacOS/obs-zcamera"
sign_path "$STAGED_BUNDLE"

echo "== verifying plugin bundle signature =="
codesign --verify --deep --strict --verbose=2 "$STAGED_BUNDLE"
SIGNED_PLUGIN_OUT="$RELEASE_DIR/$BUNDLE_NAME"
ditto --norsrc --noextattr --noacl --noqtn "$STAGED_BUNDLE" "$SIGNED_PLUGIN_OUT"

echo ""
echo "== creating component package =="
COMPONENT_PLIST="$BUILD_ROOT/components.plist"
cat > "$COMPONENT_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<dict>
		<key>BundleIsVersionChecked</key>
		<false/>
		<key>BundleOverwriteAction</key>
		<string>upgrade</string>
		<key>RootRelativeBundlePath</key>
		<string>Library/Application Support/obs-studio/plugins/$BUNDLE_NAME</string>
	</dict>
</array>
</plist>
EOF

COMPONENT_PKG="$BUILD_ROOT/$PLUGIN_NAME-component.pkg"
COPYFILE_DISABLE=1 pkgbuild \
  --root "$STAGE_ROOT" \
  --component-plist "$COMPONENT_PLIST" \
  --scripts "$SCRIPTS_DIR" \
  --identifier "$PKG_ID" \
  --version "$VERSION" \
  --install-location "/" \
  "$COMPONENT_PKG"

echo ""
echo "== creating signed installer package =="
productbuild \
  --package "$COMPONENT_PKG" \
  --sign "$INSTALLER_IDENTITY" \
  "$FINAL_PKG"

echo ""
echo "== verifying installer signature =="
pkgutil --check-signature "$FINAL_PKG"

if [ "$DO_NOTARIZE" = "1" ]; then
  echo ""
  echo "== submitting to Apple notary service =="
  NOTARY_JSON="$BUILD_ROOT/notary-result.json"
  xcrun notarytool submit "$FINAL_PKG" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait --timeout 1h --output-format json | tee "$NOTARY_JSON"

  NOTARY_ID="$(plutil -extract id raw -o - "$NOTARY_JSON")"
  echo "Notary ID: $NOTARY_ID"

  echo ""
  echo "== stapling notarization ticket =="
  xcrun stapler staple "$FINAL_PKG"
  xcrun stapler validate "$FINAL_PKG"
fi

echo ""
echo "== final Gatekeeper check =="
spctl -a -vv -t install "$FINAL_PKG"

echo ""
echo "=========================================="
echo "Release package complete"
echo "=========================================="
echo "Package: $FINAL_PKG"
echo "Plugin:  $SIGNED_PLUGIN_OUT"
if [ "$DO_NOTARIZE" = "1" ]; then
  echo "Notary:  ${NOTARY_ID:-unknown}"
fi
