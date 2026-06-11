#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
label="${PULP_MAGENTA_V2_RELEASE_BUILDER_LABEL:-com.pulp.magenta.v2.release-builder}"
state_dir="${PULP_MAGENTA_V2_RELEASE_BUILDER_STATE_DIR:-$HOME/.pulp/magenta-v2-release-builder}"
request_env="$state_dir/request.env"
status_file="$repo_root/artifacts/release-builder-agent/status.env"
mode="${1:-probe}"
build_dir="${2:-build-model-status}"
uid="$(id -u)"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
timeout="${PULP_MAGENTA_V2_RELEASE_BUILDER_TIMEOUT:-}"

case "$mode" in
  probe) timeout="${timeout:-180}" ;;
  build) timeout="${timeout:-7200}" ;;
  notary-log) timeout="${timeout:-180}" ;;
  *) echo "Usage: $0 [probe|build|notary-log] [build-dir]" >&2; exit 2 ;;
esac

mkdir -p "$state_dir" "$repo_root/artifacts/release-builder-agent"
{
  printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_RUN_ID=%q\n' "$run_id"
  printf 'PULP_MAGENTA_V2_RELEASE_BUILDER_MODE=%q\n' "$mode"
  printf 'PULP_MAGENTA_V2_BUILD_DIR=%q\n' "$build_dir"
  printf 'PULP_MAGENTA_V2_CODESIGN_KEYCHAIN=%q\n' "${PULP_MAGENTA_V2_CODESIGN_KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
  printf 'PULP_MAGENTA_V2_INSTALLER_KEYCHAIN=%q\n' "${PULP_MAGENTA_V2_INSTALLER_KEYCHAIN:-$HOME/Library/Keychains/login.keychain-db}"
  printf 'PULP_MAGENTA_V2_NOTARY_PROFILE=%q\n' "${PULP_MAGENTA_V2_NOTARY_PROFILE:-pulp-magenta-v2-notary}"
  printf 'PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN=%q\n' "${PULP_MAGENTA_V2_REQUIRE_RELEASE_SIGN:-1}"
  printf 'PULP_MAGENTA_V2_NOTARIZE=%q\n' "${PULP_MAGENTA_V2_NOTARIZE:-1}"
  printf 'PULP_STANDALONE_PACKAGE_AUDIT=%q\n' "${PULP_STANDALONE_PACKAGE_AUDIT:-0}"
  printf 'PULP_MAGENTA_V2_PACKAGE_AUDIT=%q\n' "${PULP_MAGENTA_V2_PACKAGE_AUDIT:-0}"
  printf 'PULP_MAGENTA_V2_NOTARY_SUBMISSION_ID=%q\n' "${PULP_MAGENTA_V2_NOTARY_SUBMISSION_ID:-}"
} >"$request_env"

launchctl kickstart -k "gui/$uid/$label"

deadline=$((SECONDS + timeout))
while [ "$SECONDS" -lt "$deadline" ]; do
  if [ -f "$status_file" ]; then
    # shellcheck disable=SC1090
    . "$status_file"
    if [ "${PULP_MAGENTA_V2_RELEASE_BUILDER_RUN_ID:-}" = "$run_id" ]; then
      case "${PULP_MAGENTA_V2_RELEASE_BUILDER_STATUS:-}" in
        success)
          echo "${PULP_MAGENTA_V2_RELEASE_BUILDER_MESSAGE:-Release builder succeeded.}"
          echo "Log: ${PULP_MAGENTA_V2_RELEASE_BUILDER_LOG:-$repo_root/artifacts/release-builder-agent/latest.log}"
          exit 0
          ;;
        failed|busy)
          echo "${PULP_MAGENTA_V2_RELEASE_BUILDER_MESSAGE:-Release builder failed.}" >&2
          echo "Log: ${PULP_MAGENTA_V2_RELEASE_BUILDER_LOG:-$repo_root/artifacts/release-builder-agent/latest.log}" >&2
          exit 1
          ;;
      esac
    fi
  fi
  sleep 2
done

echo "Timed out waiting for release builder $mode run $run_id." >&2
echo "Log: $repo_root/artifacts/release-builder-agent/latest.log" >&2
exit 124
