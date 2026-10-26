#!/usr/bin/env bash
# Builds ScanTailor Advanced for macOS and packages it as a self-contained .app.
#
# Requires: Xcode command line tools, Homebrew with cmake ninja qt boost
#           jpeg-turbo libpng libtiff librsvg qpdf poppler (the last two enable PDF import).
# Usage:    scripts/build-macos.sh [deployment-target]   (default 13.0)
# Output:   dist/ScanTailor Advanced.app and dist/ScanTailor-Advanced-<version>-macOS-<arch>.dmg
set -euo pipefail

DEPLOY="${1:-13.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-macos"
DIST="$ROOT/dist"
APP="$DIST/ScanTailor Advanced.app"
BREW="$(brew --prefix)"
QT="$(brew --prefix qt)"
VERSION="$(sed -n 's/.*#define[[:space:]]*VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/version.h.in" | head -1)"
ARCH="$(uname -m)"
SDK_VERSION="$(xcrun --show-sdk-version)"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
  -DCMAKE_PREFIX_PATH="$QT;$BREW" \
  -DBUILD_TESTS=OFF -DPORTABLE_VERSION=OFF
cmake --build "$BUILD"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/translations"
cp "$BUILD/scantailor-advanced" "$APP/Contents/MacOS/scantailor-advanced"
cp "$BUILD"/scantailor-advanced_*.qm "$APP/Contents/Resources/translations/"

# Icon
ICONSET="$BUILD/AppIcon.iconset"
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  rsvg-convert -w $s -h $s "$ROOT/src/resources/appicon.svg" -o "$ICONSET/icon_${s}x${s}.png"
  rsvg-convert -w $((s*2)) -h $((s*2)) "$ROOT/src/resources/appicon.svg" -o "$ICONSET/icon_${s}x${s}@2x.png"
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>ScanTailor Advanced</string>
  <key>CFBundleDisplayName</key><string>ScanTailor Advanced</string>
  <key>CFBundleIdentifier</key><string>org.scantailor.advanced</string>
  <key>CFBundleExecutable</key><string>scantailor-advanced</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>$DEPLOY</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>CFBundleDocumentTypes</key><array><dict>
    <key>CFBundleTypeName</key><string>ScanTailor project</string>
    <key>CFBundleTypeExtensions</key><array><string>ScanTailor</string></array>
    <key>CFBundleTypeRole</key><string>Editor</string>
  </dict></array>
</dict></plist>
PLIST

# Bundle Qt frameworks, plugins and Homebrew dylibs.
"$QT/bin/macdeployqt" "$APP" -always-overwrite

# macdeployqt also copies plugins this app does not use (virtual keyboard, network/TLS);
# drop them and the QML/Quick frameworks only they pulled in. QtPdf is kept on purpose:
# PDF import is done with qpdf and poppler instead (the Qt PDF image plugin needs QtPdf,
# which Homebrew ships in a different package).
rm -rf "$APP/Contents/PlugIns/platforminputcontexts" "$APP/Contents/PlugIns/networkinformation" \
       "$APP/Contents/PlugIns/tls" "$APP/Contents/PlugIns/imageformats/libqpdf.dylib"
rm -rf "$APP"/Contents/Frameworks/QtQml*.framework "$APP"/Contents/Frameworks/QtQuick*.framework \
       "$APP"/Contents/Frameworks/QtNetwork.framework

# macOS decides window look and SwiftUI/AppKit compatibility from the SDK recorded
# in LC_BUILD_VERSION, and the linker writes the deployment target there instead.
# Patch it, then re-sign (rewriting invalidates the signature).
vtool -set-build-version macos "$DEPLOY" "$SDK_VERSION" -replace \
  -output "$APP/Contents/MacOS/scantailor-advanced" "$APP/Contents/MacOS/scantailor-advanced"
codesign --force --deep --sign - "$APP"

hdiutil create -volname "ScanTailor Advanced" -srcfolder "$APP" -ov -format UDZO \
  "$DIST/ScanTailor-Advanced-$VERSION-macOS-$ARCH.dmg"
echo "Done: $APP"
