#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-"$repo_root/build-model-status"}"
case "$build_dir" in
  /*) ;;
  *) build_dir="$repo_root/$build_dir" ;;
esac
formats="${PULP_MAGENTA_V2_REAPER_FORMATS:-all}"
jobs="${PULP_JOBS:-$(sysctl -n hw.ncpu)}"
reaper_bin="${REAPER_BIN:-/Applications/REAPER.app/Contents/MacOS/REAPER}"

if [ ! -x "$reaper_bin" ]; then
  echo "REAPER binary not found or not executable: $reaper_bin" >&2
  exit 1
fi

if [ -n "${PULP_MAGENTA_V2_REAPER_ROOT:-}" ]; then
  root="$PULP_MAGENTA_V2_REAPER_ROOT"
  mkdir -p "$root"
else
  root="$(mktemp -d /tmp/pulp-v2-reaper-host-state.XXXXXX)"
fi

clap_bundle="$build_dir/CLAP/PromptableAccompanistV2.clap"
vst3_bundle="$build_dir/VST3/PromptableAccompanistV2.vst3"
user_vst3_dir="$HOME/Library/Audio/Plug-Ins/VST3"
user_vst3_bundle="$user_vst3_dir/PromptableAccompanistV2.vst3"
vst3_backup="$root/PromptableAccompanistV2.vst3.backup"
vst3_installed=0

contains_format() {
  case ",$formats," in
    *,all,*|*,"$1",*) return 0 ;;
    *) return 1 ;;
  esac
}

cleanup() {
  set +e
  pkill -f "$reaper_bin.*$root" >/dev/null 2>&1 || true
  if [ "$vst3_installed" -eq 1 ]; then
    rm -rf "$user_vst3_bundle"
    if [ -d "$vst3_backup" ]; then
      mv "$vst3_backup" "$user_vst3_bundle"
    fi
  fi
}
trap cleanup EXIT

write_reaper_ini() {
  cat >"$root/reaper.ini" <<EOF
[nag]
nag=DAD22A6AE7C2B30D9790F6240C76BB2E67858EF73F92FC1AAD

[reaper]
clap_path_macos-aarch64=$build_dir/CLAP
coreaudiobs=512
coreaudiobsuse=0
coreaudioignorereset=0
coreaudioignprojsr=0
coreaudioindevnew=<default system devices>
coreaudiooutdevnew=<default system devices>
coreaudiosrate=48000
coreaudiosrateuse=0
hasrecentsec=1
mixwnd_dock=1
numrecent=8
renderclosewhendone=4
transport_h=64
transport_w=1000
transport_x=100
transport_y=725
vstpath64=$user_vst3_dir
wnd_h=768
wnd_state=0
wnd_w=1024
wnd_x=80
wnd_y=80
EOF
}

write_lua_scripts() {
  cat >"$root/clap_create_host_param_freeze_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/clap_create_host_param_freeze_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end

local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end

local function write_all(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end

writeln("resource=" .. reaper.GetResourcePath())
reaper.Main_OnCommand(40023, 0)
reaper.InsertTrackAtIndex(0, false)
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track") end
reaper.SetMediaTrackInfo_Value(track, "D_VOL", 0.0)

local fx = reaper.TrackFX_AddByName(track, "CLAPi: PromptableAccompanistV2", false, -1000)
writeln("fx=" .. tostring(fx))
if fx < 0 then return fail("could not instantiate V2 CLAP") end

local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "PromptableAccompanistV2", 1, true) then
  return fail("loaded FX is not PromptableAccompanistV2")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found") end

writeln("freeze_before=" .. tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))
local set_one = reaper.TrackFX_SetParamNormalized(track, fx, freeze_index, 1.0)
writeln("set_freeze_one=" .. tostring(set_one))
if reaper.TrackFX_EndParamEdit then
  reaper.TrackFX_EndParamEdit(track, fx, freeze_index)
  writeln("end_param_edit_one=true")
end
writeln("freeze_after_set_before_play=" ..
        tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))

local started = reaper.time_precise()
reaper.Main_OnCommand(1007, 0)

local function check_after_play()
  if reaper.time_precise() - started < 2.0 then
    reaper.defer(check_after_play)
    return
  end

  reaper.Main_OnCommand(1016, 0)
  local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
  writeln("freeze_after_play=" .. tostring(freeze))
  if freeze < 0.5 then return fail("Freeze did not latch after CLAP host parameter playback") end

  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "clap_chunk")
  writeln("get_clap_chunk=" .. tostring(got) .. " len=" .. tostring(chunk and #chunk or 0))
  if not got or not chunk or #chunk <= 128 then
    return fail("host parameter freeze did not produce a frozen CLAP state chunk")
  end
  write_all(root .. "/clap-host-param-freeze.clapstate.b64", chunk)

  local project_path = root .. "/v2-clap-host-param-freeze-smoke.rpp"
  reaper.Main_SaveProjectEx(0, project_path, 8)
  local saved_file = io.open(project_path, "rb")
  writeln("saved_file=" .. tostring(saved_file ~= nil))
  if saved_file then saved_file:close() end
  writeln("project=" .. project_path)
  if not saved_file then return fail("Main_SaveProjectEx did not write CLAP freeze project") end

  writeln("OK CLAP host parameter freeze project saved")
  finish()
end

reaper.defer(check_after_play)
LUA

  cat >"$root/clap_reopen_verify_host_param_freeze_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/clap_reopen_verify_host_param_freeze_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function write_all(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track after CLAP freeze reopen") end

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX after CLAP freeze reopen") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "PromptableAccompanistV2", 1, true) then
  return fail("reopened FX is not PromptableAccompanistV2")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found after CLAP freeze reopen") end

local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
writeln("freeze_norm=" .. tostring(freeze))
if freeze < 0.5 then return fail("Freeze did not restore after CLAP project reopen") end

local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "clap_chunk")
writeln("get_clap_chunk=" .. tostring(got) .. " len=" .. tostring(chunk and #chunk or 0))
if not got or not chunk or #chunk <= 128 then
  return fail("reopened CLAP frozen clap_chunk is not frozen")
end
write_all(root .. "/clap-host-param-freeze-reopened.clapstate.b64", chunk)

writeln("OK reopened REAPER project recalled CLAP host-parameter frozen state")
finish()
LUA

  cat >"$root/clap_create_host_param_release_off_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/clap_create_host_param_release_off_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function write_all(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track in CLAP release project") end
reaper.SetMediaTrackInfo_Value(track, "D_VOL", 0.0)

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX in CLAP release project") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "PromptableAccompanistV2", 1, true) then
  return fail("release project FX is not PromptableAccompanistV2")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found in CLAP release project") end

writeln("freeze_before=" .. tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))
local set_zero = reaper.TrackFX_SetParamNormalized(track, fx, freeze_index, 0.0)
writeln("set_freeze_zero=" .. tostring(set_zero))
if reaper.TrackFX_EndParamEdit then
  reaper.TrackFX_EndParamEdit(track, fx, freeze_index)
  writeln("end_param_edit_zero=true")
end
writeln("freeze_after_set_before_play=" ..
        tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))

local started = reaper.time_precise()
reaper.Main_OnCommand(1007, 0)

local function check_after_play()
  if reaper.time_precise() - started < 2.0 then
    reaper.defer(check_after_play)
    return
  end

  reaper.Main_OnCommand(1016, 0)
  local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
  writeln("freeze_after_play=" .. tostring(freeze))
  if freeze >= 0.5 then return fail("Freeze stayed on after CLAP host parameter release playback") end

  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "clap_chunk")
  writeln("get_clap_chunk=" .. tostring(got) .. " len=" .. tostring(chunk and #chunk or 0))
  if not got or not chunk or #chunk == 0 then return fail("CLAP release clap_chunk is empty") end
  write_all(root .. "/clap-host-param-release-off.clapstate.b64", chunk)

  local project_path = root .. "/v2-clap-host-param-release-off-smoke.rpp"
  reaper.Main_SaveProjectEx(0, project_path, 8)
  local saved_file = io.open(project_path, "rb")
  writeln("saved_file=" .. tostring(saved_file ~= nil))
  if saved_file then saved_file:close() end
  writeln("project=" .. project_path)
  if not saved_file then return fail("Main_SaveProjectEx did not write CLAP release-off project") end

  writeln("OK CLAP host parameter release-off project saved")
  finish()
end

reaper.defer(check_after_play)
LUA

  cat >"$root/clap_reopen_verify_host_param_release_off_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/clap_reopen_verify_host_param_release_off_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function write_all(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track after CLAP release-off reopen") end

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX after CLAP release-off reopen") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "PromptableAccompanistV2", 1, true) then
  return fail("reopened release-off FX is not PromptableAccompanistV2")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found after CLAP release-off reopen") end

local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
writeln("freeze_norm=" .. tostring(freeze))
if freeze >= 0.5 then return fail("Freeze resurrected after CLAP release-off project reopen") end

local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "clap_chunk")
writeln("get_clap_chunk=" .. tostring(got) .. " len=" .. tostring(chunk and #chunk or 0))
if not got or not chunk or #chunk == 0 then return fail("reopened CLAP release-off clap_chunk is empty") end
write_all(root .. "/clap-host-param-release-off-reopened.clapstate.b64", chunk)

writeln("OK reopened REAPER project kept CLAP host-parameter Freeze released")
finish()
LUA

  cat >"$root/vst3_create_host_param_freeze_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/vst3_create_host_param_freeze_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end
local function get_vst_chunk(track, fx, prefix)
  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "vst_chunk")
  writeln(prefix .. "_get_vst_chunk=" .. tostring(got) ..
          " len=" .. tostring(chunk and #chunk or 0))
  return got, chunk
end

writeln("resource=" .. reaper.GetResourcePath())
reaper.Main_OnCommand(40023, 0)
reaper.InsertTrackAtIndex(0, false)

local scan_started = reaper.time_precise()
local function create_after_scan()
  if reaper.time_precise() - scan_started < 8.0 then
    reaper.defer(create_after_scan)
    return
  end

  local track = reaper.GetTrack(0, 0)
  if not track then return fail("no track") end
  reaper.SetMediaTrackInfo_Value(track, "D_VOL", 0.0)

  local fx = reaper.TrackFX_AddByName(track, "VST3i: PromptableAccompanistV2", false, -1000)
  writeln("fx=" .. tostring(fx))
  if fx < 0 then return fail("could not instantiate V2 VST3") end

  local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
  writeln("fx_name=" .. fx_name)
  if not string.find(fx_name, "VST3i: PromptableAccompanistV2", 1, true) then
    return fail("loaded FX is not exact V2 VST3")
  end

  local freeze_index = find_freeze_param(track, fx)
  writeln("freeze_index=" .. tostring(freeze_index))
  if freeze_index < 0 then return fail("Freeze parameter not found") end

  writeln("freeze_before=" .. tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))
  local set_one = reaper.TrackFX_SetParamNormalized(track, fx, freeze_index, 1.0)
  writeln("set_freeze_one=" .. tostring(set_one))
  if reaper.TrackFX_EndParamEdit then
    reaper.TrackFX_EndParamEdit(track, fx, freeze_index)
    writeln("end_param_edit_one=true")
  end
  writeln("freeze_after_set_before_play=" ..
          tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))
  local _, before_chunk = get_vst_chunk(track, fx, "before_play")
  local before_len = before_chunk and #before_chunk or 0

  local play_started = reaper.time_precise()
  reaper.Main_OnCommand(1007, 0)

  local function check_after_play()
    if reaper.time_precise() - play_started < 2.0 then
      reaper.defer(check_after_play)
      return
    end

    reaper.Main_OnCommand(1016, 0)
    local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
    writeln("freeze_after_play=" .. tostring(freeze))
    if freeze < 0.5 then return fail("Freeze did not latch after VST3 host parameter playback") end
    local got_after, after_chunk = get_vst_chunk(track, fx, "after_play")
    local after_len = after_chunk and #after_chunk or 0
    if not got_after or after_len <= before_len + 512 then
      return fail("VST3 frozen state chunk did not grow after Freeze")
    end

    local project_path = root .. "/v2-vst3-host-param-freeze-smoke.rpp"
    reaper.Main_SaveProjectEx(0, project_path, 8)
    local saved_file = io.open(project_path, "rb")
    writeln("saved_file=" .. tostring(saved_file ~= nil))
    if saved_file then saved_file:close() end
    writeln("project=" .. project_path)
    if not saved_file then return fail("Main_SaveProjectEx did not write VST3 freeze project") end

    writeln("OK VST3 host parameter freeze project saved")
    finish()
  end

  reaper.defer(check_after_play)
end

reaper.defer(create_after_scan)
LUA

  cat >"$root/vst3_reopen_verify_host_param_freeze_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/vst3_reopen_verify_host_param_freeze_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end
local function get_vst_chunk(track, fx, prefix)
  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "vst_chunk")
  writeln(prefix .. "_get_vst_chunk=" .. tostring(got) ..
          " len=" .. tostring(chunk and #chunk or 0))
  return got, chunk
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track after VST3 freeze reopen") end

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX after VST3 freeze reopen") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "VST3i: PromptableAccompanistV2", 1, true) then
  return fail("reopened FX is not exact V2 VST3")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found after VST3 freeze reopen") end

local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
writeln("freeze_norm=" .. tostring(freeze))
if freeze < 0.5 then return fail("Freeze did not restore after VST3 project reopen") end
local got, chunk = get_vst_chunk(track, fx, "reopened_freeze")
if not got or not chunk or #chunk <= 512 then
  return fail("reopened VST3 frozen state chunk is too small")
end

writeln("OK reopened REAPER project recalled VST3 host-parameter frozen state")
finish()
LUA

  cat >"$root/vst3_create_host_param_release_off_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/vst3_create_host_param_release_off_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end
local function get_vst_chunk(track, fx, prefix)
  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "vst_chunk")
  writeln(prefix .. "_get_vst_chunk=" .. tostring(got) ..
          " len=" .. tostring(chunk and #chunk or 0))
  return got, chunk
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track in VST3 release project") end
reaper.SetMediaTrackInfo_Value(track, "D_VOL", 0.0)

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX in VST3 release project") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "VST3i: PromptableAccompanistV2", 1, true) then
  return fail("release project FX is not exact V2 VST3")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found in VST3 release project") end

writeln("freeze_before=" .. tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))
local got_before, before_chunk = get_vst_chunk(track, fx, "before_release_play")
local before_len = before_chunk and #before_chunk or 0
if not got_before or before_len <= 512 then
  return fail("VST3 frozen state before release is too small")
end

local set_zero = reaper.TrackFX_SetParamNormalized(track, fx, freeze_index, 0.0)
writeln("set_freeze_zero=" .. tostring(set_zero))
if reaper.TrackFX_EndParamEdit then
  reaper.TrackFX_EndParamEdit(track, fx, freeze_index)
  writeln("end_param_edit_zero=true")
end
writeln("freeze_after_set_before_play=" ..
        tostring(reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)))

local started = reaper.time_precise()
reaper.Main_OnCommand(1007, 0)

local function check_after_play()
  if reaper.time_precise() - started < 2.0 then
    reaper.defer(check_after_play)
    return
  end

  reaper.Main_OnCommand(1016, 0)
  local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
  writeln("freeze_after_play=" .. tostring(freeze))
  if freeze >= 0.5 then return fail("Freeze stayed on after VST3 host parameter release playback") end
  local got_after, after_chunk = get_vst_chunk(track, fx, "after_release_play")
  local after_len = after_chunk and #after_chunk or 0
  if not got_after or after_len >= before_len or after_len > 1024 then
    return fail("VST3 release-off state chunk did not shrink")
  end

  local project_path = root .. "/v2-vst3-host-param-release-off-smoke.rpp"
  reaper.Main_SaveProjectEx(0, project_path, 8)
  local saved_file = io.open(project_path, "rb")
  writeln("saved_file=" .. tostring(saved_file ~= nil))
  if saved_file then saved_file:close() end
  writeln("project=" .. project_path)
  if not saved_file then return fail("Main_SaveProjectEx did not write VST3 release-off project") end

  writeln("OK VST3 host parameter release-off project saved")
  finish()
end

reaper.defer(check_after_play)
LUA

  cat >"$root/vst3_reopen_verify_host_param_release_off_project.lua" <<'LUA'
local root = assert(os.getenv("PULP_V2_REAPER_ROOT"))
local log = assert(io.open(root .. "/vst3_reopen_verify_host_param_release_off_project.log", "w"))

local function writeln(s) log:write(s, "\n"); log:flush() end
local function finish() log:close(); reaper.Main_OnCommand(40004, 0) end
local function fail(s) writeln("FAIL " .. s); finish() end
local function find_freeze_param(track, fx)
  for i = 0, reaper.TrackFX_GetNumParams(track, fx) - 1 do
    local _, param_name = reaper.TrackFX_GetParamName(track, fx, i, "")
    if param_name == "Freeze" then return i end
  end
  return -1
end
local function get_vst_chunk(track, fx, prefix)
  local got, chunk = reaper.TrackFX_GetNamedConfigParm(track, fx, "vst_chunk")
  writeln(prefix .. "_get_vst_chunk=" .. tostring(got) ..
          " len=" .. tostring(chunk and #chunk or 0))
  return got, chunk
end

writeln("resource=" .. reaper.GetResourcePath())
local track = reaper.GetTrack(0, 0)
if not track then return fail("no track after VST3 release-off reopen") end

local fx_count = reaper.TrackFX_GetCount(track)
writeln("fx_count=" .. tostring(fx_count))
if fx_count < 1 then return fail("no track FX after VST3 release-off reopen") end

local fx = 0
local _, fx_name = reaper.TrackFX_GetFXName(track, fx, "")
writeln("fx_name=" .. fx_name)
if not string.find(fx_name, "VST3i: PromptableAccompanistV2", 1, true) then
  return fail("reopened release-off FX is not exact V2 VST3")
end

local freeze_index = find_freeze_param(track, fx)
writeln("freeze_index=" .. tostring(freeze_index))
if freeze_index < 0 then return fail("Freeze parameter not found after VST3 release-off reopen") end

local freeze = reaper.TrackFX_GetParamNormalized(track, fx, freeze_index)
writeln("freeze_norm=" .. tostring(freeze))
if freeze >= 0.5 then return fail("Freeze resurrected after VST3 release-off project reopen") end
local got, chunk = get_vst_chunk(track, fx, "reopened_release")
if not got or not chunk or #chunk == 0 or #chunk > 1024 then
  return fail("reopened VST3 release-off state chunk is not compact")
end

writeln("OK reopened REAPER project kept VST3 host-parameter Freeze released")
finish()
LUA
}

run_reaper_step() {
  local name="$1"
  local log="$2"
  shift 2

  rm -f "$log"
  PULP_V2_REAPER_ROOT="$root" \
    "$reaper_bin" -cfgfile "$root/reaper.ini" -newinst -nosplash "$@" &
  local pid=$!
  local status=124

  for _ in $(seq 1 120); do
    if [ -f "$log" ]; then
      if grep -q '^OK ' "$log"; then
        status=0
        break
      fi
      if grep -q '^FAIL ' "$log"; then
        status=1
        break
      fi
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 1
  done

  if [ "$status" -eq 0 ]; then
    for _ in $(seq 1 10); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.2
    done
  fi

  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true

  echo "== $name =="
  if [ -f "$log" ]; then
    cat "$log"
  else
    echo "missing log: $log"
  fi

  if [ "$status" -ne 0 ]; then
    echo "step $name failed with status $status" >&2
    return "$status"
  fi
}

decode_base64() {
  base64 -D -i "$1" -o "$2"
}

verify_clap_state() {
  decode_base64 "$root/clap-host-param-freeze.clapstate.b64" \
    "$root/clap-host-param-freeze.clapstate"
  decode_base64 "$root/clap-host-param-freeze-reopened.clapstate.b64" \
    "$root/clap-host-param-freeze-reopened.clapstate"
  decode_base64 "$root/clap-host-param-release-off.clapstate.b64" \
    "$root/clap-host-param-release-off.clapstate"
  decode_base64 "$root/clap-host-param-release-off-reopened.clapstate.b64" \
    "$root/clap-host-param-release-off-reopened.clapstate"

  cmp -s "$root/clap-host-param-freeze.clapstate" \
    "$root/clap-host-param-freeze-reopened.clapstate"
  cmp -s "$root/clap-host-param-release-off.clapstate" \
    "$root/clap-host-param-release-off-reopened.clapstate"
  strings -a "$root/clap-host-param-freeze.clapstate" | grep -q 'PAV2FRZ1'
  strings -a "$root/clap-host-param-freeze-reopened.clapstate" | grep -q 'PAV2FRZ1'
  ! strings -a "$root/clap-host-param-release-off.clapstate" | grep -q 'PAV2FRZ1'
  ! strings -a "$root/clap-host-param-release-off-reopened.clapstate" | grep -q 'PAV2FRZ1'
}

install_vst3_for_reaper() {
  if [ ! -d "$vst3_bundle" ]; then
    echo "Missing VST3 bundle: $vst3_bundle" >&2
    exit 1
  fi
  mkdir -p "$user_vst3_dir"
  if [ -e "$user_vst3_bundle" ]; then
    rm -rf "$vst3_backup"
    mv "$user_vst3_bundle" "$vst3_backup"
  fi
  cp -R "$vst3_bundle" "$user_vst3_bundle"
  chmod -R u+rwX,go+rX "$user_vst3_bundle"
  rm -f "$root/reaper-vstplugins_arm64.ini"
  vst3_installed=1
}

targets=()
if contains_format clap; then targets+=(PromptableAccompanistV2_CLAP); fi
if contains_format vst3; then targets+=(PromptableAccompanistV2_VST3); fi

if [ "${#targets[@]}" -eq 0 ]; then
  echo "No formats selected. Use PULP_MAGENTA_V2_REAPER_FORMATS=clap,vst3,all." >&2
  exit 1
fi

cmake --build "$build_dir" --target "${targets[@]}" -j"$jobs"

if contains_format clap && [ ! -d "$clap_bundle" ]; then
  echo "Missing CLAP bundle: $clap_bundle" >&2
  exit 1
fi

write_reaper_ini
write_lua_scripts

if contains_format clap; then
  run_reaper_step \
    "create CLAP host-param freeze" \
    "$root/clap_create_host_param_freeze_project.log" \
    "$root/clap_create_host_param_freeze_project.lua"
  run_reaper_step \
    "reopen CLAP host-param freeze" \
    "$root/clap_reopen_verify_host_param_freeze_project.log" \
    "$root/v2-clap-host-param-freeze-smoke.rpp" \
    "$root/clap_reopen_verify_host_param_freeze_project.lua"
  run_reaper_step \
    "create CLAP host-param release-off" \
    "$root/clap_create_host_param_release_off_project.log" \
    "$root/v2-clap-host-param-freeze-smoke.rpp" \
    "$root/clap_create_host_param_release_off_project.lua"
  run_reaper_step \
    "reopen CLAP host-param release-off" \
    "$root/clap_reopen_verify_host_param_release_off_project.log" \
    "$root/v2-clap-host-param-release-off-smoke.rpp" \
    "$root/clap_reopen_verify_host_param_release_off_project.lua"
  verify_clap_state
fi

if contains_format vst3; then
  install_vst3_for_reaper
  run_reaper_step \
    "create VST3 host-param freeze" \
    "$root/vst3_create_host_param_freeze_project.log" \
    "$root/vst3_create_host_param_freeze_project.lua"
  run_reaper_step \
    "reopen VST3 host-param freeze" \
    "$root/vst3_reopen_verify_host_param_freeze_project.log" \
    "$root/v2-vst3-host-param-freeze-smoke.rpp" \
    "$root/vst3_reopen_verify_host_param_freeze_project.lua"
  run_reaper_step \
    "create VST3 host-param release-off" \
    "$root/vst3_create_host_param_release_off_project.log" \
    "$root/v2-vst3-host-param-freeze-smoke.rpp" \
    "$root/vst3_create_host_param_release_off_project.lua"
  run_reaper_step \
    "reopen VST3 host-param release-off" \
    "$root/vst3_reopen_verify_host_param_release_off_project.log" \
    "$root/v2-vst3-host-param-release-off-smoke.rpp" \
    "$root/vst3_reopen_verify_host_param_release_off_project.lua"
fi

echo "OK: PromptableAccompanistV2 REAPER host-state smoke passed"
echo "$root"
