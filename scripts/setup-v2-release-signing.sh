#!/usr/bin/env bash
set -euo pipefail

app_identity="${PULP_MAGENTA_V2_CODESIGN_IDENTITY:-${APP_CERT:-Developer ID Application: Daniel Raffel (95CX6P84C4)}}"
installer_identity="${PULP_MAGENTA_V2_INSTALLER_IDENTITY:-${INSTALLER_CERT:-Developer ID Installer: Daniel Raffel (95CX6P84C4)}}"
profile="${PULP_MAGENTA_V2_NOTARY_PROFILE:-pulp-magenta-v2-notary}"
apple_id="${APPLE_ID:-danraffel@gmail.com}"
team_id="${TEAM_ID:-95CX6P84C4}"
requested_app_keychain="${PULP_MAGENTA_V2_CODESIGN_KEYCHAIN:-${PULP_SIGNING_KEYCHAIN:-}}"
requested_installer_keychain="${PULP_MAGENTA_V2_INSTALLER_KEYCHAIN:-${PULP_SIGNING_KEYCHAIN:-}}"

user_keychains() {
  security list-keychains -d user |
    sed 's/^[[:space:]]*"\(.*\)"[[:space:]]*$/\1/'
}

find_identity_keychains() {
  local identity="$1"
  local keychain
  while IFS= read -r keychain; do
    if security find-identity -v "$keychain" 2>/dev/null | grep -Fq "$identity"; then
      printf '%s\n' "$keychain"
    fi
  done < <(user_keychains)
}

resolve_identity_keychains() {
  local identity="$1"
  local requested_keychain="$2"
  if [ -n "$requested_keychain" ]; then
    if security find-identity -v "$requested_keychain" 2>/dev/null | grep -Fq "$identity"; then
      printf '%s\n' "$requested_keychain"
      return 0
    fi
    echo "Requested keychain does not contain identity '$identity': $requested_keychain" >&2
    return 1
  fi
  find_identity_keychains "$identity"
}

first_line() {
  sed -n '1p'
}

prompt_line() {
  local prompt="$1"
  local value
  if [ ! -e /dev/tty ]; then
    echo "Interactive terminal required for prompt: $prompt" >&2
    exit 1
  fi
  printf '%s' "$prompt" >/dev/tty
  IFS= read -r value </dev/tty
  printf '%s' "$value"
}

prompt_secret() {
  local prompt="$1"
  local value
  if [ ! -e /dev/tty ]; then
    echo "Interactive terminal required for secret prompt: $prompt" >&2
    exit 1
  fi
  printf '%s' "$prompt" >/dev/tty
  IFS= read -rs value </dev/tty
  printf '\n' >/dev/tty
  printf '%s' "$value"
}

unlock_for_codesign() {
  local keychain="$1"
  local password
  echo
  echo "Signing keychain: $keychain"
  password="$(prompt_secret "Password for $keychain: ")"
  security unlock-keychain -p "$password" "$keychain"
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$keychain"
}

echo "App identity: $app_identity"
echo "Installer identity: $installer_identity"
echo "Notary profile: $profile"
echo "Apple ID: $apple_id"
echo "Team ID: $team_id"
if [ -n "$requested_app_keychain" ]; then
  echo "Application signing keychain: $requested_app_keychain"
fi
if [ -n "$requested_installer_keychain" ]; then
  echo "Installer signing keychain: $requested_installer_keychain"
fi

app_keychains="$(resolve_identity_keychains "$app_identity" "$requested_app_keychain" || true)"
installer_keychains="$(resolve_identity_keychains "$installer_identity" "$requested_installer_keychain" || true)"

if [ -z "$app_keychains" ]; then
  echo "Missing Developer ID Application identity/private key: $app_identity" >&2
  exit 1
fi
if [ -z "$installer_keychains" ]; then
  echo "Missing Developer ID Installer identity/private key: $installer_identity" >&2
  exit 1
fi

echo
echo "Unlocking keychains that contain release signing identities..."
while IFS= read -r signing_keychain; do
  [ -n "$signing_keychain" ] || continue
  unlock_for_codesign "$signing_keychain"
done < <(
  {
    printf '%s\n' "$app_keychains"
    printf '%s\n' "$installer_keychains"
  } | awk 'NF && !seen[$0]++'
)

app_signing_keychain="$(printf '%s\n' "$app_keychains" | first_line)"
installer_signing_keychain="$(printf '%s\n' "$installer_keychains" | first_line)"

echo
echo "Verifying Developer ID Application signing access..."
probe_dir="$(mktemp -d /tmp/pulp-v2-signing-probe.XXXXXX)"
trap 'rm -rf "$probe_dir"' EXIT
probe_bin="$probe_dir/probe"
printf '#!/bin/sh\necho signing-probe\n' >"$probe_bin"
chmod +x "$probe_bin"
codesign --force --timestamp=none --keychain "$app_signing_keychain" --sign "$app_identity" "$probe_bin"
codesign --verify --strict "$probe_bin"

echo "Verifying Developer ID Installer signing access..."
probe_root="$probe_dir/root"
probe_pkg="$probe_dir/probe.pkg"
mkdir -p "$probe_root/usr/local/share/pulp-signing-probe"
printf 'ok\n' >"$probe_root/usr/local/share/pulp-signing-probe/probe.txt"
pkgbuild --root "$probe_root" \
  --install-location / \
  --identifier "com.pulp.signing-probe" \
  --version "1.0" \
  --keychain "$installer_signing_keychain" \
  --sign "$installer_identity" \
  "$probe_pkg" >/dev/null
pkgutil --check-signature "$probe_pkg" >/dev/null

echo
refresh_notary="$(prompt_line "Store or refresh notary credentials for profile '$profile'? [y/N] ")"
case "$refresh_notary" in
  y|Y|yes|YES)
    app_password="$(prompt_secret "Apple app-specific password for $apple_id: ")"
    xcrun notarytool store-credentials "$profile" \
      --apple-id "$apple_id" \
      --team-id "$team_id" \
      --password "$app_password"
    ;;
esac

echo
echo "Release-signing setup complete."
echo "Use:"
echo "  PULP_MAGENTA_V2_CODESIGN_KEYCHAIN=\"$app_signing_keychain\" PULP_MAGENTA_V2_INSTALLER_KEYCHAIN=\"$installer_signing_keychain\" PULP_MAGENTA_V2_NOTARY_PROFILE=$profile PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN=1 PULP_MAGENTA_V2_NOTARIZE=1 ./scripts/build-v2-test-dmg.sh build-model-status"
