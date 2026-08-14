#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state_dir="${PULP_MAGENTA_V2_RELEASE_BUILDER_STATE_DIR:-$HOME/.pulp/magenta-v2-release-builder}"
request_env="$state_dir/request.env"
log_dir="$repo_root/artifacts/release-builder-agent"
latest_log="$log_dir/latest.log"
status_file="$log_dir/status.env"
lock_dir="$state_dir/lock"

mkdir -p "$state_dir" "$log_dir"

if ! mkdir "$lock_dir" 2>/dev/null; then
  {
    echo "PULP_MAGENTA_V2_RELEASE_BUILDER_STATUS=busy"
    echo "PULP_MAGENTA_V2_RELEASE_BUILDER_MESSAGE='Another release builder run is active.'"
  } >"$status_file"
  exit 75
fi
finished=0
cleanup() {
  local exit_code=$?
  rm -rf "$lock_dir"
  if [ "$finished" -eq 0 ] && [ "$exit_code" -ne 0 ]; then
    write_status failed "Release builder failed with exit code $exit_code"
  fi
}
trap cleanup EXIT

: >"$latest_log"
exec >>"$latest_log" 2>&1

if [ -f "$request_env" ]; then
  set -a
  # shellcheck disable=SC1090
  . "$request_env"
  set +a
fi

mode="${PULP_MAGENTA_V2_RELEASE_BUILDER_MODE:-probe}"
run_id="${PULP_MAGENTA_V2_RELEASE_BUILDER_RUN_ID:-manual-$(date -u +%Y%m%dT%H%M%SZ)}"
build_dir="${PULP_MAGENTA_V2_BUILD_DIR:-build-model-status}"
notary_submission_id="${PULP_MAGENTA_V2_NOTARY_SUBMISSION_ID:-}"
app_identity="${PULP_MAGENTA_V2_CODESIGN_IDENTITY:-${APP_CERT:-Developer ID Application: Daniel Raffel (95CX6P84C4)}}"
installer_identity="${PULP_MAGENTA_V2_INSTALLER_IDENTITY:-${INSTALLER_CERT:-Developer ID Installer: Daniel Raffel (95CX6P84C4)}}"
codesign_keychain="${PULP_MAGENTA_V2_CODESIGN_KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
installer_keychain="${PULP_MAGENTA_V2_INSTALLER_KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
notary_profile="${PULP_MAGENTA_V2_NOTARY_PROFILE:-pulp-magenta-v2-notary}"

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PULP_MAGENTA_V2_CODESIGN_IDENTITY="$app_identity"
export PULP_MAGENTA_V2_INSTALLER_IDENTITY="$installer_identity"
export PULP_MAGENTA_V2_CODESIGN_KEYCHAIN="$codesign_keychain"
export PULP_MAGENTA_V2_INSTALLER_KEYCHAIN="$installer_keychain"
export PULP_MAGENTA_V2_NOTARY_PROFILE="$notary_profile"
export PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN="${PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN:-1}"
export PULP_MAGENTA_V2_NOTARIZE="${PULP_MAGENTA_V2_NOTARIZE:-1}"
export PULP_STANDALONE_PACKAGE_AUDIT="${PULP_STANDALONE_PACKAGE_AUDIT:-0}"
export PULP_MAGENTA_V2_PACKAGE_AUDIT="${PULP_MAGENTA_V2_PACKAGE_AUDIT:-0}"

write_status() {
  local status="$1"
  local message="$2"
  {
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_RUN_ID=%q\n' "$run_id"
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_MODE=%q\n' "$mode"
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_STATUS=%q\n' "$status"
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_MESSAGE=%q\n' "$message"
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_LOG=%q\n' "$latest_log"
    printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_UPDATED_AT=%q\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >"$status_file"
}

sign_with_codesign() {
  codesign --keychain "$codesign_keychain" "$@"
}

probe_signing() {
  local probe_dir probe_bin probe_root probe_pkg
  probe_dir="$(mktemp -d /tmp/pulp-v2-release-builder-probe.XXXXXX)"
  probe_bin="$probe_dir/probe"
  probe_root="$probe_dir/root"
  probe_pkg="$probe_dir/probe.pkg"

  printf '#!/bin/sh\necho signing-probe\n' >"$probe_bin"
  chmod +x "$probe_bin"

  echo "Verifying Developer ID Application signing access via GUI LaunchAgent"
  sign_with_codesign --force --timestamp=none --sign "$app_identity" "$probe_bin"
  codesign --verify --strict "$probe_bin"

  echo "Verifying Developer ID Installer signing access via GUI LaunchAgent"
  mkdir -p "$probe_root/usr/local/share/pulp-v2-signing-probe"
  printf 'ok\n' >"$probe_root/usr/local/share/pulp-v2-signing-probe/probe.txt"
  pkgbuild \
    --root "$probe_root" \
    --install-location / \
    --identifier com.pulp.magenta.v2.release-builder-probe \
    --version 0.1.0 \
    --sign "$installer_identity" \
    --keychain "$installer_keychain" \
    "$probe_pkg" >/dev/null
  pkgutil --check-signature "$probe_pkg"

  echo "Verifying notary profile lookup"
  xcrun notarytool history --keychain-profile "$notary_profile" >/dev/null
  rm -rf "$probe_dir"
}

run_build() {
  cd "$repo_root"
  echo "Running release DMG build in GUI LaunchAgent session"
  echo "Package audit is disabled for this build; no app launch or CoreAudio smoke will run."
  ./scripts/build-v2-test-dmg.sh "$build_dir"
}

fetch_notary_log() {
  if [ -z "$notary_submission_id" ]; then
    echo "PULP_MAGENTA_V2_NOTARY_SUBMISSION_ID is required for notary-log mode." >&2
    exit 2
  fi
  xcrun notarytool log "$notary_submission_id" --keychain-profile "$notary_profile"
}

write_status running "Started"
echo "Release builder run: $run_id"
echo "Mode: $mode"
echo "Repo: $repo_root"

case "$mode" in
  probe)
    probe_signing
    finished=1
    write_status success "Signing probe passed"
    ;;
  build)
    run_build
    finished=1
    write_status success "Release build passed"
    ;;
  notary-log)
    fetch_notary_log
    finished=1
    write_status success "Notary log fetched"
    ;;
  *)
    echo "Unknown mode: $mode" >&2
    finished=1
    write_status failed "Unknown mode: $mode"
    exit 2
    ;;
esac
