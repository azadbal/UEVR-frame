// Run from a VS x64 developer prompt at the repo root:
// cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_d3d12.cpp /Febuild/frame-test.exe /Fobuild/frame-test.obj /link d3d12.lib dxgi.lib
// build\frame-test.exe
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include "../src/mods/vr/VolumetricFrameLayout.hpp"
#include "../src/mods/vr/shaders/Compiled/volumetric_frame_FrameVS.inc"
#include "../src/mods/vr/shaders/Compiled/volumetric_frame_FramePS.inc"

using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {
    if (FAILED(hr)) {
        std::printf("HRESULT: %08x\n", (unsigned)hr);
        throw std::runtime_error("D3D12 operation failed");
    }
}

int main() try {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 32;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.VS = {volumetric_frame_FrameVS, sizeof(volumetric_frame_FrameVS)};
    pso.PS = {volumetric_frame_FramePS, sizeof(volumetric_frame_FramePS)};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 512;
    texture_desc.Height = 256;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Analytic cases: centered, translated, looking away, behind the plane,
    // near the plane, and parallel to it. Both asymmetric eye frusta are tested.
    for (int test = 0; test < 9; ++test) {
        // The last two cases use production layout geometry. Compare the mask
        // with the projected HUD quad polygon, independently of ray-plane math.
        vrmod::VolumetricFrameLayout layout;
        layout.size = vrmod::volumetric_frame_size(test == 8 ? 1.2f : 2.0f);
        auto anchor = glm::mat4{1.0f};
        if (test == 8) {
            anchor = glm::rotate(anchor, 0.23f, glm::vec3{0, 1, 0});
            anchor = glm::rotate(anchor, 0.31f, glm::vec3{0, 0, 1});
        }
        layout.pose = vrmod::apply_volumetric_frame_offsets(anchor, 2.0f,
            test == 8 ? 0.37f : 0.0f, test == 8 ? 0.19f : 0.0f);
        std::array<std::array<glm::vec2, 4>, 2> hud_corners{};
        const float red[]{1, 0, 0, 1};
        list->ClearRenderTargetView(rtv, red, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (int eye = 0; eye < 2; ++eye) {
            const float origin_x = (eye == 0 ? -0.032f : 0.032f) + (test == 1 ? 0.5f : 0.0f);
            const float origin_z = test == 3 ? -0.2f : test == 4 ? 0.5f : 2.0f;
            const float left = eye == 0 ? -1.2f : -0.8f;
            const float right = eye == 0 ? 0.8f : 1.2f;
            std::array<float, 32> constants{
                1,0,0,0, 0,1,0,0, 0,0,1,0, origin_x,0,origin_z,1,
                left,right,1,-1, (float)(eye*256),0,256,256,
                1,0.5625f,0,0, 0,test == 1 ? 0.0f : 1.0f,0,1};
            if (test == 6) {
                constants[20] += 16; constants[21] = 24;
                constants[22] = 224; constants[23] = 208;
            }
            if (test == 2) { constants[0] = -1; constants[10] = -1; }
            if (test == 5) { // +90 degrees yaw
                constants[0] = 0; constants[2] = -1;
                constants[8] = 1; constants[10] = 0;
            }
            if (test >= 7) {
                auto eye_pose = glm::mat4{1.0f};
                eye_pose[3].x = origin_x;
                const auto eye_to_frame = glm::inverse(layout.pose) * eye_pose;
                std::memcpy(constants.data(), &eye_to_frame[0][0], sizeof(eye_to_frame));
                constants[24] = layout.size.x * 0.5f;
                constants[25] = layout.size.y * 0.5f;
                const glm::vec2 signs[]{{-1,-1},{1,-1},{1,1},{-1,1}};
                for (int corner = 0; corner < 4; ++corner) {
                    const auto local = signs[corner] * layout.size * 0.5f;
                    auto p = layout.pose * glm::vec4{local, 0, 1};
                    p.x -= origin_x;
                    hud_corners[eye][corner] = glm::vec2{p.x, p.y} / -p.z;
                }
            }
            D3D12_VIEWPORT viewport{constants[20],constants[21],constants[22],constants[23],0,1};
            D3D12_RECT scissor{(LONG)viewport.TopLeftX,(LONG)viewport.TopLeftY,
                (LONG)(viewport.TopLeftX+viewport.Width),(LONG)(viewport.TopLeftY+viewport.Height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRoot32BitConstants(0, 32, constants.data(), 0);
            list->DrawInstanced(3, 1, 0, 0);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = texture.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        check(list->Close());
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), test + 1));
        check(fence->SetEventOnCompletion(test + 1, event));
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
        uint8_t* pixels{};
        check(readback->Map(0, nullptr, (void**)&pixels));
        int mismatches = 0;
        for (int y = 0; y < 256; ++y) for (int x = 0; x < 512; ++x) {
            const int eye = x / 256;
            const float ox = (eye == 0 ? -0.032f : 0.032f) + (test == 1 ? 0.5f : 0);
            const float distance = test == 4 ? 0.5f : 2.0f;
            const float slope_x = (eye == 0 ? -1.2f : -0.8f) + 2 *
                ((x % 256 + 0.5f - (test == 6 ? 16 : 0)) / (test == 6 ? 224 : 256));
            const float slope_y = 1 - 2 * ((y + 0.5f - (test == 6 ? 24 : 0)) / (test == 6 ? 208 : 256));
            bool inside = test != 2 && test != 3 && test != 5 &&
                std::abs(ox + slope_x * distance) <= 1 && std::abs(slope_y * distance) <= 0.5625f;
            if (test == 6 && (x % 256 < 16 || x % 256 >= 240 || y < 24 || y >= 232)) inside = true;
            if (test >= 7) {
                inside = true;
                const glm::vec2 point{slope_x, slope_y};
                for (int edge = 0; edge < 4; ++edge) {
                    const auto a = hud_corners[eye][edge];
                    const auto b = hud_corners[eye][(edge + 1) % 4];
                    const auto side = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
                    if (side < 0) inside = false;
                }
            }
            // At 90 degrees yaw this window lies entirely outside either frustum.
            const uint8_t* pixel = pixels + y * footprint.Footprint.RowPitch + x * 4;
            const int green = inside || test == 1 ? 0 : 255;
            if (pixel[0] != 0 || pixel[1] != green || pixel[2] != (inside ? 255 : 0) || pixel[3] != 255) ++mismatches;
        }
        readback->Unmap(0, nullptr);
        std::printf("Case %d: %d pixel mismatches\n", test, mismatches);
        if (mismatches) throw std::runtime_error("Incorrect portal mask");
        check(allocator->Reset());
        check(list->Reset(allocator.Get(), nullptr));
    }
    if (debug) {
        ComPtr<ID3D12InfoQueue> info;
        check(device.As(&info));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            check(info->GetMessage(i, nullptr, &size));
            auto storage = std::make_unique<uint8_t[]>(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
            check(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::puts(message->pDescription);
                throw std::runtime_error("D3D12 debug layer error");
            }
        }
        std::puts("D3D12 debug layer: no errors");
    }
    CloseHandle(event);
    std::puts("PASS: D3D12 WARP, both eyes, asymmetric FOV, head translation, black/green, crossing, turning away, cropped viewports and shared HUD geometry.");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
