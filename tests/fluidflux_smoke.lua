-- Installed temporarily by run-fluidflux-smoke.ps1; never changes rendering modes.
local vr = uevr.params.vr
local start_time = nil
local ticks = 0
local last_second = -1
local diagnostics_restored = false
local finished = false
local views = {}

uevr.sdk.callbacks.on_pre_calculate_stereo_view_offset(function(device, view_index)
    views[view_index] = (views[view_index] or 0) + 1
end)

local function write_status(state, elapsed)
    fs.write("codex-fluidflux-smoke.txt", string.format(
        "state=%s\nelapsed=%d\nticks=%d\nopenxr=%s\nready=%s\nhmd_active=%s\nnative_fix=%s\ncrop=%s\nreduce=%s\ndiagnostics=%s\nview0=%d\nview1=%d\nview2=%d\n",
        state, elapsed, ticks, tostring(vr.is_openxr()), tostring(vr.is_runtime_ready()),
        tostring(vr.is_hmd_active()), vr:get_mod_value("VR_NativeStereoFix"),
        vr:get_mod_value("VR_VolumetricFrameCrop"), vr:get_mod_value("VR_VolumetricFrameReducePixels"),
        vr:get_mod_value("VR_VolumetricFrameDiagnostics"), views[0] or 0, views[1] or 0, views[2] or 0))
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    if finished then return end
    if not vr.is_openxr() or not vr.is_runtime_ready() or not vr.is_hmd_active() then
        start_time = nil
        ticks = 0
        diagnostics_restored = false
        last_second = -1
        write_status("waiting_for_runtime", 0)
        return
    end
    if start_time == nil then
        start_time = os.time()
        -- Exercise the setting API using diagnostics only, keeping rendering unchanged.
        vr.set_mod_value("VR_VolumetricFrameDiagnostics", "false")
        write_status("diagnostics_off", 0)
    end
    ticks = ticks + 1
    local elapsed = os.time() - start_time
    if elapsed >= 3 and not diagnostics_restored then
        if vr:get_mod_value("VR_VolumetricFrameDiagnostics") ~= "false" then
            write_status("failed_setting_readback", elapsed)
            finished = true
            return
        end
        vr.set_mod_value("VR_VolumetricFrameDiagnostics", "true")
        diagnostics_restored = true
    end
    if elapsed ~= last_second then
        last_second = elapsed
        write_status(diagnostics_restored and "observing" or "diagnostics_off", elapsed)
    end
    if elapsed >= 20 and ticks >= 100 then
        local valid = vr:get_mod_value("VR_NativeStereoFix") == "true"
            and vr:get_mod_value("VR_VolumetricFrameCrop") == "false"
            and vr:get_mod_value("VR_VolumetricFrameReducePixels") == "false"
            and vr:get_mod_value("VR_VolumetricFrameDiagnostics") == "true"
        write_status(valid and "complete" or "failed_setting_readback", elapsed)
        finished = true
    end
end)
