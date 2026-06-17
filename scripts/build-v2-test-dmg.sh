#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Permanent, keychain-independent notarization credential. The Mac Studio CI
# host churns the login keychain, so the notarytool keychain profile keeps
# vanishing mid-build ("No Keychain password item found"). A file-based App
# Store Connect API key survives that; source it here so PULP_NOTARY_KEY_* are
# set and preferred over the profile in submit_for_notarization. No-op until set.
if [ -f "$HOME/.config/pulp-notary/env" ]; then
  # shellcheck disable=SC1091
  . "$HOME/.config/pulp-notary/env"
fi

build_dir="${1:-"$repo_root/build-model-status"}"
app_name="PromptableAccompanistV2"
version="${PULP_MAGENTA_V2_DMG_VERSION:-0.1.0}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
artifacts_dir="$repo_root/artifacts"
stage_dir="$(mktemp -d /tmp/pulp-v2-dmg-stage.XXXXXX)"
mount_dir="$(mktemp -d /tmp/pulp-v2-dmg-mount.XXXXXX)"
pkg_work_dir=""
dmg_path="$artifacts_dir/${app_name}-${version}-${timestamp}-macos-arm64.dmg"
pkg_path="$artifacts_dir/${app_name}-${version}-${timestamp}-macos-arm64.pkg"
audit_log="$artifacts_dir/${app_name}-${version}-${timestamp}-package-audit.log"
hidden_build_dir=""
attached=0
app_pid=""

auto_codesign_identity() {
  security find-identity -v -p codesigning 2>/dev/null |
    sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' |
    head -n 1
}

auto_installer_identity() {
  security find-identity -v 2>/dev/null |
    sed -n 's/.*"\(Developer ID Installer: [^"]*\)".*/\1/p' |
    head -n 1
}

sign_with_codesign() {
  if [ -n "${codesign_keychain:-}" ]; then
    codesign --keychain "$codesign_keychain" "$@"
  else
    codesign "$@"
  fi
}

pkgbuild_with_installer_identity() {
  local root="$1"
  local identifier="$2"
  local output="$3"
  local args=(
    --root "$root"
    --install-location /
    --identifier "$identifier"
    --version "$version"
    --sign "$installer_identity"
  )
  if [ -n "${installer_keychain:-}" ]; then
    args+=(--keychain "$installer_keychain")
  fi
  pkgbuild "${args[@]}" "$output" >/dev/null
}

productbuild_with_installer_identity() {
  local distribution_xml="$1"
  local packages_dir="$2"
  local installer_path="$3"
  local args=(--distribution "$distribution_xml" --package-path "$packages_dir" --sign "$installer_identity")
  if [ -n "${installer_keychain:-}" ]; then
    args+=(--keychain "$installer_keychain")
  fi
  productbuild "${args[@]}" "$installer_path" >/dev/null
}

is_truthy() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

resolve_notary_password() {
  local password_spec="$1"
  local apple_id="$2"
  if [[ "$password_spec" != @keychain:* ]]; then
    printf '%s' "$password_spec"
    return 0
  fi

  local service="${password_spec#@keychain:}"
  if security find-generic-password -s "$service" -a "$apple_id" -w 2>/dev/null; then
    return 0
  fi
  security find-generic-password -s "$service" -w 2>/dev/null
}

submit_for_notarization() {
  local item_path="$1"
  local item_label="$2"

  echo "Submitting $item_label for Apple notarization"
  # Prefer the file-based App Store Connect API key (survives the CI-host
  # keychain churn that keeps eating the notarytool profile) over the keychain
  # profile. See ~/.config/pulp-notary/env, sourced at the top of this script.
  if [ -n "${PULP_NOTARY_KEY_PATH:-}" ] &&
     [ -n "${PULP_NOTARY_KEY_ID:-}" ] &&
     [ -n "${PULP_NOTARY_ISSUER_ID:-}" ]; then
    xcrun notarytool submit "$item_path" \
      --key "$PULP_NOTARY_KEY_PATH" \
      --key-id "$PULP_NOTARY_KEY_ID" \
      --issuer "$PULP_NOTARY_ISSUER_ID" \
      --wait
  elif [ -n "${PULP_MAGENTA_V2_NOTARY_PROFILE:-}" ]; then
    xcrun notarytool submit "$item_path" \
      --keychain-profile "$PULP_MAGENTA_V2_NOTARY_PROFILE" \
      --wait
  else
    local notary_apple_id="${PULP_NOTARY_APPLE_ID:-${APPLE_ID:-}}"
    local notary_team_id="${PULP_NOTARY_TEAM_ID:-${TEAM_ID:-}}"
    local notary_password="${PULP_NOTARY_PASSWORD:-${APP_SPECIFIC_PASSWORD:-@keychain:AC_PASSWORD}}"
    if [ -z "$notary_apple_id" ] || [ -z "$notary_team_id" ]; then
      echo "Notarization requested but Apple ID/team credentials are incomplete." >&2
      exit 1
    fi
    if ! notary_password="$(resolve_notary_password "$notary_password" "$notary_apple_id")"; then
      echo "Notarization requested but the app-specific password could not be resolved." >&2
      exit 1
    fi
    xcrun notarytool submit "$item_path" \
      --apple-id "$notary_apple_id" \
      --team-id "$notary_team_id" \
      --password "$notary_password" \
      --wait
  fi
  xcrun stapler staple "$item_path"
  xcrun stapler validate "$item_path"
}

require_bundle_resource() {
  local bundle="$1"
  local executable_name="$2"
  if [ ! -x "$bundle/Contents/MacOS/$executable_name" ]; then
    echo "Missing bundle executable: $bundle/Contents/MacOS/$executable_name" >&2
    exit 1
  fi
  if [ ! -f "$bundle/Contents/MacOS/mlx.metallib" ]; then
    echo "Missing bundled MLX metallib: $bundle/Contents/MacOS/mlx.metallib" >&2
    exit 1
  fi
}

normalize_bundle_permissions() {
  local bundle="$1"
  chmod -R u+rwX,go+rX "$bundle"
}

copy_bundle_clean() {
  local src="$1"
  local dst="$2"
  rm -rf "$dst"
  ditto --norsrc --noextattr "$src" "$dst"
  normalize_bundle_permissions "$dst"
}

sign_bundle_copy() {
  local bundle="$1"
  normalize_bundle_permissions "$bundle"
  require_bundle_resource "$bundle" "$app_name"
  if [ -n "$codesign_identity" ]; then
    echo "Signing bundle with: $codesign_identity"
    echo "  $bundle"
    sign_with_codesign --force --deep --options runtime --timestamp \
      --sign "$codesign_identity" "$bundle"
    normalize_bundle_permissions "$bundle"
    codesign --verify --deep --strict "$bundle"
  else
    echo "Ad-hoc signing bundle for unsigned package validation"
    echo "  $bundle"
    codesign --force --deep --timestamp=none --sign - "$bundle"
    normalize_bundle_permissions "$bundle"
    codesign --verify --deep --strict "$bundle"
  fi
}

# The diagnostic helper app is a plain SwiftUI utility — no plug-in binary,
# no bundled mlx.metallib — so sign_bundle_copy's require_bundle_resource
# guard does not apply. Re-sign it here with hardened runtime + a minimal
# entitlements plist (network client, for the optional GitHub-upload mode) so
# the copy in the installer is independently valid and notarizable regardless
# of how the prebuilt app was signed.
sign_diagnostics_copy() {
  local bundle="$1"
  normalize_bundle_permissions "$bundle"
  if [ ! -x "$bundle/Contents/MacOS/$diagnostics_app_name" ]; then
    echo "Missing diagnostics app executable: $bundle/Contents/MacOS/$diagnostics_app_name" >&2
    exit 1
  fi
  local ent
  ent="$(mktemp /tmp/pulp-v2-diag-ent.XXXXXX.plist)"
  cat >"$ent" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.network.client</key>
  <true/>
</dict>
</plist>
PLIST
  if [ -n "$codesign_identity" ]; then
    echo "Signing diagnostics app with: $codesign_identity"
    echo "  $bundle"
    sign_with_codesign --force --options runtime --timestamp \
      --entitlements "$ent" --sign "$codesign_identity" "$bundle"
  else
    echo "Ad-hoc signing diagnostics app for unsigned package validation"
    echo "  $bundle"
    codesign --force --timestamp=none --entitlements "$ent" --sign - "$bundle"
  fi
  rm -f "$ent"
  normalize_bundle_permissions "$bundle"
  codesign --verify --deep --strict "$bundle"
}

verify_release_signing_access() {
  local probe_dir
  probe_dir="$(mktemp -d /tmp/pulp-v2-release-signing-probe.XXXXXX)"
  local probe_bin="$probe_dir/probe"
  local probe_root="$probe_dir/root"
  local probe_pkg="$probe_dir/probe.pkg"
  trap 'rm -rf "$probe_dir"' RETURN

  printf '#!/bin/sh\necho signing-probe\n' >"$probe_bin"
  chmod +x "$probe_bin"
  if ! sign_with_codesign --force --timestamp=none --sign "$codesign_identity" "$probe_bin" >/dev/null 2>&1; then
    echo "Developer ID Application signing is configured but not usable from this shell." >&2
    echo "Run ./scripts/setup-v2-release-signing.sh and make sure its signing probe passes in this same Terminal session." >&2
    echo "If it already passed in another shell, rerun the release command from that shell; macOS keychain access can be session-scoped." >&2
    rm -rf "$probe_dir"  # RETURN trap does not fire on exit — clean up explicitly
    exit 1
  fi

  mkdir -p "$probe_root/usr/local/share/pulp-v2-signing-probe"
  printf 'ok\n' >"$probe_root/usr/local/share/pulp-v2-signing-probe/probe.txt"
  if ! pkgbuild_with_installer_identity "$probe_root" \
      "com.pulp.magenta.v2.signing-probe" \
      "$probe_pkg" >/dev/null 2>&1; then
    echo "Developer ID Installer signing is configured but not usable from this shell." >&2
    echo "Run ./scripts/setup-v2-release-signing.sh and make sure its installer probe passes in this same Terminal session." >&2
    echo "If it already passed in another shell, rerun the release command from that shell; macOS keychain access can be session-scoped." >&2
    rm -rf "$probe_dir"  # RETURN trap does not fire on exit — clean up explicitly
    exit 1
  fi
}

build_component_installer() {
  local installer_path="$1"
  local standalone_app="$2"
  local vst3_bundle="$build_dir/VST3/$app_name.vst3"
  local au_bundle="$build_dir/AU/$app_name.component"
  local clap_bundle="$build_dir/CLAP/$app_name.clap"

  pkg_work_dir="$(mktemp -d /tmp/pulp-v2-pkg.XXXXXX)"
  local roots_dir="$pkg_work_dir/roots"
  local packages_dir="$pkg_work_dir/packages"
  local distribution_xml="$pkg_work_dir/distribution.xml"
  mkdir -p "$roots_dir" "$packages_dir"

  build_component_pkg() {
    local root="$1"
    local identifier="$2"
    local output="$3"
    if [ -n "$installer_identity" ]; then
      pkgbuild_with_installer_identity "$root" "$identifier" "$output"
    else
      pkgbuild --root "$root" --install-location / \
        --identifier "$identifier" \
        --version "$version" \
        "$output" >/dev/null
    fi
  }

  # Accumulate distribution.xml fragments only for the components that exist.
  # VST3/AU/CLAP are each conditional on the configured SDK actually exporting
  # that format (e.g. a checkout without the VST3 SDK builds AU+CLAP only).
  local dist_outline=""
  local dist_choices=""
  local dist_pkgrefs=""

  local nl=$'\n'
  add_component() {
    local choice_id="$1" identifier="$2" pkg_file="$3" title="$4" desc="$5" selected="$6"
    # Use a real-newline variable inside double quotes so every newline is a
    # newline (ANSI-C $'\n' only interprets escapes in the $'...' segment, not
    # in plain-quoted continuation segments).
    dist_outline+="${nl}    <line choice=\"${choice_id}\"/>"
    dist_choices+="${nl}  <choice id=\"${choice_id}\" title=\"${title}\" description=\"${desc}\" enabled=\"true\" visible=\"true\" start_selected=\"${selected}\">${nl}    <pkg-ref id=\"${identifier}\"/>${nl}  </choice>"
    dist_pkgrefs+="${nl}  <pkg-ref id=\"${identifier}\">${pkg_file}</pkg-ref>"
  }

  # Standalone app — always present.
  local standalone_root="$roots_dir/standalone"
  mkdir -p "$standalone_root/Applications"
  copy_bundle_clean "$standalone_app" "$standalone_root/Applications/$app_name.app"
  sign_bundle_copy "$standalone_root/Applications/$app_name.app"
  build_component_pkg "$standalone_root" \
    "com.pulp.magenta.accompanist.v2.standalone" \
    "$packages_dir/standalone.pkg"
  add_component standalone "com.pulp.magenta.accompanist.v2.standalone" standalone.pkg \
    "Standalone App" "Install $app_name into /Applications." "true"

  if [ -d "$vst3_bundle" ]; then
    local vst3_root="$roots_dir/vst3"
    mkdir -p "$vst3_root/Library/Audio/Plug-Ins/VST3"
    copy_bundle_clean "$vst3_bundle" "$vst3_root/Library/Audio/Plug-Ins/VST3/$app_name.vst3"
    sign_bundle_copy "$vst3_root/Library/Audio/Plug-Ins/VST3/$app_name.vst3"
    build_component_pkg "$vst3_root" \
      "com.pulp.magenta.accompanist.v2.vst3" \
      "$packages_dir/vst3.pkg"
    add_component vst3 "com.pulp.magenta.accompanist.v2.vst3" vst3.pkg \
      "VST3 Plug-In" "Install the VST3 into /Library/Audio/Plug-Ins/VST3." "true"
  else
    echo "VST3 bundle not built; omitting VST3 from installer: $vst3_bundle"
  fi

  if [ -d "$au_bundle" ]; then
    local au_root="$roots_dir/au"
    mkdir -p "$au_root/Library/Audio/Plug-Ins/Components"
    copy_bundle_clean "$au_bundle" "$au_root/Library/Audio/Plug-Ins/Components/$app_name.component"
    sign_bundle_copy "$au_root/Library/Audio/Plug-Ins/Components/$app_name.component"
    build_component_pkg "$au_root" \
      "com.pulp.magenta.accompanist.v2.auv2" \
      "$packages_dir/auv2.pkg"
    add_component auv2 "com.pulp.magenta.accompanist.v2.auv2" auv2.pkg \
      "AUv2 Plug-In" "Install the AUv2 into /Library/Audio/Plug-Ins/Components." "true"
  else
    echo "AU bundle not built; omitting AUv2 from installer: $au_bundle"
  fi

  if [ -d "$clap_bundle" ]; then
    local clap_root="$roots_dir/clap"
    mkdir -p "$clap_root/Library/Audio/Plug-Ins/CLAP"
    copy_bundle_clean "$clap_bundle" "$clap_root/Library/Audio/Plug-Ins/CLAP/$app_name.clap"
    sign_bundle_copy "$clap_root/Library/Audio/Plug-Ins/CLAP/$app_name.clap"
    build_component_pkg "$clap_root" \
      "com.pulp.magenta.accompanist.v2.clap" \
      "$packages_dir/clap.pkg"
    add_component clap "com.pulp.magenta.accompanist.v2.clap" clap.pkg \
      "CLAP Plug-In" "Install the CLAP into /Library/Audio/Plug-Ins/CLAP." "true"
  else
    echo "CLAP bundle not built; omitting CLAP from installer: $clap_bundle"
  fi

  # Optional diagnostics component (resolved gracefully above — see diag_enabled).
  if [ "$diag_enabled" = "1" ]; then
    local diagnostics_root="$roots_dir/diagnostics"
    mkdir -p "$diagnostics_root/Applications"
    copy_bundle_clean "$diagnostics_app" "$diagnostics_root/Applications/$diagnostics_app_name.app"
    sign_diagnostics_copy "$diagnostics_root/Applications/$diagnostics_app_name.app"
    build_component_pkg "$diagnostics_root" \
      "com.pulp.magenta.accompanist.v2.diagnostics" \
      "$packages_dir/diagnostics.pkg"
    add_component diagnostics "com.pulp.magenta.accompanist.v2.diagnostics" diagnostics.pkg \
      "Diagnostics Helper" \
      "Optional. Installs $diagnostics_app_name.app into /Applications. Run it if a plug-in fails to load — it collects logs, codesign/notarization/architecture status, and an auval report into a ZIP on your Desktop to send back." \
      "true"
  fi

  cat >"$distribution_xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
  <title>$app_name</title>
  <options customize="allow" require-scripts="false"/>
  <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
  <choices-outline>$dist_outline
  </choices-outline>$dist_choices$dist_pkgrefs
</installer-gui-script>
EOF

  if [ -n "$installer_identity" ]; then
    productbuild_with_installer_identity "$distribution_xml" "$packages_dir" "$installer_path"
  else
    productbuild --distribution "$distribution_xml" --package-path "$packages_dir" "$installer_path" >/dev/null
  fi
  if [ -n "$installer_identity" ]; then
    pkgutil --check-signature "$installer_path"
  else
    echo "No Developer ID Installer identity available; building unsigned test PKG." >&2
  fi
}

skip_release_sign="${PULP_MAGENTA_V2_SKIP_RELEASE_SIGN:-0}"
codesign_identity="${PULP_MAGENTA_V2_CODESIGN_IDENTITY:-${APP_CERT:-}}"
codesign_keychain="${PULP_MAGENTA_V2_CODESIGN_KEYCHAIN:-${PULP_SIGNING_KEYCHAIN:-}}"
if is_truthy "$skip_release_sign"; then
  codesign_identity=""
  codesign_keychain=""
elif [ -z "$codesign_identity" ]; then
  codesign_identity="$(auto_codesign_identity || true)"
fi

installer_identity="${PULP_MAGENTA_V2_INSTALLER_IDENTITY:-${INSTALLER_CERT:-}}"
installer_keychain="${PULP_MAGENTA_V2_INSTALLER_KEYCHAIN:-${PULP_SIGNING_KEYCHAIN:-}}"
if is_truthy "$skip_release_sign"; then
  installer_identity=""
  installer_keychain=""
elif [ -z "$installer_identity" ]; then
  installer_identity="$(auto_installer_identity || true)"
fi

require_release_sign="${PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN:-0}"
if is_truthy "$require_release_sign" && is_truthy "$skip_release_sign"; then
  echo "PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN conflicts with PULP_MAGENTA_V2_SKIP_RELEASE_SIGN." >&2
  exit 1
fi
if is_truthy "$require_release_sign" && [ -z "$codesign_identity" ]; then
  echo "No Developer ID Application identity found; set PULP_MAGENTA_V2_CODESIGN_IDENTITY or APP_CERT." >&2
  exit 1
fi
if is_truthy "$require_release_sign" && [ -z "$installer_identity" ]; then
  echo "No Developer ID Installer identity found; set PULP_MAGENTA_V2_INSTALLER_IDENTITY or INSTALLER_CERT." >&2
  exit 1
fi
if is_truthy "$require_release_sign" && [ -n "$codesign_keychain" ] && [ ! -f "$codesign_keychain" ]; then
  echo "Application signing keychain does not exist: $codesign_keychain" >&2
  exit 1
fi
if is_truthy "$require_release_sign" && [ -n "$installer_keychain" ] && [ ! -f "$installer_keychain" ]; then
  echo "Installer signing keychain does not exist: $installer_keychain" >&2
  exit 1
fi
if is_truthy "$require_release_sign"; then
  verify_release_signing_access
fi

include_plugin_installer="${PULP_MAGENTA_V2_INCLUDE_PLUGIN_INSTALLER:-1}"

# Diagnostics helper — an OPTIONAL dev add-on (its own app, not part of Pulp
# core or this example). Default `auto`: include it as a selectable installer
# component IF a prebuilt, signed PromptableAccompanistV2Diagnostics.app is
# available (point PULP_MAGENTA_V2_DIAGNOSTICS_APP at it); otherwise SKIP
# gracefully — never error, since most users won't have the add-on installed.
# Force off with PULP_MAGENTA_V2_INCLUDE_DIAGNOSTICS=0; force-attempt with =1
# (still graceful — warns and skips if the app/installer is unavailable).
include_diagnostics="${PULP_MAGENTA_V2_INCLUDE_DIAGNOSTICS:-auto}"
diagnostics_app="${PULP_MAGENTA_V2_DIAGNOSTICS_APP:-}"
diagnostics_app_name="PromptableAccompanistV2Diagnostics"
diag_enabled=0
# diag_want: 0 = off, auto = include-if-available (default), explicit = user asked.
if [ "$include_diagnostics" = "auto" ]; then
  diag_want=auto
elif is_truthy "$include_diagnostics"; then
  diag_want=explicit
else
  diag_want=0
fi
if [ "$diag_want" != "0" ]; then
  if ! is_truthy "$include_plugin_installer"; then
    [ "$diag_want" = "explicit" ] && echo "Note: diagnostics requested but the plug-in installer is off — skipping the diagnostics component." >&2
  elif [ -n "$diagnostics_app" ] && [ -x "$diagnostics_app/Contents/MacOS/$diagnostics_app_name" ]; then
    diag_enabled=1
  else
    [ "$diag_want" = "explicit" ] && echo "Note: diagnostics requested but the helper app was not found at '${diagnostics_app:-<unset PULP_MAGENTA_V2_DIAGNOSTICS_APP>}' — skipping (optional add-on)." >&2
  fi
fi

cleanup() {
  set +e
  if [ -n "$app_pid" ] && kill -0 "$app_pid" 2>/dev/null; then
    kill "$app_pid" 2>/dev/null
    wait "$app_pid" 2>/dev/null
  fi
  if [ -n "$hidden_build_dir" ] && [ -d "$hidden_build_dir" ] && [ ! -e "$build_dir" ]; then
    mv "$hidden_build_dir" "$build_dir"
  fi
  if [ "$attached" -eq 1 ]; then
    hdiutil detach "$mount_dir" -quiet >/dev/null 2>&1
  fi
  if [ -n "$pkg_work_dir" ]; then
    rm -rf "$pkg_work_dir"
  fi
  rm -rf "$stage_dir" "$mount_dir"
}
trap cleanup EXIT

mkdir -p "$artifacts_dir"

(
  unset PULP_STANDALONE_PACKAGE_AUDIT
  unset PULP_MAGENTA_V2_PACKAGE_AUDIT
  unset PULP_STANDALONE_PACKAGE_AUDIT_AUDIO
  unset PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO
  "$repo_root/scripts/smoke-v2-hot-reload.sh" "$build_dir"
)
if is_truthy "$include_plugin_installer"; then
  echo "Building plug-in bundles for installer package"
  # Only build the plug-in targets the configured SDK actually defined. A
  # checkout without the VST3 SDK has no PromptableAccompanistV2_VST3 target,
  # and naming a missing target makes cmake --build fail outright.
  plugin_targets=()
  for tgt in PromptableAccompanistV2_CLAP PromptableAccompanistV2_VST3 PromptableAccompanistV2_AU; do
    # Dry-run the target build; a missing target makes this fail fast and cheap.
    if cmake --build "$build_dir" --target "$tgt" -- -n >/dev/null 2>&1; then
      plugin_targets+=("$tgt")
    else
      echo "Skipping unavailable plug-in target: $tgt"
    fi
  done
  if [ "${#plugin_targets[@]}" -gt 0 ]; then
    cmake --build "$build_dir" --target "${plugin_targets[@]}" \
      -j"$(sysctl -n hw.ncpu)"
  fi
fi

app_path="$build_dir/examples/promptable-accompanist-v2/${app_name}.app"
app_binary="$app_path/Contents/MacOS/$app_name"
if [ ! -x "$app_binary" ]; then
  echo "Missing standalone app binary: $app_binary" >&2
  exit 1
fi
require_bundle_resource "$app_path" "$app_name"

codesign --verify --deep --strict "$app_path"

copy_bundle_clean "$app_path" "$stage_dir/$app_name.app"
stage_app="$stage_dir/$app_name.app"
if [ -n "$codesign_identity" ]; then
  echo "Signing app with: $codesign_identity"
  sign_with_codesign --force --deep --options runtime --timestamp \
    --sign "$codesign_identity" "$stage_app"
  normalize_bundle_permissions "$stage_app"
  codesign --verify --deep --strict "$stage_app"
else
  echo "No Developer ID Application identity available; ad-hoc signing test DMG app." >&2
  codesign --force --deep --timestamp=none --sign - "$stage_app"
  normalize_bundle_permissions "$stage_app"
  codesign --verify --deep --strict "$stage_app"
fi

if is_truthy "$include_plugin_installer"; then
  # Include whatever formats the configured SDK actually exported. A checkout
  # without the VST3 SDK builds AU+CLAP only; require at least one plug-in
  # format and validate the ones that are present.
  found_plugin_formats=0
  for bundle in \
    "$build_dir/VST3/$app_name.vst3" \
    "$build_dir/AU/$app_name.component" \
    "$build_dir/CLAP/$app_name.clap"; do
    if [ -d "$bundle" ]; then
      require_bundle_resource "$bundle" "$app_name"
      found_plugin_formats=$((found_plugin_formats + 1))
    else
      echo "Plug-in bundle not built (will be omitted from installer): $bundle"
    fi
  done
  if [ "$found_plugin_formats" -eq 0 ]; then
    echo "No plug-in bundles (VST3/AU/CLAP) were built for the installer." >&2
    echo "Reconfigure with a Pulp SDK that exports at least one plug-in format, or set PULP_MAGENTA_V2_INCLUDE_PLUGIN_INSTALLER=0." >&2
    exit 1
  fi
  echo "Building component installer package"
  build_component_installer "$pkg_path" "$stage_app"

  notarize_enabled="${PULP_MAGENTA_V2_NOTARIZE:-0}"
  if is_truthy "$notarize_enabled"; then
    submit_for_notarization "$pkg_path" "installer package"
    spctl -a -t install -v "$pkg_path"
  fi
  cp "$pkg_path" "$stage_dir/Install $app_name.pkg"
fi

ln -s /Applications "$stage_dir/Applications"
hdiutil create -volname "$app_name" \
  -srcfolder "$stage_dir" \
  -format UDZO \
  -ov "$dmg_path" >/dev/null
hdiutil verify "$dmg_path" >/dev/null
if [ -n "$codesign_identity" ]; then
  echo "Signing DMG with: $codesign_identity"
  sign_with_codesign --force --timestamp --sign "$codesign_identity" "$dmg_path"
  codesign --verify --strict "$dmg_path"
fi

notarize_enabled="${PULP_MAGENTA_V2_NOTARIZE:-0}"
if is_truthy "$notarize_enabled"; then
  submit_for_notarization "$dmg_path" "DMG"
  spctl -a -t open --context context:primary-signature -v "$dmg_path"
fi

package_audit="${PULP_STANDALONE_PACKAGE_AUDIT:-${PULP_MAGENTA_V2_PACKAGE_AUDIT:-1}}"
if ! is_truthy "$package_audit"; then
  echo "Skipping packaged-app isolation audit (PULP_STANDALONE_PACKAGE_AUDIT=0)."
  echo "OK: DMG built"
  echo "$dmg_path"
  if [ -f "$pkg_path" ]; then
    echo "$pkg_path"
  fi
  exit 0
fi

hdiutil attach -nobrowse -readonly -mountpoint "$mount_dir" "$dmg_path" >/dev/null
attached=1

mounted_binary="$mount_dir/$app_name.app/Contents/MacOS/$app_name"
if [ ! -x "$mounted_binary" ]; then
  echo "Mounted DMG is missing standalone app binary: $mounted_binary" >&2
  exit 1
fi

hidden_build_dir="${build_dir}.hidden-package-audit-$$"
mv "$build_dir" "$hidden_build_dir"

audit_seconds="${PULP_MAGENTA_V2_PACKAGE_AUDIT_SECONDS:-30}"
audit_failure_re="Failed to load the default metallib|\\[MLXEngine\\] Failed to load model|Library not loaded|image not found|dyld\\[|terminating due to uncaught exception|Abort trap|zsh: abort"
audit_health_re="Standalone: package audit complete|model load complete|No MRT2 model installed|You need to download a model"
audit_audio="${PULP_STANDALONE_PACKAGE_AUDIT_AUDIO:-${PULP_MAGENTA_V2_PACKAGE_AUDIT_AUDIO:-0}}"
if is_truthy "$audit_audio"; then
  echo "WARNING: package audit will open CoreAudio and may play generated audio."
else
  echo "Package audit will open the packaged app hidden with generated audio muted."
fi
PULP_MAGENTA_V2_DEBUG="${PULP_MAGENTA_V2_DEBUG:-1}" \
PULP_STANDALONE_PACKAGE_AUDIT=1 \
PULP_STANDALONE_PACKAGE_AUDIT_SECONDS="$audit_seconds" \
  "$mounted_binary" >"$audit_log" 2>&1 &
app_pid=$!

for _ in $(seq 1 "$((audit_seconds + 5))"); do
  if ! kill -0 "$app_pid" 2>/dev/null; then
    break
  fi
  if grep -Eq "$audit_failure_re" "$audit_log" 2>/dev/null; then
    break
  fi
  sleep 1
done

app_status=0
audit_timed_out=0
if kill -0 "$app_pid" 2>/dev/null; then
  # Still running at the time cap: a healthy long-lived render. We terminate
  # it ourselves, so its non-zero (SIGTERM) status is expected, not a failure.
  audit_timed_out=1
  kill "$app_pid" 2>/dev/null
  wait "$app_pid" 2>/dev/null || app_status=$?
else
  wait "$app_pid" 2>/dev/null || app_status=$?
fi
app_pid=""

if [ "$app_status" -ne 0 ] && [ "$audit_timed_out" -ne 1 ] && ! grep -Eq "$audit_failure_re" "$audit_log"; then
  # A non-zero status only signals trouble when the app exited on its OWN. When
  # we terminated a still-running healthy app at the time cap (audit_timed_out),
  # the health-marker check below is the real pass/fail gate.
  echo "Packaged-app isolation audit exited with status $app_status. See: $audit_log" >&2
  exit 1
fi

if grep -Eq "$audit_failure_re" "$audit_log"; then
  echo "Packaged-app isolation audit failed. See: $audit_log" >&2
  exit 1
fi

if ! grep -Eq "$audit_health_re" "$audit_log"; then
  echo "Packaged-app isolation audit did not reach a healthy standalone state. See: $audit_log" >&2
  exit 1
fi

echo "OK: DMG built and packaged-app isolation audit passed"
echo "$dmg_path"
if [ -f "$pkg_path" ]; then
  echo "$pkg_path"
fi
echo "$audit_log"
