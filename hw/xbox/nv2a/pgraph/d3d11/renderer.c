/*
 * GeForce NV2A PGRAPH Direct3D 11 Renderer
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-settings.h"
#include "blits.h"
#include "renderer.h"

#define D3D11_FRAME_COUNT 2

static void d3d11_set_error(Error **errp, const char *operation, HRESULT hr)
{
    error_setg(errp, "D3D11: %s failed (HRESULT 0x%08lx)", operation,
               (unsigned long)hr);
}

static void d3d11_release_backbuffer(PGRAPHD3D11State *r)
{
    if (r->backbuffer_rtv) {
        ID3D11RenderTargetView_Release(r->backbuffer_rtv);
        r->backbuffer_rtv = NULL;
    }
    if (r->backbuffer) {
        ID3D11Texture2D_Release(r->backbuffer);
        r->backbuffer = NULL;
    }
}

static bool d3d11_wait_idle(PGRAPHD3D11State *r)
{
    if (!r || !r->context || !r->idle_query || r->device_lost) {
        return true;
    }

    ID3D11DeviceContext_End(r->context, (ID3D11Asynchronous *)r->idle_query);
    ID3D11DeviceContext_Flush(r->context);
    for (unsigned int attempt = 0; attempt < 5000; attempt++) {
        HRESULT hr = ID3D11DeviceContext_GetData(
            r->context, (ID3D11Asynchronous *)r->idle_query, NULL, 0, 0);
        if (hr == S_OK) {
            return true;
        }
        if (FAILED(hr)) {
            r->device_lost = true;
            return false;
        }
        Sleep(1);
    }
    qemu_host_emit_log(QEMU_HOST_LOG_WARNING,
                       "D3D11: timed out waiting for the GPU");
    return false;
}

static void d3d11_release_device_resources(PGRAPHD3D11State *r)
{
    if (!r) {
        return;
    }
    pgraph_d3d11_reports_finalize(r);
    d3d11_wait_idle(r);
    if (r->context) {
        ID3D11DeviceContext_ClearState(r->context);
        ID3D11DeviceContext_Flush(r->context);
    }
    pgraph_d3d11_draw_finalize(r);
    pgraph_d3d11_textures_finalize(r);
    pgraph_d3d11_shaders_finalize(r);
    pgraph_d3d11_surfaces_finalize(r);
    pgraph_d3d11_presentation_finalize(r);
    d3d11_release_backbuffer(r);
    if (r->idle_query) {
        ID3D11Query_Release(r->idle_query);
        r->idle_query = NULL;
    }
    if (r->swapchain) {
        IDXGISwapChain3_Release(r->swapchain);
        r->swapchain = NULL;
    }
    if (r->context) {
        ID3D11DeviceContext_Release(r->context);
        r->context = NULL;
    }
    if (r->device1) {
        ID3D11Device1_Release(r->device1);
        r->device1 = NULL;
    }
    if (r->device) {
        ID3D11Device_Release(r->device);
        r->device = NULL;
    }
    if (r->adapter) {
        IDXGIAdapter1_Release(r->adapter);
        r->adapter = NULL;
    }
    if (r->factory) {
        IDXGIFactory4_Release(r->factory);
        r->factory = NULL;
    }
    r->output_width = 0;
    r->output_height = 0;
    r->output_merger_logic_op = false;
    r->resources_ready = false;
}

static void d3d11_release_state(PGRAPHD3D11State *r)
{
    if (!r) {
        return;
    }
    d3d11_release_device_resources(r);
    g_free(r);
}

static bool d3d11_create_device(PGRAPHD3D11State *r, Error **errp)
{
    HRESULT hr =
        CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&r->factory);
    if (FAILED(hr)) {
        d3d11_set_error(errp, "CreateDXGIFactory2", hr);
        return false;
    }

    static const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    for (UINT index = 0;; index++) {
        IDXGIAdapter1 *adapter = NULL;
        hr = IDXGIFactory4_EnumAdapters1(r->factory, index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            d3d11_set_error(errp, "IDXGIFactory::EnumAdapters1", hr);
            return false;
        }
        DXGI_ADAPTER_DESC1 desc;
        hr = IDXGIAdapter1_GetDesc1(adapter, &desc);
        if (SUCCEEDED(hr) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            hr = D3D11CreateDevice(
                (IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
                ARRAY_SIZE(feature_levels), D3D11_SDK_VERSION, &r->device,
                &r->feature_level, &r->context);
            if (SUCCEEDED(hr)) {
                r->adapter = adapter;
                break;
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    if (!r->device) {
        d3d11_set_error(errp, "D3D11CreateDevice", hr);
        return false;
    }
    ID3D10Multithread *multithread = NULL;
    hr = ID3D11DeviceContext_QueryInterface(r->context, &IID_ID3D10Multithread,
                                            (void **)&multithread);
    if (FAILED(hr)) {
        d3d11_set_error(errp, "query ID3D10Multithread", hr);
        return false;
    }
    ID3D10Multithread_SetMultithreadProtected(multithread, TRUE);
    ID3D10Multithread_Release(multithread);
    ID3D11Device_QueryInterface(r->device, &IID_ID3D11Device1,
                                (void **)&r->device1);
    D3D11_FEATURE_DATA_D3D11_OPTIONS options = { 0 };
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(
            r->device, D3D11_FEATURE_D3D11_OPTIONS, &options,
            sizeof(options)))) {
        r->output_merger_logic_op = options.OutputMergerLogicOp;
    }

    D3D11_QUERY_DESC query_desc = {
        .Query = D3D11_QUERY_EVENT,
    };
    hr = ID3D11Device_CreateQuery(r->device, &query_desc, &r->idle_query);
    if (FAILED(hr)) {
        d3d11_set_error(errp, "CreateQuery(D3D11_QUERY_EVENT)", hr);
        return false;
    }
    return true;
}

static bool d3d11_create_backbuffer(PGRAPHD3D11State *r, Error **errp)
{
    HRESULT hr = IDXGISwapChain3_GetBuffer(
        r->swapchain, 0, &IID_ID3D11Texture2D, (void **)&r->backbuffer);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(
            r->device, (ID3D11Resource *)r->backbuffer, NULL,
            &r->backbuffer_rtv);
    }
    if (FAILED(hr)) {
        d3d11_set_error(errp, "create swapchain render target", hr);
        d3d11_release_backbuffer(r);
        return false;
    }
    return true;
}

static bool d3d11_create_swapchain(PGRAPHD3D11State *r, Error **errp)
{
    QemuHostD3D12PresentTarget target;
    if (qemu_host_get_d3d12_present_target(&target) || !target.width ||
        !target.height || !target.attach) {
        error_setg(errp, "D3D11: UWP presentation target is not attached");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = target.width,
        .Height = target.height,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = D3D11_FRAME_COUNT,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
    };
    IDXGISwapChain1 *swapchain1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForComposition(
        r->factory, (IUnknown *)r->device, &desc, NULL, &swapchain1);
    if (SUCCEEDED(hr)) {
        hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3,
                                            (void **)&r->swapchain);
    }
    if (SUCCEEDED(hr)) {
        hr = target.attach(target.opaque, swapchain1);
    }
    if (swapchain1) {
        IDXGISwapChain1_Release(swapchain1);
    }
    if (FAILED(hr)) {
        d3d11_set_error(errp, "create or attach composition swapchain", hr);
        return false;
    }
    r->output_width = target.width;
    r->output_height = target.height;
    return d3d11_create_backbuffer(r, errp);
}

static bool d3d11_resize_swapchain(PGRAPHD3D11State *r, Error **errp)
{
    QemuHostD3D12PresentTarget target;
    if (qemu_host_get_d3d12_present_target(&target) || !target.width ||
        !target.height ||
        (target.width == r->output_width &&
         target.height == r->output_height)) {
        return true;
    }

    d3d11_wait_idle(r);
    ID3D11DeviceContext_OMSetRenderTargets(r->context, 0, NULL, NULL);
    d3d11_release_backbuffer(r);
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(r->swapchain, D3D11_FRAME_COUNT,
                                               target.width, target.height,
                                               DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        d3d11_set_error(errp, "IDXGISwapChain::ResizeBuffers", hr);
        r->device_lost =
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
        return false;
    }
    r->output_width = target.width;
    r->output_height = target.height;
    return d3d11_create_backbuffer(r, errp);
}

static bool d3d11_create_device_resources(PGRAPHD3D11State *r, Error **errp)
{
    r->device_lost = false;
    if (!d3d11_create_device(r, errp) ||
        !pgraph_d3d11_presentation_init(r, errp) ||
        !pgraph_d3d11_shaders_init(r, errp) ||
        !pgraph_d3d11_textures_init(r, errp) || !pgraph_d3d11_reports_init(r) ||
        !pgraph_d3d11_draw_init(r, errp) || !d3d11_create_swapchain(r, errp)) {
        d3d11_release_device_resources(r);
        r->device_lost = true;
        return false;
    }
    r->resources_ready = true;
    r->next_recovery_us = 0;
    return true;
}

static void d3d11_mark_device_lost(PGRAPHD3D11State *r, const char *operation,
                                   HRESULT hr)
{
    HRESULT reason =
        r->device ? ID3D11Device_GetDeviceRemovedReason(r->device) : hr;
    if (SUCCEEDED(reason)) {
        reason = hr;
    }
    r->device_lost = true;
    r->next_recovery_us = g_get_monotonic_time() + G_USEC_PER_SEC;
    char *message = g_strdup_printf(
        "D3D11: device lost during %s (HRESULT 0x%08lx, reason 0x%08lx)",
        operation, (unsigned long)hr, (unsigned long)reason);
    qemu_host_emit_log(QEMU_HOST_LOG_ERROR, message);
    g_free(message);
}

static void d3d11_check_device_removed(PGRAPHD3D11State *r,
                                       const char *operation)
{
    if (!r || !r->device || r->device_lost) {
        return;
    }
    HRESULT reason = ID3D11Device_GetDeviceRemovedReason(r->device);
    if (FAILED(reason)) {
        d3d11_mark_device_lost(r, operation, reason);
    }
}

static bool d3d11_recreate_device(NV2AState *d, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (r->app_suspended || r->memory_suspended) {
        return false;
    }
    if (r->resources_ready) {
        d3d11_release_device_resources(r);
    }
    if (!d3d11_create_device_resources(r, errp)) {
        r->next_recovery_us = g_get_monotonic_time() + G_USEC_PER_SEC;
        return false;
    }
    d->pgraph.surface_color.buffer_dirty = true;
    d->pgraph.surface_color.draw_dirty = false;
    d->pgraph.surface_zeta.buffer_dirty = true;
    d->pgraph.surface_zeta.draw_dirty = false;
    qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                       "D3D11: graphics device and resources recreated");
    return true;
}

static void pgraph_d3d11_init(NV2AState *d, Error **errp)
{
    PGRAPHD3D11State *r = g_new0(PGRAPHD3D11State, 1);
    d->pgraph.d3d11_renderer_state = r;
    d->pgraph.surface_scale_factor =
        MAX(g_config.display.quality.surface_scale, 1U);
    if (!d3d11_create_device_resources(r, errp)) {
        d->pgraph.d3d11_renderer_state = NULL;
        d3d11_release_state(r);
        return;
    }
    qemu_host_emit_log(
        QEMU_HOST_LOG_INFO,
        "D3D11: device and UWP composition swapchain initialized");
}

static void pgraph_d3d11_finalize(NV2AState *d)
{
    d3d11_release_state(d->pgraph.d3d11_renderer_state);
    d->pgraph.d3d11_renderer_state = NULL;
}

static void pgraph_d3d11_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || r->device_lost || r->app_suspended ||
        r->memory_suspended) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d11_render_targets_clear(d, parameter, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        d3d11_check_device_removed(r, "surface clear");
    }
}

static void pgraph_d3d11_draw_begin(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (r && r->resources_ready && !r->device_lost && !r->app_suspended &&
        !r->memory_suspended) {
        r->draw->prepared = false;
        Error *error = NULL;
        if (!pgraph_d3d11_update_output_merger(d, &error)) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            d3d11_check_device_removed(r, "output-merger update");
            return;
        }
        pgraph_d3d11_bind_render_targets(r);
        if (!pgraph_d3d11_update_rasterizer(d, &error)) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            d3d11_check_device_removed(r, "rasterizer update");
            return;
        }
        if (!pgraph_d3d11_bind_vertex_shader(&d->pgraph, &error)) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            d3d11_check_device_removed(r, "vertex shader binding");
            return;
        }
        if (!pgraph_d3d11_bind_pixel_shader(&d->pgraph, &error)) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            d3d11_check_device_removed(r, "pixel shader binding");
            return;
        }
        if (!pgraph_d3d11_bind_textures(d, &error)) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            d3d11_check_device_removed(r, "texture binding");
            return;
        }
        r->draw->prepared = true;
        if (!r->logged_draw_prepared) {
            qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                               "D3D11: first NV2A draw prepared");
            r->logged_draw_prepared = true;
        }
        if (!pgraph_d3d11_report_draw_begin(d)) {
            r->draw->prepared = false;
        }
    }
}

static void pgraph_d3d11_draw_end(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (r && r->resources_ready && !r->device_lost && r->draw->prepared) {
        Error *error = NULL;
        bool draw_ok = pgraph_d3d11_flush_draw(d, &error);
        pgraph_d3d11_report_draw_end(d);
        if (!draw_ok) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
            error_free(error);
            r->draw->prepared = false;
            d3d11_check_device_removed(r, "draw submission");
            return;
        }
        r->draw->prepared = false;
        if (!r->logged_draw_submitted) {
            qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                               "D3D11: first NV2A draw submitted");
            r->logged_draw_submitted = true;
        }
        D3D11_RENDER_TARGET_BLEND_DESC1 *blend =
            &r->render_targets.blend_desc.RenderTarget[0];
        D3D11_DEPTH_STENCIL_DESC *depth = &r->render_targets.depth_stencil_desc;
        d->pgraph.surface_color.draw_dirty |= blend->RenderTargetWriteMask != 0;
        d->pgraph.surface_zeta.draw_dirty |=
            (depth->DepthEnable &&
             depth->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ||
            (depth->StencilEnable && depth->StencilWriteMask != 0);
        ID3D11DeviceContext_Flush(r->context);
    }
}

static void pgraph_d3d11_surface_update(NV2AState *d, bool upload,
                                        bool color_write, bool zeta_write)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || r->device_lost || r->app_suspended ||
        r->memory_suspended) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d11_render_targets_update(d, color_write, zeta_write,
                                            &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        d3d11_check_device_removed(r, "render-target update");
    }
}

static void pgraph_d3d11_sync(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (r && r->resources_ready) {
        d3d11_wait_idle(r);
    }
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_d3d11_wait_and_resolve(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (r && r->resources_ready) {
        d3d11_wait_idle(r);
    }
}

static void pgraph_d3d11_surface_flush_op(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || r->device_lost) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d11_surface_flush(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        d3d11_check_device_removed(r, "surface readback");
    }
}

static void pgraph_d3d11_process_pending(NV2AState *d)
{
    if (qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_d3d11_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_d3d11_wait_and_resolve(d);
            pgraph_d3d11_surface_flush_op(d);
            qatomic_set(&d->pgraph.flush_pending, false);
            qemu_event_set(&d->pgraph.flush_complete);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_d3d11_set_surface_scale_factor(NV2AState *d,
                                                  unsigned int scale)
{
    d->pgraph.surface_scale_factor = MAX(scale, 1U);
}

static unsigned int pgraph_d3d11_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor;
}

static bool pgraph_d3d11_present_frame(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || r->app_suspended || r->memory_suspended) {
        return false;
    }
    Error *error = NULL;
    if (!r->resources_ready || r->device_lost) {
        if (g_get_monotonic_time() < r->next_recovery_us ||
            !d3d11_recreate_device(d, &error)) {
            if (error) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   error_get_pretty(error));
                error_free(error);
            }
            return false;
        }
    }
    if (!d3d11_resize_swapchain(r, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        d3d11_check_device_removed(r, "swapchain resize");
        return false;
    }
    if (!pgraph_d3d11_present_color(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        d3d11_check_device_removed(r, "presentation draw");
        return false;
    }
    HRESULT hr = IDXGISwapChain3_Present(r->swapchain, 1, 0);
    if (FAILED(hr)) {
        d3d11_mark_device_lost(r, "Present", hr);
        return false;
    }
    return true;
}

static void d3d11_flush_preserved_state(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r->resources_ready || r->device_lost) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d11_surface_flush(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_WARNING, error_get_pretty(error));
        error_free(error);
    }
    pgraph_d3d11_process_pending_reports(d);
    d3d11_wait_idle(r);
}

static void pgraph_d3d11_suspend(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || r->app_suspended) {
        return;
    }
    r->app_suspended = true;
    d3d11_flush_preserved_state(d);
    d3d11_release_device_resources(r);
    qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                       "D3D11: resources released for suspension");
}

static bool pgraph_d3d11_resume(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r) {
        return false;
    }
    r->app_suspended = false;
    if (r->memory_suspended) {
        return true;
    }
    if (r->resources_ready && !r->device_lost) {
        return true;
    }
    Error *error = NULL;
    bool resumed = d3d11_recreate_device(d, &error);
    if (!resumed && error) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
    }
    return resumed;
}

static void d3d11_dxgi_trim(PGRAPHD3D11State *r)
{
    if (!r->device) {
        return;
    }
    IDXGIDevice3 *dxgi_device = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(r->device, &IID_IDXGIDevice3,
                                              (void **)&dxgi_device))) {
        IDXGIDevice3_Trim(dxgi_device);
        IDXGIDevice3_Release(dxgi_device);
    }
}

static void pgraph_d3d11_memory_pressure(NV2AState *d, unsigned int level)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r) {
        return;
    }
    if (level == QEMU_HOST_MEMORY_PRESSURE_NORMAL) {
        bool was_suspended = r->memory_suspended;
        r->memory_suspended = false;
        if (was_suspended && !r->app_suspended) {
            pgraph_d3d11_resume(d);
        }
        return;
    }
    if (!r->resources_ready) {
        return;
    }

    pgraph_d3d11_textures_trim(r);
    pgraph_d3d11_shaders_trim(r);
    pgraph_d3d11_draw_trim(r);
    if (level >= QEMU_HOST_MEMORY_PRESSURE_HIGH) {
        d3d11_flush_preserved_state(d);
        pgraph_d3d11_surfaces_finalize(r);
    }
    d3d11_dxgi_trim(r);
    if (level >= QEMU_HOST_MEMORY_PRESSURE_CRITICAL) {
        r->memory_suspended = true;
        d3d11_release_device_resources(r);
    }
    qemu_host_emit_log(
        QEMU_HOST_LOG_INFO,
        level >= QEMU_HOST_MEMORY_PRESSURE_CRITICAL ?
            "D3D11: device released under critical memory pressure" :
            "D3D11: reconstructible caches trimmed for memory pressure");
}

static GPUProperties d3d11_gpu_properties = {
    .geom_shader_winding = {
        .tri = 1,
        .tri_strip0 = 1,
        .tri_strip1 = -1,
        .tri_fan = 1,
    },
};

static GPUProperties *pgraph_d3d11_get_gpu_properties(void)
{
    return &d3d11_gpu_properties;
}

static PGRAPHRenderer pgraph_d3d11_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_D3D11,
    .name = "Direct3D 11 (experimental)",
    .ops = {
        .init = pgraph_d3d11_init,
        .finalize = pgraph_d3d11_finalize,
        .clear_report_value = pgraph_d3d11_clear_report_value,
        .clear_surface = pgraph_d3d11_clear_surface,
        .draw_begin = pgraph_d3d11_draw_begin,
        .draw_end = pgraph_d3d11_draw_end,
        .flip_stall = pgraph_d3d11_wait_and_resolve,
        .flush_draw = pgraph_d3d11_wait_and_resolve,
        .get_report = pgraph_d3d11_get_report,
        .image_blit = pgraph_d3d11_image_blit,
        .pre_savevm_trigger = pgraph_d3d11_surface_flush_op,
        .pre_savevm_wait = pgraph_d3d11_wait_and_resolve,
        .pre_shutdown_trigger = pgraph_d3d11_surface_flush_op,
        .pre_shutdown_wait = pgraph_d3d11_wait_and_resolve,
        .process_pending = pgraph_d3d11_process_pending,
        .process_pending_reports = pgraph_d3d11_process_pending_reports,
        .surface_flush = pgraph_d3d11_surface_flush_op,
        .surface_update = pgraph_d3d11_surface_update,
        .set_surface_scale_factor = pgraph_d3d11_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_d3d11_get_surface_scale_factor,
        .present_frame = pgraph_d3d11_present_frame,
        .suspend = pgraph_d3d11_suspend,
        .resume = pgraph_d3d11_resume,
        .memory_pressure = pgraph_d3d11_memory_pressure,
        .get_gpu_properties = pgraph_d3d11_get_gpu_properties,
    },
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_d3d11_renderer);
}
