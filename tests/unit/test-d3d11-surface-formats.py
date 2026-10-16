#!/usr/bin/env python3
"""Validate D3D11 render-target surface formats before allocation."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"
REGS = ROOT / "hw/xbox/nv2a/nv2a_regs.h"
COLOR_NAMES = (
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
ZETA_NAMES = (
    "NV097_SET_SURFACE_FORMAT_ZETA_Z16",
    "NV097_SET_SURFACE_FORMAT_ZETA_Z24S8",
)


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
    raise ValueError(f"unterminated function: {signature}")


def main():
    source = SOURCE.read_text(encoding="utf-8")
    registers = REGS.read_text(encoding="utf-8")
    macros = []
    values = {}
    for name in COLOR_NAMES + ZETA_NAMES:
        match = re.search(
            rf"^\s*#\s*define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)",
            registers, re.MULTILINE)
        assert match, f"missing register definition: {name}"
        value = int(match.group(1), 0)
        macros.append(f"#define {name} {value}\n")
        values[name] = value
    for name in (
        "NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE",
        "NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE",
        "NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE",
        "NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE",
    ):
        match = re.search(
            rf"^\s*#\s*define\s+{name}\s+\(1\s*<<\s*(\d+)\)",
            registers, re.MULTILINE)
        assert match, f"missing register definition: {name}"
        macros.append(f"#define {name} (1u << {match.group(1)})\n")

    color_helper = extract_function(
        source, "static bool d3d11_surface_color_format_supported(")
    color_target_format = extract_function(
        source, "static DXGI_FORMAT d3d11_color_target_format(")
    color_target_bpp = extract_function(
        source, "static unsigned int d3d11_color_target_bytes_per_pixel(")
    zeta_helper = extract_function(
        source, "static bool d3d11_surface_zeta_format_supported(")
    expand_5 = extract_function(source, "static uint8_t d3d11_expand_5_to_8(")
    expand_6 = extract_function(source, "static uint8_t d3d11_expand_6_to_8(")
    convert_to_bgra8 = extract_function(
        source, "static void d3d11_convert_color_to_bgra8(")
    convert_from_bgra8 = extract_function(
        source, "static void d3d11_convert_bgra8_to_color(")
    color_format_mask_helper = extract_function(
        source, "static uint8_t d3d11_surface_color_format_mask(")
    color_write_mask_helper = extract_function(
        source, "static uint8_t d3d11_surface_color_write_mask(")
    z16_depth_format = extract_function(
        source, "static DXGI_FORMAT d3d11_z16_depth_format(")
    depth_staging_format = extract_function(
        source, "static DXGI_FORMAT d3d11_depth_staging_format(")
    dimensions_helper = extract_function(
        source, "static void d3d11_surface_dimensions(")
    overlap_helper = extract_function(
        source, "static bool d3d11_ranges_overlap(")
    storage_range_helper = extract_function(
        source, "static bool d3d11_target_storage_range(")
    overlap_validation = extract_function(
        source, "static bool d3d11_validate_active_target_overlap(")
    target_matches = extract_function(
        source, "static bool d3d11_target_matches(")
    target_update = extract_function(
        source, "bool pgraph_d3d11_render_targets_update(")
    create_target = extract_function(source, "static bool d3d11_create_target(")
    upload_color = extract_function(source, "static void d3d11_upload_color(")
    upload_depth = extract_function(
        source, "static bool d3d11_upload_depth_stencil(")
    flush_color = extract_function(source, "static bool d3d11_flush_color(")
    store_color = extract_function(source, "static void d3d11_store_color(")
    store_depth = extract_function(
        source, "static void d3d11_store_depth_stencil(")
    assert "ID3D11Device_CheckFormatSupport" in create_target
    assert "D3D11_FORMAT_SUPPORT_BLENDABLE" in color_target_format
    assert "D3D11_FORMAT_SUPPORT_RENDER_TARGET" in color_target_format
    assert "D3D11_FORMAT_SUPPORT_SHADER_SAMPLE" in color_target_format
    assert "D3D11_FORMAT_SUPPORT_CPU_LOCKABLE" in color_target_format
    assert "DXGI_FORMAT_B5G5R5A1_UNORM" in upload_color
    assert "DXGI_FORMAT_B5G5R5A1_UNORM" in flush_color
    assert "d3d11_z16_depth_format(pg->surface_shape.z_format)" in create_target
    assert "target->surface_type == pg->surface_type" in target_matches
    assert "target->storage_length == storage_length" in target_matches
    assert "d3d11_target_matches(target, color, pg, surface, address," \
        in create_target
    assert create_target.index("target->surface_type = pg->surface_type;") < \
        create_target.rindex("d3d11_upload_color(d)")
    color_flush_transition = (
        "if (color && target->texture && pg->surface_color.draw_dirty &&")
    assert color_flush_transition in create_target
    assert create_target.index(color_flush_transition) < create_target.index(
        "d3d11_release_target(target)")
    for store in (store_color, store_depth):
        assert "target->surface_type == " \
            "NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE" in store
        assert "pg->surface_type" not in store
    for upload in (upload_color, upload_depth):
        assert "target->surface_type == " \
            "NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE" in upload
        assert "pg->surface_type" not in upload
    color_guard = "d3d11_surface_color_format_supported(nv2a_format)"
    zeta_guard = "d3d11_surface_zeta_format_supported(nv2a_format)"
    for guard in (color_guard, zeta_guard):
        assert guard in create_target
        assert create_target.index(guard) < create_target.index(
            "d3d11_color_bytes_per_pixel(nv2a_format)")
        assert create_target.index(guard) < create_target.index(
            "d3d11_target_address(")
    assert "unsupported color surface format" in create_target
    assert "unsupported zeta surface format" in create_target
    validation_call = "d3d11_validate_active_target_overlap(d, color, zeta, errp)"
    assert validation_call in target_update
    assert target_update.index(validation_call) < target_update.index(
        "d3d11_create_target(d, true")
    assert target_update.index(validation_call) < target_update.index(
        "d3d11_create_target(d, false")
    assert "d3d11_ranges_overlap(color_address, color_length, zeta_address," \
        in overlap_validation
    target_address = extract_function(
        source, "static bool d3d11_target_address(")
    assert "row_pitch > UINT64_MAX / height" in storage_range_helper
    assert "length > SIZE_MAX" in storage_range_helper
    assert "offset > UINT64_MAX - length" in storage_range_helper
    assert "d3d11_target_storage_range(width, height, surface->pitch, bpp," \
        in target_address

    preamble = r"""
#include <assert.h>
#include <stdbool.h>
#define D3D11_FORMAT_SUPPORT_TEXTURE2D (1u << 0)
#define D3D11_FORMAT_SUPPORT_RENDER_TARGET (1u << 1)
#define D3D11_FORMAT_SUPPORT_BLENDABLE (1u << 2)
#define D3D11_FORMAT_SUPPORT_SHADER_SAMPLE (1u << 3)
#define D3D11_FORMAT_SUPPORT_CPU_LOCKABLE (1u << 4)
typedef enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN,
    DXGI_FORMAT_D16_UNORM,
    DXGI_FORMAT_R16_TYPELESS,
    DXGI_FORMAT_D24_UNORM_S8_UINT,
    DXGI_FORMAT_R24G8_TYPELESS,
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_R32_TYPELESS,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B5G5R5A1_UNORM,
} DXGI_FORMAT;
"""
    color_values = ",\n        ".join(
        str(values[name]) for name in COLOR_NAMES)
    zeta_values = ", ".join(str(values[name]) for name in ZETA_NAMES)
    main_source = f"""
int main(void)
{{
    static const unsigned int valid_color[] = {{
        {color_values}
    }};
    static const unsigned int valid_zeta[] = {{{zeta_values}}};
    for (unsigned int format = 0; format < 16; format++) {{
        bool known_color = false;
        for (unsigned int i = 0;
             i < sizeof(valid_color) / sizeof(valid_color[0]); i++) {{
            known_color |= format == valid_color[i];
        }}
        bool known_zeta = false;
        for (unsigned int i = 0;
             i < sizeof(valid_zeta) / sizeof(valid_zeta[0]); i++) {{
            known_zeta |= format == valid_zeta[i];
        }}
        assert(d3d11_surface_color_format_supported(format) == known_color);
        assert(d3d11_surface_zeta_format_supported(format) == known_zeta);
    }}
    assert(!d3d11_surface_color_format_supported(16));
    assert(!d3d11_surface_zeta_format_supported(16));
    assert(d3d11_z16_depth_format(false) == DXGI_FORMAT_D16_UNORM);
    assert(d3d11_z16_depth_format(true) == DXGI_FORMAT_D32_FLOAT);
    assert(d3d11_depth_staging_format(DXGI_FORMAT_D16_UNORM) ==
           DXGI_FORMAT_R16_TYPELESS);
    assert(d3d11_depth_staging_format(DXGI_FORMAT_D24_UNORM_S8_UINT) ==
           DXGI_FORMAT_R24G8_TYPELESS);
    assert(d3d11_depth_staging_format(DXGI_FORMAT_D32_FLOAT) ==
           DXGI_FORMAT_R32_TYPELESS);
    const unsigned int x1_support =
        D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_RENDER_TARGET |
        D3D11_FORMAT_SUPPORT_BLENDABLE |
        D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
        D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
    assert(d3d11_color_target_format(
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
               x1_support, false) == DXGI_FORMAT_B5G5R5A1_UNORM);
    assert(d3d11_color_target_format(
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
               x1_support & ~D3D11_FORMAT_SUPPORT_BLENDABLE, false) ==
           DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(d3d11_color_target_format(
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
               x1_support & ~D3D11_FORMAT_SUPPORT_CPU_LOCKABLE, false) ==
           DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(d3d11_color_target_format(
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
               x1_support, true) == DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(d3d11_color_target_format(
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5,
               x1_support, false) == DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(d3d11_color_target_bytes_per_pixel(
               DXGI_FORMAT_B5G5R5A1_UNORM) == 2);
    assert(d3d11_color_target_bytes_per_pixel(
               DXGI_FORMAT_B8G8R8A8_UNORM) == 4);
    return 0;
}}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-surface-formats-") as tmp:
        c_file = pathlib.Path(tmp) / "surface-formats.c"
        c_file.write_text(
            preamble + "".join(macros) + color_helper + "\n\n" +
            color_target_format + "\n\n" + color_target_bpp + "\n\n" +
            zeta_helper + "\n\n" + z16_depth_format + "\n\n" +
            depth_staging_format + main_source,
            encoding="utf-8")
        executable = pathlib.Path(tmp) / "surface-formats"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    channel_fixture = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define D3D11_COLOR_WRITE_ENABLE_RED 0x1
#define D3D11_COLOR_WRITE_ENABLE_GREEN 0x2
#define D3D11_COLOR_WRITE_ENABLE_BLUE 0x4
#define D3D11_COLOR_WRITE_ENABLE_ALPHA 0x8
""" + "".join(macros) + expand_5 + "\n" + expand_6 + "\n" + \
        convert_to_bgra8 + "\n" + convert_from_bgra8 + "\n" + \
        color_format_mask_helper + "\n" + color_write_mask_helper + r"""
int main(void)
{
    const uint8_t b8 = 0x42;
    uint8_t bgra[4] = {0};
    uint8_t b8_readback = 0;
    d3d11_convert_color_to_bgra8(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_B8, bgra, 4, &b8, 1, 1, 1);
    assert(bgra[0] == 0 && bgra[1] == 0 && bgra[2] == b8 && bgra[3] == 0xff);
    d3d11_convert_bgra8_to_color(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_B8, &b8_readback, 1, bgra, 4, 1, 1);
    assert(b8_readback == b8);

    const uint8_t g8b8[2] = {0x12, 0x34};
    uint8_t g8b8_readback[2] = {0};
    d3d11_convert_color_to_bgra8(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8, bgra, 4, g8b8, 2, 1, 1);
    assert(bgra[0] == 0 && bgra[1] == g8b8[1] &&
           bgra[2] == g8b8[0] && bgra[3] == 0xff);
    d3d11_convert_bgra8_to_color(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8, g8b8_readback, 2,
        bgra, 4, 1, 1);
    assert(g8b8_readback[0] == g8b8[0] && g8b8_readback[1] == g8b8[1]);

    const uint8_t x1z_alpha_one[2] = {0x00, 0x80};
    uint8_t x1_readback[2] = {0};
    d3d11_convert_color_to_bgra8(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
        bgra, 4, x1z_alpha_one, 2, 1, 1);
    assert(bgra[3] == 0xff);
    d3d11_convert_bgra8_to_color(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5,
        x1_readback, 2, bgra, 4, 1, 1);
    assert(x1_readback[0] == x1z_alpha_one[0] &&
           x1_readback[1] == x1z_alpha_one[1]);

    const uint8_t x8z[4] = {0x11, 0x22, 0x33, 0x7f};
    uint8_t x8_readback[4] = {0};
    d3d11_convert_color_to_bgra8(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8,
        bgra, 4, x8z, 4, 1, 1);
    assert(memcmp(bgra, x8z, sizeof(x8z)) == 0);
    d3d11_convert_bgra8_to_color(
        NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8,
        x8_readback, 4, bgra, 4, 1, 1);
    assert(memcmp(x8_readback, x8z, sizeof(x8z)) == 0);

    uint32_t all_write = NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                         NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                         NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                         NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    assert(d3d11_surface_color_write_mask(
               all_write, NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5) == 0x7);
    assert(d3d11_surface_color_write_mask(
               all_write, NV097_SET_SURFACE_FORMAT_COLOR_LE_B8) == 0x1);
    assert(d3d11_surface_color_write_mask(
               all_write, NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8) == 0x3);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5) == 0xf);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5) == 0x7);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) == 0xf);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) == 0x7);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8) ==
           0xf);
    assert(d3d11_surface_color_write_mask(
               all_write,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8) ==
           0xf);
    assert(d3d11_surface_color_write_mask(
               all_write, NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8) == 0xf);
    assert(d3d11_surface_color_write_mask(
               NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                   NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_B8) == 0);
    assert(d3d11_surface_color_write_mask(
               NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE |
                   NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8) == 0);
    assert(d3d11_surface_color_write_mask(
               all_write & ~NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE,
               NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) == 0x7);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-channel-conversion-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "channel-conversion.c"
        c_file.write_text(channel_fixture, encoding="utf-8")
        executable = tmp / "channel-conversion"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    dimensions_fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE 1
typedef struct SurfaceShape {
    uint32_t log_width, log_height;
    uint32_t clip_x, clip_y, clip_width, clip_height;
} SurfaceShape;
typedef struct PGRAPHState {
    uint32_t surface_type, aa_factor, scale_factor;
    SurfaceShape surface_shape;
} PGRAPHState;
static void pgraph_apply_anti_aliasing_factor(PGRAPHState *pg,
                                               uint32_t *width,
                                               uint32_t *height)
{
    *width *= pg->aa_factor;
    *height *= pg->aa_factor;
}
static void pgraph_apply_scaling_factor(PGRAPHState *pg, uint32_t *width,
                                        uint32_t *height)
{
    *width *= pg->scale_factor;
    *height *= pg->scale_factor;
}
""" + dimensions_helper + r"""
int main(void)
{
    PGRAPHState pg = {
        .surface_type = 0,
        .aa_factor = 2,
        .scale_factor = 3,
        .surface_shape = {
            .clip_x = 5, .clip_y = 7,
            .clip_width = 320, .clip_height = 240,
        },
    };
    uint32_t guest_width, guest_height, width, height;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height, &width, &height);
    assert(guest_width == 645 && guest_height == 487);
    assert(width == 1935 && height == 1461);

    pg.aa_factor = 1;
    pg.scale_factor = 1;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height, &width, &height);
    assert(guest_width == 325 && guest_height == 247);

    pg.aa_factor = 4;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height, &width, &height);
    assert(guest_width == 1285 && guest_height == 967);

    pg.surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
    pg.surface_shape.log_width = 8;
    pg.surface_shape.log_height = 7;
    pg.aa_factor = 2;
    pg.scale_factor = 3;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height, &width, &height);
    assert(guest_width == 512 && guest_height == 256);
    assert(width == 1536 && height == 768);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-surface-dimensions-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "surface-dimensions.c"
        c_file.write_text(dimensions_fixture, encoding="utf-8")
        executable = tmp / "surface-dimensions"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    overlap_fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t hwaddr;
""" + overlap_helper + r"""
int main(void)
{
    assert(d3d11_ranges_overlap(100, 10, 109, 2));
    assert(d3d11_ranges_overlap(100, 10, 100, 10));
    assert(d3d11_ranges_overlap(100, 10, 102, 2));
    assert(d3d11_ranges_overlap(UINT64_MAX - 4, 8, UINT64_MAX - 1, 1));
    assert(d3d11_ranges_overlap(UINT64_MAX - 1, 1, UINT64_MAX - 4, 8));
    assert(!d3d11_ranges_overlap(100, 10, 110, 1));
    assert(!d3d11_ranges_overlap(UINT64_MAX - 4, 2, UINT64_MAX - 1, 1));
    assert(!d3d11_ranges_overlap(100, 0, 100, 10));
    assert(!d3d11_ranges_overlap(100, 10, 100, 0));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-target-overlap-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "target-overlap.c"
        c_file.write_text(overlap_fixture, encoding="utf-8")
        executable = tmp / "target-overlap"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    storage_fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef uint64_t hwaddr;
""" + storage_range_helper + r"""
int main(void)
{
    size_t length = 0;
    uint64_t end = 0;
    assert(d3d11_target_storage_range(640, 480, 0, 4, 0x100,
                                     &length, &end));
    assert(length == 640U * 480U * 4U);
    assert(end == 0x100 + length);
    assert(d3d11_target_storage_range(3, 2, 16, 4, 0x100,
                                     &length, &end));
    assert(length == 32 && end == 0x120);
    assert(!d3d11_target_storage_range(UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                      4, 0, &length, &end));
    assert(!d3d11_target_storage_range(1, 1, 4, 4, UINT64_MAX - 2,
                                      &length, &end));
    assert(!d3d11_target_storage_range(1, 1, 4, 0, 0,
                                      &length, &end));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-target-storage-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "target-storage.c"
        c_file.write_text(storage_fixture, encoding="utf-8")
        executable = tmp / "target-storage"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    target_match_fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t hwaddr;
typedef enum DXGI_FORMAT {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_B5G5R5A1_UNORM,
} DXGI_FORMAT;
typedef struct SurfaceShape { unsigned int z_format; } SurfaceShape;
typedef struct PGRAPHState {
    unsigned int surface_type;
    SurfaceShape surface_shape;
} PGRAPHState;
typedef struct Surface { uint32_t pitch; } Surface;
typedef struct PGRAPHD3D11RenderTarget {
    void *texture;
    hwaddr vram_address;
    uint32_t width, height, guest_width, guest_height, pitch;
    size_t storage_length;
    unsigned int surface_type;
    DXGI_FORMAT format;
    unsigned int nv2a_format;
    bool z_format;
} PGRAPHD3D11RenderTarget;
""" + target_matches + r"""
#define MATCH_TARGET(is_color) \
    d3d11_target_matches(&target, (is_color), &pg, &surface, 0x1000, \
                         614400, 640, 480, 320, 240, \
                         DXGI_FORMAT_B8G8R8A8_UNORM, 7)
int main(void)
{
    PGRAPHD3D11RenderTarget target = {
        .texture = (void *)1,
        .vram_address = 0x1000,
        .width = 640, .height = 480,
        .guest_width = 320, .guest_height = 240,
        .pitch = 1280, .storage_length = 614400,
        .surface_type = 0,
        .format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .nv2a_format = 7,
        .z_format = false,
    };
    PGRAPHState pg = { .surface_type = 0,
                       .surface_shape = { .z_format = false } };
    Surface surface = { .pitch = 1280 };
    assert(MATCH_TARGET(true));

    target.texture = NULL;
    assert(!MATCH_TARGET(true));
    target.texture = (void *)1;
#define CHECK_MISMATCH(field, value) do { \
    __typeof__(target.field) saved = target.field; \
    target.field = (value); \
    assert(!MATCH_TARGET(true)); \
    target.field = saved; \
} while (0)
    CHECK_MISMATCH(vram_address, 0x1004);
    CHECK_MISMATCH(width, 641);
    CHECK_MISMATCH(height, 481);
    CHECK_MISMATCH(guest_width, 321);
    CHECK_MISMATCH(guest_height, 241);
    CHECK_MISMATCH(pitch, 1284);
    CHECK_MISMATCH(storage_length, 614404);
    CHECK_MISMATCH(surface_type, 1);
    CHECK_MISMATCH(format, DXGI_FORMAT_B5G5R5A1_UNORM);
    CHECK_MISMATCH(nv2a_format, 8);
#undef CHECK_MISMATCH

    surface.pitch++;
    assert(!MATCH_TARGET(true));
    surface.pitch--;
    pg.surface_type++;
    assert(!MATCH_TARGET(true));
    pg.surface_type--;

    pg.surface_shape.z_format = true;
    assert(MATCH_TARGET(true)); /* color target identity ignores z format */
    assert(!MATCH_TARGET(false));
    target.z_format = true;
    assert(MATCH_TARGET(false));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-target-match-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "target-match.c"
        c_file.write_text(target_match_fixture, encoding="utf-8")
        executable = tmp / "target-match"
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    surface_store_fixture = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE 1
typedef struct PGRAPHD3D11RenderTarget {
    uint32_t guest_width, guest_height, pitch;
    unsigned int surface_type;
} PGRAPHD3D11RenderTarget;
static unsigned int swizzle_calls;
static void swizzle_rect(const uint8_t *source, uint32_t width,
                         uint32_t height, uint8_t *destination,
                         size_t source_pitch, unsigned int bpp)
{
    swizzle_calls++;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(destination + y * source_pitch,
               source + y * source_pitch, width * bpp);
    }
}
""" + store_color + "\n" + store_depth + r"""
int main(void)
{
    PGRAPHD3D11RenderTarget target = {
        .guest_width = 2, .guest_height = 2, .pitch = 6,
        .surface_type = 0,
    };
    const uint8_t linear[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t color_vram[12];
    memset(color_vram, 0xcc, sizeof(color_vram));
    d3d11_store_color(&target, color_vram, linear, 2);
    assert(swizzle_calls == 0);
    assert(memcmp(color_vram, linear, 4) == 0);
    assert(color_vram[4] == 0xcc && color_vram[5] == 0xcc);
    assert(memcmp(color_vram + 6, linear + 4, 4) == 0);

    uint8_t depth_vram[12];
    memset(depth_vram, 0xcc, sizeof(depth_vram));
    d3d11_store_depth_stencil(&target, depth_vram, linear, 2);
    assert(memcmp(depth_vram, linear, 4) == 0);
    assert(depth_vram[4] == 0xcc && depth_vram[5] == 0xcc);
    assert(memcmp(depth_vram + 6, linear + 4, 4) == 0);

    target.surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
    memset(color_vram, 0, sizeof(color_vram));
    d3d11_store_color(&target, color_vram, linear, 2);
    assert(swizzle_calls == 1);
    assert(memcmp(color_vram, linear, 8) == 0);
    memset(depth_vram, 0, sizeof(depth_vram));
    d3d11_store_depth_stencil(&target, depth_vram, linear, 2);
    assert(swizzle_calls == 2);
    assert(memcmp(depth_vram, linear, 8) == 0);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-surface-store-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "surface-store.c"
        c_file.write_text(surface_store_fixture, encoding="utf-8")
        executable = tmp / "surface-store"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: ten color/two ZETA formats accepted; reserved codes rejected before allocation")
    print("PASS: linear clip extents receive AA before unscaled origin; "
          "swizzled dimensions preserved")
    print("PASS: overlapping active color/ZETA VRAM ranges are rejected before target allocation")
    print("PASS: target storage range preserves pitch and rejects uint64 overflow")
    print("PASS: B8/G8B8 preserve GL_R8/GL_RG8 logical channels through BGRA8")
    print("PASS: X1/Z1 and X8/Z8 preserve OpenGL alpha through upload/readback and write masks")
    print("PASS: X1 selects packed B5G5R5A1 only with all RT/blend/sample/"
          "CPU-lock flags and no logic op")
    print("PASS: dirty color targets flush before replacement and store using "
          "their captured linear/swizzled layout")


if __name__ == "__main__":
    main()
