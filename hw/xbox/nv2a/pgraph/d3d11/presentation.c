/*
 * GeForce NV2A PGRAPH Direct3D 11 presentation
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include <d3dcompiler.h>
#include "ui/xemu-settings.h"
#include "ui/xui/xemu-hud.h"
#include "ui/xemu-widescreen.h"
#include "presentation.h"

static const char d3d11_presentation_shader[] =
    "struct VSOut { float4 position:SV_Position; float2 uv:TEXCOORD0; };"
    "VSOut vs_main(uint id:SV_VertexID) {"
    "  VSOut o;"
    "  if (id == 0) { o.position=float4(-1,-1,0,1); o.uv=float2(0,1); }"
    "  else if (id == 1) { o.position=float4(-1,3,0,1); o.uv=float2(0,-1); }"
    "  else { o.position=float4(3,-1,0,1); o.uv=float2(2,1); }"
    "  return o;"
    "}"
    "Texture2D source_texture:register(t0);"
    "SamplerState source_sampler:register(s0);"
    "float4 ps_main(VSOut input):SV_Target {"
    "  return source_texture.Sample(source_sampler,input.uv);"
    "}";

static bool d3d11_compile_presentation_shader(const char *entry,
                                              const char *profile,
                                              ID3DBlob **blob, Error **errp)
{
    ID3DBlob *messages = NULL;
    HRESULT hr =
        D3DCompile(d3d11_presentation_shader, strlen(d3d11_presentation_shader),
                   "d3d11-presentation", NULL, NULL, entry, profile,
                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &messages);
    if (FAILED(hr)) {
        const char *details = messages ? ID3D10Blob_GetBufferPointer(messages) :
                                         "no compiler diagnostics";
        error_setg(errp,
                   "D3D11: presentation %s compilation failed "
                   "(HRESULT 0x%08lx): %s",
                   profile, (unsigned long)hr, details);
    }
    if (messages) {
        ID3D10Blob_Release(messages);
    }
    return SUCCEEDED(hr);
}

static bool d3d11_create_sampler(PGRAPHD3D11State *r, D3D11_FILTER filter,
                                 ID3D11SamplerState **sampler, Error **errp)
{
    D3D11_SAMPLER_DESC desc = {
        .Filter = filter,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MinLOD = 0,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    HRESULT hr = ID3D11Device_CreateSamplerState(r->device, &desc, sampler);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: presentation sampler creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    return true;
}

bool pgraph_d3d11_presentation_init(PGRAPHD3D11State *r, Error **errp)
{
    PGRAPHD3D11PresentationState *presentation =
        g_new0(PGRAPHD3D11PresentationState, 1);
    r->presentation = presentation;
    ID3DBlob *vertex = NULL;
    ID3DBlob *pixel = NULL;
    if (!d3d11_compile_presentation_shader("vs_main", "vs_4_0", &vertex,
                                           errp) ||
        !d3d11_compile_presentation_shader("ps_main", "ps_4_0", &pixel, errp)) {
        goto fail;
    }
    HRESULT hr = ID3D11Device_CreateVertexShader(
        r->device, ID3D10Blob_GetBufferPointer(vertex),
        ID3D10Blob_GetBufferSize(vertex), NULL, &presentation->vertex_shader);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(
            r->device, ID3D10Blob_GetBufferPointer(pixel),
            ID3D10Blob_GetBufferSize(pixel), NULL, &presentation->pixel_shader);
    }
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: presentation shader creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        goto fail;
    }
    if (!d3d11_create_sampler(r, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                              &presentation->linear_sampler, errp) ||
        !d3d11_create_sampler(r, D3D11_FILTER_MIN_MAG_MIP_POINT,
                              &presentation->nearest_sampler, errp)) {
        goto fail;
    }
    ID3D10Blob_Release(vertex);
    ID3D10Blob_Release(pixel);
    return true;

fail:
    if (vertex) {
        ID3D10Blob_Release(vertex);
    }
    if (pixel) {
        ID3D10Blob_Release(pixel);
    }
    pgraph_d3d11_presentation_finalize(r);
    return false;
}

void pgraph_d3d11_presentation_finalize(PGRAPHD3D11State *r)
{
    PGRAPHD3D11PresentationState *presentation = r->presentation;
    if (!presentation) {
        return;
    }
    if (presentation->scanout_srv) {
        ID3D11ShaderResourceView_Release(presentation->scanout_srv);
    }
    if (presentation->scanout_texture) {
        ID3D11Texture2D_Release(presentation->scanout_texture);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(presentation->retained); i++) {
        if (presentation->retained[i].srv) {
            ID3D11ShaderResourceView_Release(presentation->retained[i].srv);
            ID3D11Texture2D_Release(presentation->retained[i].texture);
        }
    }
    if (presentation->nearest_sampler) {
        ID3D11SamplerState_Release(presentation->nearest_sampler);
    }
    if (presentation->linear_sampler) {
        ID3D11SamplerState_Release(presentation->linear_sampler);
    }
    if (presentation->pixel_shader) {
        ID3D11PixelShader_Release(presentation->pixel_shader);
    }
    if (presentation->vertex_shader) {
        ID3D11VertexShader_Release(presentation->vertex_shader);
    }
    g_free(presentation);
    r->presentation = NULL;
}

static bool d3d11_retain_scanout(PGRAPHD3D11State *r,
                                 PGRAPHD3D11RenderTarget *color, Error **errp)
{
    PGRAPHD3D11PresentationState *presentation = r->presentation;
    (void)errp;
    if (!color->texture || !color->srv || !color->width || !color->height) {
        return false;
    }
    const uint64_t budget = 64 * 1024 * 1024;
    uint64_t bytes = (uint64_t)color->width * color->height * 4;
    if (bytes > budget) {
        return true;
    }
    ID3D11Texture2D_AddRef(color->texture);
    ID3D11ShaderResourceView_AddRef(color->srv);
    unsigned int slot = 0;
    uint64_t used = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(presentation->retained); i++) {
        PGRAPHD3D11CachedScanout *entry = &presentation->retained[i];
        if (entry->srv && entry->address == color->vram_address) {
            ID3D11ShaderResourceView_Release(entry->srv);
            ID3D11Texture2D_Release(entry->texture);
            memset(entry, 0, sizeof(*entry));
        }
        used += (uint64_t)entry->width * entry->height * 4;
        if (entry->serial < presentation->retained[slot].serial) {
            slot = i;
        }
    }
    for (;;) {
        PGRAPHD3D11CachedScanout *entry = &presentation->retained[slot];
        if (entry->srv) {
            used -= (uint64_t)entry->width * entry->height * 4;
            ID3D11ShaderResourceView_Release(entry->srv);
            ID3D11Texture2D_Release(entry->texture);
            memset(entry, 0, sizeof(*entry));
        }
        if (used + bytes <= budget) {
            *entry = (PGRAPHD3D11CachedScanout){
                .texture = color->texture,
                .srv = color->srv,
                .width = color->width,
                .height = color->height,
                .address = color->vram_address,
                .length = color->storage_length,
                .serial = ++presentation->retained_serial,
            };
            break;
        }
        /* Evict the oldest occupied entry; empty slots cannot reduce usage. */
        uint64_t oldest = UINT64_MAX;
        for (unsigned int i = 0; i < ARRAY_SIZE(presentation->retained); i++) {
            if (presentation->retained[i].srv &&
                presentation->retained[i].serial < oldest) {
                oldest = presentation->retained[i].serial;
                slot = i;
            }
        }
    }
    return true;
}

bool pgraph_d3d11_retain_color_scanout(NV2AState *d, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *color = &r->render_targets.color;
    if (!r->presentation || !color->srv) {
        return true;
    }
    return d3d11_retain_scanout(r, color, errp);
}

static PGRAPHD3D11CachedScanout *d3d11_find_retained_scanout(NV2AState *d)
{
    PGRAPHD3D11PresentationState *presentation =
        d->pgraph.d3d11_renderer_state->presentation;
    PGRAPHD3D11CachedScanout *best = NULL;
    uint64_t row_probe =
        d->pcrtc.start + (uint64_t)presentation->scanout_line_offset;
    for (unsigned int i = 0; i < ARRAY_SIZE(presentation->retained); i++) {
        PGRAPHD3D11CachedScanout *entry = &presentation->retained[i];
        if (!entry->srv) {
            continue;
        }
        if (memory_region_test_and_clear_dirty(
                d->vram, entry->address, entry->length, DIRTY_MEMORY_NV2A)) {
            ID3D11ShaderResourceView_Release(entry->srv);
            ID3D11Texture2D_Release(entry->texture);
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        bool contains_start = d->pcrtc.start >= entry->address &&
                              d->pcrtc.start - entry->address < entry->length;
        bool contains_row = row_probe >= entry->address &&
                            row_probe - entry->address < entry->length;
        if ((contains_start || contains_row) &&
            (!best || entry->serial > best->serial)) {
            best = entry;
        }
    }
    return best;
}

static bool d3d11_prepare_raw_scanout(NV2AState *d,
                                      ID3D11ShaderResourceView **view,
                                      uint32_t *width, uint32_t *height,
                                      Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    PGRAPHD3D11PresentationState *presentation = r->presentation;
    VGADisplayParams params;
    int display_width, display_height;
    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    d->vga.get_params(&d->vga, &params);
    int bpp = d->vga.get_bpp(&d->vga);
    if (display_width <= 0 || display_height <= 0 ||
        (bpp != 15 && bpp != 16 && bpp != 32)) {
        return false;
    }

    uint32_t bytes_per_pixel = bpp == 32 ? 4 : 2;
    uint32_t source_pitch = params.line_offset ?
                                params.line_offset :
                                display_width * bytes_per_pixel;
    uint64_t source_length = (uint64_t)(display_height - 1) * source_pitch +
                             (uint64_t)display_width * bytes_per_pixel;
    uint64_t vram_size = memory_region_size(d->vram);
    if (d->pcrtc.start > vram_size ||
        source_length > vram_size - d->pcrtc.start) {
        error_setg(errp,
                   "D3D11: PCRTC scanout exceeds VRAM "
                   "(start=0x%" HWADDR_PRIx " pitch=%u size=%dx%d bpp=%d)",
                   d->pcrtc.start, source_pitch, display_width, display_height,
                   bpp);
        return false;
    }

    if (!presentation->scanout_texture ||
        presentation->scanout_width != (uint32_t)display_width ||
        presentation->scanout_height != (uint32_t)display_height) {
        if (presentation->scanout_srv) {
            ID3D11ShaderResourceView_Release(presentation->scanout_srv);
            presentation->scanout_srv = NULL;
        }
        if (presentation->scanout_texture) {
            ID3D11Texture2D_Release(presentation->scanout_texture);
            presentation->scanout_texture = NULL;
        }
        D3D11_TEXTURE2D_DESC desc = {
            .Width = display_width,
            .Height = display_height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };
        HRESULT hr = ID3D11Device_CreateTexture2D(
            r->device, &desc, NULL, &presentation->scanout_texture);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateShaderResourceView(
                r->device, (ID3D11Resource *)presentation->scanout_texture,
                NULL, &presentation->scanout_srv);
        }
        if (FAILED(hr)) {
            error_setg(errp,
                       "D3D11: failed to create PCRTC scanout texture "
                       "(HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
        presentation->scanout_width = display_width;
        presentation->scanout_height = display_height;
    }

    const uint8_t *source = d->vram_ptr + d->pcrtc.start;
    uint32_t destination_pitch = display_width * 4;
    uint8_t *pixels = g_malloc((size_t)destination_pitch * display_height);
    for (int y = 0; y < display_height; y++) {
        const uint8_t *src = source + (size_t)y * source_pitch;
        uint8_t *dst = pixels + (size_t)y * destination_pitch;
        if (bpp == 32) {
            memcpy(dst, src, destination_pitch);
            continue;
        }
        for (int x = 0; x < display_width; x++) {
            uint16_t pixel;
            memcpy(&pixel, src + x * 2, sizeof(pixel));
            uint8_t red, green, blue;
            if (bpp == 16) {
                red = ((pixel >> 11) & 0x1f) * 255 / 31;
                green = ((pixel >> 5) & 0x3f) * 255 / 63;
                blue = (pixel & 0x1f) * 255 / 31;
            } else {
                red = ((pixel >> 10) & 0x1f) * 255 / 31;
                green = ((pixel >> 5) & 0x1f) * 255 / 31;
                blue = (pixel & 0x1f) * 255 / 31;
            }
            dst[x * 4] = blue;
            dst[x * 4 + 1] = green;
            dst[x * 4 + 2] = red;
            dst[x * 4 + 3] = 0xff;
        }
    }
    ID3D11DeviceContext_UpdateSubresource(
        r->context, (ID3D11Resource *)presentation->scanout_texture, 0, NULL,
        pixels, destination_pitch, 0);
    g_free(pixels);
    *view = presentation->scanout_srv;
    *width = display_width;
    *height = display_height;
    return true;
}

static double d3d11_display_aspect(const PGRAPHD3D11RenderTarget *color)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (double)color->width / color->height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0 / 9.0;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0 / 3.0;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        return xemu_get_widescreen() ? 16.0 / 9.0 : 4.0 / 3.0;
    }
}

static D3D11_VIEWPORT
d3d11_presentation_viewport(const PGRAPHD3D11State *r,
                            const PGRAPHD3D11RenderTarget *color)
{
    double width = r->output_width;
    double height = r->output_height;
    if (g_config.display.ui.fit != CONFIG_DISPLAY_UI_FIT_STRETCH) {
        double aspect = d3d11_display_aspect(color);
        if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
            height = color->height;
            width = height * aspect;
        } else if (width / height > aspect) {
            width = height * aspect;
        } else {
            height = width / aspect;
        }
    }
    return (D3D11_VIEWPORT){
        .TopLeftX = (float)((r->output_width - width) * 0.5),
        .TopLeftY = (float)((r->output_height - height) * 0.5),
        .Width = (float)MAX(width, 1.0),
        .Height = (float)MAX(height, 1.0),
        .MinDepth = 0,
        .MaxDepth = 1,
    };
}

static bool d3d11_present_color(NV2AState *d, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *color = &r->render_targets.color;
    if (!r->presentation || !r->backbuffer_rtv) {
        error_setg(errp, "D3D11: presentation resources are unavailable");
        return false;
    }

    const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(r->context, 1, &r->backbuffer_rtv,
                                           NULL);
    ID3D11DeviceContext_OMSetBlendState(r->context, NULL, NULL, UINT_MAX);
    ID3D11DeviceContext_OMSetDepthStencilState(r->context, NULL, 0);
    ID3D11DeviceContext_RSSetState(r->context, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(r->context, r->backbuffer_rtv,
                                              black);
    VGADisplayParams params;
    d->vga.get_params(&d->vga, &params);
    hwaddr scanout_address = d->pcrtc.start;
    uint64_t row_probe = scanout_address + (uint64_t)params.line_offset;
    r->presentation->scanout_line_offset = params.line_offset;
    bool color_contains_scanout =
        color->srv &&
        ((scanout_address >= color->vram_address &&
          scanout_address - color->vram_address < color->storage_length) ||
         (row_probe >= color->vram_address &&
          row_probe - color->vram_address < color->storage_length));
    ID3D11ShaderResourceView *source_view =
        color_contains_scanout ? color->srv : NULL;
    uint32_t source_width = color->width;
    uint32_t source_height = color->height;
    PGRAPHD3D11PresentationState *presentation = r->presentation;
    bool retained_scanout = false;
    bool raw_scanout = false;
    if (color_contains_scanout && !d3d11_retain_scanout(r, color, errp)) {
        return false;
    }
    if (!source_view) {
        PGRAPHD3D11CachedScanout *retained = d3d11_find_retained_scanout(d);
        if (retained) {
            source_view = retained->srv;
            source_width = retained->width;
            source_height = retained->height;
            retained_scanout = true;
        }
    }
    if (!source_view) {
        raw_scanout = d3d11_prepare_raw_scanout(d, &source_view, &source_width,
                                                &source_height, errp);
    }
    if (!source_view && errp && *errp) {
        return false;
    }
    if (!source_view && color->srv && color->width && color->height) {
        /*
         * PCRTC programming can lag the PGRAPH surface transition by a flip.
         * Keep presenting the current color target during that interval.  The
         * exact PCRTC mapping or raw VRAM scanout takes precedence as soon as
         * either becomes usable.
         */
        source_view = color->srv;
        source_width = color->width;
        source_height = color->height;
    }
    if (!source_view) {
        /* The guest has not programmed a display mode or color surface yet. */
        xemu_hud_d3d11_render();
        return true;
    }

    unsigned int source_kind = color_contains_scanout ? 1 :
                               retained_scanout       ? 2 :
                               raw_scanout            ? 3 :
                                                        4;
    if (!r->logged_present_with_color ||
        (presentation->logged_source_kind != source_kind &&
         presentation->source_logs < 8)) {
        char *message = g_strdup_printf(
            "D3D11: presenting %s scanout size=%ux%u pcrtc=0x%" HWADDR_PRIx
            " line-offset=%u",
            color_contains_scanout ?
                "render-target" :
                (retained_scanout ?
                     "retained render-target" :
                     (raw_scanout ? "raw PCRTC" :
                                    "current render-target fallback")),
            source_width, source_height, d->pcrtc.start, params.line_offset);
        qemu_host_emit_log(QEMU_HOST_LOG_INFO, message);
        g_free(message);
        r->logged_present_with_color = true;
        presentation->logged_source_kind = source_kind;
        presentation->source_logs++;
    }

    PGRAPHD3D11RenderTarget scanout = *color;
    scanout.width = source_width;
    scanout.height = source_height;
    D3D11_VIEWPORT viewport = d3d11_presentation_viewport(r, &scanout);
    ID3D11SamplerState *sampler =
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_LINEAR ?
            presentation->linear_sampler :
            presentation->nearest_sampler;
    ID3D11DeviceContext_RSSetViewports(r->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(r->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        r->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(r->context, presentation->vertex_shader,
                                    NULL, 0);
    ID3D11DeviceContext_PSSetShader(r->context, presentation->pixel_shader,
                                    NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(r->context, 0, 1, &sampler);
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0, 1, &source_view);
    ID3D11DeviceContext_Draw(r->context, 3, 0);

    ID3D11ShaderResourceView *null_view = NULL;
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0, 1, &null_view);
    if (xemu_hud_screenshot_pending()) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(r->backbuffer, &desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D *staging = NULL;
        HRESULT hr =
            ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &staging);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_CopyResource(r->context,
                                             (ID3D11Resource *)staging,
                                             (ID3D11Resource *)r->backbuffer);
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = ID3D11DeviceContext_Map(r->context, (ID3D11Resource *)staging,
                                         0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                xemu_hud_d3d12_capture_frame(mapped.pData, desc.Width,
                                             desc.Height, mapped.RowPitch);
                ID3D11DeviceContext_Unmap(r->context, (ID3D11Resource *)staging,
                                          0);
            }
            ID3D11Texture2D_Release(staging);
        }
        if (FAILED(hr)) {
            error_setg(errp,
                       "D3D11: screenshot readback failed (HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
    }
    xemu_hud_d3d11_render();
    return true;
}

bool pgraph_d3d11_present_color(NV2AState *d, Error **errp)
{
    ID3D11DeviceContext *context = d->pgraph.d3d11_renderer_state->context;
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *depth;
    ID3D11BlendState *blend;
    FLOAT blend_factor[4];
    UINT sample_mask, stencil_ref;
    ID3D11DepthStencilState *depth_state;
    ID3D11RasterizerState *raster;
    D3D11_VIEWPORT
    viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT viewport_count = ARRAY_SIZE(viewports);
    ID3D11InputLayout *layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11VertexShader *vertex;
    ID3D11PixelShader *pixel;
    ID3D11ShaderResourceView *views[NV2A_MAX_TEXTURES];
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES];
    ID3D11DeviceContext_OMGetRenderTargets(context, ARRAY_SIZE(targets),
                                           targets, &depth);
    ID3D11DeviceContext_OMGetBlendState(context, &blend, blend_factor,
                                        &sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(context, &depth_state,
                                               &stencil_ref);
    ID3D11DeviceContext_RSGetState(context, &raster);
    ID3D11DeviceContext_RSGetViewports(context, &viewport_count, viewports);
    ID3D11DeviceContext_IAGetInputLayout(context, &layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology);
    ID3D11DeviceContext_VSGetShader(context, &vertex, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(context, &pixel, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(context, 0, ARRAY_SIZE(views),
                                             views);
    ID3D11DeviceContext_PSGetSamplers(context, 0, ARRAY_SIZE(samplers),
                                      samplers);

    bool result = d3d11_present_color(d, errp);

    ID3D11DeviceContext_OMSetRenderTargets(context, ARRAY_SIZE(targets),
                                           targets, depth);
    ID3D11DeviceContext_OMSetBlendState(context, blend, blend_factor,
                                        sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(context, depth_state,
                                               stencil_ref);
    ID3D11DeviceContext_RSSetState(context, raster);
    ID3D11DeviceContext_RSSetViewports(context, viewport_count, viewports);
    ID3D11DeviceContext_IASetInputLayout(context, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, topology);
    ID3D11DeviceContext_VSSetShader(context, vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, ARRAY_SIZE(views),
                                             views);
    ID3D11DeviceContext_PSSetSamplers(context, 0, ARRAY_SIZE(samplers),
                                      samplers);
#define RELEASE_SAVED(type, object) \
    if (object) {                   \
        type##_Release(object);     \
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
        RELEASE_SAVED(ID3D11RenderTargetView, targets[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(views); i++) {
        RELEASE_SAVED(ID3D11ShaderResourceView, views[i]);
        RELEASE_SAVED(ID3D11SamplerState, samplers[i]);
    }
    RELEASE_SAVED(ID3D11DepthStencilView, depth);
    RELEASE_SAVED(ID3D11BlendState, blend);
    RELEASE_SAVED(ID3D11DepthStencilState, depth_state);
    RELEASE_SAVED(ID3D11RasterizerState, raster);
    RELEASE_SAVED(ID3D11InputLayout, layout);
    RELEASE_SAVED(ID3D11VertexShader, vertex);
    RELEASE_SAVED(ID3D11PixelShader, pixel);
#undef RELEASE_SAVED
    return result;
}
