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
#include <math.h>
#include "ui/xemu-settings.h"
#include "ui/xui/xemu-hud.h"
#include "ui/xemu-widescreen.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "presentation.h"

typedef struct D3D11PresentationConstants {
    float viewport[4];
    float display[4];
    float pvideo_pos[4];
    float pvideo_in_scale[4];
    float pvideo_color_key[4];
    float pvideo_control[4];
} D3D11PresentationConstants;

G_STATIC_ASSERT(sizeof(D3D11PresentationConstants) == 96);

static const char d3d11_presentation_shader[] =
    "struct VSOut { float4 position:SV_Position; float2 uv:TEXCOORD0; };"
    "VSOut vs_main(uint id:SV_VertexID) {"
    "  VSOut o;"
    "  if (id == 0) { o.position=float4(-1,-1,0,1); o.uv=float2(0,1); }"
    "  else if (id == 1) { o.position=float4(-1,3,0,1); o.uv=float2(0,-1); }"
    "  else { o.position=float4(3,-1,0,1); o.uv=float2(2,1); }"
    "  return o;"
    "}"
    "cbuffer PresentationConstants:register(b0){"
    "float4 viewportRect;float4 displayInfo;float4 pvideoPos;"
    "float4 pvideoInScale;float4 pvideoColorKey;float4 pvideoControl;};"
    "Texture2D source_texture:register(t0);"
    "Texture2D pvideo_texture:register(t1);"
    "SamplerState source_sampler:register(s0);"
    "SamplerState pvideo_sampler:register(s1);"
    "float4 ps_main(VSOut input):SV_Target {"
    "  float2 sourceUV=input.uv;"
    "  sourceUV.y*=displayInfo.z;"
    "  float4 color=source_texture.Sample(source_sampler,sourceUV);"
    "  if(pvideoControl.x>0.5){"
    "    float2 displayCoord=(input.position.xy-viewportRect.xy)*"
    "                        displayInfo.xy/viewportRect.zw;"
    "    float2 screenCoord=float2(displayCoord.x-0.5,"
    "                        displayInfo.y-displayCoord.y-0.5);"
    "    bool inside=all(screenCoord>=pvideoPos.xy)&&"
    "                all(screenCoord<=pvideoPos.xy+pvideoPos.zw);"
    "    bool colorKeyMatch=all(color.rgb==pvideoColorKey.rgb);"
    "    if(inside&&(pvideoControl.y<0.5||colorKeyMatch)){"
    "      float2 outXY=(screenCoord-pvideoPos.xy)*displayInfo.w;"
    "      float2 inSt=(pvideoInScale.xy+outXY*pvideoInScale.zw)/"
    "                  pvideoControl.zw;"
    "      inSt.y=1.0-inSt.y;"
    "      color=pvideo_texture.Sample(pvideo_sampler,inSt);"
    "    }"
    "  }"
    "  return color;"
    "}";

static bool d3d11_compile_presentation_shader(const char *entry,
                                              const char *profile,
                                              ID3DBlob **blob, Error **errp)
{
    ID3DBlob *messages = NULL;
    HRESULT hr =
        D3DCompile2(d3d11_presentation_shader,
                    strlen(d3d11_presentation_shader), "d3d11-presentation",
                    NULL, NULL, entry, profile,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, 0, NULL, 0, blob,
                    &messages);
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
                                 D3D11_TEXTURE_ADDRESS_MODE address,
                                 ID3D11SamplerState **sampler, Error **errp)
{
    D3D11_SAMPLER_DESC desc = {
        .Filter = filter,
        .AddressU = address,
        .AddressV = address,
        .AddressW = address,
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
    D3D11_BUFFER_DESC constants_desc = {
        .ByteWidth = sizeof(D3D11PresentationConstants),
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    hr = ID3D11Device_CreateBuffer(r->device, &constants_desc, NULL,
                                   &presentation->constants);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: presentation constant-buffer creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        goto fail;
    }
    if (!d3d11_create_sampler(r, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                              D3D11_TEXTURE_ADDRESS_CLAMP,
                              &presentation->linear_sampler, errp) ||
        !d3d11_create_sampler(r, D3D11_FILTER_MIN_MAG_MIP_POINT,
                              D3D11_TEXTURE_ADDRESS_CLAMP,
                              &presentation->nearest_sampler, errp) ||
        !d3d11_create_sampler(r, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                              D3D11_TEXTURE_ADDRESS_WRAP,
                              &presentation->pvideo_sampler, errp)) {
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
    if (presentation->pvideo_srv) {
        ID3D11ShaderResourceView_Release(presentation->pvideo_srv);
    }
    if (presentation->pvideo_texture) {
        ID3D11Texture2D_Release(presentation->pvideo_texture);
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
    if (presentation->pvideo_sampler) {
        ID3D11SamplerState_Release(presentation->pvideo_sampler);
    }
    if (presentation->linear_sampler) {
        ID3D11SamplerState_Release(presentation->linear_sampler);
    }
    if (presentation->constants) {
        ID3D11Buffer_Release(presentation->constants);
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
                .pitch = color->pitch,
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

static float d3d11_pvideo_calculate_scale(unsigned int din_dout,
                                           unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);
    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static bool d3d11_prepare_pvideo_overlay(
    NV2AState *d, D3D11PresentationConstants *constants, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    PGRAPHD3D11PresentationState *presentation = r->presentation;
    uint32_t *regs = d->pvideo.regs;
    if (!(regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) ||
        regs[NV_PVIDEO_SIZE_IN] == 0xFFFFFFFF) {
        return true;
    }

    unsigned int input_width =
        GET_MASK(regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    unsigned int input_height =
        GET_MASK(regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);
    unsigned int output_width =
        GET_MASK(regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    unsigned int output_height =
        GET_MASK(regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);
    unsigned int pitch =
        GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    unsigned int format =
        GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);
    if (!input_width || !input_height || !output_width || !output_height) {
        return true;
    }
    if (format != NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8) {
        error_setg(errp, "D3D11: unsupported PVIDEO color format %u", format);
        return false;
    }
    unsigned int in_s =
        GET_MASK(regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_S);
    unsigned int in_t =
        GET_MASK(regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_T);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    unsigned int ds_dx = regs[NV_PVIDEO_DS_DX];
    unsigned int dt_dy = regs[NV_PVIDEO_DT_DY];
    if (ds_dx != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_x = d3d11_pvideo_calculate_scale(ds_dx, output_width);
    }
    if (dt_dy != NV_PVIDEO_DIN_DOUT_UNITY) {
        scale_y = d3d11_pvideo_calculate_scale(dt_dy, output_height);
    }
    if (input_width > output_width) {
        input_width = floorf((float)output_width * scale_x + 0.5f);
    }
    if (input_height > output_height) {
        input_height = floorf((float)output_height * scale_y + 0.5f);
    }
    if (!input_width || !input_height) {
        return true;
    }
    uint64_t required_pitch = ((uint64_t)input_width + 1) / 2 * 4;
    uint64_t bytes = (uint64_t)pitch * input_height;
    hwaddr base = regs[NV_PVIDEO_BASE];
    hwaddr offset = regs[NV_PVIDEO_OFFSET];
    hwaddr limit = regs[NV_PVIDEO_LIMIT];
    uint64_t vram_size = memory_region_size(d->vram);
    if (pitch < required_pitch || offset > limit || bytes > limit - offset ||
        base > UINT64_MAX - offset || base + offset > vram_size ||
        bytes > vram_size - (base + offset)) {
        error_setg(errp,
                   "D3D11: PVIDEO buffer is invalid or exceeds VRAM "
                   "(base=0x%" HWADDR_PRIx " offset=%" HWADDR_PRIx
                   " pitch=%u size=%ux%u)",
                   base, offset, pitch, input_width, input_height);
        return false;
    }

    if (!presentation->pvideo_texture ||
        presentation->pvideo_width != input_width ||
        presentation->pvideo_height != input_height) {
        ID3D11Texture2D *texture = NULL;
        ID3D11ShaderResourceView *view = NULL;
        D3D11_TEXTURE2D_DESC desc = {
            .Width = input_width,
            .Height = input_height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };
        HRESULT hr = ID3D11Device_CreateTexture2D(r->device, &desc, NULL,
                                                  &texture);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateShaderResourceView(
                r->device, (ID3D11Resource *)texture, NULL, &view);
        }
        if (FAILED(hr)) {
            if (view) {
                ID3D11ShaderResourceView_Release(view);
            }
            if (texture) {
                ID3D11Texture2D_Release(texture);
            }
            error_setg(errp,
                       "D3D11: PVIDEO texture creation failed "
                       "(HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
        if (presentation->pvideo_srv) {
            ID3D11ShaderResourceView_Release(presentation->pvideo_srv);
        }
        if (presentation->pvideo_texture) {
            ID3D11Texture2D_Release(presentation->pvideo_texture);
        }
        presentation->pvideo_texture = texture;
        presentation->pvideo_srv = view;
        presentation->pvideo_width = input_width;
        presentation->pvideo_height = input_height;
    }

    uint32_t destination_pitch = input_width * 4;
    uint8_t *pixels = g_malloc((size_t)destination_pitch * input_height);
    const uint8_t *source = d->vram_ptr + base + offset;
    for (unsigned int y = 0; y < input_height; y++) {
        const uint8_t *line = source + (size_t)y * pitch;
        for (unsigned int x = 0; x < input_width; x++) {
            uint8_t *pixel = pixels + ((size_t)y * input_width + x) * 4;
            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
    ID3D11DeviceContext_UpdateSubresource(
        r->context, (ID3D11Resource *)presentation->pvideo_texture, 0, NULL,
        pixels, destination_pitch, 0);
    g_free(pixels);

    unsigned int out_x =
        GET_MASK(regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    unsigned int out_y =
        GET_MASK(regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);
    pgraph_apply_scaling_factor(&d->pgraph, &out_x, &out_y);
    pgraph_apply_scaling_factor(&d->pgraph, &output_width, &output_height);
    out_y = MAX((int)constants->display[1] - 1 -
                    (int)(out_y + output_height),
                0);

    constants->pvideo_pos[0] = out_x;
    constants->pvideo_pos[1] = out_y;
    constants->pvideo_pos[2] = output_width;
    constants->pvideo_pos[3] = output_height;
    constants->pvideo_in_scale[0] = in_s / 16.0f;
    constants->pvideo_in_scale[1] = in_t / 8.0f;
    constants->pvideo_in_scale[2] = scale_x;
    constants->pvideo_in_scale[3] = scale_y;
    uint32_t color_key = regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;
    constants->pvideo_color_key[0] =
        GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0f;
    constants->pvideo_color_key[1] =
        GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0f;
    constants->pvideo_color_key[2] =
        GET_MASK(color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0f;
    constants->pvideo_control[0] = 1.0f;
    constants->pvideo_control[1] =
        GET_MASK(regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);
    constants->pvideo_control[2] = presentation->pvideo_width;
    constants->pvideo_control[3] = presentation->pvideo_height;
    return true;
}

static float d3d11_scanout_texcoord_scale_y(uint32_t display_height,
                                            uint32_t source_height,
                                            uint32_t surface_pitch,
                                            uint32_t display_pitch)
{
    if (!source_height) {
        return 1.0f;
    }
    uint32_t line_offset = display_pitch ? surface_pitch / display_pitch : 1;
    line_offset = MAX(line_offset, 1u);
    return (float)display_height / (float)source_height / (float)line_offset;
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
    uint32_t source_pitch = 0;
    if (color_contains_scanout && !d3d11_retain_scanout(r, color, errp)) {
        return false;
    }
    if (color_contains_scanout) {
        source_pitch = color->pitch;
    }
    if (!source_view) {
        PGRAPHD3D11CachedScanout *retained = d3d11_find_retained_scanout(d);
        if (retained) {
            source_view = retained->srv;
            source_width = retained->width;
            source_height = retained->height;
            source_pitch = retained->pitch;
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
        source_pitch = color->pitch;
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
    D3D11PresentationConstants constants = { 0 };
    constants.viewport[0] = viewport.TopLeftX;
    constants.viewport[1] = viewport.TopLeftY;
    constants.viewport[2] = viewport.Width;
    constants.viewport[3] = viewport.Height;
    int display_width, display_height;
    d->vga.get_resolution(&d->vga, &display_width, &display_height);
    if (display_width <= 0 || display_height <= 0) {
        display_width = source_width;
        display_height = source_height;
    }
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        display_height *= 2;
    }
    unsigned int scaled_display_width = display_width;
    unsigned int scaled_display_height = display_height;
    pgraph_apply_scaling_factor(&d->pgraph, &scaled_display_width,
                                &scaled_display_height);
    constants.display[0] = scaled_display_width;
    constants.display[1] = scaled_display_height;
    constants.display[2] =
        source_pitch ? d3d11_scanout_texcoord_scale_y(
                           scaled_display_height, source_height, source_pitch,
                           params.line_offset) :
                       1.0f;
    constants.display[3] = d->pgraph.surface_scale_factor ?
                               1.0f / d->pgraph.surface_scale_factor :
                               1.0f;
    if (!d3d11_prepare_pvideo_overlay(d, &constants, errp)) {
        return false;
    }
    ID3D11DeviceContext_UpdateSubresource(
        r->context, (ID3D11Resource *)presentation->constants, 0, NULL,
        &constants, 0, 0);
    ID3D11SamplerState *sampler =
        g_config.display.filtering == CONFIG_DISPLAY_FILTERING_LINEAR ?
            presentation->linear_sampler :
            presentation->nearest_sampler;
    ID3D11ShaderResourceView *presentation_views[2] = {
        source_view, presentation->pvideo_srv,
    };
    ID3D11SamplerState *presentation_samplers[2] = {
        sampler, presentation->pvideo_sampler,
    };
    ID3D11DeviceContext_RSSetViewports(r->context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(r->context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        r->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(r->context, presentation->vertex_shader,
                                   NULL, 0);
    ID3D11DeviceContext_GSSetShader(r->context, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(r->context, presentation->pixel_shader,
                                    NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(r->context, 0, 1,
                                             &presentation->constants);
    ID3D11DeviceContext_PSSetSamplers(r->context, 0,
                                      ARRAY_SIZE(presentation_samplers),
                                      presentation_samplers);
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0,
                                             ARRAY_SIZE(presentation_views),
                                             presentation_views);
    ID3D11DeviceContext_Draw(r->context, 3, 0);

    ID3D11ShaderResourceView *null_views[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0,
                                             ARRAY_SIZE(null_views), null_views);
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
    ID3D11Buffer *constant_buffers[1];
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
    ID3D11DeviceContext_PSGetConstantBuffers(context, 0,
                                             ARRAY_SIZE(constant_buffers),
                                             constant_buffers);

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
    ID3D11DeviceContext_GSSetShader(context, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, ARRAY_SIZE(views),
                                             views);
    ID3D11DeviceContext_PSSetSamplers(context, 0, ARRAY_SIZE(samplers),
                                      samplers);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0,
                                             ARRAY_SIZE(constant_buffers),
                                             constant_buffers);
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
    for (unsigned int i = 0; i < ARRAY_SIZE(constant_buffers); i++) {
        RELEASE_SAVED(ID3D11Buffer, constant_buffers[i]);
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
