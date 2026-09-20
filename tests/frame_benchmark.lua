-- Installed temporarily by run-frame-benchmark.ps1.
-- The runner replaces this marker with a local `config` table before launch.
-- BENCHMARK_CONFIGURATION

local vr = uevr.params.vr
local api = uevr.api
local phase_index = 1
local phase_state = "waiting"
local phase_started_at = nil
local last_status_second = -1
local phase_samples = {}
local phase_tick_count = 0
local phase_name = nil
local finished = false
local failure = nil
local saved_screen_percentage = nil
local screen_percentage_variable = nil
local phase_world = ""
local phase_pawn = ""
local capture_done = false
local capabilities_recorded = false
local gameplay_confirmed = config.game ~= "DeepRock"

local function record_capabilities()
    local lines = {"effective_scene_resolution=unknown", "gpu_scope=uevr_openxr_submission"}
    for _, name in ipairs({"r.ScreenPercentage", "r.DynamicRes.OperationMode", "r.DynamicRes.MinScreenPercentage", "r.DynamicRes.MaxScreenPercentage", "t.MaxFPS", "r.VSync", "r.GPUCsvStatsEnabled"}) do
        local ok, value = pcall(function()
            local variable = api:get_console_manager():find_variable(name)
            if variable == nil then return "unavailable" end
            return tostring(variable:get_float())
        end)
        lines[#lines+1] = name .. "=" .. (ok and value or "probe_error")
    end
    fs.write((config.result_prefix or "codex-frame-benchmark") .. "-capabilities.txt", table.concat(lines, "\n") .. "\n")
end

local function prefix() return config.result_prefix or "codex-frame-benchmark" end
local function timestamp() return os.date("!%Y-%m-%dT%H:%M:%SZ") end
local events = {}
local function marker(message)
    events[#events+1] = string.format("%s epoch=%d %s", timestamp(), os.time(), message)
    fs.write(prefix() .. "-events.txt", table.concat(events, "\n") .. "\n")
end
local function status(state, reason)
    fs.write(prefix() .. ".txt", string.format("state=%s\nsegment=%d\nmode=%s\nreason=%s\n", state,phase_index,tostring(phase_name),tostring(reason or "")))
end
local function safe_call(fn) local ok, value = pcall(fn); if ok then return value end; return nil end

local function world_name(engine)
    return safe_call(function() return engine.GameViewport.World:get_full_name() end) or ""
end

local function pawn_name()
    return safe_call(function()
        local pawn = api:get_local_pawn(0)
        if pawn == nil then return "" end
        return pawn:get_full_name()
    end) or ""
end

local function game_ready(engine)
    if not gameplay_confirmed then
        gameplay_confirmed = safe_call(function() return fs.read(prefix() .. "-ready.txt") end) == "gameplay"
        if not gameplay_confirmed then return false, "gameplay_confirmation" end
    end
    if not vr.is_openxr() or not vr.is_runtime_ready() or not vr.is_hmd_active() then return false, "runtime" end
    if world_name(engine) == "" then return false, "world" end
    if pawn_name() == "" then return false, "pawn" end
    return true, "ready"
end

local function read_mode_value(key)
    return safe_call(function() return vr:get_mod_value(key) end)
end

local function set_and_verify_mode(mode)
    local expected_native = config.native_stereo_fix and "true" or "false"
    local actual_native = read_mode_value("VR_NativeStereoFix")
    if actual_native ~= expected_native then return false, "native stereo fix readback mismatch" end
    local crop = mode == "crop" or mode == "reduced" or mode == "fixed" or mode == "native_scaled"
    local reduce = mode == "reduced" or mode == "fixed"
    local fixed = mode == "fixed" and (config.fixed_scale or 0.8) or 0
    vr.set_mod_value("VR_VolumetricFrameCrop", crop and "true" or "false")
    vr.set_mod_value("VR_VolumetricFrameReducePixels", reduce and "true" or "false")
    vr.set_mod_value("VR_VolumetricFrameFixedViewScale", tostring(fixed))
    local actual_crop = read_mode_value("VR_VolumetricFrameCrop")
    local actual_reduce = read_mode_value("VR_VolumetricFrameReducePixels")
    local actual_fixed = read_mode_value("VR_VolumetricFrameFixedViewScale")
    if actual_crop ~= (crop and "true" or "false") then return false, "crop readback mismatch" end
    if actual_reduce ~= (reduce and "true" or "false") then return false, "reduce readback mismatch" end
    if tonumber(actual_fixed) == nil or math.abs(tonumber(actual_fixed) - fixed) > 0.00001 then return false, "fixed view scale readback mismatch" end
    if mode ~= "native_scaled" and screen_percentage_variable ~= nil then
        screen_percentage_variable:set_float(saved_screen_percentage)
        if math.abs(screen_percentage_variable:get_float() - saved_screen_percentage) > 0.01 then
            return false, "screen percentage restore failed"
        end
    end
    if mode == "native_scaled" then
        if config.native_percentage == nil then return false, "native_scaled requires native_percentage" end
        local variable = safe_call(function() return api:get_console_manager():find_variable("r.ScreenPercentage") end)
        if variable == nil then return false, "r.ScreenPercentage is unavailable" end
        if saved_screen_percentage == nil then
            saved_screen_percentage = safe_call(function() return variable:get_float() end)
            screen_percentage_variable = variable
        end
        if not safe_call(function() variable:set_float(config.native_percentage); return true end) then return false, "r.ScreenPercentage could not be set" end
        local applied = safe_call(function() return variable:get_float() end)
        if applied == nil or math.abs(applied - config.native_percentage) > 0.01 then return false, "r.ScreenPercentage readback mismatch" end
        marker("r.ScreenPercentage=" .. tostring(applied) .. " effective_scene_resolution=unknown")
    end
    return true, "applied"
end

local function restore_screen_percentage()
    if screen_percentage_variable ~= nil and saved_screen_percentage ~= nil then
        safe_call(function() screen_percentage_variable:set_float(saved_screen_percentage) end)
    end
end

local function percentile(samples, fraction)
    if #samples == 0 then return 0 end
    local sorted = {}
    for i, value in ipairs(samples) do sorted[i] = value end
    table.sort(sorted)
    return sorted[math.max(1, math.ceil(#sorted * fraction))]
end

local function write_phase_result(state, reason)
    local median = percentile(phase_samples, 0.50)
    local p95 = percentile(phase_samples, 0.95)
    local csv = { "sample,delta_seconds" }
    for i, delta in ipairs(phase_samples) do csv[#csv + 1] = string.format("%d,%.9f", i, delta) end
    fs.write(prefix() .. string.format("-%02d-",phase_index) .. tostring(phase_name) .. ".csv", table.concat(csv, "\n") .. "\n")
    local summary = string.format(
        "state=%s\nphase=%s\nreason=%s\ntimestamp=%s\nticks=%d\nsamples=%d\nmedian_delta=%.9f\np95_delta=%.9f\nwidth=%d\nheight=%d\nnative_stereo_fix=%s\n",
        state, tostring(phase_name), tostring(reason or ""), timestamp(), phase_tick_count, #phase_samples,
        median, p95, vr.get_hmd_width(), vr.get_hmd_height(), tostring(config.native_stereo_fix and true or false))
    summary = summary .. string.format("segment=%d\nmode=%s\nstart_epoch=%d\nwarmup_seconds=%d\nmeasure_start_epoch=%d\nend_epoch=%d\nworld=%s\npawn=%s\n",
        phase_index, tostring(phase_name), phase_started_at or 0, config.warmup_seconds or 10,
        (phase_started_at or 0) + (config.warmup_seconds or 10), os.time(), phase_world, phase_pawn)
    fs.write(prefix() .. string.format("-%02d-",phase_index) .. tostring(phase_name) .. ".txt", summary)
    marker(string.format("phase=%s state=%s samples=%d median_delta=%.6f p95_delta=%.6f", phase_name, state, #phase_samples, median, p95))
end

local function fail(reason)
    failure = reason
    marker("state=failed reason=" .. tostring(reason))
    if phase_name ~= nil then write_phase_result("failed", reason) end
    restore_screen_percentage()
    status("failed", reason)
    finished = true
end

local function begin_phase()
    phase_name = config.modes[phase_index]
    if phase_name == nil then
        restore_screen_percentage()
        status("complete", "phases=" .. tostring(phase_index-1))
        marker("state=complete")
        finished = true
        return
    end
    phase_samples = {}
    phase_tick_count = 0
    phase_started_at = os.time()
    capture_done = not config.capture_frames
    local applied, reason = set_and_verify_mode(phase_name)
    if not applied then fail("phase " .. tostring(phase_name) .. ": " .. tostring(reason)); return end
    phase_state = "warming"
    phase_started_at = os.time()
    last_status_second = -1
    phase_samples = {}
    phase_tick_count = 0
    marker("segment=" .. phase_index .. " phase=" .. tostring(phase_name) .. " state=warming native_stereo_fix=" .. tostring(config.native_stereo_fix and true or false))
    status("warming")
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    if finished then return end
    local ready, reason = game_ready(engine)
    if not ready then
        phase_started_at = nil
        phase_state = "waiting"
        if last_status_second ~= reason then last_status_second = reason; marker("state=waiting_for_" .. tostring(reason)); status("waiting_for_" .. reason) end
        return
    end
    if phase_state == "waiting" then
        if not capabilities_recorded then record_capabilities(); capabilities_recorded = true end
        phase_world = world_name(engine)
        phase_pawn = pawn_name()
        begin_phase(); if finished then return end
        marker("world="..world_name(engine).." pawn="..pawn_name())
    end
    if world_name(engine) ~= phase_world or pawn_name() ~= phase_pawn then
        fail("world or pawn changed during phase"); return
    end
    local elapsed = os.time() - phase_started_at
    if phase_state == "capturing" then
        if read_mode_value("VR_VolumetricFrameCapture") == "false" then
            capture_done = true
            phase_state = "warming"
            phase_started_at = os.time()
            marker("capture_finished segment=" .. phase_index)
        elseif elapsed > 30 then fail("capture timeout") end
        return
    end
    if not capture_done and elapsed >= 2 then
        vr.set_mod_value("VR_VolumetricFrameCapture", "true")
        phase_state = "capturing"
        phase_started_at = os.time()
        marker("capture_requested segment=" .. phase_index)
        status("capturing")
        return
    end
    phase_tick_count = phase_tick_count + 1
    if elapsed >= (config.warmup_seconds or 10) then
        phase_state = "measuring"
        if elapsed < (config.warmup_seconds or 10) + (config.measure_seconds or 20) and delta > 0 then phase_samples[#phase_samples + 1] = delta end
    end
    if elapsed ~= last_status_second then
        last_status_second = elapsed
        marker(string.format("segment=%d phase=%s state=%s elapsed=%d ticks=%d", phase_index,phase_name,phase_state,elapsed,phase_tick_count))
        status(phase_state)
    end
    if elapsed >= (config.warmup_seconds or 10) + (config.measure_seconds or 20) then
        write_phase_result("complete", "")
        phase_index = phase_index + 1
        phase_state = "waiting"
        phase_started_at = nil
    end
end)
