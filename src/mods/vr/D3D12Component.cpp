#include <d3dcompiler.h>
#include <cmath>
#include <fstream>

#include <openvr.h>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"
#include "shaders/Compiled/volumetric_frame_FrameVS.inc"
#include "shaders/Compiled/volumetric_frame_FramePS.inc"
#include "shaders/Compiled/volumetric_frame_resolve_ResolvePS.inc"

#include "d3d12/DirectXTK.hpp"

#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace vrmod {
void D3D12Component::report_frame_crop(VolumetricFrameResolveResult result, uint32_t cropped, uint32_t reduced,
    const char* reason, uint32_t frame) {
    const auto& vr = VR::get();
    const uint32_t settings = (uint32_t)vr->m_volumetric_frame->value() |
        ((uint32_t)vr->m_volumetric_frame_crop->value() << 1) |
        ((uint32_t)vr->m_volumetric_frame_reduce_pixels->value() << 2) |
        ((uint32_t)vr->is_native_stereo_fix_enabled() << 3) |
        ((uint32_t)vr->is_sceneview_compatibility_enabled() << 4) |
        ((uint32_t)vr->is_splitscreen_compatibility_enabled() << 5) |
        ((uint32_t)vr->m_volumetric_frame_diagnostics->value() << 6);
    std::scoped_lock lock{m_frame_status_mtx};
    const bool changed = m_frame_status_settings != settings || m_frame_status.result != result ||
        m_frame_status.cropped != cropped || m_frame_status.reduced != reduced ||
        std::string_view{m_frame_status.reason} != reason;
    if (changed && (vr->m_volumetric_frame_diagnostics->value() || (m_frame_status_settings != ~0u && (m_frame_status_settings & 64)))) {
        const char* names[]{"baseline", "resolved", "suppressed", "failed"};
        spdlog::info("[Frame Perf] state frame={} frame_enabled={} crop_setting={} reduce_setting={} native_fix={} sceneview={} splitscreen={} result={} cropped={} reduced={} reason={} resource_failed={}",
            frame, vr->m_volumetric_frame->value(), vr->m_volumetric_frame_crop->value(), vr->m_volumetric_frame_reduce_pixels->value(),
            vr->is_native_stereo_fix_enabled(), vr->is_sceneview_compatibility_enabled(), vr->is_splitscreen_compatibility_enabled(),
            names[(int)result], cropped, reduced, reason, m_frame_resolve_failed.load());
    }
    m_frame_status_settings = settings;
    m_frame_status = {result, cropped, reduced, reason, std::chrono::steady_clock::now()};
}

bool D3D12Component::setup_frame_resolve(ID3D12Device* device) {
    if (m_frame_resolve_pipeline != nullptr) {
        return true;
    }
    if (m_frame_resolve_failed) {
        return false;
    }
    if (!setup_volumetric_frame(device)) {
        m_frame_resolve_failed = true;
        m_frame_crop_dimensions = 0;
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 8;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized{}, error{};
    auto result = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (SUCCEEDED(result)) {
        result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_frame_resolve_root));
    }
    if (SUCCEEDED(result)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_frame_resolve_root.Get();
        desc.VS = {volumetric_frame_FrameVS, sizeof(volumetric_frame_FrameVS)};
        desc.PS = {volumetric_frame_resolve_ResolvePS, sizeof(volumetric_frame_resolve_ResolvePS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_frame_resolve_pipeline));
    }
    if (FAILED(result)) {
        spdlog::error("[Frame Perf] Failed to create crop resolve pipeline: {:x}", (uint32_t)result);
        m_frame_resolve_failed = true;
        m_frame_crop_dimensions = 0;
        return false;
    }
    spdlog::info("[Frame Perf] D3D12 crop resolve pipeline ready");
    return true;
}

bool D3D12Component::resolve_volumetric_frame(d3d12::TextureContext& target, d3d12::TextureContext* scratch,
    ID3D12Resource* source, D3D12_RESOURCE_STATES source_state, bool direct_copy,
    ID3D12QueryHeap* queries, SubmissionSample* sample) {
    auto& vr = VR::get();
    const auto state = vr->m_openxr->get_submit_state(false);
    const auto& probe = state.frame_probe;
    if (sample != nullptr) {
        sample->frame = state.frame_count;
        sample->pose = state.pose_frame_count;
        sample->probe = probe.pose_frame;
        sample->generation = state.pose_generation;
        sample->fixed_view_scale = probe.fixed_view_scale;
        sample->cropped = probe.cropped_mask;
        sample->reduced = probe.reduced_mask;
        sample->lost = state.frame_crop_lost;
    }
    m_frame_submission_suppressed = false;
    m_frame_submission_resolved = false;
    if (probe.cropped_mask == 0 && probe.reduced_mask == 0 && !state.frame_crop_lost) {
        const auto blocker = vr->frame_crop_block_reason();
        const char* reason = blocker != nullptr ? blocker : m_frame_resolve_failed ? "resource-failure" :
            m_frame_crop_dimensions == 0 ? "initializing-resolve" : !probe.prepared ? "geometry-not-prepared" :
            !probe.layout.active ? "frame-layout-inactive" : probe.rect_mask != 3 || probe.projection_mask != 3 ? "incomplete-view-callbacks" :
            "full-view-fallback";
        report_frame_crop(m_frame_resolve_failed ? VolumetricFrameResolveResult::FAILED : VolumetricFrameResolveResult::BASELINE,
            0, 0, reason, state.frame_count);
        return false;
    }

    // A crop already returned to Unreal must be resolved even if settings changed.
    // Any invalid association/resources suppress this frame rather than stretching
    // cropped rays over a full-FOV image.
    const auto desc = target.texture->GetDesc();
    const auto source_desc = source != nullptr ? source->GetDesc() : D3D12_RESOURCE_DESC{};
    bool snapshot_valid = (probe.reduced_mask & ~probe.cropped_mask) == 0 &&
        probe.separate_eye_sources == (source == target.texture.Get()) &&
        (!probe.separate_eye_sources || probe.reduced_mask == 0) &&
        probe.rect_mask == 3 && probe.projection_mask == 3 &&
        probe.matches(state.frame_count, (int)desc.Width / 2, (int)desc.Height) &&
        state.stage_views.size() == 2;
    const bool resources_valid = scratch != nullptr && scratch->texture != nullptr && scratch->srv_heap != nullptr &&
        m_frame_resolve_pipeline != nullptr && m_frame_pipeline != nullptr && direct_copy &&
        source_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && source_desc.Width == desc.Width &&
        source_desc.Height == desc.Height && source_desc.DepthOrArraySize == 1 && source_desc.MipLevels == 1 &&
        source_desc.SampleDesc.Count == 1 &&
        (source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            source_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    for (uint32_t eye = 0; snapshot_valid && eye < 2; ++eye) {
        if ((probe.cropped_mask & (1u << eye)) == 0) {
            snapshot_valid = probe.baseline_rect(eye, probe.scene_rects[eye]);
            continue;
        }
        const auto& rect = probe.crops[eye].rect;
        snapshot_valid = probe.can_crop(eye) && rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
            rect.x <= probe.width - rect.width && rect.y <= probe.height - rect.height;
    }
    const auto result = frame_resolve_result(probe.has_modified_view(), state.frame_crop_lost,
        snapshot_valid, resources_valid, m_frame_resolve_failed.load());
    if (target.rtv_heap == nullptr) {
        if (!target.create_rtv(g_framework->get_d3d12_hook()->get_device(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            m_frame_crop_dimensions = 0;
            m_frame_resolve_failed = true;
            m_frame_submission_suppressed = true;
            report_frame_crop(VolumetricFrameResolveResult::FAILED, 0, 0, "output-rtv-failure", state.frame_count);
            spdlog::error("[Frame Perf] Cannot clear invalid cropped frame: no output RTV");
            return true;
        }
    }
    const float background[]{0, probe.green ? 1.0f : 0.0f, 0, 1};
    auto list = target.commands.cmd_list.Get();
    const auto rtv = target.get_rtv();
    // Native Stereo Fix has just assembled the two eye-local sources into this
    // target. Preserve that image before clearing it for the cropped resolve.
    if (result == VolumetricFrameResolveResult::RESOLVED) {
        if (queries != nullptr) list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        target.commands.copy(source, scratch->texture.Get(), source_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (queries != nullptr) list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    }
    list->ClearRenderTargetView(rtv, background, 0, nullptr);
    target.commands.has_commands = true;
    if (result != VolumetricFrameResolveResult::RESOLVED) {
        // Association/callback loss suppresses this frame only. Keep readiness
        // so the next correctly associated frame can recover automatically.
        if (result == VolumetricFrameResolveResult::FAILED) {
            m_frame_crop_dimensions = 0;
            m_frame_resolve_failed = true;
        }
        m_frame_submission_suppressed = true;
        vr->m_volumetric_frame_layout.active = false;
        const auto& diagnostic = state.frame_crop_diagnostic;
        const char* reason = state.frame_crop_lost ? frame_crop_loss_name(diagnostic.reason) :
            !snapshot_valid ? "invalid-snapshot" : "incompatible-resolve-resources";
        SPDLOG_INFO_EVERY_N_SEC(1, "[Frame Perf] resolve rejected frame={} reason={} transient={} associated={} requested_render={} source_pose={} source_probe={} generation={} source_cropped={} source_reduced={} source_projections={} source_rects={} snapshot_valid={} resources_valid={}",
            state.frame_count, reason, result == VolumetricFrameResolveResult::SUPPRESSED,
            diagnostic.render_frame_associated, diagnostic.requested_render_frame, diagnostic.source_pose_frame,
            diagnostic.source_probe_frame, diagnostic.source_pose_generation, diagnostic.source_cropped_mask,
            diagnostic.source_reduced_mask, diagnostic.source_projection_mask, diagnostic.source_rect_mask, snapshot_valid, resources_valid);
        report_frame_crop(result, 0, 0, reason, state.frame_count);
        return true;
    }
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(m_frame_resolve_root.Get());
    list->SetPipelineState(m_frame_resolve_pipeline.Get());
    ID3D12DescriptorHeap* heaps[]{scratch->srv_heap->Heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(1, scratch->get_srv_gpu());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto crop = (probe.cropped_mask & (1u << eye)) != 0
            ? probe.crops[eye].rect : VolumetricFramePixelRect{0, 0, probe.width, probe.height};
        const auto source_rect = (probe.cropped_mask & (1u << eye)) != 0
            ? probe.packed_scene_rect(eye) : VolumetricFramePixelRect{(int)eye * probe.width, 0, probe.width, probe.height};
        const int x = (int)eye * probe.width + crop.x;
        const std::array<glm::vec4, 2> constants{
            glm::vec4{(float)x, (float)crop.y, (float)crop.width, (float)crop.height},
            glm::vec4{(float)source_rect.x, (float)source_rect.y, (float)source_rect.width, (float)source_rect.height}
        };
        const D3D12_VIEWPORT viewport{(float)x, (float)crop.y, (float)crop.width, (float)crop.height, 0, 1};
        const D3D12_RECT scissor{x, crop.y, x + crop.width, crop.y + crop.height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRoot32BitConstants(0, 8, constants.data(), 0);
        list->DrawInstanced(3, 1, 0, 0);
    }
    m_frame_submission_resolved = true;
    m_frame_resolved_frame = state.frame_count;
    m_frame_resolved_generation = state.pose_generation;
    return true;
}

bool D3D12Component::setup_volumetric_frame(ID3D12Device* device) {
    if (m_frame_pipeline != nullptr) {
        return true;
    }
    if (m_frame_mask_failed) {
        return false;
    }

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 32;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ComPtr<ID3DBlob> serialized{}, error{};
    auto result = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (SUCCEEDED(result)) {
        result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_frame_root));
    }
    if (SUCCEEDED(result)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_frame_root.Get();
        desc.VS = {volumetric_frame_FrameVS, sizeof(volumetric_frame_FrameVS)};
        desc.PS = {volumetric_frame_FramePS, sizeof(volumetric_frame_FramePS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_frame_pipeline));
    }
    if (FAILED(result)) {
        spdlog::error("[Volumetric Frame] Failed to create D3D12 mask pipeline: {:x}", (uint32_t)result);
        m_frame_mask_failed = true;
        return false;
    }
    spdlog::info("[Volumetric Frame] D3D12 mask pipeline ready");
    return true;
}

void D3D12Component::reset_volumetric_frame_anchor() {
    std::scoped_lock lock{m_frame_anchor_mtx};
    m_frame_was_enabled = false;
    m_frame_was_ui_matched = false;
}

VolumetricFrameLayout D3D12Component::prepare_volumetric_frame(const std::array<glm::mat4, 2>& eyes) {
    std::scoped_lock lock{m_frame_anchor_mtx};
    auto& vr = VR::get();
    const bool recenter = vr->m_volumetric_frame_recenter.exchange(false);

    const auto make_slate_anchor = [&]() {
        auto rotation_offset = glm::inverse(vr->get_rotation_offset());

        if (vr->is_decoupled_pitch_enabled() && vr->is_decoupled_pitch_ui_adjust_enabled()) {
            const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
            const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);
            rotation_offset = glm::normalize(glm::inverse(pre_flat_pitch * vr->get_rotation_offset()));
        }

        auto anchor = Matrix4x4f{rotation_offset};
        anchor[3] += vr->get_standing_origin();
        return anchor;
    };

    const bool match_ui = vr->m_volumetric_frame_match_ui->value();
    const bool rematch = match_ui != m_frame_was_ui_matched;
    const bool initial_activation = !m_frame_was_enabled;
    const auto anchor_source = volumetric_frame_anchor_source(match_ui, initial_activation, rematch, recenter);
    if (anchor_source != VolumetricFrameAnchorSource::KEEP) {
        if (anchor_source == VolumetricFrameAnchorSource::UI) {
            // Always use the normal stage-anchored UI placement, even if its
            // saved presentation preference follows the head.
            m_frame_anchor = make_slate_anchor();
        } else {
            // Explicit recenter and initial manual placement use the head.
            const auto forward = -glm::vec3{eyes[0][2] + eyes[1][2]};
            const auto yaw = std::atan2(-forward.x, -forward.z);
            m_frame_anchor = glm::mat4_cast(glm::angleAxis(yaw, glm::vec3{0, 1, 0}));
            m_frame_anchor[3] = (eyes[0][3] + eyes[1][3]) * 0.5f;
        }
        spdlog::info("[Volumetric Frame] Recentered window");
    }
    m_frame_was_enabled = true;
    m_frame_was_ui_matched = match_ui;
    const auto frame_size = match_ui
        ? volumetric_frame_size(vr->get_overlay_component().slate_size())
        : glm::vec2{vr->m_volumetric_frame_width->value(), vr->m_volumetric_frame_width->value() * 9.0f / 16.0f};
    const auto frame_pose = match_ui
        ? apply_volumetric_frame_offsets(
            m_frame_anchor,
            vr->get_overlay_component().slate_distance(),
            vr->get_overlay_component().slate_x_offset(),
            vr->get_overlay_component().slate_y_offset())
        : apply_volumetric_frame_offsets(
            m_frame_anchor,
            vr->m_volumetric_frame_distance->value(),
            0.0f,
            0.0f);

    return {frame_pose, frame_size, true};
}

void D3D12Component::draw_volumetric_frame(d3d12::TextureContext& target, ID3D12Resource* resource, ID3D12Resource* source,
    SubmissionSample* sample) {
    auto& vr = VR::get();
    // Peek without consuming the render-frame association needed by xrEndFrame.
    const auto state = vr->m_openxr->get_submit_state(false);
    const bool cropped = state.frame_probe.cropped_mask != 0 || state.frame_probe.reduced_mask != 0;
    // The layer builder runs later in this same frame. Never leave it with a
    // pose from a previous frame when the mask cannot be produced.
    vr->m_volumetric_frame_layout.active = false;
    if (m_frame_submission_suppressed) return;
    auto reject_mask = [&](const char* reason, bool resource_failure) {
        if (!cropped && !m_frame_submission_resolved && !state.frame_crop_lost) return;
        if (resource_failure) {
            m_frame_resolve_failed = true;
            m_frame_crop_dimensions = 0;
        }
        m_frame_submission_suppressed = true;
        if (target.rtv_heap != nullptr) {
            const float background[]{0, state.frame_probe.green ? 1.0f : 0.0f, 0, 1};
            target.commands.cmd_list->ClearRenderTargetView(target.get_rtv(), background, 0, nullptr);
            target.commands.has_commands = true;
        }
        report_frame_crop(m_frame_resolve_failed ? VolumetricFrameResolveResult::FAILED : VolumetricFrameResolveResult::SUPPRESSED,
            0, 0, reason, state.frame_count);
    };
    if (state.frame_crop_lost || (cropped && m_frame_resolve_failed) || (cropped && !state.frame_probe.matches(state.frame_count,
            (int)resource->GetDesc().Width / 2, (int)resource->GetDesc().Height))) {
        reject_mask("mask-frame-invalid", false);
        return;
    }
    if (m_frame_submission_resolved && (!cropped || state.frame_count != m_frame_resolved_frame ||
        state.pose_generation != m_frame_resolved_generation)) {
        reject_mask("mask-resolve-frame-changed", false);
        return;
    }
    if (!cropped && !vr->is_volumetric_frame_enabled()) {
        reset_volumetric_frame_anchor();
        return;
    }

    if (state.stage_views.size() != 2) {
        reject_mask("mask-eye-poses-unavailable", false);
        return;
    }
    const auto desc = resource->GetDesc();
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (desc.SampleDesc.Count != 1) {
        if (!m_frame_mask_failed) {
            spdlog::error("[Volumetric Frame] Multisampled swapchains are unsupported");
        }
        m_frame_mask_failed = true;
        reject_mask("mask-multisampling-unsupported", true);
        return;
    }
    if (!setup_volumetric_frame(device)) {
        reject_mask("mask-pipeline-failure", true);
        return;
    }
    if (target.rtv_heap == nullptr) {
        target.texture = resource;
        if (!target.create_rtv(device, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            m_frame_mask_failed = true;
            reject_mask("mask-rtv-failure", true);
            return;
        }
    }

    auto pose_matrix = [](const XrPosef& pose) {
        auto matrix = glm::mat4_cast(glm::quat{pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z});
        matrix[3] = glm::vec4{pose.position.x, pose.position.y, pose.position.z, 1.0f};
        return matrix;
    };
    const std::array<glm::mat4, 2> eyes{pose_matrix(state.stage_views[0].pose), pose_matrix(state.stage_views[1].pose)};
    const auto& probe = state.frame_probe;
    const bool use_probe = probe.matches(state.frame_count, (int)desc.Width / 2, (int)desc.Height);
    const auto layout = use_probe ? probe.layout : prepare_volumetric_frame(eyes);
    vr->m_volumetric_frame_layout = layout;
    const auto& frame_pose = layout.pose;
    const auto& frame_size = layout.size;

    if (vr->m_volumetric_frame_diagnostics->value()) {
        const auto source_desc = source != nullptr ? source->GetDesc() : D3D12_RESOURCE_DESC{};
        // A record for each eye, rate-limited together; this is candidate area,
        // never a measured saving or proof of Unreal's visibility behavior.
        static auto last_log = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(2)) {
            last_log = now;
            spdlog::info("[Frame Perf] submit frame={} pose={} prepared={} matched={} projections={} rects={} scene={}x{} output={}x{} native_fix={} sceneview={} splitscreen={} cropped={} reduced={}",
                state.frame_count, state.pose_frame_count, probe.prepared, use_probe, probe.projection_mask, probe.rect_mask,
                source_desc.Width, source_desc.Height, desc.Width, desc.Height,
                vr->is_native_stereo_fix_enabled(), vr->is_sceneview_compatibility_enabled(), vr->is_splitscreen_compatibility_enabled(), probe.cropped_mask, probe.reduced_mask);
            spdlog::info("[Frame Perf] geometry frame={} center={},{},{} size={},{} boundsL={},{},{},{} boundsR={},{},{},{}",
                state.frame_count, layout.pose[3].x, layout.pose[3].y, layout.pose[3].z, layout.size.x, layout.size.y,
                probe.bounds[0].x, probe.bounds[0].y, probe.bounds[0].z, probe.bounds[0].w,
                probe.bounds[1].x, probe.bounds[1].y, probe.bounds[1].z, probe.bounds[1].w);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                const auto& crop = probe.crops[eye];
                const auto& rect = probe.scene_rects[eye];
                const auto& p = probe.projections[eye];
                const auto area = probe.width > 0 && probe.height > 0
                    ? 100.0 * crop.rect.width * crop.rect.height / ((double)probe.width * probe.height) : 100.0;
                spdlog::info("[Frame Perf] candidate frame={} eye={} crop={},{},{},{} area_pct={:.2f} fallback={} view={},{},{},{} P=[{},{},{},{};{},{},{},{};{},{},{},{};{},{},{},{}]",
                    state.frame_count, eye, crop.rect.x, crop.rect.y, crop.rect.width, crop.rect.height, area, (int)crop.fallback,
                    rect.x, rect.y, rect.width, rect.height,
                    p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3],
                    p[2][0], p[2][1], p[2][2], p[2][3], p[3][0], p[3][1], p[3][2], p[3][3]);
            }
        }
    }

    const auto stage_to_frame = glm::inverse(frame_pose);

    auto list = target.commands.cmd_list.Get();
    const auto rtv = target.get_rtv();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(m_frame_root.Get());
    list->SetPipelineState(m_frame_pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (uint32_t i = 0; i < 2; ++i) {
        const auto eye_to_frame = stage_to_frame * eyes[i];
        const auto& fov = state.stage_views[i].fov;
        const auto bounds = use_probe ? probe.bounds[i] : glm::vec4{
            vr->m_openxr->view_bounds[i][0], vr->m_openxr->view_bounds[i][1],
            vr->m_openxr->view_bounds[i][2], vr->m_openxr->view_bounds[i][3]};
        const int eye_width = (int)desc.Width / 2;
        // Match the integer sub-image rectangle in OpenXR::end_frame exactly.
        const int x = i * eye_width + (int)(bounds[0] * eye_width);
        const int y = (int)(bounds[2] * desc.Height);
        const int width = (int)(bounds[1] * eye_width) - (x - i * eye_width);
        const int height = (int)(bounds[3] * desc.Height) - y;
        if (width <= 0 || height <= 0) {
            continue;
        }
        const float half_width = frame_size.x * 0.5f;
        const std::array<glm::vec4, 8> constants{
            eye_to_frame[0], eye_to_frame[1], eye_to_frame[2], eye_to_frame[3],
            glm::vec4{std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleUp), std::tan(fov.angleDown)},
            glm::vec4{(float)x, (float)y, (float)width, (float)height},
            glm::vec4{half_width, frame_size.y * 0.5f, 0, 0},
            glm::vec4{0, (cropped ? probe.green : vr->m_volumetric_frame_green->value()) ? 1.0f : 0.0f, 0, 1}
        };
        static_assert(sizeof(constants) == 32 * sizeof(float));
        const D3D12_VIEWPORT viewport{(float)x, (float)y, (float)width, (float)height, 0, 1};
        const D3D12_RECT scissor{x, y, x + width, y + height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRoot32BitConstants(0, 32, constants.data(), 0);
        list->DrawInstanced(3, 1, 0, 0);
        if (sample != nullptr) sample->mask_drawn |= 1u << i;
    }
    target.commands.has_commands = true;
    if (m_frame_submission_resolved) {
        report_frame_crop(VolumetricFrameResolveResult::RESOLVED, probe.cropped_mask, probe.reduced_mask, "none", state.frame_count);
    }
}

vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (!vr->is_volumetric_frame_enabled()) {
        reset_volumetric_frame_anchor();
        vr->m_volumetric_frame_layout.active = false;
    }

    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    const auto ui_should_invert_alpha = vr->get_overlay_component().should_invert_ui_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    const auto ui_target = ffsr->get_render_target_manager()->get_ui_target();

    const auto frame_count = vr->m_render_frame_count;

    if (m_game_tex.texture.Get() == nullptr && backbuffer.Get() == real_backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer->GetDesc();
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), std::nullopt, std::nullopt, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // UE backbuffer is not VR compatible, so we need to copy it to a new texture with this one.

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && m_game_tex.texture.Get() != backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as reference to original");

        if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        }
    }

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D12Resource*)scene_capture->get_native_resource() : nullptr;

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");

            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (render_target == nullptr) {
            return;
        }

        // Also the same for right, even though it's not a double wide texture
        D3D12_BOX left_src_box{
            .left = 0,
            .top = 0,
            .front = 0,
            .right = m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1
        };

        commands.copy_region_stereo(
            m_game_tex.texture.Get(), m_scene_capture_tex.texture.Get(), render_target,
            &left_src_box, &left_src_box,
            0, 0, 0, m_backbuffer_size[0] / 2, 0, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );
    };

    // For copying the real backbuffer if we need to
    if (m_game_tex.texture.Get() != nullptr && backbuffer == real_backbuffer) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            command_ctx.wait(INFINITE);
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_ctx.copy(real_backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            //m_game_tex_commands[idx].copy(backbuffer.Get(), m_game_tex.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, ENGINE_SRC_COLOR);
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                command_ctx.cmd_list.Get(),
                m_backbuffer_copy,
                m_game_tex,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            command_ctx.execute();
        }

        backbuffer = m_game_tex.texture;
    }

    if (ui_target != nullptr) {
        if (m_game_ui_tex.texture.Get() != ui_target->get_native_resource()) {
            if (!m_game_ui_tex.setup(device, 
                (ID3D12Resource*)ui_target->get_native_resource(), 
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const auto native = (ID3D12Resource*)ui_target->get_native_resource();
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        m_game_ui_tex.reset(); // Probably fixes non-resident errors.
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (ui_should_invert_alpha && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            d3d12::render_srv_to_rtv(m_ui_batch_alpha_invert.get(), commands.cmd_list.Get(), m_game_ui_tex, m_game_ui_tex, std::nullopt, ENGINE_SRC_COLOR, ENGINE_SRC_COLOR);
        }

        draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame);

        if (is_2d_screen && m_game_tex.texture.Get() != nullptr && m_game_tex.srv_heap != nullptr) {
            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            // Render left side to left screen tex
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                commands.cmd_list.Get(),
                m_game_tex,
                m_2d_screen_tex[0],
                RECT{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]},
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                // Render right side to right screen tex
                if (m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_2d_screen_tex[1],
                        RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]},
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
        if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }

        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_should_invert_alpha ? 1.0f : 0.0f };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target != nullptr) {
                m_openvr.ui_tex.commands.copy((ID3D12Resource*)ui_target->get_native_resource(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (is_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, std::nullopt, ENGINE_SRC_COLOR);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, clear_rt, ENGINE_SRC_COLOR);
                }
            } else if (ui_target != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, (ID3D12Resource*)ui_target->get_native_resource(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
            }

            auto fw_rt = g_framework->get_rendertarget_d3d12();

            if (fw_rt && g_framework->is_drawing_anything()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
        } else if (m_game_ui_tex.commands.ready()) {
            m_game_ui_tex.commands.wait(INFINITE);
            draw_2d_view(m_game_ui_tex.commands, nullptr);
            clear_rt(m_game_ui_tex.commands);
            m_game_ui_tex.commands.execute();
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};
    const auto crop_state = runtime->is_openxr() ? vr->m_openxr->get_submit_state(false) : runtimes::OpenXR::PipelineState{};
    const bool cropped_submission = crop_state.frame_crop_lost || crop_state.frame_probe.cropped_mask != 0 || crop_state.frame_probe.reduced_mask != 0;

    if (vr->is_depth_enabled() && runtime->is_depth_allowed() && !vr->is_volumetric_frame_enabled() && !cropped_submission) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }

            if (is_actually_afr) {
                D3D12_BOX src_box{};

                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            } else {
                // Copy over the entire double wide instead
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, true);
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr) {
                m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }

            if (!is_afr) {
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openvr.copy_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                } else {
                    m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                m_openvr.copy_left_to_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            thread_local std::vector<XrCompositionLayerBaseHeader*> quad_layers{}; quad_layers.clear();

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame) {
    if (command_list == nullptr || m_game_ui_tex.texture == nullptr) {
        return;
    }

    if (m_game_ui_tex.srv_heap == nullptr || m_game_ui_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    if (m_game_tex.texture == nullptr || m_game_tex.srv_heap == nullptr || m_game_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    const auto& vr = VR::get();

    if (!vr->is_hmd_active() || !vr->m_desktop_fix->value()) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if (vr->is_using_afr() && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;

    const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
    const auto eye_height = (float)m_backbuffer_size[1];
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled()) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0] / 2;
        source_rect.bottom = m_backbuffer_size[1];
    } else {
        source_rect.left = (LONG)m_backbuffer_size[0] / 2;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0];
        source_rect.bottom = m_backbuffer_size[1];
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { m_game_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(m_game_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)m_backbuffer_size[0], (uint32_t)m_backbuffer_size[1] },
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    //////
    // UI
    //////
    // Set descriptor heaps
    ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, ui_heaps);

    batch->Draw(m_game_ui_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height },
        dest_rect, 
        DirectX::Colors::White);

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        clear_backbuffer();
    }
}

void D3D12Component::on_reset(VR* vr) {
    if (vr->m_volumetric_frame_diagnostics->value()) {
        spdlog::info("[Frame Perf] reset: clearing crop readiness and status; resource failure was {}", m_frame_resolve_failed.load());
    }
    m_frame_crop_dimensions = 0;
    m_frame_submission_suppressed = false;
    m_frame_submission_resolved = false;
    {
        std::scoped_lock lock{m_frame_status_mtx};
        m_frame_status = {};
        m_frame_status_settings = ~0u;
    }
    m_force_reset = true;
    vr->m_volumetric_frame_layout.active = false;

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_game_tex.reset();
    m_scene_capture_tex.reset();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            g_framework->get_d3d12_rt_size()[0] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            g_framework->get_d3d12_rt_size()[1] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_frame_pipeline.Reset();
    m_frame_root.Reset();
    m_frame_resolve_pipeline.Reset();
    m_frame_resolve_root.Reset();
    m_frame_resolve_failed = false;
    m_frame_mask_failed = false;
    reset_volumetric_frame_anchor();
    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        auto screen_desc = backbuffer_desc;
        screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        for (auto& context : m_2d_screen_tex) {
            ComPtr<ID3D12Resource> screen_tex{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &screen_desc, ENGINE_SRC_COLOR, nullptr,
                    IID_PPV_ARGS(&screen_tex)))) {
                spdlog::error("[VR] Failed to create 2D screen texture.");
                continue;
            }

            screen_tex->SetName(L"2D Screen Texture");

            if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
                spdlog::error("[VR] Failed to setup 2D screen context.");
                continue;
            }
        }
    }

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture. it's the desktop resolution.
        auto ui_desc = backbuffer_desc;
        ui_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        ui_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(backbuffer_desc.Format);

    // Custom blend state to flip the alpha in-place of the UI texture without an intermediate render target
    {
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{backbuffer_desc.Format, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_ONE;
        bdrt.DestBlendAlpha = D3D12_BLEND_ZERO;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            backbuffer_desc.Format, 
            alpha_luminance_sprite_ps_SpritePixelShader, 
            alpha_luminance_sprite_ps_SpriteVertexShader, 
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    for (const auto& [index, context] : this->contexts) {
        if (context.capture.readback != nullptr) {
            spdlog::error("[Frame Capture] failed reason=swapchain-recreated frame={}", context.capture.sample.frame);
            vr->m_volumetric_frame_capture->value() = false;
        }
    }
    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                texture_ctx->texture.Reset();
                texture_ctx->rtv_heap.reset();

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_swapchain_create_info.width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_swapchain_create_info.height = g_framework->get_d3d12_rt_size().y;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_desc.Height = g_framework->get_d3d12_rt_size().y;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};
    VR::get()->d3d12().m_frame_crop_dimensions = 0;

    if (this->contexts.empty()) {
        return;
    }
    
    auto& vr = VR::get();
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        if (ctx.capture.readback != nullptr) {
            spdlog::error("[Frame Capture] failed reason=swapchain-destroyed frame={}", ctx.capture.sample.frame);
            vr->m_volumetric_frame_capture->value() = false;
        }

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();
        ctx.frame_scratch.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

bool D3D12Component::OpenXR::SubmissionCapture::begin(d3d12::CommandContext& commands,
    ID3D12Resource* source, const SubmissionSample& source_sample) {
    const auto desc = source->GetDesc();
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (!commands.ready() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        (!bgra && !rgba)) {
        spdlog::error("[Frame Capture] failed reason=unsupported-output ready={} dimension={} format={} samples={} quality={} array={} mips={} size={}x{} flags={}",
            commands.ready(), uint32_t(desc.Dimension), uint32_t(desc.Format), desc.SampleDesc.Count, desc.SampleDesc.Quality,
            desc.DepthOrArraySize, desc.MipLevels, desc.Width, desc.Height, uint32_t(desc.Flags));
        return false;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    // Bound this diagnostic allocation even if an accidental request arrives at extreme resolution.
    if (bytes == 0 || bytes > 1024ull * 1024 * 1024) {
        spdlog::error("[Frame Capture] failed reason=readback-size bytes={}", bytes);
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) {
        spdlog::error("[Frame Capture] failed reason=readback-allocation hr={:x} bytes={}", uint32_t(result), bytes);
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands.cmd_list->ResourceBarrier(1, &barrier);
    commands.cmd_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands.cmd_list->ResourceBarrier(1, &barrier);
    commands.has_commands = true;
    fence = commands.fence;
    fence_value = commands.fence_value + 1;
    sample = source_sample;
    spdlog::info("[Frame Capture] queued frame={} pose={} generation={} cropped={} reduced={} fixed_scale={} resolved={} suppressed={} mask_drawn={} size={}x{} bytes={} format={} rgba_swizzle={}",
        sample.frame, sample.pose, sample.generation, sample.cropped, sample.reduced, sample.fixed_view_scale,
        sample.resolved, sample.suppressed, sample.mask_drawn, desc.Width, desc.Height, bytes, uint32_t(desc.Format), rgba);
    return true;
}

bool D3D12Component::OpenXR::SubmissionCapture::finish() {
    if (readback == nullptr || abandoned) return false;
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        spdlog::error("[Frame Capture] failed reason=device-removed frame={}", sample.frame);
        *this = {};
        return true;
    }
    if (completed < fence_value) return false;
    const auto start = std::chrono::steady_clock::now();
    void* mapped{};
    D3D12_RANGE range{0, size_t(bytes)};
    if (FAILED(readback->Map(0, &range, &mapped))) {
        spdlog::error("[Frame Capture] failed reason=readback-map frame={}", sample.frame);
        *this = {};
        return true;
    }
    try {
        const auto directory = Framework::get_persistent_dir("diagnostics");
        std::filesystem::create_directories(directory);
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto path = directory / ("frame-capture-" + std::to_string(stamp) + "-" + std::to_string(sample.frame) + ".bmp");
        const auto width = footprint.Footprint.Width;
        const auto height = footprint.Footprint.Height;
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        static_assert(sizeof(file) == 14 && sizeof(info) == 40);
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        info.biSize = sizeof(info);
        info.biWidth = LONG(width);
        info.biHeight = -LONG(height); // Top-down BGRA; no extra full-image CPU copy.
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = width * height * 4;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info), sizeof(info));
        const auto pixels = static_cast<const char*>(mapped) + footprint.Offset;
        std::vector<char> bgra_row(rgba ? size_t(width) * 4 : 0);
        for (uint32_t row = 0; row < height; ++row) {
            const auto source_row = pixels + size_t(row) * footprint.Footprint.RowPitch;
            if (rgba) {
                for (size_t x = 0; x < size_t(width) * 4; x += 4) {
                    bgra_row[x] = source_row[x + 2];
                    bgra_row[x + 1] = source_row[x + 1];
                    bgra_row[x + 2] = source_row[x];
                    bgra_row[x + 3] = source_row[x + 3];
                }
            }
            output.write(rgba ? bgra_row.data() : source_row, size_t(width) * 4);
        }
        output.close();
        spdlog::info("[Frame Capture] complete frame={} pose={} generation={} cropped={} reduced={} fixed_scale={} resolved={} suppressed={} mask_drawn={} size={}x{} write_stall_ms={:.3f} file=\"{}\"",
            sample.frame, sample.pose, sample.generation, sample.cropped, sample.reduced, sample.fixed_view_scale,
            sample.resolved, sample.suppressed, sample.mask_drawn, width, height,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), path.string());
    } catch (const std::exception& error) {
        spdlog::error("[Frame Capture] failed reason=file-write frame={} error={}", sample.frame, error.what());
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    *this = {};
    return true;
}

bool D3D12Component::OpenXR::SubmissionTiming::begin(d3d12::CommandContext& commands) {
    if (unavailable || fence != nullptr || !commands.ready()) {
        return false;
    }
    if (queries == nullptr) {
        auto& hook = g_framework->get_d3d12_hook();
        auto device = hook->get_device();
        auto queue = hook->get_command_queue();
        D3D12_QUERY_HEAP_DESC query_desc{};
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_desc.Count = query_count;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = sizeof(uint64_t) * query_count;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0 ||
            FAILED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
            unavailable = true;
            queries.Reset();
            readback.Reset();
            spdlog::warn("[Frame Timing] D3D12 submission GPU timestamps unavailable for image; CPU diagnostics remain enabled");
            return false;
        }
    }
    commands.cmd_list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    // Reserve before execute: failed Close/Signal must never permit unsafe reuse.
    fence = commands.fence;
    fence_value = commands.fence_value + 1;
    return true;
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box,
    bool native_eye_assembly)
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];
    const bool double_wide = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE;
    if (double_wide && ctx.capture.finish()) vr->m_volumetric_frame_capture->value() = false;
    if (double_wide && ctx.capture.abandoned && vr->m_volumetric_frame_capture->value()) {
        spdlog::error("[Frame Capture] failed reason=previous-submission-failed");
        vr->m_volumetric_frame_capture->value() = false;
    }
    const bool capture_requested = double_wide && vr->m_volumetric_frame_capture->value();
    // The harness waits for request=false and the complete log before warming up.
    // Allocation, GPU readback, and synchronous disk writing are outside timing epochs.
    const bool capture_active = capture_requested || (ctx.capture.readback != nullptr && !ctx.capture.abandoned);
    const bool timing_enabled = double_wide && vr->m_volumetric_frame_diagnostics->value() && !capture_active;
    const bool sample_enabled = timing_enabled || capture_requested;
    const bool mask_enabled = vr->is_volumetric_frame_enabled();
    const uint32_t timing_settings = uint32_t(timing_enabled) | (uint32_t(mask_enabled) << 1) |
        (uint32_t(vr->is_native_stereo_fix_enabled()) << 2) |
        (uint32_t(vr->m_volumetric_frame_crop->value()) << 3) |
        (uint32_t(vr->m_volumetric_frame_reduce_pixels->value()) << 4);
    const auto fixed_scale = vr->m_volumetric_frame_fixed_view_scale->value();
    const auto fixed_scale_bits = std::bit_cast<uint32_t>(fixed_scale);
    if (ctx.timing_settings != timing_settings || ctx.timing_fixed_scale_bits != fixed_scale_bits) {
        ctx.timing_intervals.clear();
        ctx.timing_pending = ctx.timing_invalid = ctx.timing_stale = 0;
        ctx.timing_report = std::chrono::steady_clock::now();
        ctx.timing_settings = timing_settings;
        ctx.timing_fixed_scale_bits = fixed_scale_bits;
        ++ctx.timing_epoch;
    }

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            const auto fence_wait_start = timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            texture_ctx->commands.wait(INFINITE);
            const auto fence_wait_end = timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (timing_enabled) {
                ctx.timings.resize(ctx.texture_contexts.size());
            }

            auto& component = vr->d3d12();
            const auto output_resource = ctx.textures[texture_index].texture;
            const auto output_desc = output_resource->GetDesc();
            auto valid_eye_source = [&](ID3D12Resource* eye_source) {
                if (eye_source == nullptr) return false;
                const auto desc = eye_source->GetDesc();
                return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    desc.Width >= output_desc.Width / 2 && desc.Height == output_desc.Height &&
                    desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
                    (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
            };
            const bool assembled_eyes = native_eye_assembly && double_wide && resource == nullptr &&
                pre_commands && !additional_commands && src_box == nullptr &&
                component.m_backbuffer_size[0] == output_desc.Width && component.m_backbuffer_size[1] == output_desc.Height &&
                valid_eye_source(component.m_game_tex.texture.Get()) && valid_eye_source(component.m_scene_capture_tex.texture.Get());
            const bool direct_resolve = src_box == nullptr && !pre_commands && !additional_commands;
            auto resolve_source = assembled_eyes ? output_resource : resource;
            if (double_wide) {
                texture_ctx->texture = ctx.textures[texture_index].texture;
                // Prepare every image before allowing a projection hook to crop.
                // Scratch is never resized/replaced while an output fence uses it.
                if (vr->is_frame_crop_requested() && !component.m_frame_resolve_failed &&
                    component.m_frame_crop_dimensions == 0 && resolve_source != nullptr &&
                    (direct_resolve || assembled_eyes)) {
                    auto device = g_framework->get_d3d12_hook()->get_device();
                    const auto desc = texture_ctx->texture->GetDesc();
                    const auto source_desc = resolve_source->GetDesc();
                    bool ready = desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
                        source_desc.Width == desc.Width && source_desc.Height == desc.Height &&
                        source_desc.SampleDesc.Count == 1 && source_desc.DepthOrArraySize == 1 && source_desc.MipLevels == 1 &&
                        (source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                            source_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) && component.setup_frame_resolve(device);
                    if (ready) {
                        ctx.frame_scratch.resize(ctx.textures.size());
                        for (size_t image = 0; ready && image < ctx.textures.size(); ++image) {
                            auto& output = *ctx.texture_contexts[image];
                            output.commands.wait(INFINITE);
                            output.texture = ctx.textures[image].texture;
                            ready = output.rtv_heap != nullptr || output.create_rtv(device, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                            if (ready && ctx.frame_scratch[image] == nullptr) {
                                auto scratch = std::make_unique<d3d12::TextureContext>();
                                auto scratch_desc = desc;
                                scratch_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                                scratch_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                                D3D12_HEAP_PROPERTIES heap{};
                                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                                ready = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scratch_desc,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&scratch->texture))) &&
                                    scratch->create_srv(device, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                                if (ready) {
                                    scratch->texture->SetName(L"Aperture crop resolve scratch");
                                    ctx.frame_scratch[image] = std::move(scratch);
                                }
                            }
                        }
                        if (!ready) {
                            component.m_frame_resolve_failed = true;
                            spdlog::error("[Frame Perf] Crop scratch/RTV allocation failed; retaining full projections");
                        }
                    }
                    if (ready) {
                        component.m_frame_crop_dimensions = ((uint64_t)(uint32_t)(desc.Width / 2) << 32) | desc.Height;
                        spdlog::info("[Frame Perf] Crop resolve ready for {}x{} per eye ({} fenced images)", desc.Width / 2, desc.Height, ctx.textures.size());
                    }
                }
            }

            SubmissionTiming* timing = texture_index < ctx.timings.size() ? &ctx.timings[texture_index] : nullptr;
            if (timing != nullptr && timing->fence != nullptr) {
                const auto completed = timing->fence->GetCompletedValue();
                if (completed == UINT64_MAX) {
                    // Device removed; timestamps cannot be interpreted.
                    timing->unavailable = true;
                    timing->fence.Reset();
                    ++ctx.timing_invalid;
                } else if (completed >= timing->fence_value) {
                    if (timing_enabled && !timing->unavailable && timing->epoch == ctx.timing_epoch) {
                        uint64_t* ticks{};
                        D3D12_RANGE range{0, sizeof(uint64_t) * SubmissionTiming::query_count};
                        if (SUCCEEDED(timing->readback->Map(0, &range, reinterpret_cast<void**>(&ticks)))) {
                            bool ordered = true;
                            for (uint32_t i = 1; i < SubmissionTiming::query_count; ++i) ordered &= ticks[i - 1] <= ticks[i];
                            if (ordered) {
                                auto& interval = ctx.timing_intervals[timing->sample.key()];
                                interval.source(timing->sample);
                                const double ms_per_tick = 1000.0 / timing->frequency;
                                const auto copy_ticks = ticks[3] - ticks[2];
                                interval.gpu_pre.add(double(ticks[1] - ticks[0]) * ms_per_tick);
                                interval.gpu_copy.add(double(copy_ticks) * ms_per_tick);
                                // Clear and resolve bracket the scratch copy; exclude that copy.
                                interval.gpu_reconstruct.add(double(ticks[4] - ticks[1] - copy_ticks) * ms_per_tick);
                                interval.gpu_additional.add(double(ticks[5] - ticks[4]) * ms_per_tick);
                                interval.gpu_mask.add(double(ticks[6] - ticks[5]) * ms_per_tick);
                            } else {
                                ++ctx.timing_invalid;
                            }
                            D3D12_RANGE written{0, 0};
                            timing->readback->Unmap(0, &written);
                        } else {
                            timing->unavailable = true;
                            ++ctx.timing_invalid;
                            spdlog::warn("[Frame Timing] D3D12 timestamp readback unavailable for image; CPU diagnostics remain enabled");
                        }
                    } else if (timing_enabled && timing->epoch != ctx.timing_epoch) {
                        ++ctx.timing_stale;
                    }
                    timing->fence.Reset();
                } else if (timing_enabled) {
                    ++ctx.timing_pending;
                }
            }
            const bool gpu_timing = timing_enabled && timing->begin(texture_ctx->commands);
            SubmissionSample sample{};
            if (gpu_timing) timing->epoch = ctx.timing_epoch;
            const auto record_start = timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            if (pre_commands) {
                (*pre_commands)(texture_ctx->commands, ctx.textures[texture_index].texture);
            }
            if (gpu_timing) texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

            const bool resolved = double_wide && component.resolve_volumetric_frame(*texture_ctx,
                texture_index < ctx.frame_scratch.size() ? ctx.frame_scratch[texture_index].get() : nullptr,
                resolve_source, assembled_eyes ? D3D12_RESOURCE_STATE_RENDER_TARGET : src_state, direct_resolve || assembled_eyes,
                gpu_timing ? timing->queries.Get() : nullptr, sample_enabled ? &sample : nullptr);

            // The resolve wrote its copy pair only when it copied into scratch.
            const bool scratch_copy = double_wide && component.m_frame_submission_resolved;
            if (gpu_timing && !scratch_copy) texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr && !resolved) {
                if (src_box == nullptr) {
                    const auto is_depth = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;
                    const auto dst_state = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        dst_state);
                } else {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, src_box,
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            if (gpu_timing) {
                if (!scratch_copy) texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
                texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
            }
            if (additional_commands && !resolved) {
                (*additional_commands)(texture_ctx->commands);
            }

            if (gpu_timing) texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
            if (swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE) {
                component.draw_volumetric_frame(*texture_ctx, ctx.textures[texture_index].texture, resolve_source, sample_enabled ? &sample : nullptr);
            }
            sample.resolved = component.m_frame_submission_resolved;
            sample.suppressed = component.m_frame_submission_suppressed;
            if (gpu_timing) {
                timing->sample = sample;
                texture_ctx->commands.cmd_list->EndQuery(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
                texture_ctx->commands.cmd_list->ResolveQueryData(timing->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, SubmissionTiming::query_count, timing->readback.Get(), 0);
                texture_ctx->commands.has_commands = true;
            }

            bool capture_queued = false;
            if (capture_requested && ctx.capture.readback == nullptr) {
                capture_queued = ctx.capture.begin(texture_ctx->commands, ctx.textures[texture_index].texture, sample);
                if (!capture_queued) vr->m_volumetric_frame_capture->value() = false;
            }

            const auto execute_start = timing_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            texture_ctx->commands.execute();
            if (capture_queued && texture_ctx->commands.fence_value != ctx.capture.fence_value) {
                // Retain the referenced readback until the command context is destroyed.
                ctx.capture.abandoned = true;
                vr->m_volumetric_frame_capture->value() = false;
                spdlog::error("[Frame Capture] failed reason=submission-failed frame={}", sample.frame);
            }
            if (gpu_timing && texture_ctx->commands.fence_value != timing->fence_value) {
                // The command list did not submit. Retain resources and stop sampling it.
                timing->unavailable = true;
                ++ctx.timing_invalid;
            }
            if (timing_enabled) {
                const auto now = std::chrono::steady_clock::now();
                auto& interval = ctx.timing_intervals[sample.key()];
                // CommandContext::wait also resets its allocator/list; report both costs.
                interval.cpu_fence_wait.add(std::chrono::duration<double, std::milli>(fence_wait_end - fence_wait_start).count());
                interval.cpu_record.add(std::chrono::duration<double, std::milli>(execute_start - record_start).count());
                interval.cpu_execute.add(std::chrono::duration<double, std::milli>(now - execute_start).count());
                if (now - ctx.timing_report >= std::chrono::seconds(2)) {
                    uint32_t unavailable_images{0};
                    for (const auto& image_timing : ctx.timings) unavailable_images += image_timing.unavailable;
                    for (const auto& [key, data] : ctx.timing_intervals) {
                        spdlog::info("[Frame Timing] scope=uevr_openxr_submission unit_ms=1 epoch={} interval_s={:.3f} native_fix={} mask_enabled={} crop_setting={} reduce_setting={} fixed_scale={} source_fixed_scale={} cropped={} reduced={} lost={} resolved={} suppressed={} mask_drawn={} gpu_n={} frame_first={} frame_last={} pose_first={} pose_last={} probe_first={} probe_last={} generation_first={} generation_last={} gpu_pre_avg={:.4f} gpu_pre_max={:.4f} gpu_source_copy_avg={:.4f} gpu_source_copy_max={:.4f} gpu_reconstruct_avg={:.4f} gpu_reconstruct_max={:.4f} gpu_additional_avg={:.4f} gpu_additional_max={:.4f} gpu_mask_avg={:.4f} gpu_mask_max={:.4f} cpu_n={} cpu_image_wait_reset_avg={:.4f} cpu_image_wait_reset_max={:.4f} cpu_record_avg={:.4f} cpu_record_max={:.4f} cpu_execute_avg={:.4f} cpu_execute_max={:.4f} pending_skips={} invalid={} stale_skips={} gpu_unavailable_images={} excludes=unreal_scene_gpu,xr_runtime_pacing,initial_scratch_setup",
                            ctx.timing_epoch, std::chrono::duration<double>(now - ctx.timing_report).count(),
                            (timing_settings >> 2) & 1, uint32_t(mask_enabled), (timing_settings >> 3) & 1, (timing_settings >> 4) & 1,
                            fixed_scale, std::bit_cast<float>(uint32_t(key >> 32)), key & 3, (key >> 2) & 3,
                            (key >> 4) & 1, (key >> 5) & 1, (key >> 6) & 1, (key >> 7) & 3,
                            data.gpu_copy.count, data.first.frame, data.last.frame, data.first.pose, data.last.pose,
                            data.first.probe, data.last.probe, data.first.generation, data.last.generation,
                            data.gpu_pre.average(), data.gpu_pre.maximum, data.gpu_copy.average(), data.gpu_copy.maximum,
                            data.gpu_reconstruct.average(), data.gpu_reconstruct.maximum, data.gpu_additional.average(), data.gpu_additional.maximum,
                            data.gpu_mask.average(), data.gpu_mask.maximum, data.cpu_record.count,
                            data.cpu_fence_wait.average(), data.cpu_fence_wait.maximum, data.cpu_record.average(), data.cpu_record.maximum,
                            data.cpu_execute.average(), data.cpu_execute.maximum, ctx.timing_pending, ctx.timing_invalid, ctx.timing_stale, unavailable_images);
                    }
                    ctx.timing_intervals.clear();
                    ctx.timing_pending = ctx.timing_invalid = ctx.timing_stale = 0;
                    ctx.timing_report = now;
                }
            }

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod
