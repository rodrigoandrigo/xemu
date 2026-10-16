#!/usr/bin/env python3
"""Compile and run the D3D11 CPU texture converter without starting xemu.

Run in an MSYS2 UCRT64 shell: python tests/unit/test-d3d11-textures.py
Use --overlap-only for the portable GCC VRAM-range regression on other hosts.
Tests actual functions extracted from textures.c, including native D3D11 WARP
resource creation, GPU readback, and indexed-palette target-alias rebinding. No
UWP application is installed or started.
The native S3TC decoder is exercised for bordered cube mip chains; Y16
luminance and depth conversions are also covered.
"""

import pathlib
import shlex
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
PGRAPH = ROOT / "hw/xbox/nv2a/pgraph"

PREAMBLE = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <glib.h>
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d11.h>
#include <d3dcompiler.h>
#define NV2A_MAX_TEXTURES 4
#define NV2A_CUBEMAP_FACE_ALIGNMENT 128
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef uint64_t hwaddr;
#define ctz32(value) __builtin_ctz(value)
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#define ROUND_UP(n, d) (((n) + (d) - 1) / (d) * (d))
"""

SUPPORT = r"""
static const BasicColorFormatInfo kelvin_color_format_info_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = {1, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = {4, true, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] = {2, true, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] = {4, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] = {2, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] = {2, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] = {4, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] = {4, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] = {4, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] = {2, false, true},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] = {4, true, true},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT] = {4, true, true},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] = {2, true, true},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] = {2, true, true},
};
static uint8_t *pgraph_convert_texture_data(TextureShape s, const uint8_t *data,
    const uint8_t *palette, unsigned int w, unsigned int h, unsigned int d,
    unsigned int row, unsigned int slice, size_t *size)
{
    (void)s; (void)data; (void)palette; (void)w; (void)h; (void)d;
    (void)row; (void)slice; (void)size;
    if (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        size_t bytes = (size_t)w * h * d * 4;
        uint8_t *converted = g_malloc(bytes);
        for (unsigned int z = 0; z < d; z++) {
            for (unsigned int y = 0; y < h; y++) {
                for (unsigned int x = 0; x < w; x++) {
                    uint8_t index = data[z * slice + y * row + x];
                    memcpy(converted + ((size_t)z * h * w + y * w + x) * 4,
                           palette + index * 4, 4);
                }
            }
        }
        if (size) {
            *size = bytes;
        }
        return converted;
    }
    if (s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
        static const int8_t signed_rgb[] = {
            -128, -1, 0, 1, 126, 127,
        };
        size_t bytes = (size_t)w * h * d * 3;
        uint8_t *converted = g_malloc(bytes);
        for (size_t i = 0; i < bytes; i++) {
            converted[i] = (uint8_t)signed_rgb[i % sizeof(signed_rgb)];
        }
        if (size) {
            *size = bytes;
        }
        return converted;
    }
    return NULL;
}
"""

DEVICE_SUPPORT = r"""
static unsigned int inject_texture_failures;
static unsigned int transfer_warning_count;
static HRESULT fixture_create_texture(ID3D11Device *device,
    const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
    ID3D11Texture2D **texture)
{
    if (inject_texture_failures) {
        inject_texture_failures--;
        *texture = NULL;
        return E_OUTOFMEMORY;
    }
    return ID3D11Device_CreateTexture2D(device, desc, data, texture);
}
#define ID3D11Device_CreateTexture2D fixture_create_texture
typedef struct PGRAPHD3D11RenderTarget {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    hwaddr vram_address;
    size_t storage_length;
    unsigned int guest_width, guest_height, pitch, surface_type;
    unsigned int width, height;
    bool z_format;
    unsigned int nv2a_format;
    DXGI_FORMAT format;
} PGRAPHD3D11RenderTarget;
typedef struct PGRAPHD3D11State {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    PGRAPHD3D11TextureState *textures;
    struct {
        PGRAPHD3D11RenderTarget color, depth_stencil;
    } render_targets;
} PGRAPHD3D11State;
typedef struct Surface {
    bool draw_dirty;
    bool buffer_dirty;
} Surface;
static void pgraph_d3d11_bind_render_targets(PGRAPHD3D11State *r)
{
    ID3D11DepthStencilView *dsv = r->render_targets.depth_stencil.dsv;
    if (dsv && r->render_targets.color.texture) {
        D3D11_TEXTURE2D_DESC color, depth;
        ID3D11Texture2D_GetDesc(r->render_targets.color.texture, &color);
        ID3D11Texture2D_GetDesc(r->render_targets.depth_stencil.texture, &depth);
        if (color.Width != depth.Width || color.Height != depth.Height) {
            dsv = NULL; /* This fixture deliberately owns different-sized mips. */
        }
    }
    ID3D11DeviceContext_OMSetRenderTargets(r->context, 1,
        &r->render_targets.color.rtv, dsv);
}
typedef struct PGRAPHState {
    PGRAPHD3D11State *d3d11_renderer_state;
    Surface surface_color;
    Surface surface_zeta;
    bool texture_enabled[NV2A_MAX_TEXTURES];
    bool texture_dirty[NV2A_MAX_TEXTURES];
    hwaddr dma_a;
    hwaddr dma_b;
    bool texture_dma_b;
    bool palette_dma_b;
    unsigned int palette_length_index;
    TextureShape texture_shape;
    hwaddr texture_address;
    hwaddr palette_address;
} PGRAPHState;
typedef struct NV2AState {
    PGRAPHState pgraph;
    uint8_t *vram_ptr;
    void *vram;
    size_t vram_size;
} NV2AState;
typedef int Error;
static bool expect_texture_error;
static char texture_error_message[256];
static size_t fixture_vram_size;
static void error_setg(Error **errp, const char *format, ...)
{
    (void)errp;
    va_list args;
    va_start(args, format);
    if (expect_texture_error) {
        vsnprintf(texture_error_message, sizeof(texture_error_message), format,
                  args);
        va_end(args);
        return;
    }
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}
static size_t memory_region_size(void *region)
{
    (void)region;
    return fixture_vram_size;
}
#define QEMU_HOST_LOG_WARNING 2
static const char *error_get_pretty(Error *error) { (void)error; return "mock"; }
static void error_free(Error *error) { (void)error; }
static void qemu_host_emit_log(int level, const char *text)
{
    assert(level == QEMU_HOST_LOG_WARNING && text);
    transfer_warning_count++;
}
static uint32_t pgraph_reg_r(PGRAPHState *pg, unsigned int reg)
{
    if (reg == NV_PGRAPH_TEXFMT0) {
        uint32_t format = pg->texture_dma_b ?
                              NV_PGRAPH_TEXFMT0_CONTEXT_DMA : 0;
        format |= (pg->texture_shape.color_format <<
                   ctz32(NV_PGRAPH_TEXFMT0_COLOR)) &
                  NV_PGRAPH_TEXFMT0_COLOR;
        format |= (pg->texture_shape.dimensionality <<
                   ctz32(NV_PGRAPH_TEXFMT0_DIMENSIONALITY)) &
                  NV_PGRAPH_TEXFMT0_DIMENSIONALITY;
        format |= (pg->texture_shape.levels <<
                   ctz32(NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS)) &
                  NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS;
        return format;
    }
    if (reg == NV_PGRAPH_TEXOFFSET0) {
        return pg->texture_address;
    }
    if (reg == NV_PGRAPH_TEXPALETTE0) {
        return (pg->palette_dma_b ? NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA : 0) |
               ((pg->palette_length_index <<
                 ctz32(NV_PGRAPH_TEXPALETTE0_LENGTH)) &
                NV_PGRAPH_TEXPALETTE0_LENGTH) |
               (pg->palette_address & NV_PGRAPH_TEXPALETTE0_OFFSET);
    }
    return 0;
}
static bool nv_dma_map_checked(NV2AState *d, hwaddr object,
                               uint8_t **mapping, hwaddr *length)
{
    if (object != d->pgraph.dma_a && object != d->pgraph.dma_b) {
        *mapping = NULL;
        *length = 0;
        return false;
    }
    *length = d->vram_size;
    *mapping = d->vram_ptr;
    return true;
}
static bool pgraph_is_texture_enabled(PGRAPHState *pg, unsigned int stage)
{
    return pg->texture_enabled[stage];
}
static TextureShape pgraph_get_texture_shape(PGRAPHState *pg, int stage)
{
    (void)stage;
    return pg->texture_shape;
}
static bool d3d11_update_sampler(PGRAPHD3D11State *r, PGRAPHState *pg,
                                 unsigned int stage, Error **errp)
{
    (void)r; (void)pg; (void)stage; (void)errp;
    return true;
}
static unsigned int alias_flush_count;
static unsigned int alias_hash_count;
static bool alias_hash_requires_flush;
static uint64_t fast_hash(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    if (alias_hash_requires_flush) {
        assert(alias_flush_count == 1);
    }
    alias_hash_count++;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}
/* Guest-memory accounting includes BC block padding and cube face alignment. */
static size_t pgraph_get_texture_length(PGRAPHState *pg, TextureShape *s)
{
    (void)pg;
    if (kelvin_color_format_info_map[s->color_format].linear) {
        return (size_t)s->height * s->pitch;
    }
    unsigned int w = s->width;
    unsigned int h = s->dimensionality >= 2 ? s->height : 1;
    unsigned int d = s->dimensionality == 3 ? s->depth : 1;
    if (s->border && !kelvin_color_format_info_map[s->color_format].linear) {
        w = MAX(16, w * 2);
        if (s->dimensionality >= 2) h = MAX(16, h * 2);
        if (s->dimensionality == 3) d = MAX(16, d * 2);
    }
    size_t length = 0;
    for (unsigned int level = 0; level < s->levels; level++) {
        if (s->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ||
            s->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
            s->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
            size_t block = s->color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 : 16;
            length += ROUND_UP(MAX(1, w), 4) / 4 *
                      (ROUND_UP(MAX(1, h), 4) / 4) * MAX(1, d) * block;
        } else {
            length += (size_t)MAX(1, w) * MAX(1, h) * MAX(1, d) *
                      kelvin_color_format_info_map[s->color_format].bytes_per_pixel;
        }
        w /= 2; h /= 2; d /= 2;
    }
    if (s->cubemap) {
        length = (length + 127) & ~(size_t)127;
        length *= 6;
    }
    return length;
}
static bool pgraph_d3d11_surface_flush(NV2AState *nv2a, Error **errp)
{
    (void)errp;
    PGRAPHD3D11State *renderer = nv2a->pgraph.d3d11_renderer_state;
    ID3D11Texture2D *target = renderer->render_targets.color.texture;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(target, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(renderer->device, &desc, NULL,
                                             &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(renderer->context,
                                    (ID3D11Resource *)staging,
                                    (ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(renderer->context, (ID3D11Resource *)staging,
                                 0, D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    size_t row_bytes = (size_t)desc.Width * 4;
    for (unsigned int y = 0; y < desc.Height; y++) {
        memcpy(nv2a->vram_ptr + renderer->render_targets.color.vram_address +
                   y * row_bytes,
               (const uint8_t *)mapped.pData + y * mapped.RowPitch, row_bytes);
    }
    ID3D11DeviceContext_Unmap(renderer->context,
                              (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    nv2a->pgraph.surface_color.draw_dirty = false;
    alias_flush_count++;
    return true;
}
"""

DEVICE_TEST = r"""
static void test_unknown_texture_format_rejected(void)
{
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .textures = &textures };
    NV2AState nv2a = { .pgraph = { .d3d11_renderer_state = &renderer } };
    TextureShape shape = {
        .dimensionality = 2, .width = 1, .height = 1, .levels = 1,
        .color_format = 0x3d,
        .pitch = 4,
    };
    Error *error = NULL;
    expect_texture_error = true;
    bool created = d3d11_create_texture(&nv2a, 0, shape, 0, NULL, &error);
    expect_texture_error = false;
    assert(!created);
    assert(strstr(texture_error_message, "unsupported or depth texture format"));
    g_print("PASS: unknown texture format rejected before upload\n");
}

static void test_depth_texture_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    uint16_t guest[] = {0x0000, 0x8000, 0xffff};
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer },
        .vram_ptr = (uint8_t *)guest,
    };
    TextureShape shape = {
        .dimensionality = 2, .width = 3, .height = 1, .levels = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED,
        .pitch = sizeof(guest),
    };
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t *middle_pixel = (const uint8_t *)mapped.pData + 4;
    assert(middle_pixel[0] == 0x80 && middle_pixel[1] == 0 &&
           middle_pixel[2] == 0 && middle_pixel[3] == 0xff);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP depth16 conversion and readback\n");
}

static void test_r6g5b5_texture_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    uint32_t guest[2] = { 0, 0 };
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer },
        .vram_ptr = (uint8_t *)guest,
    };
    TextureShape shape = {
        .dimensionality = 2, .width = 2, .height = 1, .levels = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5,
        .pitch = sizeof(guest),
    };
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Format == DXGI_FORMAT_R8G8B8A8_SNORM);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t expected[] = {
        0x80, 0xff, 0x00, 0x7f, 0x01, 0x7e, 0x7f, 0x7f,
    };
    assert(memcmp(mapped.pData, expected, sizeof(expected)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP R6G5B5 native-SNORM upload and readback\n");
}

static void test_volume_upload(const uint8_t *swizzled,
                               const uint8_t *expected)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    uint8_t guest[36];
    memcpy(guest, swizzled, 32);
    /* Mip 1 is 1x1x1, not a second 2-slice volume. */
    const uint32_t mip_pixel = 0xffaabbcc;
    memcpy(guest + 32, &mip_pixel, 4);
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer }, .vram_ptr = guest,
    };
    TextureShape shape = {
        .dimensionality = 3, .width = 2, .height = 2, .depth = 2, .levels = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
    };
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture3D *volume = (ID3D11Texture3D *)textures.bindings[0].resource;
    D3D11_TEXTURE3D_DESC desc;
    ID3D11Texture3D_GetDesc(volume, &desc);
    assert(desc.Depth == 2 && desc.MipLevels == 2);
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &view);
    assert(view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D);
    assert(view.Texture3D.MipLevels == 2);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture3D *staging = NULL;
    hr = ID3D11Device_CreateTexture3D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)volume);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    for (unsigned int z = 0; z < 2; z++) {
        for (unsigned int y = 0; y < 2; y++) {
            const uint8_t *row = (const uint8_t *)mapped.pData +
                                 z * mapped.DepthPitch + y * mapped.RowPitch;
            assert(memcmp(row, expected + (z * 2 + y) * 8, 8) == 0);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 1,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t mip_expected[] = {0xaa, 0xbb, 0xcc, 0xff};
    assert(memcmp(mapped.pData, mip_expected, 4) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 1);
    ID3D11Texture3D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP volume, SRV, both mip levels GPU readback\n");
}

static void test_border_volume_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));

    uint32_t base[16 * 16 * 16];
    uint32_t mip[8 * 8 * 8];
    uint8_t guest[sizeof(base) + sizeof(mip)];
    for (unsigned int z = 0; z < 16; z++) {
        for (unsigned int y = 0; y < 16; y++) {
            for (unsigned int x = 0; x < 16; x++) {
                base[(z * 16 + y) * 16 + x] =
                    0xff000000 | (z << 16) | (y << 8) | x;
            }
        }
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(mip); i++) {
        mip[i] = 0xff336699;
    }
    swizzle_box((const uint8_t *)base, 16, 16, 16, guest, 16 * 4,
                16 * 16 * 4, 4);
    swizzle_box((const uint8_t *)mip, 8, 8, 8, guest + sizeof(base), 8 * 4,
                8 * 8 * 4, 4);

    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer }, .vram_ptr = guest,
    };
    TextureShape shape = {
        .dimensionality = 3, .width = 8, .height = 8, .depth = 8, .levels = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
        .border = true,
    };
    assert(pgraph_get_texture_length(&nv2a.pgraph, &shape) == sizeof(guest));
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));

    ID3D11Texture3D *volume = (ID3D11Texture3D *)textures.bindings[0].resource;
    D3D11_TEXTURE3D_DESC desc;
    ID3D11Texture3D_GetDesc(volume, &desc);
    assert(desc.Width == 16 && desc.Height == 16 && desc.Depth == 16 &&
           desc.MipLevels == 2);
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &view);
    assert(view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D &&
           view.Texture3D.MipLevels == 2);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture3D *staging = NULL;
    hr = ID3D11Device_CreateTexture3D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)volume);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t border_texel[] = {0, 0, 0, 0xff};
    const uint8_t interior_texel[] = {4, 4, 4, 0xff};
    const uint8_t *first_slice = mapped.pData;
    const uint8_t *interior = first_slice + 4 * mapped.DepthPitch +
                              4 * mapped.RowPitch + 4 * 4;
    assert(memcmp(first_slice, border_texel, sizeof(border_texel)) == 0);
    assert(memcmp(interior, interior_texel, sizeof(interior_texel)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);

    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 1,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t mip_texel[] = {0x33, 0x66, 0x99, 0xff};
    assert(memcmp(mapped.pData, mip_texel, sizeof(mip_texel)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 1);

    ID3D11Texture3D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP 3D border volume dimensions and mip readback\n");
}

static void test_border_texture_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    uint32_t base[16 * 16], mip[8 * 8];
    for (unsigned int i = 0; i < G_N_ELEMENTS(base); i++) {
        base[i] = 0xff000000 | (i << 16) | (i << 8) | i;
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(mip); i++) {
        mip[i] = 0xff336699;
    }
    uint8_t guest[16 * 16 * 4 + 8 * 8 * 4];
    swizzle_box((const uint8_t *)base, 16, 16, 1, guest, 16 * 4,
                16 * 16 * 4, 4);
    swizzle_box((const uint8_t *)mip, 8, 8, 1, guest + 16 * 16 * 4,
                8 * 4, 8 * 8 * 4, 4);
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer }, .vram_ptr = guest,
    };
    TextureShape shape = {
        .dimensionality = 2, .width = 2, .height = 2, .levels = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
        .border = true,
    };
    assert(pgraph_get_texture_length(&nv2a.pgraph, &shape) == sizeof(guest));
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Width == 16 && desc.Height == 16 && desc.MipLevels == 2);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t first_texel[] = {0, 0, 0, 255};
    assert(memcmp(mapped.pData, first_texel, sizeof(first_texel)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 1,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t mip_texel[] = {0x33, 0x66, 0x99, 0xff};
    assert(memcmp(mapped.pData, mip_texel, sizeof(mip_texel)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 1);
    ID3D11Texture2D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP border-expanded dimensions and mip offsets\n");
}

static void test_border_cubemap_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    uint32_t linear[6][16 * 16];
    uint8_t guest[sizeof(linear)];
    for (unsigned int face = 0; face < 6; face++) {
        for (unsigned int y = 0; y < 16; y++) {
            for (unsigned int x = 0; x < 16; x++) {
                linear[face][y * 16 + x] =
                    0xff000000 | (x << 16) | (y << 8) | face;
            }
        }
        swizzle_box((const uint8_t *)linear[face], 16, 16, 1,
                    guest + face * sizeof(linear[face]), 16 * 4,
                    16 * 16 * 4, 4);
    }
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer }, .vram_ptr = guest,
    };
    TextureShape shape = {
        .cubemap = true, .dimensionality = 2, .width = 8, .height = 8,
        .depth = 1, .levels = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
        .border = true,
    };
    assert(pgraph_get_texture_length(&nv2a.pgraph, &shape) == sizeof(guest));
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Width == 8 && desc.Height == 8 && desc.ArraySize == 6 &&
           desc.MipLevels == 1 &&
           (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)texture);
    for (unsigned int face = 0; face < 6; face++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        UINT subresource = face; /* mip 0 + face * one mip level */
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
                                     subresource, D3D11_MAP_READ, 0, &mapped);
        assert(SUCCEEDED(hr));
        const uint8_t *pixels = mapped.pData;
        const uint8_t first_texel[] = {4, 4, face, 0xff};
        const uint8_t last_texel[] = {11, 11, face, 0xff};
        assert(memcmp(pixels, first_texel, sizeof(first_texel)) == 0);
        assert(memcmp(pixels + 7 * mapped.RowPitch + 7 * 4, last_texel,
                      sizeof(last_texel)) == 0);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging,
                                  subresource);
    }
    ID3D11Texture2D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP cubemap border is cropped to the GL face extent\n");
}

static void test_border_cubemap_mip_upload(unsigned int format)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)));
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = {
        .device = device, .context = context, .textures = &textures,
    };
    /* Vulkan layout: each face contains 16x16, 8x8, 4x4, 2x2, followed
     * by face-alignment padding. Pattern and padding differ on every face.
     */
    enum { levels = 4 };
    bool compressed = format != NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    unsigned int block_bytes = format ==
        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 : 16;
    unsigned int face_size = compressed ? ROUND_UP(22 * block_bytes, 128) : 1408;
    uint8_t guest[6 * 1408];
    memset(guest, 0xde, sizeof(guest));
    for (unsigned int face = 0; face < 6; face++) {
        size_t offset = face * face_size;
        for (unsigned int level = 0, size = 16; level < levels; level++, size >>= 1) {
            if (compressed) {
                unsigned int blocks = ROUND_UP(size, 4) / 4;
                for (unsigned int block = 0; block < blocks * blocks; block++) {
                    uint8_t *encoded = guest + offset + block * block_bytes;
                    memset(encoded, 0, block_bytes);
                    uint16_t color = (face + 1) << 11 | (level + 1) << 5 | 1;
                    memcpy(encoded + (block_bytes == 8 ? 0 : 8), &color, 2);
                    if (format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8) {
                        memset(encoded, ((face + level + 1) % 15) * 17, 8);
                    } else if (format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
                        encoded[0] = 32 + face * 16 + level * 4;
                    }
                }
                offset += blocks * blocks * block_bytes;
                continue;
            }
            uint32_t linear[16 * 16];
            for (unsigned int y = 0; y < size; y++) {
                for (unsigned int x = 0; x < size; x++) {
                    linear[y * size + x] = 0xff000000 | x << 16 | y << 8 |
                                          (face * 16 + level);
                }
            }
            swizzle_rect((const uint8_t *)linear, size, size, guest + offset,
                         size * 4, 4);
            offset += size * size * 4;
        }
    }
    NV2AState nv2a = { .pgraph = { .d3d11_renderer_state = &renderer },
                       .vram_ptr = guest };
    TextureShape shape = {
        .cubemap = true, .dimensionality = 2, .width = 8, .height = 8,
        .depth = 1, .levels = levels,
        .color_format = format,
        .border = true,
    };
    assert(pgraph_get_texture_length(&nv2a.pgraph, &shape) == 6 * face_size);
    assert(get_cubemap_layer_size(&nv2a.pgraph, shape) == face_size);
    for (unsigned int first_mip = 0; first_mip < levels; first_mip++) {
        shape.min_mipmap_level = first_mip;
        assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
        ID3D11Texture2D *texture = (ID3D11Texture2D *)textures.bindings[0].resource;
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(texture, &desc);
        assert(desc.Width == 16 && desc.Height == 16 && desc.ArraySize == 6 &&
               desc.MipLevels == levels &&
               (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE));
        D3D11_SHADER_RESOURCE_VIEW_DESC view;
        ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &view);
        assert(view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE &&
               view.TextureCube.MostDetailedMip == first_mip &&
               view.TextureCube.MipLevels == levels - first_mip);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D *staging = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                        (ID3D11Resource *)texture);
        for (unsigned int face = 0; face < 6; face++) {
            for (unsigned int level = 0, size = 16; level < levels; level++, size >>= 1) {
                unsigned int subresource = face * levels + level;
                D3D11_MAPPED_SUBRESOURCE mapped;
                assert(SUCCEEDED(ID3D11DeviceContext_Map(context,
                    (ID3D11Resource *)staging, subresource, D3D11_MAP_READ, 0, &mapped)));
                for (unsigned int y = 0; y < size; y++) {
                    for (unsigned int x = 0; x < size; x++) {
                        uint8_t expected[] = { x, y, face * 16 + level, 0xff };
                        if (compressed) {
                            expected[0] = (face + 1) * 255 / 31;
                            expected[1] = (level + 1) * 255 / 63;
                            expected[2] = 255 / 31;
                            if (format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8) {
                                expected[3] = ((face + level + 1) % 15) * 17;
                            } else if (format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
                                expected[3] = 32 + face * 16 + level * 4;
                            }
                        }
                        assert(memcmp((const uint8_t *)mapped.pData +
                            y * mapped.RowPitch + x * 4, expected, 4) == 0);
                    }
                }
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, subresource);
            }
        }
        ID3D11Texture2D_Release(staging);
        d3d11_release_texture(&textures.bindings[0]);
    }
    shape.levels = 6; /* a 16x16 stored image cannot contain six levels */
    shape.min_mipmap_level = 0;
    assert(!d3d11_texture_shape_valid(shape));
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: WARP Vulkan-layout cube format 0x%x, six faces/all mips/border texels, alignment and SRV min mip\n", format);
}

static void test_y16_texture_upload(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    uint16_t guest[] = {0x0000, 0x8000, 0xffff};
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer },
        .vram_ptr = (uint8_t *)guest,
    };
    TextureShape shape = {
        .dimensionality = 2, .width = 3, .height = 1, .levels = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16,
        .pitch = sizeof(guest),
    };
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM);
    assert(desc.Width == 3 && desc.Height == 1);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint16_t midpoint[] = {0x8000, 0x8000, 0x8000, 0xffff};
    const uint16_t white[] = {0xffff, 0xffff, 0xffff, 0xffff};
    assert(memcmp((const uint8_t *)mapped.pData + 8, midpoint,
                  sizeof(midpoint)) == 0);
    assert(memcmp((const uint8_t *)mapped.pData + 16, white,
                  sizeof(white)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP Y16 replicated R16_UNORM readback\n");
}

static void test_linear_texture_uses_base_mip(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    const uint32_t guest[] = {
        0xff112233, 0xff445566, 0xdeadbeef,
        0xff778899, 0xffaabbcc, 0xdeadbeef,
    };
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer },
        .vram_ptr = (uint8_t *)guest,
    };
    TextureShape shape = {
        .dimensionality = 2,
        .width = 2,
        .height = 2,
        .levels = 4,
        .min_mipmap_level = 3,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .pitch = 3 * sizeof(uint32_t),
    };
    assert(pgraph_get_texture_length(&nv2a.pgraph, &shape) == sizeof(guest));
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture2D *texture =
        (ID3D11Texture2D *)textures.bindings[0].resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    assert(desc.Width == 2 && desc.Height == 2 && desc.MipLevels == 1);
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &view);
    assert(view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D);
    assert(view.Texture2D.MostDetailedMip == 0);
    assert(view.Texture2D.MipLevels == 1);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP linear texture ignores guest mip chain\n");
}
"""
WARP_BIND_TEST = r"""
static void test_transfer_allocation_fallback(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    uint8_t vram[32] = { 0 };
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State r = { .device = device, .context = context, .textures = &textures };
    NV2AState d = {
        .pgraph = {
            .d3d11_renderer_state = &r, .texture_enabled = { true },
            .surface_color = { .draw_dirty = true }, .dma_a = 1, .dma_b = 2,
            .texture_shape = { .width = 2, .height = 2, .depth = 1,
                .dimensionality = 2, .levels = 2,
                .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 },
        },
        .vram_ptr = vram, .vram = vram, .vram_size = sizeof(vram),
    };
    fixture_vram_size = sizeof(vram);
    PGRAPHD3D11RenderTarget *target = &r.render_targets.color;
    *target = (PGRAPHD3D11RenderTarget) {
        .guest_width = 2, .guest_height = 2, .width = 2, .height = 2,
        .pitch = 8, .storage_length = 16,
        .format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .nv2a_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
        .surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE,
    };
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 2, .Height = 2, .MipLevels = 1, .ArraySize = 1,
        .Format = target->format, .SampleDesc = {1, 0},
        .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target->texture)));
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
        (ID3D11Resource *)target->texture, NULL, &target->rtv)));
    const float color[] = {0.2f, 0.4f, 0.6f, 1};
    ID3D11DeviceContext_ClearRenderTargetView(context, target->rtv, color);
    alias_flush_count = alias_hash_count = transfer_warning_count = 0;
    alias_hash_requires_flush = false;
    inject_texture_failures = 1;
    expect_texture_error = true;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    expect_texture_error = false;
    assert(inject_texture_failures == 0 && transfer_warning_count == 1);
    assert(textures.transfer_disabled && !textures.bindings[0].surface_copy);
    assert(alias_flush_count == 1 && alias_hash_count == 1);
    assert(vram[0] == 153 && vram[1] == 102 && vram[2] == 51 && vram[3] == 255);
    d.pgraph.surface_color.draw_dirty = true;
    alias_flush_count = alias_hash_count = 0;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(alias_flush_count == 1 && alias_hash_count == 1 && transfer_warning_count == 1);
    d3d11_transfer_trim(&textures);
    d.pgraph.texture_shape.levels = 1;
    d.pgraph.surface_color.draw_dirty = true;
    inject_texture_failures = 1;
    expect_texture_error = true;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    expect_texture_error = false;
    assert(inject_texture_failures == 0 && transfer_warning_count == 2);
    assert(textures.transfer_disabled && !textures.bindings[0].surface_copy);
    ID3D11DeviceContext_ClearState(context);
    d3d11_release_texture(&textures.bindings[0]);
    d3d11_transfer_trim(&textures);
    assert(!textures.transfer_disabled);
    ID3D11RenderTargetView_Release(target->rtv);
    ID3D11Texture2D_Release(target->texture);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: GPU transfer allocation failure falls back to CPU, logs once and retries only after trim");
}

static void test_transfer_chains(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    for (unsigned int cube = 0; cube < 2; cube++) {
        for (unsigned int narrow = 0; narrow < 2; narrow++) {
            for (unsigned int floating = 0; floating < 2; floating++) {
                for (unsigned int scale = 1; scale <= 2; scale++) {
                    PGRAPHD3D11TextureState textures = { 0 };
                    PGRAPHD3D11State r = { .device = device, .context = context, .textures = &textures };
                    unsigned int bpp = narrow ? 2 : 4;
                    size_t face_bytes = cube ? 128 : 21 * bpp;
                    unsigned int faces = cube ? 6 : 1;
                    uint8_t vram[1024];
                    memset(vram, 0xcd, sizeof(vram));
                    TextureShape shape = {
                        .cubemap = cube, .dimensionality = 2,
                        .color_format = narrow ? NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5 :
                                                 NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
                        .width = 4, .height = 4, .depth = 1, .levels = 3,
                    };
                    size_t offsets[6][3];
                    for (unsigned int face = 0; face < faces; face++) {
                        size_t offset = face * face_bytes;
                        for (unsigned int mip = 0; mip < 3; mip++) {
                            unsigned int w = 4 >> mip;
                            uint8_t linear[64];
                            for (unsigned int k = 0; k < w * w * bpp; k++) {
                                linear[k] = 7 + face * 19 + mip * 11 + k;
                            }
                            offsets[face][mip] = offset;
                            swizzle_box(linear, w, w, 1, vram + offset, w * bpp,
                                        w * w * bpp, bpp);
                            offset += w * w * bpp;
                        }
                    }
                    NV2AState d = {
                        .pgraph = {
                            .d3d11_renderer_state = &r,
                            .surface_color = { .draw_dirty = true },
                            .surface_zeta = { .draw_dirty = true },
                            .texture_enabled = { true }, .dma_a = 1, .dma_b = 2,
                            .texture_shape = shape,
                        },
                        .vram_ptr = vram, .vram = vram, .vram_size = sizeof(vram),
                    };
                    fixture_vram_size = sizeof(vram);
                    unsigned int color_face = cube ? 3 : 0, color_mip = cube ? 1 : 0;
                    unsigned int depth_face = cube ? 5 : 0, depth_mip = cube ? 0 : 1;
                    PGRAPHD3D11RenderTarget *color = &r.render_targets.color;
                    PGRAPHD3D11RenderTarget *depth = &r.render_targets.depth_stencil;
                    *color = (PGRAPHD3D11RenderTarget) {
                        .vram_address = offsets[color_face][color_mip],
                        .guest_width = 4 >> color_mip, .guest_height = 4 >> color_mip,
                        .surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE,
                        .format = DXGI_FORMAT_B8G8R8A8_UNORM,
                        .nv2a_format = narrow ? NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5 :
                                               NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
                    };
                    color->width = color->guest_width * scale;
                    color->height = color->guest_height * scale;
                    color->pitch = color->guest_width * bpp;
                    color->storage_length = color->guest_width * color->guest_height * bpp;
                    *depth = (PGRAPHD3D11RenderTarget) {
                        .vram_address = offsets[depth_face][depth_mip],
                        .guest_width = 4 >> depth_mip, .guest_height = 4 >> depth_mip,
                        .surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE,
                        .format = narrow ? (floating ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D16_UNORM) :
                                          DXGI_FORMAT_D24_UNORM_S8_UINT,
                        .nv2a_format = narrow ? NV097_SET_SURFACE_FORMAT_ZETA_Z16 :
                                               NV097_SET_SURFACE_FORMAT_ZETA_Z24S8,
                        .z_format = floating,
                    };
                    depth->width = depth->guest_width * scale;
                    depth->height = depth->guest_height * scale;
                    depth->pitch = depth->guest_width * bpp;
                    depth->storage_length = depth->guest_width * depth->guest_height * bpp;
                    uint8_t rendered[8 * 8 * 4];
                    for (unsigned int k = 0; k < color->width * color->height * 4; k++) {
                        rendered[k] = 33 + k;
                    }
                    D3D11_TEXTURE2D_DESC desc = {
                        .Width = color->width, .Height = color->height,
                        .MipLevels = 1, .ArraySize = 1, .Format = color->format,
                        .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
                        .BindFlags = D3D11_BIND_RENDER_TARGET,
                    };
                    D3D11_SUBRESOURCE_DATA data = { .pSysMem = rendered, .SysMemPitch = color->width * 4 };
                    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, &data, &color->texture)));
                    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
                        (ID3D11Resource *)color->texture, NULL, &color->rtv)));
                    desc.Width = depth->width; desc.Height = depth->height;
                    desc.Format = depth->format; desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &depth->texture)));
                    assert(SUCCEEDED(ID3D11Device_CreateDepthStencilView(device,
                        (ID3D11Resource *)depth->texture, NULL, &depth->dsv)));
                    ID3D11DeviceContext_ClearDepthStencilView(context, depth->dsv,
                        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 0xa5);
                    bool handled = true;
                    PGRAPHD3D11RenderTarget original = *color;
                    color->vram_address += bpp;
                    assert(d3d11_transfer_chain(&d, 0, shape, 0, face_bytes * faces, &handled, NULL));
                    assert(!handled);
                    *color = original;
                    color->width++;
                    assert(d3d11_transfer_chain(&d, 0, shape, 0, face_bytes * faces, &handled, NULL));
                    assert(!handled);
                    *color = original;
                    d.pgraph.surface_color.buffer_dirty = true;
                    assert(d3d11_transfer_chain(&d, 0, shape, 0, face_bytes * faces, &handled, NULL));
                    assert(!handled);
                    d.pgraph.surface_color.buffer_dirty = false;
                    /* Color/depth shapes may differ in this fixture; only the copy
                     * pass uses them, not a guest draw requiring matching DSV size. */
                    unsigned int original_width = color->width;
                    uint8_t saved_vram[sizeof(vram)];
                    memcpy(saved_vram, vram, sizeof(vram));
                    ID3D11PixelShader *sentinel = NULL;
                    assert(d3d11_transfer_shaders(&r, color, false,
                        NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8, &sentinel, NULL));
                    ID3D11DeviceContext_PSSetShader(context, sentinel, NULL, 0);
                    D3D11_RASTERIZER_DESC rs_desc = {
                        .FillMode = D3D11_FILL_WIREFRAME,
                        .CullMode = D3D11_CULL_FRONT, .ScissorEnable = true,
                    };
                    ID3D11RasterizerState *rs = NULL;
                    assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &rs_desc, &rs)));
                    ID3D11DeviceContext_RSSetState(context, rs);
                    D3D11_VIEWPORT previous_view = {
                        .TopLeftX = 1, .TopLeftY = 2, .Width = 7, .Height = 9, .MaxDepth = 1,
                    };
                    ID3D11DeviceContext_RSSetViewports(context, 1, &previous_view);
                    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                    D3D11_BLEND_DESC blend_desc = { 0 };
                    ID3D11BlendState *blend = NULL;
                    assert(SUCCEEDED(ID3D11Device_CreateBlendState(device, &blend_desc, &blend)));
                    FLOAT factors[4] = { .1f, .2f, .3f, .4f };
                    ID3D11DeviceContext_OMSetBlendState(context, blend, factors, 0);
                    for (unsigned int min = 0; min < 3; min++) {
                        d.pgraph.texture_shape.min_mipmap_level = min;
                        alias_hash_requires_flush = false;
                        alias_flush_count = alias_hash_count = 0;
                        Error *error = NULL;
                        assert(pgraph_d3d11_bind_textures(&d, &error));
                        assert(textures.bindings[0].surface_copy);
                        assert(alias_flush_count == 0 && alias_hash_count == 0);
                        assert(memcmp(vram, saved_vram, sizeof(vram)) == 0);
                        ID3D11PixelShader *old_ps = NULL;
                        ID3D11DeviceContext_PSGetShader(context, &old_ps, NULL, NULL);
                        assert(old_ps == sentinel);
                        ID3D11PixelShader_Release(old_ps);
                        ID3D11RasterizerState *old_rs = NULL;
                        ID3D11DeviceContext_RSGetState(context, &old_rs);
                        assert(old_rs == rs);
                        ID3D11RasterizerState_Release(old_rs);
                        UINT viewport_count = 1;
                        D3D11_VIEWPORT restored;
                        ID3D11DeviceContext_RSGetViewports(context, &viewport_count, &restored);
                        assert(viewport_count == 1 && memcmp(&restored, &previous_view, sizeof(restored)) == 0);
                        D3D11_PRIMITIVE_TOPOLOGY topology;
                        ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology);
                        assert(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                        ID3D11BlendState *old_blend = NULL;
                        FLOAT old_factors[4]; UINT sample_mask;
                        ID3D11DeviceContext_OMGetBlendState(context, &old_blend, old_factors, &sample_mask);
                        assert(old_blend == blend && sample_mask == 0 && memcmp(factors, old_factors, sizeof(factors)) == 0);
                        ID3D11BlendState_Release(old_blend);
                        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
                        ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &vd);
                        assert((cube ? vd.TextureCube.MostDetailedMip : vd.Texture2D.MostDetailedMip) == min);
                        ID3D11Texture2D *image = (ID3D11Texture2D *)textures.bindings[0].resource;
                        ID3D11Texture2D_GetDesc(image, &desc);
                        assert(desc.MipLevels == 3 && desc.ArraySize == faces && desc.Width == 4 * scale);
                        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
                        ID3D11Texture2D *staging = NULL;
                        assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
                        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                            textures.bindings[0].resource);
                        for (unsigned int face = 0; face < faces; face++) {
                            for (unsigned int mip = 0; mip < 3; mip++) {
                                unsigned int w = 4 >> mip, consumed;
                                uint8_t *cpu = d3d11_convert_level(shape, vram + offsets[face][mip],
                                    NULL, w, w, 1, &consumed);
                                D3D11_MAPPED_SUBRESOURCE mapped;
                                assert(SUCCEEDED(ID3D11DeviceContext_Map(context,
                                    (ID3D11Resource *)staging, face * 3 + mip, D3D11_MAP_READ, 0, &mapped)));
                                for (unsigned int y = 0; y < w * scale; y++) {
                                    for (unsigned int x = 0; x < w * scale; x++) {
                                        uint8_t expected[4];
                                        if (face == color_face && mip == color_mip) {
                                            uint32_t bits = 0;
                                            d3d11_convert_bgra8_to_color(color->nv2a_format,
                                                (uint8_t *)&bits, 4,
                                                rendered + (y * original_width + x) * 4, 4, 1, 1);
                                            d3d11_decode_pixel(shape.color_format, (uint8_t *)&bits, expected);
                                        } else if (face == depth_face && mip == depth_mip) {
                                            uint32_t bits = narrow ?
                                                (floating ? d3d11_float_to_f16(0.5f * f16_max) : 32768) :
                                                ((floating ? d3d11_float_to_f24(8388608.0f / 16777215.0f * f24_max) :
                                                             0x800000u) << 8) | 0xa5;
                                            d3d11_decode_pixel(shape.color_format, (uint8_t *)&bits, expected);
                                        } else {
                                            memcpy(expected, cpu + ((y / scale) * w + x / scale) * 4, 4);
                                        }
                                        const uint8_t *actual = (uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4;
                                        if (memcmp(actual, expected, 4)) {
                                            fprintf(stderr, "chain cube=%u narrow=%u float=%u scale=%u min=%u face=%u mip=%u xy=%u,%u\n",
                                                cube,narrow,floating,scale,min,face,mip,x,y);
                                            assert(!"chain pixel mismatch");
                                        }
                                    }
                                }
                                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, face * 3 + mip);
                                g_free(cpu);
                            }
                        }
                        ID3D11Texture2D_Release(staging);
                    }
                    ID3D11DeviceContext_ClearState(context);
                    ID3D11RasterizerState_Release(rs);
                    ID3D11BlendState_Release(blend);
                    d3d11_release_texture(&textures.bindings[0]);
                    d3d11_transfer_trim(&textures);
                    ID3D11RenderTargetView_Release(color->rtv);
                    ID3D11DepthStencilView_Release(depth->dsv);
                    ID3D11Texture2D_Release(color->texture);
                    ID3D11Texture2D_Release(depth->texture);
                }
            }
        }
    }
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: mixed color/depth mip chains and cubemap faces, fixed/float 16/24-bit, scale 1x/2x, min-mip selection, preserved guest mips, zero CPU flush/hash");
}

static void test_transfer_format_matrix(float depth_value, uint8_t seed)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, &requested, 1, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State r = { .device = device, .context = context, .textures = &textures };
    const unsigned int sources[] = {
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8,
        NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
    };
    unsigned int checked = 0;
    for (unsigned int source = 0; source < ARRAY_SIZE(sources) + 4; source++) {
        bool depth = source >= ARRAY_SIZE(sources);
        PGRAPHD3D11RenderTarget target = { 0 };
        target.nv2a_format = depth ?
            (source & 1 ? NV097_SET_SURFACE_FORMAT_ZETA_Z16 :
                          NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) : sources[source];
        target.z_format = depth && source >= ARRAY_SIZE(sources) + 2;
        target.format = depth ?
            (target.nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ?
                (target.z_format ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D16_UNORM) :
                 DXGI_FORMAT_D24_UNORM_S8_UINT) : DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_TEXTURE2D_DESC source_desc = {
            .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1,
            .Format = target.format, .SampleDesc = {1, 0},
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET,
        };
        const uint8_t color_bytes[] = {seed, seed + 62, seed + 150, seed + 133};
        D3D11_SUBRESOURCE_DATA data = { .pSysMem = color_bytes, .SysMemPitch = 4 };
        assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &source_desc,
            depth ? NULL : &data, &target.texture)));
        if (depth) {
            ID3D11DepthStencilView *dsv = NULL;
            assert(SUCCEEDED(ID3D11Device_CreateDepthStencilView(device,
                (ID3D11Resource *)target.texture, NULL, &dsv)));
            ID3D11DeviceContext_ClearDepthStencilView(context, dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth_value, 0x8e);
            ID3D11DepthStencilView_Release(dsv);
        }
        uint32_t guest_bits = 0;
        if (!depth) {
            d3d11_convert_bgra8_to_color(target.nv2a_format,
                (uint8_t *)&guest_bits, 4, color_bytes, 4, 1, 1);
        } else {
            D3D11_TEXTURE2D_DESC read = source_desc;
            read.Format = target.format == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_TYPELESS :
                          target.format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_TYPELESS :
                                                                 DXGI_FORMAT_R24G8_TYPELESS;
            read.Usage = D3D11_USAGE_STAGING;
            read.BindFlags = 0;
            read.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *staging = NULL;
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &read, NULL, &staging)));
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                (ID3D11Resource *)target.texture);
            D3D11_MAPPED_SUBRESOURCE mapped;
            assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
                0, D3D11_MAP_READ, 0, &mapped)));
            if (target.format == DXGI_FORMAT_D16_UNORM) {
                memcpy(&guest_bits, mapped.pData, 2);
            } else if (target.format == DXGI_FORMAT_D32_FLOAT) {
                float value;
                memcpy(&value, mapped.pData, 4);
                guest_bits = d3d11_float_to_f16(value * f16_max);
            } else {
                uint32_t value;
                memcpy(&value, mapped.pData, 4);
                uint32_t z = value & 0xffffff;
                if (target.z_format) {
                    z = d3d11_float_to_f24(z / 16777215.0f * f24_max);
                }
                guest_bits = (z << 8) | (value >> 24);
            }
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
            ID3D11Texture2D_Release(staging);
        }
        ID3D11ShaderResourceView *views[2] = { 0 };
        assert(d3d11_transfer_views(&r, &target, depth, views, NULL));
        for (unsigned int format = 0; format < 66; format++) {
            char *source_text = d3d11_transfer_pixel_source(target.nv2a_format,
                depth, target.z_format, format);
            if (!source_text) { continue; }
            g_free(source_text);
            Error *error = NULL;
            ID3D11PixelShader *pixel = NULL;
            assert(d3d11_transfer_shaders(&r, &target, depth, format, &pixel, &error));
            assert(pixel);
            D3D11_TEXTURE2D_DESC output_desc = source_desc;
            output_desc.Format = d3d11_texture_dxgi_format(format);
            output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            ID3D11Texture2D *output = NULL, *readback = NULL;
            ID3D11RenderTargetView *rtv = NULL;
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &output_desc, NULL, &output)));
            assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
                (ID3D11Resource *)output, NULL, &rtv)));
            d3d11_transfer_draw(&r, rtv, views, pixel, 1, 1);
            output_desc.Usage = D3D11_USAGE_STAGING;
            output_desc.BindFlags = 0;
            output_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &output_desc, NULL, &readback)));
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback,
                (ID3D11Resource *)output);
            D3D11_MAPPED_SUBRESOURCE mapped;
            assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback,
                0, D3D11_MAP_READ, 0, &mapped)));
            unsigned int consumed;
            TextureShape shape = { .color_format = format, .pitch = 4 };
            uint8_t *expected = d3d11_convert_level(shape, (uint8_t *)&guest_bits,
                NULL, 1, 1, 1, &consumed);
            unsigned int size = d3d11_texture_pixel_bytes(format);
            if (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
                uint32_t v = guest_bits ^ 528;
                int8_t signed_expected[4] = {
                    ((v >> 10) & 63) * 127 / 63,
                    (int)(((v >> 5) & 31) * 255 / 31) - 128,
                    (int)((v & 31) * 255 / 31) - 128, 127,
                };
                for (unsigned int k = 0; k < 4; k++) {
                    assert(MAX(-127, ((const int8_t *)mapped.pData)[k]) == MAX(-127, signed_expected[k]));
                }
            } else if (memcmp(mapped.pData, expected, size)) {
                fprintf(stderr, "transfer source=%u depth=%u float=%u dest=%u bits=%08x actual=",
                    target.nv2a_format, depth, target.z_format, format, guest_bits);
                for (unsigned int k = 0; k < size; k++) {
                    fprintf(stderr, "%02x/%02x ", ((const uint8_t *)mapped.pData)[k], expected[k]);
                }
                fprintf(stderr, "\n");
                assert(!"GPU format transfer differs from CPU conversion");
            }
            g_free(expected);
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
            ID3D11RenderTargetView_Release(rtv);
            ID3D11Texture2D_Release(output);
            ID3D11Texture2D_Release(readback);
            checked++;
        }
        for (unsigned int k = 0; k < 2; k++) {
            if (views[k]) { ID3D11ShaderResourceView_Release(views[k]); }
        }
        ID3D11Texture2D_Release(target.texture);
    }
    assert(g_hash_table_size(textures.transfer_pixels) <= 128);
    d3d11_transfer_trim(&textures);
    assert(!textures.transfer_pixels && !textures.transfer_vertex && !textures.transfer_raster);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    printf("PASS: %u color/depth/float format transfer shaders and CPU-equivalent pixel readbacks on WARP FL10.0\n", checked);
}

static void assert_texture_first_pixel(ID3D11Device *device,
                                      ID3D11DeviceContext *context,
                                      ID3D11Resource *resource,
                                      const uint8_t expected[4])
{
    ID3D11Texture2D *texture = (ID3D11Texture2D *)resource;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    resource);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    assert(memcmp(mapped.pData, expected, 4) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}

static void test_surface_gpu_snapshot(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION,
        &device, NULL, &context)));
    for (unsigned int swizzled = 0; swizzled < 2; swizzled++) {
        for (unsigned int scale = 1; scale <= 2; scale++) {
            uint8_t vram[64];
            memset(vram, 0x91, sizeof(vram));
            PGRAPHD3D11TextureState textures = { 0 };
            PGRAPHD3D11State renderer = {
                .device = device, .context = context, .textures = &textures,
            };
            TextureShape shape = {
                .dimensionality = 2, .width = 2, .height = 2, .depth = 1,
                .levels = 1, .pitch = 8,
                .color_format = swizzled ? NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 :
                                          NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
            };
            NV2AState d = {
                .pgraph = {
                    .d3d11_renderer_state = &renderer,
                    .surface_color = { .draw_dirty = true },
                    .texture_enabled = { true }, .dma_a = 1, .dma_b = 2,
                    .texture_shape = shape,
                },
                .vram_ptr = vram, .vram = vram, .vram_size = sizeof(vram),
            };
            fixture_vram_size = sizeof(vram);
            PGRAPHD3D11RenderTarget *target = &renderer.render_targets.color;
            *target = (PGRAPHD3D11RenderTarget) {
                .guest_width = 2, .guest_height = 2, .pitch = 8,
                .storage_length = 16, .format = DXGI_FORMAT_B8G8R8A8_UNORM,
                .nv2a_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8,
                .surface_type = swizzled ? NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE :
                                          NV097_SET_SURFACE_FORMAT_TYPE_PITCH,
            };
            D3D11_TEXTURE2D_DESC desc = {
                .Width = 2 * scale, .Height = 2 * scale,
                .MipLevels = 1, .ArraySize = 1,
                .Format = target->format, .SampleDesc = {1, 0},
                .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_RENDER_TARGET,
            };
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL,
                &target->texture)));
            assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
                (ID3D11Resource *)target->texture, NULL, &target->rtv)));
            pgraph_d3d11_bind_render_targets(&renderer);
            assert(d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                shape, 0, 16));
            d.pgraph.surface_color.buffer_dirty = true;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                shape, 0, 16));
            d.pgraph.surface_color.buffer_dirty = false;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                shape, 4, 16));
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                shape, 0, 12));
            TextureShape invalid = shape;
            invalid.levels = 2;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            invalid = shape;
            invalid.cubemap = true;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            invalid = shape;
            invalid.border = true;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            invalid = shape;
            invalid.width = 1;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            invalid = shape;
            invalid.min_mipmap_level = 1;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            invalid = shape;
            invalid.color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                invalid, 0, 16));
            target->format = DXGI_FORMAT_R8G8B8A8_UNORM;
            assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                shape, 0, 16));
            target->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (!swizzled) {
                target->pitch++;
                assert(!d3d11_surface_texture_compatible(target, &d.pgraph.surface_color,
                    shape, 0, 16));
                target->pitch--;
            }
            uint8_t pixels[4 * 4 * 4];
            for (unsigned int i = 0; i < desc.Width * desc.Height; i++) {
                pixels[i * 4] = 10 + i;
                pixels[i * 4 + 1] = 30 + i;
                pixels[i * 4 + 2] = 50 + i;
                pixels[i * 4 + 3] = 70 + i;
            }
            ID3D11DeviceContext_UpdateSubresource(context,
                (ID3D11Resource *)target->texture, 0, NULL, pixels, desc.Width * 4, 0);
            alias_hash_requires_flush = false;
            alias_flush_count = alias_hash_count = 0;
            assert(pgraph_d3d11_bind_textures(&d, NULL));
            PGRAPHD3D11TextureBinding *binding = &textures.bindings[0];
            assert(binding->surface_copy && binding->resource != (ID3D11Resource *)target->texture);
            assert(alias_flush_count == 0 && alias_hash_count == 0);
            assert(d.pgraph.surface_color.draw_dirty && vram[0] == 0x91);
            ID3D11RenderTargetView *bound = NULL;
            ID3D11DeviceContext_OMGetRenderTargets(context, 1, &bound, NULL);
            assert(bound == target->rtv);
            ID3D11RenderTargetView_Release(bound);
            D3D11_TEXTURE2D_DESC read = desc;
            read.Usage = D3D11_USAGE_STAGING;
            read.BindFlags = 0;
            read.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *staging = NULL;
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &read, NULL, &staging)));
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, binding->resource);
            D3D11_MAPPED_SUBRESOURCE mapped;
            assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
                0, D3D11_MAP_READ, 0, &mapped)));
            for (unsigned int y = 0; y < desc.Height; y++) {
                assert(memcmp((uint8_t *)mapped.pData + y * mapped.RowPitch,
                    pixels + y * desc.Width * 4, desc.Width * 4) == 0);
            }
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
            ID3D11Resource *snapshot = binding->resource;
            const float color[] = {0.2f, 0.4f, 0.6f, 1};
            ID3D11DeviceContext_ClearRenderTargetView(context, target->rtv, color);
            /* The previous draw's snapshot must not become a live RTV alias. */
            assert_texture_first_pixel(device, context, snapshot, pixels);
            assert(pgraph_d3d11_bind_textures(&d, NULL));
            assert(binding->resource == snapshot);
            const uint8_t updated[] = {153, 102, 51, 255};
            assert_texture_first_pixel(device, context, snapshot, updated);
            assert(alias_flush_count == 0 && alias_hash_count == 0);
            /* A resolution change must replace the snapshot, not copy between
             * incompatible extents or retain the previous scaled image. */
            ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
            ID3D11RenderTargetView_Release(target->rtv);
            ID3D11Texture2D_Release(target->texture);
            desc.Width *= 2;
            desc.Height *= 2;
            assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL,
                &target->texture)));
            assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(device,
                (ID3D11Resource *)target->texture, NULL, &target->rtv)));
            ID3D11DeviceContext_ClearRenderTargetView(context, target->rtv, color);
            pgraph_d3d11_bind_render_targets(&renderer);
            assert(pgraph_d3d11_bind_textures(&d, NULL));
            assert(binding->resource != snapshot);
            D3D11_TEXTURE2D_DESC resized;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)binding->resource, &resized);
            assert(resized.Width == desc.Width && resized.Height == desc.Height);
            assert_texture_first_pixel(device, context, binding->resource, updated);
            assert(alias_flush_count == 0 && alias_hash_count == 0);
            /* Returning to VRAM must not accept a GPU snapshot via hash equality. */
            d.pgraph.surface_color.draw_dirty = false;
            binding->data_hash = fast_hash(vram, 16);
            assert(pgraph_d3d11_bind_textures(&d, NULL));
            assert(!binding->surface_copy);
            const uint8_t cpu[] = {0x91, 0x91, 0x91, 0x91};
            assert_texture_first_pixel(device, context, binding->resource, cpu);
            ID3D11DeviceContext_ClearState(context);
            d3d11_release_texture(binding);
            d3d11_transfer_trim(&textures);
            ID3D11Texture2D_Release(staging);
            ID3D11RenderTargetView_Release(target->rtv);
            ID3D11Texture2D_Release(target->texture);
        }
    }
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: WARP GPU surface snapshots, linear/swizzled, 1x/2x pixels, reuse, freshness, feedback isolation, OM restoration and CPU fallback");
}

static void test_indexed_palette_target_alias_refresh(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));

    uint8_t guest_vram[0x1000] = { 0 };
    memset(guest_vram + 0x100, 0x20, 16);
    const uint32_t old_palette_color = 0xff010203;
    memcpy(guest_vram + 0x400 + 0x20 * 4, &old_palette_color,
           sizeof(old_palette_color));

    D3D11_TEXTURE2D_DESC target_desc = {
        .Width = 4,
        .Height = 4,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = {1, 0},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    ID3D11Texture2D *target = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &target_desc, NULL, &target);
    assert(SUCCEEDED(hr));
    ID3D11RenderTargetView *target_rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(device,
        (ID3D11Resource *)target, NULL, &target_rtv);
    assert(SUCCEEDED(hr));
    const FLOAT initial_color[4] = {1.0f / 255, 2.0f / 255,
                                    3.0f / 255, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(context, target_rtv,
                                              initial_color);

    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = {
        .device = device,
        .context = context,
        .textures = &textures,
    };
    renderer.render_targets.color.texture = target;
    renderer.render_targets.color.vram_address = 0x480;
    renderer.render_targets.color.storage_length = 4 * 4 * 4;
    TextureShape shape = {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
    };
    NV2AState nv2a = {
        .pgraph = {
            .d3d11_renderer_state = &renderer,
            .texture_enabled = { true },
            .dma_a = 1,
            .dma_b = 2,
            .palette_length_index = NV_PGRAPH_TEXPALETTE0_LENGTH_256,
            .texture_shape = shape,
            .texture_address = 0x100,
            .palette_address = 0x400,
        },
        .vram_ptr = guest_vram,
        .vram = guest_vram,
        .vram_size = sizeof(guest_vram),
    };
    fixture_vram_size = sizeof(guest_vram);
    alias_flush_count = alias_hash_count = 0;
    alias_hash_requires_flush = false;
    assert(pgraph_d3d11_bind_textures(&nv2a, NULL));
    assert(alias_flush_count == 0 && alias_hash_count == 2);
    const uint8_t old_expected[] = {1, 2, 3, 0xff};
    assert_texture_first_pixel(device, context, textures.bindings[0].resource,
                               old_expected);

    const FLOAT updated_color[4] = {17.0f / 255, 34.0f / 255,
                                    51.0f / 255, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(context, target_rtv,
                                              updated_color);
    nv2a.pgraph.surface_color.draw_dirty = true;
    alias_flush_count = alias_hash_count = 0;
    alias_hash_requires_flush = true;
    assert(pgraph_d3d11_bind_textures(&nv2a, NULL));
    assert(alias_flush_count == 1 && alias_hash_count == 2);
    assert(guest_vram[0x480] == 0x33 && guest_vram[0x481] == 0x22 &&
           guest_vram[0x482] == 0x11 && guest_vram[0x483] == 0xff);
    const uint8_t updated_expected[] = {0x11, 0x22, 0x33, 0xff};
    assert_texture_first_pixel(device, context, textures.bindings[0].resource,
                               updated_expected);

    ID3D11ShaderResourceView *null_views[NV2A_MAX_TEXTURES] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(context, 0, NV2A_MAX_TEXTURES,
                                             null_views);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11RenderTargetView_Release(target_rtv);
    ID3D11Texture2D_Release(target);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP bind flushes aliased indexed palette and refreshes sampled pixel\n");
}
"""

TEST = r"""
int main(void)
{
    assert(d3d11_vram_ranges_overlap(0x100, 16, 0x108, 4));
    assert(d3d11_vram_ranges_overlap(0x108, 4, 0x100, 16));
    assert(!d3d11_vram_ranges_overlap(0x100, 16, 0x110, 4));
    assert(!d3d11_vram_ranges_overlap(0x100, 0, 0x100, 16));
    g_print("PASS: overflow-safe VRAM interval overlap and adjacent ranges\n");
    test_unknown_texture_format_rejected();
    test_depth_texture_upload();
    /* Padding bytes must never turn into visible pixels. Two slices also
     * exercise slice_pitch independently of the tight destination layout. */
    const uint32_t source[] = {
        0xff112233, 0x80445566, 0xdeadbeef,
        0x40778899, 0x20aabbcc, 0xdeadbeef,
        0xff010203, 0xff040506, 0xdeadbeef,
        0xff070809, 0xff0a0b0c, 0xdeadbeef,
    };
    const uint8_t expected[] = {
        0x11,0x22,0x33,0xff, 0x44,0x55,0x66,0x80,
        0x77,0x88,0x99,0x40, 0xaa,0xbb,0xcc,0x20,
        1,2,3,0xff, 4,5,6,0xff, 7,8,9,0xff, 10,11,12,0xff,
    };
    TextureShape shape = {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .pitch = 12,
    };
    unsigned int consumed = 0;
    uint8_t *rgba = d3d11_convert_level(shape, (const uint8_t *)source, NULL,
                                       2, 2, 2, &consumed);
    assert(consumed == sizeof(source));
    assert(memcmp(rgba, expected, sizeof(expected)) == 0);
    g_free(rgba);

    const uint16_t r6_guest[] = {0, 0};
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5,
        .pitch = sizeof(r6_guest),
    };
    rgba = d3d11_convert_level(shape, (const uint8_t *)r6_guest, NULL, 2, 1, 1,
                               &consumed);
    const uint8_t r6_expected[] = {
        0x80, 0xff, 0x00, 0x7f, 0x01, 0x7e, 0x7f, 0x7f,
    };
    assert(consumed == sizeof(r6_guest));
    assert(memcmp(rgba, r6_expected, sizeof(r6_expected)) == 0);
    g_free(rgba);

    const uint8_t r8b8[] = {0x25, 0xc0};
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8,
        .pitch = sizeof(r8b8),
    };
    rgba = d3d11_convert_level(shape, r8b8, NULL, 1, 1, 1, &consumed);
    const uint8_t r8b8_expected[] = {0xc0, 0x25, 0x25, 0xc0};
    assert(consumed == sizeof(r8b8));
    assert(memcmp(rgba, r8b8_expected, sizeof(r8b8_expected)) == 0);
    g_free(rgba);

    const uint16_t y16[] = { 0x0000, 0x8000, 0xffff };
    const uint16_t y16_expected[] = {
        0, 0, 0, 0xffff, 0x8000, 0x8000, 0x8000, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff,
    };
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16,
        .pitch = sizeof(y16),
    };
    rgba = d3d11_convert_level(shape, (const uint8_t *)y16, NULL, 3, 1, 1,
                               &consumed);
    assert(consumed == sizeof(y16));
    assert(memcmp(rgba, y16_expected, sizeof(y16_expected)) == 0);
    g_free(rgba);

    const uint16_t depth16[] = {0x0000, 0x8000, 0xffff};
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED,
        .pitch = sizeof(depth16),
    };
    rgba = d3d11_convert_level(shape, (const uint8_t *)depth16, NULL, 3, 1, 1,
                               &consumed);
    assert(consumed == sizeof(depth16));
    const uint8_t packed_depth16[] = {
        0,0,0,0xff, 0x80,0,0,0xff, 0xff,0xff,0,0xff,
    };
    assert(memcmp(rgba, packed_depth16, sizeof(packed_depth16)) == 0);
    g_free(rgba);

    const uint16_t swizzled_depth_linear[] = {0x0000, 0x4000, 0x8000, 0xffff};
    uint8_t swizzled_depth[sizeof(swizzled_depth_linear)];
    swizzle_box((const uint8_t *)swizzled_depth_linear, 2, 2, 1,
                swizzled_depth, 4, 8, 2);
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED,
    };
    rgba = d3d11_convert_level(shape, swizzled_depth, NULL, 2, 2, 1,
                               &consumed);
    assert(consumed == sizeof(swizzled_depth_linear));
    const uint8_t swizzled_depth_expected[] = {
        0,0,0,0xff, 0x40,0,0,0xff, 0x80,0,0,0xff,
        0xff,0xff,0,0xff,
    };
    assert(memcmp(rgba, swizzled_depth_expected,
                  sizeof(swizzled_depth_expected)) == 0);
    g_free(rgba);

    const uint32_t depth24[] = {0x00000000, 0x7fffff00, 0xffffff00};
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED,
        .pitch = sizeof(depth24),
    };
    rgba = d3d11_convert_level(shape, (const uint8_t *)depth24, NULL, 3, 1, 1,
                               &consumed);
    assert(consumed == sizeof(depth24));
    const uint8_t packed_depth24[] = {
        0,0,0,0xff, 0x7f,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
    };
    assert(memcmp(rgba, packed_depth24, sizeof(packed_depth24)) == 0);
    g_free(rgba);

    const uint16_t depth_half[] = {0x0000, 0x3800, 0x3c00, 0xbc00};
    shape = (TextureShape) {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT,
        .pitch = sizeof(depth_half),
    };
    rgba = d3d11_convert_level(shape, (const uint8_t *)depth_half, NULL, 4, 1, 1,
                               &consumed);
    assert(consumed == sizeof(depth_half));
    const uint8_t packed_depth_half[] = {
        0,0,0,0xff, 0x80,0,0,0xff, 0xff,0xff,0,0xff, 0,0,0,0xff,
    };
    assert(memcmp(rgba, packed_depth_half, sizeof(packed_depth_half)) == 0);
    g_free(rgba);

    /* Round-trip a real swizzled volume through the shared swizzle helper. */
    uint32_t linear[8] = {
        source[0], source[1], source[3], source[4],
        source[6], source[7], source[9], source[10],
    };
    uint8_t swizzled[sizeof(linear)];
    swizzle_box((const uint8_t *)linear, 2, 2, 2, swizzled, 8, 16, 4);
    shape.color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    rgba = d3d11_convert_level(shape, swizzled, NULL, 2, 2, 2, &consumed);
    assert(consumed == sizeof(linear));
    assert(memcmp(rgba, expected, sizeof(expected)) == 0);
    g_free(rgba);
    g_print("PASS: padded rows, slice pitch, channel order, swizzled volume\n");
    test_r6g5b5_texture_upload();
    test_y16_texture_upload();
    test_linear_texture_uses_base_mip();
    test_volume_upload(swizzled, expected);
    test_border_volume_upload();
    test_border_texture_upload();
    test_border_cubemap_upload();
    test_border_cubemap_mip_upload(NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8);
    test_border_cubemap_mip_upload(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5);
    test_border_cubemap_mip_upload(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8);
    test_border_cubemap_mip_upload(NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8);
    test_indexed_palette_target_alias_refresh();
    test_surface_gpu_snapshot();
    test_transfer_format_matrix(0.0f, 0);
    test_transfer_format_matrix(0.5f, 61);
    test_transfer_format_matrix(0.75f, 127);
    test_transfer_format_matrix(1.0f, 255);
    test_transfer_chains();
    test_transfer_allocation_fallback();
    return 0;
}
"""


def main():
    source = (PGRAPH / "d3d11/textures.c").read_text(encoding="utf-8")
    assert source.count("d3d11_vram_ranges_overlap(") == 4
    overlap_start = source.index("static bool d3d11_vram_ranges_overlap(")
    overlap_end = source.index("\nstatic ", overlap_start + 1)
    overlap_helper = source[overlap_start:overlap_end]
    texture_overlap_start = source.index(
        "static bool d3d11_texture_data_overlaps_target(")
    texture_overlap_end = source.index("\nstatic ", texture_overlap_start + 1)
    texture_overlap_helper = source[texture_overlap_start:texture_overlap_end]
    crop_start = source.index("static bool d3d11_crop_cubemap_border(")
    crop_end = source.index(
        "\nstatic bool d3d11_border_resource_dimensions(", crop_start)
    crop_helper = source[crop_start:crop_end]
    dimensions_start = source.index(
        "static bool d3d11_border_resource_dimensions(")
    dimensions_end = source.index(
        "\nstatic D3D11_TEXTURE_ADDRESS_MODE", dimensions_start)
    dimensions_helper = source[dimensions_start:dimensions_end]
    cubemap_shape_start = source.index(
        "static bool d3d11_cubemap_shape_valid(")
    cubemap_shape_end = source.index("\nstatic ", cubemap_shape_start + 1)
    cubemap_shape_helper = source[cubemap_shape_start:cubemap_shape_end]
    cubemap_border_mips_start = source.index(
        "static bool d3d11_cubemap_border_mips_supported(")
    cubemap_border_mips_end = source.index(
        "\nstatic ", cubemap_border_mips_start + 1)
    cubemap_border_mips_helper = source[
        cubemap_border_mips_start:cubemap_border_mips_end]
    create = source[source.index("static bool d3d11_create_texture"):
                    source.index("bool pgraph_d3d11_bind_textures")]
    assert "shape.cubemap && shape.border" in create
    assert "levels == 1" in create
    assert "d3d11_crop_cubemap_border(" in create
    assert "d3d11_cubemap_shape_valid(" in create
    assert "d3d11_cubemap_border_mips_supported(" in create
    assert "d3d11_texture_shape_valid(" in create
    assert create.index("d3d11_cubemap_shape_valid(") < \
        create.index("ID3D11Device_CreateTexture2D")
    assert create.index("d3d11_cubemap_border_mips_supported(") < \
        create.index("ID3D11Device_CreateTexture2D")
    assert create.index("d3d11_texture_shape_valid(") < \
        create.index("ID3D11Device_CreateTexture2D")
    dma_helpers = "\n".join(
        source[source.index(start):
               source.index("\nstatic ", source.index(start) + 1)]
        for start in (
            "static bool d3d11_dma_range(",
            "static bool d3d11_texture_dma_range(",
            "static bool d3d11_palette_dma_range(",
        )
    )
    overlap_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef uint64_t hwaddr;
''' + overlap_helper + "\n" + texture_overlap_helper + "\n" + crop_helper + "\n" + dimensions_helper + "\n" + cubemap_shape_helper + "\n" + cubemap_border_mips_helper + r'''
int main(void)
{
    assert(d3d11_vram_ranges_overlap(100, 10, 109, 2));
    assert(d3d11_vram_ranges_overlap(105, 2, 100, 10));
    assert(d3d11_vram_ranges_overlap(100, 10, 102, 2));
    assert(!d3d11_vram_ranges_overlap(100, 10, 110, 1));
    assert(!d3d11_vram_ranges_overlap(100, 0, 100, 1));
    assert(!d3d11_vram_ranges_overlap(100, 1, 100, 0));
    assert(d3d11_vram_ranges_overlap(0, SIZE_MAX, SIZE_MAX - 1, 2));
    puts("PASS: VRAM range overlap boundaries");
    /* Indexed texels may be separate from a dirty surface while their palette
     * aliases it; binding must flush before palette bytes are read. */
    assert(d3d11_texture_data_overlaps_target(
        0x100, 0x20, true, 0x300, 0x100, 0x380, 0x20));
    assert(!d3d11_texture_data_overlaps_target(
        0x100, 0x20, false, 0x300, 0x100, 0x380, 0x20));
    assert(d3d11_texture_data_overlaps_target(
        0x100, 0x20, false, 0x300, 0x100, 0x110, 0x20));
    assert(!d3d11_texture_data_overlaps_target(
        0x100, 0x20, true, 0x300, 0x80, 0x380, 0x20));
    assert(!d3d11_texture_data_overlaps_target(
        0x100, 0x20, true, 0x300, 0, 0x300, 0x20));
    puts("PASS: indexed palette and texture data trigger target overlap only when applicable");
    uint8_t cubemap[16 * 16 * 4];
    for (unsigned int y = 0; y < 16; y++) {
        for (unsigned int x = 0; x < 16; x++) {
            for (unsigned int channel = 0; channel < 4; channel++) {
                cubemap[(y * 16 + x) * 4 + channel] =
                    (uint8_t)(y * 16 + x + channel);
            }
        }
    }
    assert(d3d11_crop_cubemap_border(cubemap, 16, 16, 8, 8, 4));
    for (unsigned int y = 0; y < 8; y++) {
        for (unsigned int x = 0; x < 8; x++) {
            for (unsigned int channel = 0; channel < 4; channel++) {
                assert(cubemap[(y * 8 + x) * 4 + channel] ==
                       (uint8_t)((y + 4) * 16 + (x + 4) + channel));
            }
        }
    }
    assert(!d3d11_crop_cubemap_border(cubemap, 8, 8, 8, 8, 4));
    puts("PASS: single-level cubemap crop discards four border texels safely");
    assert(d3d11_cubemap_shape_valid(false, true, 1, 8, 4));
    assert(d3d11_cubemap_shape_valid(true, false, 2, 16, 16));
    assert(!d3d11_cubemap_shape_valid(true, true, 2, 16, 16));
    assert(!d3d11_cubemap_shape_valid(true, false, 1, 16, 16));
    assert(!d3d11_cubemap_shape_valid(true, false, 3, 16, 16));
    assert(!d3d11_cubemap_shape_valid(true, false, 2, 16, 8));
    puts("PASS: cubemap shape validation rejects linear, non-2D, and non-square faces");
    assert(d3d11_cubemap_border_mips_supported(false, true, 4));
    assert(d3d11_cubemap_border_mips_supported(true, false, 4));
    assert(d3d11_cubemap_border_mips_supported(true, true, 1));
    assert(d3d11_cubemap_border_mips_supported(true, true, 2));
    assert(!d3d11_cubemap_border_mips_supported(true, true, 0));
    puts("PASS: bordered cubemap mip chains accepted; zero levels rejected");
    unsigned int width, height, depth;
    assert(d3d11_border_resource_dimensions(
        true, false, 3, 8, 4, 2, &width, &height, &depth));
    assert(width == 16 && height == 16 && depth == 16);
    assert(d3d11_border_resource_dimensions(
        true, false, 2, 3, 5, 1, &width, &height, &depth));
    assert(width == 16 && height == 16 && depth == 1);
    assert(d3d11_border_resource_dimensions(
        true, false, 1, 8, 0, 0, &width, &height, &depth));
    assert(width == 16 && height == 1 && depth == 1);
    assert(d3d11_border_resource_dimensions(
        false, false, 3, 8, 4, 2, &width, &height, &depth));
    assert(width == 8 && height == 4 && depth == 2);
    assert(d3d11_border_resource_dimensions(
        true, true, 3, 8, 4, 2, &width, &height, &depth));
    assert(width == 8 && height == 4 && depth == 2);
    assert(!d3d11_border_resource_dimensions(
        true, false, 3, UINT_MAX / 2 + 1, 4, 2,
        &width, &height, &depth));
    assert(!d3d11_border_resource_dimensions(
        true, false, 3, 8, 4, UINT_MAX / 2 + 1,
        &width, &height, &depth));
    assert(!d3d11_border_resource_dimensions(
        true, false, 0, 8, 4, 2, &width, &height, &depth));
    puts("PASS: border-expanded texture dimensions and overflow checks");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="xemu-vram-overlap-") as tmp:
        tmp = pathlib.Path(tmp)
        test = tmp / "test.c"
        test.write_text(overlap_test, encoding="utf-8")
        exe = tmp / "test"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    binding_start = source.index("bool pgraph_d3d11_bind_textures(")
    binding = source[binding_start:]
    assert binding.index("d3d11_cubemap_border_mips_supported(") < \
        binding.index("d3d11_texture_dma_range(")
    assert binding.index("d3d11_texture_shape_valid(") < \
        binding.index("d3d11_texture_dma_range(")
    assert ("if (surfaces[i]->draw_dirty && targets[i]->texture &&\n"
            "                d3d11_texture_data_overlaps_target(" in binding)
    if "--overlap-only" in sys.argv:
        return

    if sys.platform != "win32":
        print("SKIP: D3D11 texture creation/readback fixture requires Windows/MSYS2")
        return

    assert "uses_palette &&" in texture_overlap_helper
    converter = source[source.index("static uint8_t expand_bits"):
                       source.index("static D3D11_TEXTURE_ADDRESS_MODE")]
    texture_upload_helpers = "\n".join(
        source[source.index(start):
               source.index("\nstatic ", source.index(start) + 1)]
        for start in (
            "static unsigned int d3d11_effective_mip_levels(",
            "static unsigned int d3d11_effective_min_mipmap_level(",
            "static bool d3d11_cubemap_shape_valid(",
            "static bool d3d11_cubemap_border_mips_supported(",
            "static bool d3d11_texture_shape_valid(",
        )
    )
    assert "static bool d3d11_texture_shape_valid(" in texture_upload_helpers
    header = (PGRAPH / "texture.h").read_text(encoding="utf-8")
    types = header[header.index("typedef struct TextureShape"):
                   header.index("extern const BasicColorFormatInfo")]
    common = (PGRAPH / "texture.c").read_text(encoding="utf-8")
    map_start = common.index("const BasicColorFormatInfo kelvin_color_format_info_map")
    common_map = common[map_start:common.index("};", map_start) + 2]
    support_start = SUPPORT.index("static const BasicColorFormatInfo")
    support_end = SUPPORT.index("};", support_start) + 2
    support = (SUPPORT[:support_start] + "static " + common_map +
               SUPPORT[support_end:])
    bindings = (PGRAPH / "d3d11/textures.h").read_text(encoding="utf-8")
    bindings = bindings[bindings.index("typedef struct PGRAPHD3D11TextureBinding"):
                        bindings.index("bool pgraph_d3d11_textures_init")]
    release = source[source.index("static void d3d11_release_texture"):
                     source.index("bool pgraph_d3d11_textures_init")]
    transfer_release = source[source.index("static void d3d11_transfer_pixel_release"):
                              source.index("static void d3d11_release_texture")]
    surfaces = (PGRAPH / "d3d11/surfaces.c").read_text(encoding="utf-8")
    references = "static const float f16_max = 511.9375f, f24_max = 1.0e30f;\n"
    references += surfaces[surfaces.index("static uint16_t d3d11_float_to_f16("):
                           surfaces.index("static void d3d11_convert_color_to_bgra8(")]
    references += surfaces[surfaces.index("static void d3d11_convert_bgra8_to_color("):
                           surfaces.index("static void d3d11_surface_dimensions(")]
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    s3tc = (PGRAPH / "s3tc.c").read_text(encoding="utf-8")
    s3tc = s3tc.replace('#include "qemu/osdep.h"', "")
    s3tc = s3tc.replace('#include "s3tc.h"', "")
    vk_source = (PGRAPH / "vk/texture.c").read_text(encoding="utf-8")
    vk_start = vk_source.index("static size_t get_cubemap_layer_size(")
    vk_end = vk_source.index("\n// FIXME:", vk_start)
    vk_layout = r'''
static bool pgraph_is_texture_format_compressed(PGRAPHState *pg, unsigned int f)
{
    (void)pg;
    return f == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ||
           f == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
           f == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8;
}
''' + vk_source[vk_start:vk_end]
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-textures-") as tmp:
        tmp = pathlib.Path(tmp)
        test = tmp / "test.c"
        test.write_text(PREAMBLE + types + bindings + support + s3tc + DEVICE_SUPPORT + vk_layout +
                        transfer_release + release + converter + texture_upload_helpers + create +
                        binding + references + DEVICE_TEST +
                        WARP_BIND_TEST + TEST,
                        encoding="utf-8")
        exe = tmp / "test.exe"
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare",
                        f"-I{ROOT}", str(test), str(PGRAPH / "swizzle.c"),
                        "-o", str(exe), *flags, "-ld3d11", "-ld3dcompiler"], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
