#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
label="${PULP_MAGENTA_V2_RELEASE_BUILDER_LABEL:-com.pulp.magenta.v2.release-builder}"
plist="$HOME/Library/LaunchAgents/$label.plist"
log_dir="$repo_root/artifacts/release-builder-agent"
uid="$(id -u)"

mkdir -p "$(dirname "$plist")" "$log_dir"
chmod +x "$repo_root/scripts/v2-release-builder-agent.sh"

/usr/libexec/PlistBuddy -c Clear "$plist" >/dev/null 2>&1 || true
/usr/libexec/PlistBuddy -c 'Add :Label string '"$label" "$plist"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments array' "$plist"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments:0 string /bin/bash' "$plist"
/usr/libexec/PlistBuddy -c 'Add :ProgramArguments:1 string '"$repo_root/scripts/v2-release-builder-agent.sh" "$plist"
/usr/libexec/PlistBuddy -c 'Add :WorkingDirectory string '"$repo_root" "$plist"
/usr/libexec/PlistBuddy -c 'Add :RunAtLoad bool false' "$plist"
/usr/libexec/PlistBuddy -c 'Add :StandardOutPath string '"$log_dir/launchd.out.log" "$plist"
/usr/libexec/PlistBuddy -c 'Add :StandardErrorPath string '"$log_dir/launchd.err.log" "$plist"
chmod 644 "$plist"

launchctl bootout "gui/$uid/$label" >/dev/null 2>&1 || true
launchctl bootstrap "gui/$uid" "$plist"
launchctl enable "gui/$uid/$label" >/dev/null 2>&1 || true

echo "Installed LaunchAgent: $label"
echo "Plist: $plist"
echo "Run a probe with: ./scripts/run-v2-release-builder-agent.sh probe"
echo "Run a release build with: ./scripts/run-v2-release-builder-agent.sh build"
