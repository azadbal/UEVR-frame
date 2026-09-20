// Run from a VS x64 developer prompt at the repo root:
// cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_resolve_d3d12.cpp /Febuild/frame-resolve-test.exe /Fobuild/frame-resolve-test.obj /link d3d12.lib dxgi.lib
// build\frame-resolve-test.exe
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
#include "../src/mods/vr/shaders/Compiled/volumetric_frame_resolve_ResolvePS.inc"
#include <algorithm>
#include <vector>

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
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.VS = {volumetric_frame_FrameVS, sizeof(volumetric_frame_FrameVS)};
    pso.PS = {volumetric_frame_resolve_ResolvePS, sizeof(volumetric_frame_resolve_ResolvePS)};
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

    ComPtr<ID3D12Resource> source, scratch, upload;
    auto scratch_desc = texture_desc;
    scratch_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    scratch_desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scratch_desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&scratch)));
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source)));
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    uint8_t* upload_pixels{};
    check(upload->Map(0, nullptr, (void**)&upload_pixels));
    // Distinct eye colors, gradient, and alternating high contrast samples test
    // packing, texel centers and linear-light interpolation independently.
    auto channel = [](int x, int y, int c) -> int {
        if (c == 3) return 255;
        if (c == 0) return x < 256 ? 32 + (x % 256) * 3 / 4 : 15;
        if (c == 1) return (y % 2) ? 220 : 40;
        return x < 256 ? 15 : 220 - (x % 256) / 2;
    };
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 512; ++x)
        for (int c = 0; c < 4; ++c) upload_pixels[y * footprint.Footprint.RowPitch + x * 4 + c] = (uint8_t)channel(x,y,c);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    to.pResource = source.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    };
    transition(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 1;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    check(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(scratch.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
    const auto decode = [](double value) {
        value /= 255;
        return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
    };
    const auto encode = [](double value) {
        return (int)std::round(255 * (value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - .055));
    };
    // Native Stereo Fix supplies two independent eye-local textures. Populate
    // them from the distinct-eye fixture, then exercise assembly -> scratch ->
    // clear -> resolve, including the ordering needed when output is the source.
    std::array<ComPtr<ID3D12Resource>, 2> native_eyes;
    auto eye_desc = texture_desc;
    eye_desc.Width = 256;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    transition(source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int eye = 0; eye < 2; ++eye) {
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &eye_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&native_eyes[eye])));
        from = {}; to = {};
        from.pResource = source.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = native_eyes[eye].Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{(UINT)eye * 256, 0, 0, (UINT)(eye + 1) * 256, 256, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        transition(native_eyes[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    transition(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (int test = 0; test < 13; ++test) {
        const bool native = test >= 11;
        const bool green = test == 5 || test == 12;
        const bool fail_closed = test == 6;
        std::array<std::array<int,4>,2> rects{{{31,47,181,133},{281,61,199,173}}};
        if (test == 0 || test == 3) rects[0] = {0,0,256,256};
        if (test == 0 || test == 2) rects[1] = {256,0,256,256};
        if (test == 4) rects = {{{255,0,1,256},{256,0,1,256}}};
        std::array<std::array<int,4>,2> sources{{{0,0,256,256},{256,0,256,256}}};
        // Reduced views occupy the top-left of each original eye allocation.
        // Exercise both reduced eyes, mixed full/reduced views, and a 1px edge.
        if (test == 10) rects = {{{255,255,1,1},{256,0,1,256}}};
        for (int eye = 0; eye < 2; ++eye) {
            if (test == 7 || test == 10 || (test == 8 && eye == 0) || (test == 9 && eye == 1)) {
                sources[eye][2] = rects[eye][2];
                sources[eye][3] = rects[eye][3];
            }
        }
        const float background[]{0,green ? 1.0f : 0.0f,0,1};
        if (!native) list->ClearRenderTargetView(rtv, background, 0, nullptr);
        if (!fail_closed) {
            auto* copy_source = native ? texture.Get() : source.Get();
            if (native) {
                transition(texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                for (int eye = 0; eye < 2; ++eye) {
                    from = {}; to = {};
                    from.pResource = native_eyes[eye].Get();
                    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    to.pResource = texture.Get();
                    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    list->CopyTextureRegion(&to, eye * 256, 0, 0, &from, nullptr);
                }
                transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            } else {
                transition(source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            }
            transition(scratch.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(scratch.Get(), copy_source);
            transition(copy_source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (native) list->ClearRenderTargetView(rtv, background, 0, nullptr);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->SetGraphicsRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
            for (int eye = 0; eye < 2; ++eye) {
                const auto& r = rects[eye];
                const auto& s = sources[eye];
                const std::array<float,8> constants{(float)r[0],(float)r[1],(float)r[2],(float)r[3],(float)s[0],(float)s[1],(float)s[2],(float)s[3]};
                const D3D12_VIEWPORT viewport{(float)r[0],(float)r[1],(float)r[2],(float)r[3],0,1};
                const D3D12_RECT scissor{r[0],r[1],r[0]+r[2],r[1]+r[3]};
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->SetGraphicsRoot32BitConstants(0, 8, constants.data(), 0);
                list->DrawInstanced(3,1,0,0);
            }
        }
        transition(texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        from.pResource = texture.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        check(list->Close());
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), test+1));
        check(fence->SetEventOnCompletion(test+1,event));
        if (WaitForSingleObject(event,10000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
        uint8_t* pixels{};
        check(readback->Map(0,nullptr,(void**)&pixels));
        int mismatches = 0, max_error = 0;
        for (int y=0; y<256; ++y) for (int x=0; x<512; ++x) {
            const int eye=x/256;
            const auto& r=rects[eye];
            const auto& s=sources[eye];
            const bool inside=!fail_closed && x>=r[0] && y>=r[1] && x<r[0]+r[2] && y<r[1]+r[3];
            for (int c=0; c<4; ++c) {
                int expected=c==3 ? 255 : c==1 && green ? 255 : 0;
                if (inside) {
                    const double sx=std::clamp((x+.5-r[0])*s[2]/r[2]-.5,0.,(double)s[2]-1);
                    const double sy=std::clamp((y+.5-r[1])*s[3]/r[3]-.5,0.,(double)s[3]-1);
                    const int x0=(int)sx, y0=(int)sy;
                    const double tx=sx-x0, ty=sy-y0;
                    double value=0;
                    for (int dy=0; dy<2; ++dy) for (int dx=0; dx<2; ++dx) {
                        const int raw=channel(s[0]+std::min(x0+dx,s[2]-1),s[1]+std::min(y0+dy,s[3]-1),c);
                        value+=(dx ? tx : 1-tx)*(dy ? ty : 1-ty)*(c==3 ? raw/255. : decode(raw));
                    }
                    expected=c==3 ? (int)std::round(value*255) : encode(value);
                }
                const int error=std::abs((int)pixels[y*footprint.Footprint.RowPitch+x*4+c]-expected);
                max_error=std::max(max_error,error);
                // The alternating 40/220 sRGB pattern magnifies fixed-point
                // sampler interpolation error. Permit three code values for
                // resized interiors; identity mapping/background must be exact.
                const int tolerance = inside && (r[2] != s[2] || r[3] != s[3]) ? 3 : 0;
                if (error>tolerance) ++mismatches;
            }
        }
        readback->Unmap(0,nullptr);
        std::printf("Resolve case %d: %d mismatches, max channel error %d\n",test,mismatches,max_error);
        if (mismatches) throw std::runtime_error("Incorrect crop resolve");
        check(allocator->Reset());
        check(list->Reset(allocator.Get(),nullptr));
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
    std::puts("PASS: D3D12 WARP crop resolve, both eyes, full/mixed crops, pixel mapping, sRGB filtering, seams, black/green and fail-closed clear.");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
