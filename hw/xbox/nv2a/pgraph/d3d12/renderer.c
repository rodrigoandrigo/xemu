/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-host.h"
#include "hw/display/vga_int.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xui/xemu-hud.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-widescreen.h"
#include "blits.h"
#include "renderer.h"
#include "textures.h"
#include <d3d12sdklayers.h>

typedef HRESULT(WINAPI *D3D12GetDebugInterfaceFn)(REFIID, void **);

static void d3d12_enable_optional_debug_layer(void)
{
    HMODULE module = LoadPackagedLibrary(L"d3d12.dll", 0);
    if (!module) {
        return;
    }
    D3D12GetDebugInterfaceFn get_debug =
        (D3D12GetDebugInterfaceFn)GetProcAddress(module,
                                                 "D3D12GetDebugInterface");
    ID3D12Debug *debug = NULL;
    HRESULT hr = get_debug ?
                     get_debug(&IID_ID3D12Debug, (void **)&debug) :
                     E_NOINTERFACE;
    if (SUCCEEDED(hr) && debug) {
        ID3D12Debug_EnableDebugLayer(debug);
        ID3D12Debug_Release(debug);
        qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                           "D3D12: optional desktop debug layer enabled");
    }
    FreeLibrary(module);
}

static void d3d12_set_error(Error **errp, const char *operation, HRESULT hr)
{
    error_setg(errp, "D3D12: %s failed (HRESULT 0x%08lx)", operation,
               (unsigned long)hr);
}

bool pgraph_d3d12_note_device_error(PGRAPHD3D12State *r, HRESULT hr,
                                    const char *operation)
{
    if (!r || SUCCEEDED(hr)) {
        return false;
    }
    HRESULT reason =
        r->device ? ID3D12Device_GetDeviceRemovedReason(r->device) : hr;
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET &&
        hr != DXGI_ERROR_DEVICE_HUNG && reason != DXGI_ERROR_DEVICE_REMOVED &&
        reason != DXGI_ERROR_DEVICE_RESET && reason != DXGI_ERROR_DEVICE_HUNG &&
        reason != DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        return false;
    }
    r->device_lost = true;
    char *message = g_strdup_printf(
        "D3D12: device lost during %s (HRESULT 0x%08lx, reason 0x%08lx)",
        operation, (unsigned long)hr, (unsigned long)reason);
    qemu_host_emit_log(QEMU_HOST_LOG_ERROR, message);
    g_free(message);
    return true;
}

bool pgraph_d3d12_make_resident(PGRAPHD3D12State *r, ID3D12Pageable *resource,
                                Error **errp)
{
    HRESULT hr = ID3D12Device_MakeResident(r->device, 1, &resource);
    if (FAILED(hr)) {
        pgraph_d3d12_note_device_error(r, hr, "MakeResident");
        d3d12_set_error(errp, "MakeResident", hr);
        return false;
    }
    return true;
}

static bool d3d12_update_memory_budget(PGRAPHD3D12State *r)
{
    if (!r->adapter) {
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info;
    HRESULT hr = IDXGIAdapter3_QueryVideoMemoryInfo(
        r->adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(hr)) {
        return false;
    }
    r->memory_budget = info.Budget;
    r->memory_usage = info.CurrentUsage;
    return info.Budget && info.CurrentUsage > info.Budget * 9 / 10;
}

static bool d3d12_wait_for_frame(PGRAPHD3D12State *r, PGRAPHD3D12Frame *frame)
{
    if (!frame->fence_value) {
        return true;
    }

    uint64_t completed = ID3D12Fence_GetCompletedValue(r->fence);
    if (completed == UINT64_MAX) {
        HRESULT hr = ID3D12Device_GetDeviceRemovedReason(r->device);
        if (SUCCEEDED(hr)) {
            hr = DXGI_ERROR_DEVICE_REMOVED;
        }
        pgraph_d3d12_note_device_error(r, hr, "frame fence status");
        return false;
    }
    if (completed >= frame->fence_value) {
        return true;
    }

    HRESULT hr = ID3D12Fence_SetEventOnCompletion(r->fence, frame->fence_value,
                                                  r->fence_event);
    if (SUCCEEDED(hr) &&
        WaitForSingleObjectEx(r->fence_event, 5000, FALSE) != WAIT_OBJECT_0) {
        hr = ID3D12Device_GetDeviceRemovedReason(r->device);
        if (SUCCEEDED(hr)) {
            hr = DXGI_ERROR_DEVICE_HUNG;
        }
    }
    if (FAILED(hr)) {
        pgraph_d3d12_note_device_error(r, hr, "frame fence wait");
        return false;
    }
    return true;
}

static bool d3d12_wait_idle(PGRAPHD3D12State *r, Error **errp)
{
    HRESULT hr;
    uint64_t fence_value;

    qemu_mutex_lock(&r->queue_lock);
    fence_value = r->next_fence_value++;
    hr = ID3D12CommandQueue_Signal(r->queue, r->fence, fence_value);
    uint64_t completed =
        SUCCEEDED(hr) ? ID3D12Fence_GetCompletedValue(r->fence) : UINT64_MAX;
    if (SUCCEEDED(hr) && completed == UINT64_MAX) {
        hr = ID3D12Device_GetDeviceRemovedReason(r->device);
        if (SUCCEEDED(hr)) {
            hr = DXGI_ERROR_DEVICE_REMOVED;
        }
    }
    if (SUCCEEDED(hr) && completed < fence_value) {
        hr = ID3D12Fence_SetEventOnCompletion(r->fence, fence_value,
                                              r->fence_event);
        if (SUCCEEDED(hr) && WaitForSingleObjectEx(r->fence_event, 5000,
                                                   FALSE) != WAIT_OBJECT_0) {
            hr = ID3D12Device_GetDeviceRemovedReason(r->device);
            if (SUCCEEDED(hr)) {
                hr = DXGI_ERROR_DEVICE_HUNG;
            }
        }
    }
    qemu_mutex_unlock(&r->queue_lock);

    if (FAILED(hr)) {
        pgraph_d3d12_note_device_error(r, hr, "queue synchronization");
        d3d12_set_error(errp, "queue synchronization", hr);
        return false;
    }
    return true;
}

static void d3d12_release_state(PGRAPHD3D12State *r)
{
    if (!r) {
        return;
    }

    if (r->queue && r->fence && r->fence_event) {
        Error *error = NULL;
        if (!d3d12_wait_idle(r, &error)) {
            qemu_host_emit_log(
                QEMU_HOST_LOG_WARNING,
                error ? error_get_pretty(error) :
                        "D3D12: renderer shutdown synchronization failed");
            error_free(error);
        }
    }

    if (r->hud_initialized) {
        xemu_hud_d3d12_shutdown();
        r->hud_initialized = false;
    }
    pgraph_d3d12_draw_finalize(r);
    pgraph_d3d12_shader_cache_finalize(r->shader_cache);
    pgraph_d3d12_reports_finalize(r);
    pgraph_d3d12_surfaces_finalize(r);
    pgraph_d3d12_textures_finalize(r);

    for (unsigned int i = 0; i < D3D12_FRAME_COUNT; i++) {
        PGRAPHD3D12Frame *frame = &r->frames[i];
        if (frame->upload_data) {
            ID3D12Resource_Unmap(frame->upload_buffer, 0, NULL);
        }
        if (frame->upload_buffer) {
            ID3D12Resource_Release(frame->upload_buffer);
        }
        if (frame->render_target) {
            ID3D12Resource_Release(frame->render_target);
        }
        if (frame->command_list) {
            ID3D12GraphicsCommandList_Release(frame->command_list);
        }
        if (frame->allocator) {
            ID3D12CommandAllocator_Release(frame->allocator);
        }
    }
    if (r->swapchain) {
        IDXGISwapChain3_Release(r->swapchain);
    }
    if (r->rtv_heap) {
        ID3D12DescriptorHeap_Release(r->rtv_heap);
    }
    if (r->hud_srv_heap) {
        ID3D12DescriptorHeap_Release(r->hud_srv_heap);
    }
    if (r->fence) {
        ID3D12Fence_Release(r->fence);
    }
    if (r->queue) {
        ID3D12CommandQueue_Release(r->queue);
    }
    if (r->device) {
        ID3D12Device_Release(r->device);
    }
    if (r->adapter) {
        IDXGIAdapter3_Release(r->adapter);
    }
    if (r->factory) {
        IDXGIFactory4_Release(r->factory);
    }
    if (r->fence_event) {
        CloseHandle(r->fence_event);
    }
    qemu_mutex_destroy(&r->queue_lock);
    g_free(r);
}

static bool d3d12_create_device(PGRAPHD3D12State *r, Error **errp)
{
    /* The SDK layer is absent on Xbox retail. Resolve it dynamically so its
     * absence never becomes an import or device-creation failure there. */
    d3d12_enable_optional_debug_layer();
    HRESULT hr =
        CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&r->factory);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateDXGIFactory2", hr);
        return false;
    }

    IDXGIAdapter1 *adapter1 = NULL;
    for (UINT index = 0;; index++) {
        hr = IDXGIFactory4_EnumAdapters1(r->factory, index, &adapter1);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            d3d12_set_error(errp, "IDXGIFactory::EnumAdapters1", hr);
            return false;
        }
        DXGI_ADAPTER_DESC1 desc;
        hr = IDXGIAdapter1_GetDesc1(adapter1, &desc);
        if (FAILED(hr)) {
            IDXGIAdapter1_Release(adapter1);
            adapter1 = NULL;
            d3d12_set_error(errp, "IDXGIAdapter::GetDesc1", hr);
            return false;
        }
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(
                D3D12CreateDevice((IUnknown *)adapter1, D3D_FEATURE_LEVEL_11_0,
                                  &IID_ID3D12Device, (void **)&r->device))) {
            IDXGIAdapter1_QueryInterface(adapter1, &IID_IDXGIAdapter3,
                                         (void **)&r->adapter);
            IDXGIAdapter1_Release(adapter1);
            adapter1 = NULL;
            break;
        }
        IDXGIAdapter1_Release(adapter1);
        adapter1 = NULL;
    }
    if (!r->device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                               (void **)&r->device);
    } else {
        hr = S_OK;
    }
    if (FAILED(hr)) {
        d3d12_set_error(errp, "D3D12CreateDevice", hr);
        return false;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(
        r->device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CheckFeatureSupport(D3D12_OPTIONS)", hr);
        return false;
    }
    r->output_merger_logic_op = options.OutputMergerLogicOp;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    hr = ID3D12Device_CreateCommandQueue(
        r->device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&r->queue);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateCommandQueue", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hud_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = XEMU_HUD_D3D12_DESCRIPTOR_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(r->device, &hud_heap_desc,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&r->hud_srv_heap);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateDescriptorHeap(HUD)", hr);
        return false;
    }

    hr = ID3D12Device_CreateFence(r->device, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void **)&r->fence);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateFence", hr);
        return false;
    }
    r->next_fence_value = 1;
    r->fence_event =
        CreateEventExW(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!r->fence_event) {
        error_setg_win32(errp, GetLastError(), "D3D12: CreateEventExW failed");
        return false;
    }

    for (unsigned int i = 0; i < D3D12_FRAME_COUNT; i++) {
        PGRAPHD3D12Frame *frame = &r->frames[i];
        hr = ID3D12Device_CreateCommandAllocator(
            r->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&frame->allocator);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "CreateCommandAllocator", hr);
            return false;
        }
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame->allocator,
            NULL, &IID_ID3D12GraphicsCommandList,
            (void **)&frame->command_list);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "CreateCommandList", hr);
            return false;
        }
        hr = ID3D12GraphicsCommandList_Close(frame->command_list);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "initial command-list close", hr);
            return false;
        }
    }
    return true;
}

static bool d3d12_create_swapchain(PGRAPHD3D12State *r, Error **errp)
{
    QemuHostD3D12PresentTarget target;
    int target_result = qemu_host_get_d3d12_present_target(&target);
    if (target_result) {
        error_setg(errp, "D3D12: UWP presentation target is not attached");
        return false;
    }
    if (!target.width || !target.height || !target.attach) {
        error_setg(errp, "D3D12: UWP presentation target is incomplete");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = target.width,
        .Height = target.height,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .Stereo = FALSE,
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = D3D12_FRAME_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
        .Flags = 0,
    };
    r->output_width = desc.Width;
    r->output_height = desc.Height;
    IDXGISwapChain1 *swapchain1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForComposition(
        r->factory, (IUnknown *)r->queue, &desc, NULL, &swapchain1);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateSwapChainForComposition", hr);
        return false;
    }
    hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3,
                                        (void **)&r->swapchain);
    if (SUCCEEDED(hr)) {
        hr = target.attach(target.opaque, swapchain1);
    }
    IDXGISwapChain1_Release(swapchain1);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "attach SwapChainPanel", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = D3D12_FRAME_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    hr = ID3D12Device_CreateDescriptorHeap(r->device, &heap_desc,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&r->rtv_heap);
    if (FAILED(hr)) {
        d3d12_set_error(errp, "CreateDescriptorHeap", hr);
        return false;
    }

    r->rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtv_heap);
    for (unsigned int i = 0; i < D3D12_FRAME_COUNT; i++) {
        PGRAPHD3D12Frame *frame = &r->frames[i];
        UINT64 upload_size = 0;
        hr = IDXGISwapChain3_GetBuffer(r->swapchain, i, &IID_ID3D12Resource,
                                       (void **)&frame->render_target);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "IDXGISwapChain::GetBuffer", hr);
            return false;
        }
        ID3D12Device_CreateRenderTargetView(r->device, frame->render_target,
                                            NULL, rtv);
        frame->render_target_state = D3D12_RESOURCE_STATE_PRESENT;

        D3D12_RESOURCE_DESC target_desc =
            ID3D12Resource_GetDesc(frame->render_target);
        ID3D12Device_GetCopyableFootprints(r->device, &target_desc, 0, 1, 0,
                                           &frame->upload_footprint, NULL, NULL,
                                           &upload_size);

        D3D12_HEAP_PROPERTIES upload_heap = {
            .Type = D3D12_HEAP_TYPE_UPLOAD,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1,
        };
        D3D12_RESOURCE_DESC upload_desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = upload_size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleDesc = { 1, 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };
        hr = ID3D12Device_CreateCommittedResource(
            r->device, &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void **)&frame->upload_buffer);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "CreateCommittedResource(upload)", hr);
            return false;
        }
        D3D12_RANGE no_read = { 0, 0 };
        hr = ID3D12Resource_Map(frame->upload_buffer, 0, &no_read,
                                (void **)&frame->upload_data);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "Map(upload)", hr);
            return false;
        }
        rtv.ptr += r->rtv_increment;
    }
    r->frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapchain);
    return true;
}

static void d3d12_release_swapchain_buffers(PGRAPHD3D12State *r)
{
    for (unsigned int i = 0; i < D3D12_FRAME_COUNT; i++) {
        PGRAPHD3D12Frame *frame = &r->frames[i];
        if (frame->upload_data) {
            ID3D12Resource_Unmap(frame->upload_buffer, 0, NULL);
            frame->upload_data = NULL;
        }
        if (frame->upload_buffer) {
            ID3D12Resource_Release(frame->upload_buffer);
            frame->upload_buffer = NULL;
        }
        if (frame->render_target) {
            ID3D12Resource_Release(frame->render_target);
            frame->render_target = NULL;
        }
        memset(&frame->upload_footprint, 0, sizeof(frame->upload_footprint));
        frame->fence_value = 0;
    }
}

static bool d3d12_resize_swapchain(PGRAPHD3D12State *r, Error **errp)
{
    QemuHostD3D12PresentTarget target;
    if (qemu_host_get_d3d12_present_target(&target) || !target.width ||
        !target.height) {
        return true;
    }
    bool dimensions_changed =
        target.width != r->output_width || target.height != r->output_height;
    if (!dimensions_changed && r->frames[0].render_target &&
        r->frames[0].upload_buffer) {
        return true;
    }
    if (!d3d12_wait_idle(r, errp)) {
        return false;
    }
    d3d12_release_swapchain_buffers(r);
    HRESULT hr = S_OK;
    if (dimensions_changed) {
        hr = IDXGISwapChain3_ResizeBuffers(r->swapchain, D3D12_FRAME_COUNT,
                                           target.width, target.height,
                                           DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) {
            pgraph_d3d12_note_device_error(r, hr, "swapchain resize");
            d3d12_set_error(errp, "IDXGISwapChain::ResizeBuffers", hr);
            return false;
        }
        /* ResizeBuffers preserves the SwapChainPanel association. Reattaching
         * from the render thread is redundant and, on UWP, can race an
         * asynchronous UI-dispatch attach with the next frame. */
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtv_heap);
    for (unsigned int i = 0; i < D3D12_FRAME_COUNT; i++) {
        PGRAPHD3D12Frame *frame = &r->frames[i];
        UINT64 upload_size = 0;
        hr = IDXGISwapChain3_GetBuffer(r->swapchain, i, &IID_ID3D12Resource,
                                       (void **)&frame->render_target);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "IDXGISwapChain::GetBuffer", hr);
            return false;
        }
        ID3D12Device_CreateRenderTargetView(r->device, frame->render_target,
                                            NULL, rtv);
        frame->render_target_state = D3D12_RESOURCE_STATE_PRESENT;

        D3D12_RESOURCE_DESC target_desc =
            ID3D12Resource_GetDesc(frame->render_target);
        ID3D12Device_GetCopyableFootprints(r->device, &target_desc, 0, 1, 0,
                                           &frame->upload_footprint, NULL, NULL,
                                           &upload_size);

        D3D12_HEAP_PROPERTIES upload_heap = {
            .Type = D3D12_HEAP_TYPE_UPLOAD,
            .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
            .CreationNodeMask = 1,
            .VisibleNodeMask = 1,
        };
        D3D12_RESOURCE_DESC upload_desc = {
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Alignment = 0,
            .Width = upload_size,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = { 1, 0 },
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = D3D12_RESOURCE_FLAG_NONE,
        };
        hr = ID3D12Device_CreateCommittedResource(
            r->device, &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void **)&frame->upload_buffer);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "CreateCommittedResource(upload)", hr);
            return false;
        }
        D3D12_RANGE no_read = { 0, 0 };
        hr = ID3D12Resource_Map(frame->upload_buffer, 0, &no_read,
                                (void **)&frame->upload_data);
        if (FAILED(hr)) {
            d3d12_set_error(errp, "Map(upload)", hr);
            return false;
        }
        rtv.ptr += r->rtv_increment;
    }
    r->frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapchain);
    r->output_width = target.width;
    r->output_height = target.height;
    return true;
}

static void d3d12_transition(ID3D12GraphicsCommandList *command_list,
                             ID3D12Resource *resource,
                             D3D12_RESOURCE_STATES *current_state,
                             D3D12_RESOURCE_STATES new_state)
{
    if (*current_state == new_state) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = *current_state,
            .StateAfter = new_state,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
    *current_state = new_state;
}

static bool d3d12_get_vram_surface(NV2AState *d, PGRAPHD3D12Surface *surface)
{
    int width = 0;
    int height = 0;
    VGADisplayParams params = { 0 };

    d->vga.get_resolution(&d->vga, &width, &height);
    d->vga.get_params(&d->vga, &params);
    int bpp = d->vga.get_bpp(&d->vga);
    unsigned int bytes_per_pixel;
    switch (bpp) {
    case 8:
        surface->format = PGRAPH_D3D12_SURFACE_INDEX8;
        bytes_per_pixel = 1;
        break;
    case 15:
        surface->format = PGRAPH_D3D12_SURFACE_X1R5G5B5;
        bytes_per_pixel = 2;
        break;
    case 16:
        surface->format = PGRAPH_D3D12_SURFACE_R5G6B5;
        bytes_per_pixel = 2;
        break;
    case 32:
        surface->format = PGRAPH_D3D12_SURFACE_X8R8G8B8;
        bytes_per_pixel = 4;
        break;
    default:
        return false;
    }

    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    uint32_t pitch = params.line_offset;
    uint64_t minimum_pitch = (uint64_t)width * bytes_per_pixel;
    if (!pitch) {
        pitch = minimum_pitch;
    }
    uint64_t end = (uint64_t)d->pcrtc.start + (uint64_t)(height - 1) * pitch +
                   minimum_pitch;
    if (pitch < minimum_pitch || end > memory_region_size(d->vram)) {
        return false;
    }

    surface->data = d->vram_ptr + d->pcrtc.start;
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    return true;
}

static uint32_t d3d12_read_vram_pixel(const PGRAPHD3D12Surface *surface,
                                      uint32_t x, uint32_t y)
{
    const uint8_t *row = surface->data + (uint64_t)y * surface->pitch;
    switch (surface->format) {
    case PGRAPH_D3D12_SURFACE_INDEX8: {
        const uint8_t *palette = nv2a_get_dac_palette();
        uint8_t index = row[x];
        return palette[index * 3 + 2] | (palette[index * 3 + 1] << 8) |
               (palette[index * 3] << 16) | 0xff000000u;
    }
    case PGRAPH_D3D12_SURFACE_X1R5G5B5: {
        uint16_t pixel;
        memcpy(&pixel, row + x * 2, sizeof(pixel));
        uint8_t b = (pixel & 0x1f) * 255 / 31;
        uint8_t g = ((pixel >> 5) & 0x1f) * 255 / 31;
        uint8_t r = ((pixel >> 10) & 0x1f) * 255 / 31;
        return b | (g << 8) | (r << 16) | 0xff000000u;
    }
    case PGRAPH_D3D12_SURFACE_R5G6B5: {
        uint16_t pixel;
        memcpy(&pixel, row + x * 2, sizeof(pixel));
        uint8_t b = (pixel & 0x1f) * 255 / 31;
        uint8_t g = ((pixel >> 5) & 0x3f) * 255 / 63;
        uint8_t r = ((pixel >> 11) & 0x1f) * 255 / 31;
        return b | (g << 8) | (r << 16) | 0xff000000u;
    }
    case PGRAPH_D3D12_SURFACE_X8R8G8B8: {
        uint32_t pixel;
        memcpy(&pixel, row + x * 4, sizeof(pixel));
        return pixel | 0xff000000u;
    }
    default:
        return 0xff000000u;
    }
}

static double d3d12_get_display_aspect_ratio(const PGRAPHD3D12Surface *surface)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (double)surface->width / surface->height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0 / 9.0;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0 / 3.0;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        return xemu_get_widescreen() ? 16.0 / 9.0 : 4.0 / 3.0;
    }
}

static uint32_t d3d12_lerp_pixel(uint32_t a, uint32_t b, uint32_t weight)
{
    uint32_t inverse = 256 - weight;
    uint32_t rb =
        (((a & 0x00ff00ffu) * inverse + (b & 0x00ff00ffu) * weight) >> 8) &
        0x00ff00ffu;
    uint32_t ag =
        (((a >> 8 & 0x00ff00ffu) * inverse + (b >> 8 & 0x00ff00ffu) * weight) >>
         8) &
        0x00ff00ffu;
    return rb | (ag << 8);
}

static uint32_t d3d12_sample_vram_surface(const PGRAPHD3D12Surface *surface,
                                          uint64_t source_x, uint64_t source_y)
{
    uint32_t x0 = MIN(source_x >> 16, surface->width - 1);
    uint32_t y0 = MIN(source_y >> 16, surface->height - 1);
    if (g_config.display.filtering != CONFIG_DISPLAY_FILTERING_LINEAR) {
        return d3d12_read_vram_pixel(surface, x0, y0);
    }

    uint32_t x1 = MIN(x0 + 1, surface->width - 1);
    uint32_t y1 = MIN(y0 + 1, surface->height - 1);
    uint32_t fx = (source_x >> 8) & 0xff;
    uint32_t fy = (source_y >> 8) & 0xff;
    uint32_t top = d3d12_lerp_pixel(d3d12_read_vram_pixel(surface, x0, y0),
                                    d3d12_read_vram_pixel(surface, x1, y0), fx);
    uint32_t bottom =
        d3d12_lerp_pixel(d3d12_read_vram_pixel(surface, x0, y1),
                         d3d12_read_vram_pixel(surface, x1, y1), fx);
    return d3d12_lerp_pixel(top, bottom, fy);
}

static void d3d12_upload_vram_surface(PGRAPHD3D12State *r,
                                      PGRAPHD3D12Frame *frame,
                                      const PGRAPHD3D12Surface *surface)
{
    uint32_t *destination = (uint32_t *)frame->upload_data;
    uint32_t destination_pitch =
        frame->upload_footprint.Footprint.RowPitch / sizeof(uint32_t);

    memset(destination, 0,
           (size_t)destination_pitch * r->output_height * sizeof(*destination));
    int64_t display_width = r->output_width;
    int64_t display_height = r->output_height;
    double aspect = d3d12_get_display_aspect_ratio(surface);
    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        display_height = surface->height;
        display_width = MAX((int64_t)(display_height * aspect + 0.5), 1);
    } else if (g_config.display.ui.fit != CONFIG_DISPLAY_UI_FIT_STRETCH) {
        double output_aspect = (double)r->output_width / r->output_height;
        if (output_aspect >= aspect) {
            display_width = MAX((int64_t)(r->output_height * aspect + 0.5), 1);
        } else {
            display_height = MAX((int64_t)(r->output_width / aspect + 0.5), 1);
        }
    }
    int64_t offset_x = ((int64_t)r->output_width - display_width) / 2;
    int64_t offset_y = ((int64_t)r->output_height - display_height) / 2;
    int64_t first_x = MAX(offset_x, 0);
    int64_t first_y = MAX(offset_y, 0);
    int64_t last_x = MIN(offset_x + display_width, r->output_width);
    int64_t last_y = MIN(offset_y + display_height, r->output_height);
    for (int64_t y = first_y; y < last_y; y++) {
        uint64_t source_y =
            (((uint64_t)(y - offset_y) * surface->height) << 16) /
            display_height;
        uint32_t *destination_row = destination + y * destination_pitch;
        for (int64_t x = first_x; x < last_x; x++) {
            uint64_t source_x =
                (((uint64_t)(x - offset_x) * surface->width) << 16) /
                display_width;
            destination_row[x] =
                d3d12_sample_vram_surface(surface, source_x, source_y);
        }
    }
}

static void pgraph_d3d12_init(NV2AState *d, Error **errp)
{
    PGRAPHD3D12State *r = g_new0(PGRAPHD3D12State, 1);
    qemu_mutex_init(&r->queue_lock);
    d->pgraph.d3d12_renderer_state = r;
    d->pgraph.surface_scale_factor =
        MAX(g_config.display.quality.surface_scale, 1U);
    if (!d3d12_create_device(r, errp) || !pgraph_d3d12_reports_init(r, errp) ||
        !pgraph_d3d12_shader_cache_init(&r->shader_cache, r->device, errp) ||
        !pgraph_d3d12_draw_init(r, errp) ||
        !pgraph_d3d12_surfaces_init(r, errp) ||
        !pgraph_d3d12_textures_init(r, errp) ||
        !d3d12_create_swapchain(r, errp)) {
        d->pgraph.d3d12_renderer_state = NULL;
        d3d12_release_state(r);
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            r->hud_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
            r->hud_srv_heap);
    r->hud_initialized = xemu_hud_d3d12_init(
        r->device, r->queue, r->hud_srv_heap, cpu.ptr, gpu.ptr,
        D3D12_FRAME_COUNT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!r->hud_initialized) {
        qemu_host_emit_log(QEMU_HOST_LOG_WARNING,
                           "D3D12: ImGui HUD backend is unavailable");
    }
}

static bool pgraph_d3d12_recover_device(NV2AState *d)
{
    PGRAPHD3D12State *old = d->pgraph.d3d12_renderer_state;
    if (!old || !old->device_lost) {
        return true;
    }
    Error *error = NULL;
    PGRAPHD3D12State *replacement = g_new0(PGRAPHD3D12State, 1);
    qemu_mutex_init(&replacement->queue_lock);
    if (!d3d12_create_device(replacement, &error) ||
        !pgraph_d3d12_reports_init(replacement, &error) ||
        !pgraph_d3d12_shader_cache_init(&replacement->shader_cache,
                                        replacement->device, &error) ||
        !pgraph_d3d12_draw_init(replacement, &error) ||
        !pgraph_d3d12_surfaces_init(replacement, &error) ||
        !pgraph_d3d12_textures_init(replacement, &error) ||
        !d3d12_create_swapchain(replacement, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: device recovery failed");
        error_free(error);
        d3d12_release_state(replacement);
        return false;
    }
    pgraph_d3d12_reports_device_lost(d);
    if (old->hud_initialized) {
        xemu_hud_d3d12_shutdown();
        old->hud_initialized = false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            replacement->hud_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
            replacement->hud_srv_heap);
    replacement->hud_initialized = xemu_hud_d3d12_init(
        replacement->device, replacement->queue, replacement->hud_srv_heap,
        cpu.ptr, gpu.ptr, D3D12_FRAME_COUNT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!replacement->hud_initialized) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, "D3D12: HUD recovery failed");
    }
    d->pgraph.d3d12_renderer_state = replacement;
    d3d12_release_state(old);
    for (unsigned int i = 0; i < 4; i++) {
        d->pgraph.texture_dirty[i] = true;
    }
    qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                       "D3D12: device and reconstructible resources recovered");
    return true;
}

static void pgraph_d3d12_finalize(NV2AState *d)
{
    d3d12_release_state(d->pgraph.d3d12_renderer_state);
    d->pgraph.d3d12_renderer_state = NULL;
}

static bool pgraph_d3d12_present_frame_unlocked(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    if (!r) {
        return false;
    }
    HRESULT reason = ID3D12Device_GetDeviceRemovedReason(r->device);
    if (FAILED(reason)) {
        pgraph_d3d12_note_device_error(r, reason, "present preflight");
    }
    if (r->device_lost) {
        if (!pgraph_d3d12_recover_device(d)) {
            return true;
        }
        r = d->pgraph.d3d12_renderer_state;
    }
    Error *resize_error = NULL;
    if (!d3d12_resize_swapchain(r, &resize_error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           resize_error ? error_get_pretty(resize_error) :
                                          "D3D12: swapchain resize failed");
        error_free(resize_error);
        return true;
    }
    bool pressure = d3d12_update_memory_budget(r);
    if (pressure && !r->memory_pressure) {
        Error *error = NULL;
        if (d3d12_wait_idle(r, &error)) {
            pgraph_d3d12_textures_trim(r);
            for (unsigned int i = 0; i < 4; i++) {
                d->pgraph.texture_dirty[i] = true;
            }
            qemu_host_emit_log(
                QEMU_HOST_LOG_WARNING,
                "D3D12: video-memory pressure, texture cache evicted");
        } else {
            error_free(error);
        }
    }
    r->memory_pressure = pressure;

    /* The scanout path reads VRAM.  Native D3D12 render targets are the
     * authoritative copy after a draw, so resolve them before sampling the
     * emulated CRTC surface. */
    Error *flush_error = NULL;
    if (!pgraph_d3d12_surface_flush(d, &flush_error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           flush_error ? error_get_pretty(flush_error) :
                                         "D3D12: scanout surface flush failed");
        error_free(flush_error);
        return true;
    }
    qemu_mutex_lock(&r->queue_lock);
    PGRAPHD3D12Frame *frame = &r->frames[r->frame_index];
    PGRAPHD3D12Surface surface = { 0 };
    HRESULT hr;

    if (!d3d12_wait_for_frame(r, frame)) {
        hr = ID3D12Device_GetDeviceRemovedReason(r->device);
        goto fail;
    }
    hr = ID3D12CommandAllocator_Reset(frame->allocator);
    if (FAILED(hr)) {
        goto fail;
    }
    hr = ID3D12GraphicsCommandList_Reset(frame->command_list, frame->allocator,
                                         NULL);
    if (FAILED(hr)) {
        goto fail;
    }

    if (d3d12_get_vram_surface(d, &surface)) {
        d3d12_upload_vram_surface(r, frame, &surface);
    } else {
        memset(frame->upload_data, 0,
               (size_t)frame->upload_footprint.Footprint.RowPitch *
                   r->output_height);
    }
    xemu_hud_d3d12_capture_frame(frame->upload_data, r->output_width,
                                 r->output_height,
                                 frame->upload_footprint.Footprint.RowPitch);

    d3d12_transition(frame->command_list, frame->render_target,
                     &frame->render_target_state,
                     D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination = {
        .pResource = frame->render_target,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    D3D12_TEXTURE_COPY_LOCATION source = {
        .pResource = frame->upload_buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = frame->upload_footprint,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(
        frame->command_list, &destination, 0, 0, 0, &source, NULL);
    d3d12_transition(frame->command_list, frame->render_target,
                     &frame->render_target_state,
                     D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE frame_rtv =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtv_heap);
    frame_rtv.ptr += (SIZE_T)r->frame_index * r->rtv_increment;
    ID3D12GraphicsCommandList_OMSetRenderTargets(frame->command_list, 1,
                                                 &frame_rtv, FALSE, NULL);
    ID3D12DescriptorHeap *hud_heaps[] = { r->hud_srv_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(frame->command_list, 1,
                                                 hud_heaps);
    if (r->hud_initialized) {
        xemu_hud_d3d12_render(frame->command_list);
    }
    d3d12_transition(frame->command_list, frame->render_target,
                     &frame->render_target_state, D3D12_RESOURCE_STATE_PRESENT);
    hr = ID3D12GraphicsCommandList_Close(frame->command_list);
    if (FAILED(hr)) {
        goto fail;
    }
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)frame->command_list };
    ID3D12CommandQueue_ExecuteCommandLists(r->queue, 1, lists);
    hr = IDXGISwapChain3_Present(r->swapchain,
                                 g_config.display.window.vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        goto fail;
    }

    frame->fence_value = r->next_fence_value++;
    hr = ID3D12CommandQueue_Signal(r->queue, r->fence, frame->fence_value);
    if (FAILED(hr)) {
        goto fail;
    }
    r->frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapchain);
    qemu_mutex_unlock(&r->queue_lock);
    return true;

fail:
    qemu_mutex_unlock(&r->queue_lock);
    pgraph_d3d12_note_device_error(r, hr, "frame presentation");
    if (r->device_lost) {
        pgraph_d3d12_recover_device(d);
    }
    /* The D3D12 renderer owns the UWP presentation target.  Returning false
     * would make the UI try the unrelated OpenGL fallback with no valid GL
     * framebuffer texture, hiding the real error and potentially corrupting
     * renderer state. Keep the last swapchain image visible instead. */
    return true;
}

static bool pgraph_d3d12_present_frame(NV2AState *d)
{
    bool presented;

    /* The UI thread owns presentation, while PFIFO records draws on the
     * PGRAPH thread. Surface state, report queues, texture caches and device
     * recovery are shared by both paths, so serialize the complete native
     * present transaction with the same lock used by PGRAPH methods. The
     * caller already owns renderer_lock, matching renderer-switch lock order.
     */
    qemu_mutex_lock(&d->pgraph.lock);
    presented = pgraph_d3d12_present_frame_unlocked(d);
    qemu_mutex_unlock(&d->pgraph.lock);
    return presented;
}

static void pgraph_d3d12_process_pending(NV2AState *d)
{
    if (qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&d->pgraph.sync_pending)) {
            Error *error = NULL;
            PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
            if (!d3d12_wait_idle(r, &error) ||
                !pgraph_d3d12_draw_collect(r, false, &error)) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   error_get_pretty(error));
                error_free(error);
            }
            qatomic_set(&d->pgraph.sync_pending, false);
            qemu_event_set(&d->pgraph.sync_complete);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            Error *error = NULL;
            PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
            bool idle = d3d12_wait_idle(r, &error);
            if (idle) {
                idle = pgraph_d3d12_draw_collect(r, false, &error);
            }
            if (!idle) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   error_get_pretty(error));
                error_free(error);
                error = NULL;
            }
            if (idle) {
                idle = pgraph_d3d12_surface_flush(d, &error);
            }
            if (idle) {
                pgraph_d3d12_process_reports_now(d, &error);
            }
            if (error) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   error_get_pretty(error));
                error_free(error);
            }
            qatomic_set(&d->pgraph.flush_pending, false);
            qemu_event_set(&d->pgraph.flush_complete);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_d3d12_wait_and_resolve(NV2AState *d)
{
    Error *error = NULL;
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    if (!d3d12_wait_idle(r, &error) ||
        !pgraph_d3d12_draw_collect(r, false, &error) ||
        !pgraph_d3d12_process_reports_now(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: synchronization failed");
        error_free(error);
    }
}

static void pgraph_d3d12_wait_complete(NV2AState *d)
{
    pgraph_d3d12_wait_and_resolve(d);
}

static void pgraph_d3d12_pre_savevm_trigger(NV2AState *d)
{
    pgraph_d3d12_wait_and_resolve(d);
    pgraph_d3d12_shader_cache_flush(
        d->pgraph.d3d12_renderer_state->shader_cache);
}

static void pgraph_d3d12_pre_shutdown_trigger(NV2AState *d)
{
    pgraph_d3d12_wait_and_resolve(d);
    pgraph_d3d12_shader_cache_flush(
        d->pgraph.d3d12_renderer_state->shader_cache);
}

static void pgraph_d3d12_draw_begin(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    Error *error = NULL;
    r->draw_prepared = pgraph_d3d12_update_output_merger(d, &error) &&
                       pgraph_d3d12_bind_textures(d, &error);
    if (!r->draw_prepared) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: texture binding failed");
        error_free(error);
    }
}

static void pgraph_d3d12_draw_end(NV2AState *d)
{
    /* draw_end is the point where the common PGRAPH frontend has finished
     * collecting draw arrays/inline vertices.  Never leave a pending draw
     * silently unconsumed: the draw module records and submits it here. */
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    if (!r || !r->draw_prepared) {
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attribute = &d->pgraph.vertex_attributes[i];
            if (attribute->inline_buffer_populated &&
                d->pgraph.inline_buffer_length) {
                memcpy(attribute->inline_value,
                       attribute->inline_buffer +
                           (d->pgraph.inline_buffer_length - 1) * 4,
                       sizeof(attribute->inline_value));
                attribute->inline_buffer_populated = false;
            }
        }
        return;
    }
    r->draw_prepared = false;
    Error *error = NULL;
    if (!pgraph_d3d12_flush_draw(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: draw submission failed");
        error_free(error);
    }
}

static void pgraph_d3d12_surface_flush_op(NV2AState *d)
{
    Error *error = NULL;
    if (!pgraph_d3d12_surface_flush(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: surface download failed");
        error_free(error);
    }
}

static void pgraph_d3d12_set_surface_scale_factor(NV2AState *d,
                                                  unsigned int scale)
{
    PGRAPHState *pg = &d->pgraph;
    qemu_mutex_lock(&pg->lock);
    scale = MAX(scale, 1U);
    if (pg->surface_scale_factor == scale) {
        qemu_mutex_unlock(&pg->lock);
        return;
    }
    pgraph_d3d12_wait_and_resolve(d);
    pgraph_d3d12_surface_flush_op(d);
    pgraph_d3d12_surfaces_finalize(pg->d3d12_renderer_state);
    Error *error = NULL;
    if (!pgraph_d3d12_surfaces_init(pg->d3d12_renderer_state, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: failed to recreate scaled surfaces");
        error_free(error);
        qemu_mutex_unlock(&pg->lock);
        return;
    }
    pg->surface_scale_factor = scale;
    g_config.display.quality.surface_scale = scale;
    pg->surface_color.buffer_dirty = true;
    pg->surface_zeta.buffer_dirty = true;
    qemu_mutex_unlock(&pg->lock);
}

static unsigned int pgraph_d3d12_get_surface_scale_factor(NV2AState *d)
{
    return MAX(d->pgraph.surface_scale_factor, 1U);
}

static GPUProperties *pgraph_d3d12_get_gpu_properties(void)
{
    /* D3D12 preserves the vertex order emitted by the geometry shader. */
    static GPUProperties properties = {
        .geom_shader_winding = {
            .tri = 0,
            .tri_strip0 = 0,
            .tri_strip1 = 0,
            .tri_fan = 1,
        },
    };
    return &properties;
}

static void pgraph_d3d12_clear_surface_op(NV2AState *d, uint32_t parameter)
{
    Error *error = NULL;
    if (!pgraph_d3d12_clear_surface(d, parameter, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: surface clear failed");
        error_free(error);
    }
}

static void pgraph_d3d12_surface_update_op(NV2AState *d, bool upload,
                                           bool color_write, bool zeta_write)
{
    if (!upload) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d12_surface_update(d, color_write, zeta_write, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: surface update failed");
        error_free(error);
    }
}

static PGRAPHRenderer pgraph_d3d12_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_D3D12,
    .name = "Direct3D 12",
    .ops = {
        .init = pgraph_d3d12_init,
        .finalize = pgraph_d3d12_finalize,
        .clear_report_value = pgraph_d3d12_clear_report_value,
        .clear_surface = pgraph_d3d12_clear_surface_op,
        .draw_begin = pgraph_d3d12_draw_begin,
        .draw_end = pgraph_d3d12_draw_end,
        .flip_stall = pgraph_d3d12_wait_and_resolve,
        .flush_draw = pgraph_d3d12_wait_and_resolve,
        .get_report = pgraph_d3d12_get_report,
        .image_blit = pgraph_d3d12_image_blit,
        .pre_savevm_trigger = pgraph_d3d12_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_d3d12_wait_complete,
        .pre_shutdown_trigger = pgraph_d3d12_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_d3d12_wait_complete,
        .process_pending = pgraph_d3d12_process_pending,
        .process_pending_reports = pgraph_d3d12_process_pending_reports,
        .surface_flush = pgraph_d3d12_surface_flush_op,
        .surface_update = pgraph_d3d12_surface_update_op,
        .set_surface_scale_factor = pgraph_d3d12_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_d3d12_get_surface_scale_factor,
        .present_frame = pgraph_d3d12_present_frame,
        .get_gpu_properties = pgraph_d3d12_get_gpu_properties,
    },
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_d3d12_renderer);
}
