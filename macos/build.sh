#!/bin/bash
# Builds build/Ather Screenshot.app.
#   ./build.sh            release build for this Mac's architecture
#   ./build.sh debug      debug build
#   ./build.sh package    universal (arm64 + x86_64) release + dist/*.zip, *.dmg, SHA256SUMS.txt
# Signing: ad-hoc by default. Set SIGN_IDENTITY="Developer ID Application: …" to sign for distribution,
# and NOTARY_PROFILE=<notarytool keychain profile> with `package` to notarize the DMG.
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-release}"
CONFIG=release
[ "$MODE" = "debug" ] && CONFIG=debug
ARCHS=()
[ "$MODE" = "package" ] && ARCHS=(--arch arm64 --arch x86_64)

# The version lives in ../src/version.h, shared with the Windows build.
VERSION=$(sed -n 's/.*ATHER_VERSION_STR "\(.*\)".*/\1/p' ../src/version.h)
# The build (CFBundleVersion) is what the updater compares; re-releases of a version raise only the build.
BUILD_NUM=$(sed -n 's/.*ATHER_BUILD_STR "\(.*\)".*/\1/p' ../src/version.h)

# `swift` on PATH can be something else entirely (python-swiftclient); use the Xcode toolchain.
SWIFT="xcrun swift"
$SWIFT build -c "$CONFIG" ${ARCHS[@]+"${ARCHS[@]}"}
BIN_DIR=$($SWIFT build -c "$CONFIG" ${ARCHS[@]+"${ARCHS[@]}"} --show-bin-path)

APP="build/Ather Screenshot.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/AtherScreenshot" "$APP/Contents/MacOS/AtherScreenshot"
sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD_NUM/" Resources/Info.plist > "$APP/Contents/Info.plist"

# App icon, rendered from the vector A⁵ mark by the app itself.
ICONSET=$(mktemp -d)/AppIcon.iconset
"$APP/Contents/MacOS/AtherScreenshot" --write-iconset "$ICONSET"
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

IDENTITY="${SIGN_IDENTITY:--}"
if [ "$IDENTITY" = "-" ]; then
  # Ad-hoc signatures are pinned to their hash, so every rebuild would lose the Screen Recording
  # grant. Requiring only the bundle id keeps the grant across rebuilds.
  codesign --force --sign - -r='designated => identifier "com.ather.screenshot"' "$APP"
else
  codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
fi
echo "Built $APP ($VERSION)"

if [ "$MODE" = "package" ]; then
  mkdir -p dist
  ZIP="dist/AtherScreenshot-$VERSION-macOS.zip"
  DMG="dist/AtherScreenshot-$VERSION-macOS.dmg"
  rm -f "$ZIP" "$DMG"
  ditto -c -k --keepParent "$APP" "$ZIP"
  STAGE=$(mktemp -d)
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "Ather Screenshot" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
  if [ "$IDENTITY" != "-" ]; then
    codesign --force --sign "$IDENTITY" "$DMG"
    if [ -n "${NOTARY_PROFILE:-}" ]; then
      xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
      xcrun stapler staple "$DMG"
    fi
  fi
  (cd dist && shasum -a 256 "$(basename "$ZIP")" "$(basename "$DMG")" > SHA256SUMS.txt)
  echo "Packaged: $ZIP, $DMG"
fi
