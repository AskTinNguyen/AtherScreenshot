#!/bin/bash
# Builds build/Ather Screenshot.app.
#   ./build.sh            release build for this Mac's architecture
#   ./build.sh debug      debug build
#   ./build.sh package    universal (arm64 + x86_64) release + dist/*.zip (the update), *.dmg, SHA256SUMS.txt
#   ./build.sh publish    package, then add the Mac side to the GitHub Release v<version> (made by the Windows
#                         package.bat publish): uploads the DMG and the zip, sets the "macos" entry of the
#                         release's latest.json (keeping "windows"), and writes the same to ../downloads/latest.json.
#                         Needs gh. Only when the owner says so.
# Signing: ad-hoc by default. Set SIGN_IDENTITY="Developer ID Application: …" to sign for distribution,
# and NOTARY_PROFILE=<notarytool keychain profile> with `package` to notarize the DMG.
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-release}"
CONFIG=release
[ "$MODE" = "debug" ] && CONFIG=debug
ARCHS=()
[ "$MODE" = "package" ] || [ "$MODE" = "publish" ] && ARCHS=(--arch arm64 --arch x86_64)

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

if [ "$MODE" = "package" ] || [ "$MODE" = "publish" ]; then
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

if [ "$MODE" = "publish" ]; then
  REPO=AskTinNguyen/AtherScreenshot
  TAG="v$VERSION"
  gh release view "$TAG" --repo "$REPO" >/dev/null || { echo "No release $TAG yet: publish Windows first (package.bat publish)."; exit 1; }
  cp dist/SHA256SUMS.txt dist/SHA256SUMS-macOS.txt  # the release's SHA256SUMS.txt is the Windows one
  gh release upload "$TAG" "$DMG" "$ZIP" dist/SHA256SUMS-macOS.txt --repo "$REPO" --clobber
  WORK=$(mktemp -d)
  gh release download "$TAG" --repo "$REPO" --pattern latest.json --dir "$WORK" 2>/dev/null || echo '{}' > "$WORK/latest.json"
  # Only the macos entry changes; windows (and anything else) stays as it is.
  /usr/bin/python3 - "$WORK/latest.json" "$ZIP" "$BUILD_NUM" "https://github.com/$REPO/releases/download/$TAG/$(basename "$ZIP")" "${UPDATE_NOTES:-}" <<'PY'
import hashlib, json, os, sys
path, zip_, build, url, notes = sys.argv[1:6]
raw = open(path, 'rb').read().decode('utf-8-sig').strip() or '{}'
m = json.loads(raw)
m['macos'] = {'version': build, 'url': url, 'sha256': hashlib.sha256(open(zip_, 'rb').read()).hexdigest(),
              'size': os.path.getsize(zip_), 'notes': notes}
open(path, 'w').write(json.dumps(m, indent=4) + '\n')
PY
  gh release upload "$TAG" "$WORK/latest.json" --repo "$REPO" --clobber
  /usr/bin/python3 - "$WORK/latest.json" ../downloads/latest.json <<'PY'
import json, sys
new = json.load(open(sys.argv[1]))
try: m = json.loads(open(sys.argv[2], 'rb').read().decode('utf-8-sig'))
except Exception: m = {}
m['macos'] = new['macos']
open(sys.argv[2], 'w').write(json.dumps(m, indent=4) + '\n')
PY
  echo "Published the Mac side of $TAG (build $BUILD_NUM). Commit and push downloads/latest.json."
fi
