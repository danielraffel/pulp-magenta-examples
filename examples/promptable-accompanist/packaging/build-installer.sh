#!/usr/bin/env bash
# Build a signed + notarized, customizable macOS installer (.pkg) for PromptableAccompanist.
# The user can choose which of AU / VST3 / CLAP / Standalone to install.
#
# Reads signing + notarization config from a .env file (default: $PULP_ENV or ./.env):
#   APP_CERT="Developer ID Application: …"   INSTALLER_CERT="Developer ID Installer: …"
#   APPLE_ID=…   TEAM_ID=…   APP_SPECIFIC_PASSWORD=…
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BUILD="${PULP_BUILD_DIR:-$REPO/build}"
ENTITLEMENTS="$HERE/entitlements.plist"
VERSION="${1:-1.0.0}"
OUT="$REPO/dist"; STAGE="$(mktemp -d)"; PKGS="$(mktemp -d)"
ID_PREFIX="com.danielraffel.promptableaccompanist"

set -a; source "${PULP_ENV:-${ENV_FILE:-$REPO/.env}}" 2>/dev/null || true; set +a
: "${APP_CERT:?APP_CERT not set}"; : "${INSTALLER_CERT:?INSTALLER_CERT not set}"

mkdir -p "$OUT"
APP="$BUILD/examples/promptable-accompanist/PromptableAccompanist.app"
AU="$BUILD/AU/PromptableAccompanist.component"
VST3="$BUILD/VST3/PromptableAccompanist.vst3"
CLAP="$BUILD/CLAP/PromptableAccompanist.clap"

sign() {  # sign a bundle deep, hardened runtime
  echo "  signing $(basename "$1")"
  codesign --force --deep --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" --sign "$APP_CERT" "$1"
}

echo "== 1. Sign bundles =="
for b in "$APP" "$AU" "$VST3" "$CLAP"; do [ -e "$b" ] && sign "$b"; done

echo "== 2. Component pkgs =="
mk() {  # mk <bundle> <install-dir> <id-suffix> <pkg-name>
  local root="$STAGE/$4"; mkdir -p "$root/$2"
  cp -R "$1" "$root/$2/"
  pkgbuild --root "$root" --identifier "$ID_PREFIX.$3" --version "$VERSION" \
    --install-location "/" "$PKGS/$4.pkg" >/dev/null
  echo "  built $4.pkg"
}
[ -e "$AU" ]   && mk "$AU"   "Library/Audio/Plug-Ins/Components" au   "AU"
[ -e "$VST3" ] && mk "$VST3" "Library/Audio/Plug-Ins/VST3"       vst3 "VST3"
[ -e "$CLAP" ] && mk "$CLAP" "Library/Audio/Plug-Ins/CLAP"       clap "CLAP"
[ -e "$APP" ]  && mk "$APP"  "Applications"                      app  "Standalone"

echo "== 3. Distribution (with format choices) =="
DIST="$STAGE/distribution.xml"
cat > "$DIST" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Promptable Accompanist</title>
  <organization>$ID_PREFIX</organization>
  <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <choices-outline>
    <line choice="au"/><line choice="vst3"/><line choice="clap"/><line choice="standalone"/>
  </choices-outline>
  <choice id="au" title="Audio Unit (AU)" start_selected="true"><pkg-ref id="$ID_PREFIX.au"/></choice>
  <choice id="vst3" title="VST3" start_selected="true"><pkg-ref id="$ID_PREFIX.vst3"/></choice>
  <choice id="clap" title="CLAP" start_selected="true"><pkg-ref id="$ID_PREFIX.clap"/></choice>
  <choice id="standalone" title="Standalone App" start_selected="true"><pkg-ref id="$ID_PREFIX.app"/></choice>
  <pkg-ref id="$ID_PREFIX.au" version="$VERSION">AU.pkg</pkg-ref>
  <pkg-ref id="$ID_PREFIX.vst3" version="$VERSION">VST3.pkg</pkg-ref>
  <pkg-ref id="$ID_PREFIX.clap" version="$VERSION">CLAP.pkg</pkg-ref>
  <pkg-ref id="$ID_PREFIX.app" version="$VERSION">Standalone.pkg</pkg-ref>
</installer-gui-script>
XML

UNSIGNED="$PKGS/PromptableAccompanist-unsigned.pkg"
FINAL="$OUT/PromptableAccompanist-$VERSION.pkg"
productbuild --distribution "$DIST" --package-path "$PKGS" "$UNSIGNED" >/dev/null
echo "  built distribution pkg"

echo "== 4. Sign installer =="
productsign --sign "$INSTALLER_CERT" --timestamp "$UNSIGNED" "$FINAL"
echo "  signed -> $FINAL"

echo "== 5. Notarize =="
if [ -n "${APPLE_ID:-}" ] && [ -n "${TEAM_ID:-}" ] && [ -n "${APP_SPECIFIC_PASSWORD:-}" ]; then
  xcrun notarytool submit "$FINAL" --apple-id "$APPLE_ID" --team-id "$TEAM_ID" \
    --password "$APP_SPECIFIC_PASSWORD" --wait
  xcrun stapler staple "$FINAL"
  echo "  notarized + stapled"
else
  echo "  SKIPPED (no notarization creds)"
fi

rm -rf "$STAGE" "$PKGS"
echo "DONE: $FINAL"
spctl --assess --type install -vv "$FINAL" 2>&1 | head -3 || true
