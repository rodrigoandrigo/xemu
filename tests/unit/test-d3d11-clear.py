#!/usr/bin/env python3
"""Test color-clear channel masks and render-target replacement ordering."""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SURFACES = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"
REGS = ROOT / "hw/xbox/nv2a/nv2a_regs.h"


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


source = SURFACES.read_text(encoding="utf-8")
registers = REGS.read_text(encoding="utf-8")
macros = []
for name in ("NV097_CLEAR_SURFACE_R", "NV097_CLEAR_SURFACE_G",
             "NV097_CLEAR_SURFACE_B", "NV097_CLEAR_SURFACE_A",
             "NV097_CLEAR_SURFACE_Z", "NV097_CLEAR_SURFACE_STENCIL"):
    match = re.search(
        rf"^\s*#\s*define\s+{name}\s+\(\s*1\s*<<\s*(\d+)\s*\)",
        registers, re.MULTILINE)
    assert match, f"missing clear-channel register: {name}"
    shift = int(match.group(1))
    macros.append(f"#define {name} (1U << {shift})\n")

SURFACE_FORMAT_NAMES = (
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_B8",
    "NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8",
)
for name in SURFACE_FORMAT_NAMES:
    match = re.search(
        rf"^\s*#\s*define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)",
        registers, re.MULTILINE)
    assert match, f"missing surface-format register: {name}"
    macros.append(f"#define {name} {match.group(1)}U\n")

unorm8 = extract_function(source, "static uint8_t d3d11_clear_unorm8(")
unorm5 = extract_function(source, "static uint16_t d3d11_clear_unorm5(")
scissor_coordinate = extract_function(
    source, "static LONG d3d11_scissor_coordinate(")
gl_scissor_rect = extract_function(
    source, "D3D11_RECT pgraph_d3d11_gl_scissor_rect(")
apply_clear = extract_function(
    source, "static void d3d11_apply_masked_color_clear(")
apply_clear_b5g5r5a1 = extract_function(
    source, "static void d3d11_apply_masked_color_clear_b5g5r5a1(")
apply_depth_clear = extract_function(
    source, "static void d3d11_apply_depth_stencil_clear(")
apply_cpu_clear = extract_function(
    source, "static bool d3d11_clear_color_region_cpu(")
apply_depth_cpu_clear = extract_function(
    source, "static bool d3d11_clear_depth_stencil_region_cpu(")
depth_staging_format = extract_function(
    source, "static DXGI_FORMAT d3d11_depth_staging_format(")
depth_upload_texture = extract_function(
    source, "static bool d3d11_upload_depth_stencil_texture(")
render_clear = extract_function(
    source, "bool pgraph_d3d11_render_targets_clear(")
create_target = extract_function(source, "static bool d3d11_create_target(")
update_targets = extract_function(
    source, "bool pgraph_d3d11_render_targets_update(")
update_output_merger = extract_function(
    source, "bool pgraph_d3d11_update_output_merger(")
color_target_active = extract_function(
    source, "static bool d3d11_color_target_active(")
color_format_mask = extract_function(
    source, "static uint8_t d3d11_surface_color_format_mask(")
color_clear_mask = extract_function(
    source, "static uint32_t d3d11_surface_color_clear_mask(")
color_clear_supported = extract_function(
    source, "static bool d3d11_surface_color_clear_supported(")

assert "D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE" in apply_cpu_clear
assert "D3D11_MAP_READ_WRITE" in apply_cpu_clear
assert "CopySubresourceRegion" in apply_cpu_clear
assert "d3d11_apply_masked_color_clear_b5g5r5a1" in apply_cpu_clear
assert "d3d11_flush_color(d, errp)" in create_target
assert apply_cpu_clear.index("OMSetRenderTargets") < apply_cpu_clear.index(
    "CreateTexture2D")
assert "pgraph_d3d11_bind_render_targets(r);" in apply_cpu_clear
assert "d3d11_depth_staging_format(target->format)" in apply_depth_cpu_clear
assert "DXGI_FORMAT_R32_TYPELESS" in depth_staging_format
assert "DXGI_FORMAT_R24G8_TYPELESS" in depth_staging_format
assert "DXGI_FORMAT_R16_TYPELESS" in depth_staging_format
assert "D3D_FEATURE_LEVEL_10_1" in apply_depth_cpu_clear
assert "D3D11_MAP_READ_WRITE" in apply_depth_cpu_clear
assert apply_depth_cpu_clear.count("ID3D11DeviceContext_CopyResource") == 2
assert "resource.RowPitch" in apply_depth_cpu_clear
assert "UpdateSubresource" not in apply_depth_cpu_clear
assert "D3D11_MAP_WRITE" in depth_upload_texture
assert "ID3D11DeviceContext_CopyResource" in depth_upload_texture
assert "d3d11_clear_depth_stencil_region_cpu(" in render_clear
assert create_target.index("d3d11_flush_depth_stencil(d, errp)") < \
       create_target.index("d3d11_release_target(target)")
assert update_targets.index("NV_PGRAPH_SETUPRASTER_Z_FORMAT") < \
       update_targets.index("d3d11_create_target(d, false, errp)")
assert "targets->color.format" in update_output_merger
assert "d3d11_color_target_active(" in update_output_merger
assert "logic_op_enable && format ==" in color_target_active
assert "DXGI_FORMAT_B5G5R5A1_UNORM" in color_target_active
assert "rt->LogicOpEnable" in update_output_merger
assert render_clear.index("NV_PGRAPH_SETUPRASTER_Z_FORMAT") < \
       render_clear.index("pgraph_d3d11_render_targets_update")
assert render_clear.index("NV_PGRAPH_SETUPRASTER_Z_FORMAT") < \
       render_clear.index("pgraph_get_clear_depth_stencil_value")
assert "pgraph_d3d11_gl_scissor_rect(" in render_clear
assert "target->height, xmin, ymin, clear_width, clear_height" in render_clear
assert "channel_mask == all_channels && full_clear" in render_clear
assert "d3d11_clear_color_region_cpu(" in render_clear
assert "d3d11_surface_color_clear_mask(" in render_clear
assert render_clear.index("d3d11_surface_color_clear_mask(") < \
       render_clear.index("channel_mask == all_channels")
assert "d3d11_surface_color_clear_supported(target->nv2a_format)" in render_clear
assert render_clear.index("if (channel_mask && rect.right > rect.left &&") < \
       render_clear.index("d3d11_surface_color_clear_supported(")
assert render_clear.index("d3d11_surface_color_clear_supported(") < \
       render_clear.index("pgraph_get_clear_color(")
assert "r, target, rect, channel_mask, clear_color" in render_clear
assert "pg->surface_color.draw_dirty = true;" in render_clear

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + "".join(macros) + r'''
typedef int32_t LONG;
typedef enum DXGI_FORMAT {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B5G5R5A1_UNORM,
} DXGI_FORMAT;
typedef struct D3D11_RECT {
    LONG left, top, right, bottom;
} D3D11_RECT;
#define CLAMP(v, low, high) ((v) < (low) ? (low) : ((v) > (high) ? (high) : (v)))
#define D3D11_COLOR_WRITE_ENABLE_RED 0x1
#define D3D11_COLOR_WRITE_ENABLE_GREEN 0x2
#define D3D11_COLOR_WRITE_ENABLE_BLUE 0x4
#define D3D11_COLOR_WRITE_ENABLE_ALPHA 0x8
''' + scissor_coordinate + gl_scissor_rect + unorm8 + unorm5 + apply_clear + \
    apply_clear_b5g5r5a1 + color_format_mask + color_clear_mask + \
    color_clear_supported + apply_depth_clear + color_target_active + r'''
int main(void)
{
    const uint32_t all_clear_channels = NV097_CLEAR_SURFACE_R |
        NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A;
    assert(!d3d11_surface_color_clear_supported(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_B8));
    assert(!d3d11_surface_color_clear_supported(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8));
    assert(d3d11_surface_color_clear_supported(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5));
    assert(d3d11_surface_color_clear_supported(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8));
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels, NV097_SET_SURFACE_FORMAT_COLOR_LE_B8) ==
           NV097_CLEAR_SURFACE_R);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels, NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8) ==
           (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G));
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) ==
           (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
            NV097_CLEAR_SURFACE_B));
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) ==
           all_clear_channels);
    const uint32_t rgb_clear_channels = NV097_CLEAR_SURFACE_R |
        NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B;
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5) ==
           all_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5) ==
           rgb_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) ==
           rgb_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8) ==
           all_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8) ==
           all_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8) ==
           all_clear_channels);
    assert(d3d11_surface_color_clear_mask(
               all_clear_channels, NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5) ==
           rgb_clear_channels);
    uint8_t narrow_pixel[4] = {10, 20, 30, 40};
    const float narrow_clear[4] = {1.0f, 0.5f, 0.25f, 0.0f};
    d3d11_apply_masked_color_clear(
        narrow_pixel, sizeof(narrow_pixel), 1, 1,
        d3d11_surface_color_clear_mask(
            all_clear_channels, NV097_SET_SURFACE_FORMAT_COLOR_LE_B8),
        narrow_clear);
    assert(narrow_pixel[0] == 10 && narrow_pixel[1] == 20 &&
           narrow_pixel[2] == 255 && narrow_pixel[3] == 40);
    narrow_pixel[0] = 10; narrow_pixel[1] = 20;
    narrow_pixel[2] = 30; narrow_pixel[3] = 40;
    d3d11_apply_masked_color_clear(
        narrow_pixel, sizeof(narrow_pixel), 1, 1,
        d3d11_surface_color_clear_mask(
            all_clear_channels, NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8),
        narrow_clear);
    assert(narrow_pixel[0] == 10 && narrow_pixel[1] == 128 &&
           narrow_pixel[2] == 255 && narrow_pixel[3] == 40);

    assert(!d3d11_color_target_active(0, false,
                                     DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(!d3d11_color_target_active(0, true,
                                     DXGI_FORMAT_B8G8R8A8_UNORM));
    assert(d3d11_color_target_active(0, true,
                                     DXGI_FORMAT_B5G5R5A1_UNORM));
    assert(d3d11_color_target_active(1, false,
                                     DXGI_FORMAT_B8G8R8A8_UNORM));
    D3D11_RECT rect = pgraph_d3d11_gl_scissor_rect(1080, 100, 200, 300, 400);
    assert(rect.left == 100 && rect.top == 480 &&
           rect.right == 400 && rect.bottom == 880);
    rect = pgraph_d3d11_gl_scissor_rect(1080, 0, 0, 1920, 100);
    assert(rect.top == 980 && rect.bottom == 1080);
    rect = pgraph_d3d11_gl_scissor_rect(1080, 0, 1100, 10, 20);
    assert(rect.top == -40 && rect.bottom == -20);
    uint32_t packed = 0x11223344;
    d3d11_apply_depth_stencil_clear((uint8_t *)&packed, true, false,
                                    0.5f, 0, false, false);
    assert(packed == 0x11800000);
    packed = 0x11223344;
    d3d11_apply_depth_stencil_clear((uint8_t *)&packed, false, true,
                                    0.25f, 0xa5, false, false);
    assert(packed == 0xa5223344);
    packed = 0x11223344;
    d3d11_apply_depth_stencil_clear((uint8_t *)&packed, true, true,
                                    0.5f, 0x7f, false, false);
    assert(packed == 0x7f800000);
    float float_depth = 0.25f;
    d3d11_apply_depth_stencil_clear((uint8_t *)&float_depth, true, true,
                                    2.0f, 0xa5, true, false);
    assert(float_depth == 1.0f);
    uint16_t depth16 = 0;
    d3d11_apply_depth_stencil_clear((uint8_t *)&depth16, true, false,
                                    0.5f, 0, false, true);
    assert(depth16 == 32768);
    depth16 = 0x1234;
    d3d11_apply_depth_stencil_clear((uint8_t *)&depth16, false, true,
                                    0.0f, 0xa5, false, true);
    assert(depth16 == 0x1234);
    uint8_t pixels[24];
    for (unsigned int y = 0; y < 2; y++) {
        for (unsigned int x = 0; x < 2; x++) {
            uint8_t *p = pixels + y * 12 + x * 4;
            p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 40;
        }
        pixels[y * 12 + 8] = pixels[y * 12 + 9] = 0xCC;
        pixels[y * 12 + 10] = pixels[y * 12 + 11] = 0xCC;
    }
    const float rgba[4] = {1.0f, 0.5f, 0.25f, 0.75f};
    const uint32_t red_alpha = NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_A;
    d3d11_apply_masked_color_clear(pixels, 12, 2, 2, red_alpha, rgba);
    for (unsigned int y = 0; y < 2; y++) {
        for (unsigned int x = 0; x < 2; x++) {
            const uint8_t *p = pixels + y * 12 + x * 4;
            assert(p[0] == 10 && p[1] == 20 && p[2] == 255 && p[3] == 191);
        }
        assert(pixels[y * 12 + 8] == 0xCC &&
               pixels[y * 12 + 9] == 0xCC &&
               pixels[y * 12 + 10] == 0xCC &&
               pixels[y * 12 + 11] == 0xCC);
    }
    d3d11_apply_masked_color_clear(pixels, 12, 2, 2,
                                   NV097_CLEAR_SURFACE_G |
                                   NV097_CLEAR_SURFACE_B, rgba);
    for (unsigned int y = 0; y < 2; y++) {
        for (unsigned int x = 0; x < 2; x++) {
            const uint8_t *p = pixels + y * 12 + x * 4;
            assert(p[0] == 64 && p[1] == 128 && p[2] == 255 && p[3] == 191);
        }
    }
    d3d11_apply_masked_color_clear(pixels, 12, 2, 2, 0, rgba);
    assert(pixels[0] == 64 && pixels[1] == 128 &&
           pixels[2] == 255 && pixels[3] == 191);
    assert(d3d11_clear_unorm8(-1.0f) == 0);
    assert(d3d11_clear_unorm8(2.0f) == 255);
    uint8_t packed_pixels[12] = {0};
    packed_pixels[4] = packed_pixels[5] = 0xCC;
    packed_pixels[10] = packed_pixels[11] = 0xCC;
    const uint32_t packed_red_alpha = NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_A;
    d3d11_apply_masked_color_clear_b5g5r5a1(
        packed_pixels, 6, 2, 1, packed_red_alpha, rgba);
    uint16_t packed16;
    memcpy(&packed16, packed_pixels, sizeof(packed16));
    assert(packed16 == ((31U << 10) | 0x8000U));
    memcpy(&packed16, packed_pixels + 2, sizeof(packed16));
    assert(packed16 == ((31U << 10) | 0x8000U));
    assert(packed_pixels[4] == 0xCC && packed_pixels[5] == 0xCC);
    d3d11_apply_masked_color_clear_b5g5r5a1(
        packed_pixels, 6, 2, 1,
        NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B, rgba);
    memcpy(&packed16, packed_pixels, sizeof(packed16));
    assert(packed16 == ((31U << 10) | (16U << 5) | 8U | 0x8000U));
    const float alpha_clear[4] = {0.0f, 0.0f, 0.0f, 0.25f};
    d3d11_apply_masked_color_clear_b5g5r5a1(
        packed_pixels, 6, 1, 1, NV097_CLEAR_SURFACE_A, alpha_clear);
    memcpy(&packed16, packed_pixels, sizeof(packed16));
    assert(packed16 == ((31U << 10) | (16U << 5) | 8U));
    assert(packed_pixels[10] == 0xCC && packed_pixels[11] == 0xCC);
    assert(d3d11_clear_unorm5(-1.0f) == 0);
    assert(d3d11_clear_unorm5(2.0f) == 31);
    puts("PASS: BGRA8 masked clears preserve unselected channels and row padding");
    puts("PASS: B5G5R5A1 masked clears quantize selected channels and "
         "preserve packed bits/padding");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-d3d11-clear-") as directory:
    cfile = pathlib.Path(directory) / "clear.c"
    exe = pathlib.Path(directory) / "clear"
    cfile.write_text(fixture, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: masked clear fallback, scissored region path, and dirty zeta flush ordering")
print("PASS: A/X and opaque clear masks; B8/G8B8 unsupported clears rejected explicitly")

WARP_FIXTURE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d11.h>
typedef int Error;
typedef struct PGRAPHD3D11State {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
} PGRAPHD3D11State;
typedef struct PGRAPHD3D11RenderTarget {
    ID3D11Texture2D *texture;
    DXGI_FORMAT format;
} PGRAPHD3D11RenderTarget;
''' + "".join(macros) + r'''
#define CLAMP(v, low, high) ((v) < (low) ? (low) : ((v) > (high) ? (high) : (v)))
static int error_reported;
static void error_setg(Error **errp, const char *format, ...)
{
    (void)errp;
    (void)format;
    error_reported = 1;
}
static void pgraph_d3d11_bind_render_targets(PGRAPHD3D11State *r)
{
    (void)r;
}
''' + depth_staging_format + depth_upload_texture + apply_depth_clear + \
    apply_depth_cpu_clear + r'''
static uint32_t depth24(float value)
{
    return (uint32_t)(CLAMP(value, 0.0f, 1.0f) * 16777215.0f + 0.5f);
}
static void read_d24(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11Texture2D *texture, uint32_t pixels[12])
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            memcpy(&pixels[y * 4 + x],
                   (uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4, 4);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}
static void read_d32(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11Texture2D *texture, float pixels[12])
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            memcpy(&pixels[y * 4 + x],
                   (uint8_t *)mapped.pData + y * mapped.RowPitch + x * 4, 4);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}
static void read_d16(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11Texture2D *texture, uint16_t pixels[12])
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            memcpy(&pixels[y * 4 + x],
                   (uint8_t *)mapped.pData + y * mapped.RowPitch + x * 2, 2);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}
static void test_d16(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 4, .Height = 3, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_D16_UNORM,
        .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_DEPTH_STENCIL,
    };
    ID3D11Texture2D *texture = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreateDepthStencilView(
        device, (ID3D11Resource *)texture, NULL, &dsv);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_ClearDepthStencilView(
        context, dsv, D3D11_CLEAR_DEPTH, 0.25f, 0);
    PGRAPHD3D11State renderer = {.device = device, .context = context};
    PGRAPHD3D11RenderTarget target = {
        .texture = texture, .format = DXGI_FORMAT_D16_UNORM,
    };
    uint16_t before[12], after[12];
    read_d16(device, context, texture, before);
    Error *error = NULL;
    error_reported = 0;
    assert(d3d11_clear_depth_stencil_region_cpu(
        &renderer, &target, (D3D11_RECT){1, 1, 3, 2},
        NV097_CLEAR_SURFACE_Z, 0.75f, 0, &error));
    assert(!error_reported);
    read_d16(device, context, texture, after);
    for (unsigned int i = 0; i < 12; i++) {
        bool changed = i == 5 || i == 6;
        assert(after[i] == (changed ? 49151 : before[i]));
    }

    uint16_t upload[12];
    for (unsigned int i = 0; i < 12; i++) {
        upload[i] = (uint16_t)(i * 5000U);
    }
    assert(d3d11_upload_depth_stencil_texture(
        &renderer, &target, (const uint8_t *)upload, 4 * sizeof(uint16_t),
        &error));
    read_d16(device, context, texture, after);
    assert(memcmp(after, upload, sizeof(upload)) == 0);
    ID3D11DepthStencilView_Release(dsv);
    ID3D11Texture2D_Release(texture);
}
static void test_d24(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 4, .Height = 3, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_D24_UNORM_S8_UINT,
        .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_DEPTH_STENCIL,
    };
    ID3D11Texture2D *texture = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreateDepthStencilView(
        device, (ID3D11Resource *)texture, NULL, &dsv);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_ClearDepthStencilView(
        context, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 0x44);
    PGRAPHD3D11State renderer = {.device = device, .context = context};
    PGRAPHD3D11RenderTarget target = {
        .texture = texture, .format = DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    uint32_t before[12], after[12];
    read_d24(device, context, texture, before);
    Error *error = NULL;
    error_reported = 0;
    assert(d3d11_clear_depth_stencil_region_cpu(
        &renderer, &target, (D3D11_RECT){1, 1, 3, 2},
        NV097_CLEAR_SURFACE_Z, 0.75f, 0, &error));
    assert(!error_reported);
    read_d24(device, context, texture, after);
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            unsigned int i = y * 4 + x;
            bool changed = y == 1 && x >= 1 && x < 3;
            assert((after[i] >> 24) == (before[i] >> 24));
            assert((after[i] & 0x00ffffffU) ==
                   (changed ? depth24(0.75f) : before[i] & 0x00ffffffU));
        }
    }

    uint32_t before_stencil[12];
    memcpy(before_stencil, after, sizeof(after));
    assert(d3d11_clear_depth_stencil_region_cpu(
        &renderer, &target, (D3D11_RECT){0, 0, 1, 1},
        NV097_CLEAR_SURFACE_STENCIL, 0.0f, 0xa5, &error));
    read_d24(device, context, texture, after);
    for (unsigned int i = 0; i < 12; i++) {
        uint32_t expected = i == 0 ?
            ((before_stencil[i] & 0x00ffffffU) | 0xa5000000U) :
            before_stencil[i];
        assert(after[i] == expected);
    }

    uint32_t before_both[12];
    memcpy(before_both, after, sizeof(after));
    assert(d3d11_clear_depth_stencil_region_cpu(
        &renderer, &target, (D3D11_RECT){2, 0, 4, 1},
        NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL,
        0.5f, 0x7f, &error));
    read_d24(device, context, texture, after);
    for (unsigned int i = 0; i < 12; i++) {
        bool changed = i == 2 || i == 3;
        uint32_t expected = changed ?
            ((before_both[i] & 0x00ffffffU) | 0x7f000000U) :
            before_both[i];
        if (changed) {
            expected = (expected & 0xff000000U) | depth24(0.5f);
        }
        assert(after[i] == expected);
    }
    ID3D11DepthStencilView_Release(dsv);
    ID3D11Texture2D_Release(texture);
}
static void test_d32(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 4, .Height = 3, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_D32_FLOAT,
        .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_DEPTH_STENCIL,
    };
    ID3D11Texture2D *texture = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreateDepthStencilView(
        device, (ID3D11Resource *)texture, NULL, &dsv);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_ClearDepthStencilView(context, dsv,
                                               D3D11_CLEAR_DEPTH, 0.25f, 0);
    PGRAPHD3D11State renderer = {.device = device, .context = context};
    PGRAPHD3D11RenderTarget target = {
        .texture = texture, .format = DXGI_FORMAT_D32_FLOAT,
    };
    float before[12], after[12];
    read_d32(device, context, texture, before);
    Error *error = NULL;
    error_reported = 0;
    assert(d3d11_clear_depth_stencil_region_cpu(
        &renderer, &target, (D3D11_RECT){1, 0, 3, 2},
        NV097_CLEAR_SURFACE_Z, 0.75f, 0, &error));
    assert(!error_reported);
    read_d32(device, context, texture, after);
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            unsigned int i = y * 4 + x;
            bool changed = y < 2 && x >= 1 && x < 3;
            assert(after[i] == (changed ? 0.75f : before[i]));
        }
    }
    ID3D11DepthStencilView_Release(dsv);
    ID3D11Texture2D_Release(texture);
}
static void test_b5g5r5a1(ID3D11Device *device,
                          ID3D11DeviceContext *context)
{
    UINT support = 0;
    const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                         D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                         D3D11_FORMAT_SUPPORT_BLENDABLE |
                         D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                         D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
    HRESULT hr = ID3D11Device_CheckFormatSupport(
        device, DXGI_FORMAT_B5G5R5A1_UNORM, &support);
    if (FAILED(hr) || (support & required) != required) {
        puts("SKIP: WARP does not expose B5G5R5A1 as sampleable/blendable RTV");
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 4, .Height = 3, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_B5G5R5A1_UNORM,
        .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    ID3D11Texture2D *texture = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)texture, NULL, &rtv);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreateShaderResourceView(
        device, (ID3D11Resource *)texture, NULL, &srv);
    assert(SUCCEEDED(hr));
    const float low_alpha[4] = {1.0f, 0.5f, 0.25f, 0.25f};
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, low_alpha);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint16_t alpha_zero = (31U << 10) | (16U << 5) | 8U;
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            uint16_t pixel;
            memcpy(&pixel, (uint8_t *)mapped.pData + y * mapped.RowPitch +
                               x * 2,
                   sizeof(pixel));
            assert(pixel == alpha_zero);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    const float high_alpha[4] = {1.0f, 0.5f, 0.25f, 0.75f};
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, high_alpha);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)texture);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint16_t alpha_one = alpha_zero | 0x8000U;
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 4; x++) {
            uint16_t pixel;
            memcpy(&pixel, (uint8_t *)mapped.pData + y * mapped.RowPitch +
                               x * 2,
                   sizeof(pixel));
            assert(pixel == alpha_one);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(texture);
    puts("PASS: WARP B5G5R5A1 RTV/SRV clear quantizes alpha to one bit");
}
int main(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                   0, D3D11_SDK_VERSION, &device, &level,
                                   &context);
    assert(SUCCEEDED(hr));
    if (ID3D11Device_GetFeatureLevel(device) < D3D_FEATURE_LEVEL_10_1) {
        puts("SKIP: WARP feature level below 10_1 for DSV CopyResource");
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return 0;
    }
    test_d24(device, context);
    test_d32(device, context);
    test_d16(device, context);
    test_b5g5r5a1(device, context);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: WARP partial D16/D24S8/D32 clear and D16 upload/readback");
    return 0;
}
'''

if sys.platform == "win32":
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-clear-warp-") as directory:
        cfile = pathlib.Path(directory) / "clear-warp.c"
        exe = pathlib.Path(directory) / "clear-warp.exe"
        cfile.write_text(WARP_FIXTURE, encoding="utf-8")
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare", str(cfile), "-o", str(exe),
                        "-ld3d11"], check=True)
        subprocess.run([str(exe)], check=True)
else:
    print("SKIP: D3D11 WARP clear fixture requires Windows/MSYS2")
