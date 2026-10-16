#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Portable regressions for the D3D11 texture filter helpers.

Extracts the production C helpers verbatim, compiles/runs them with GCC, and
optionally syntax-compiles their generated HLSL with glslangValidator. It is not
a substitute for D3DCompile, WARP rasterization, or Xbox pixel comparisons.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "hw/xbox/nv2a/pgraph/d3d11/shaders.c"
TEXTURES = ROOT / "hw/xbox/nv2a/pgraph/d3d11/textures.c"
REGS = ROOT / "hw/xbox/nv2a/nv2a_regs.h"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    i = brace
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "line-comment":
            if ch == "\n":
                state = "code"
        elif state == "block-comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (
                state == "char" and ch == "'"
            ):
                state = "code"
        elif ch == "/" and nxt == "/":
            state = "line-comment"
            i += 1
        elif ch == "/" and nxt == "*":
            state = "block-comment"
            i += 1
        elif ch == '"':
            state = "string"
        elif ch == "'":
            state = "char"
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    raise ValueError(f"unterminated function: {signature}")


def register_constant(name: str) -> int:
    text = REGS.read_text(encoding="utf-8")
    match = re.search(r"^\s*#\s*define\s+" + re.escape(name) + r"\s+(\d+)\s*$", text, re.M)
    if not match:
        raise AssertionError(f"missing register constant {name}")
    return int(match.group(1))


def main() -> None:
    shader_text = SHADERS.read_text(encoding="utf-8")
    texture_text = TEXTURES.read_text(encoding="utf-8")
    border_helper = extract_function(
        shader_text, "static void d3d11_ps_append_border_adjustment("
    )
    convolution_helper = extract_function(
        shader_text, "static bool d3d11_ps_append_convolution_filter("
    )
    clamp_helper = extract_function(
        shader_text, "static void d3d11_ps_append_clamp_ogl_helpers("
    )
    snorm_state_helper = extract_function(
        shader_text, "static void d3d11_ps_set_snorm_textures("
    )
    texture_format_helper = extract_function(
        texture_text, "static DXGI_FORMAT d3d11_texture_dxgi_format("
    )
    filter_helper = extract_function(texture_text, "static D3D11_FILTER d3d11_filter(")
    address_helper = extract_function(
        texture_text, "static D3D11_TEXTURE_ADDRESS_MODE d3d11_address_mode("
    )
    mip_helper = extract_function(texture_text, "static bool d3d11_mipmap_enabled(")
    mip_levels_helper = extract_function(
        texture_text, "static unsigned int d3d11_effective_mip_levels("
    )
    min_mip_helper = extract_function(
        texture_text, "static unsigned int d3d11_effective_min_mipmap_level("
    )

    constants = {
        name: register_constant(name)
        for name in (
            "NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0",
            "NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0",
            "NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD",
            "NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD",
            "NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD",
            "NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD",
            "NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0",
        )
    }
    defines = "\n".join(f"#define {name} {value}" for name, value in constants.items())
    c_source = f"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <glib.h>
{defines}
#define CONVOLUTION_FILTER_DISABLED 0
enum ConvolutionFilter {{
    CONVOLUTION_FILTER_QUINCUNX = 1,
    CONVOLUTION_FILTER_GAUSSIAN = 2,
}};
typedef unsigned int D3D11_FILTER;
typedef unsigned int D3D11_TEXTURE_ADDRESS_MODE;
typedef unsigned int DXGI_FORMAT;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5 23
#define NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16 24
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define DXGI_FORMAT_R8G8B8A8_SNORM 31
#define DXGI_FORMAT_R16G16B16A16_UNORM 11
#define D3D11_TEXTURE_ADDRESS_CLAMP 1
#define D3D11_TEXTURE_ADDRESS_WRAP 2
#define D3D11_TEXTURE_ADDRESS_MIRROR 3
#define D3D11_TEXTURE_ADDRESS_BORDER 4
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NV2A_MAX_TEXTURES 4
typedef struct Error {{ char message[160]; }} Error;
typedef struct TextureShape {{
    unsigned int color_format, levels, width, height;
    bool cubemap, border;
}} TextureShape;
typedef struct PGRAPHState {{
    bool enabled[4]; unsigned int format[4]; TextureShape shape[4];
}} PGRAPHState;
typedef struct PshState {{
    enum ConvolutionFilter conv_tex[4];
    unsigned int dim_tex[4];
    bool tex_cubemap[4];
    bool snorm_tex[4];
    bool clamp_ogl[4][3];
    float border_logical_size[4][3];
    float border_inv_real_size[4][3];
}} PshState;
static bool pgraph_is_texture_enabled(PGRAPHState *pg, unsigned int stage) {{
    return pg->enabled[stage];
}}
static TextureShape pgraph_get_texture_shape(PGRAPHState *pg, unsigned int stage) {{
    TextureShape shape = pg->shape[stage];
    shape.color_format = pg->format[stage];
    return shape;
}}
static bool d3d11_is_depth_texture_format(unsigned int format) {{
    return format == 100;
}}
static void error_setg(Error **errp, const char *format, ...) {{
    if (!errp || *errp) return;
    *errp = calloc(1, sizeof(**errp));
    va_list args;
    va_start(args, format);
    vsnprintf((*errp)->message, sizeof((*errp)->message), format, args);
    va_end(args);
}}
{border_helper}
{convolution_helper}
{clamp_helper}
{snorm_state_helper}
{texture_format_helper}
{filter_helper}
{address_helper}
{mip_helper}
{mip_levels_helper}
{min_mip_helper}
int main(void) {{
    assert(d3d11_texture_dxgi_format(NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5)
           == DXGI_FORMAT_R8G8B8A8_SNORM);
    assert(d3d11_texture_dxgi_format(NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16)
           == DXGI_FORMAT_R16G16B16A16_UNORM);
    assert(d3d11_texture_dxgi_format(100) == DXGI_FORMAT_R8G8B8A8_UNORM);
    assert(d3d11_texture_dxgi_format(25) == DXGI_FORMAT_R8G8B8A8_UNORM);

    PshState clamp_state = {{ 0 }};
    clamp_state.clamp_ogl[0][0] = true;
    clamp_state.clamp_ogl[0][1] = true;
    GString *helpers = g_string_new(NULL);
    d3d11_ps_append_clamp_ogl_helpers(helpers, &clamp_state);
    assert(strstr(helpers->str, "coord.x=clamp(coord.x,0.0,1.0)"));
    assert(strstr(helpers->str, "coord.y=clamp(coord.y,0.0,1.0)"));
    assert(!strstr(helpers->str, "coord.z=clamp(coord.z,0.0,1.0)"));
    puts("--HLSL-HELPERS-BEGIN--");
    fputs(helpers->str, stdout);
    puts("--HLSL-HELPERS-END--");
    g_string_free(helpers, TRUE);

    PGRAPHState texture_pg = {{ 0 }};
    texture_pg.enabled[0] = texture_pg.enabled[1] = true;
    texture_pg.format[0] = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5;
    texture_pg.format[1] = 7;
    PshState texture_state = {{ 0 }};
    texture_state.snorm_tex[1] = true;
    d3d11_ps_set_snorm_textures(&texture_pg, &texture_state);
    assert(texture_state.snorm_tex[0]);
    assert(texture_state.snorm_tex[1]);
    assert(!texture_state.snorm_tex[2]);
    texture_pg.shape[1] = (TextureShape) {{
        .cubemap = true, .border = true, .levels = 4, .width = 8, .height = 8,
    }};
    d3d11_ps_set_snorm_textures(&texture_pg, &texture_state);
    assert(texture_state.border_logical_size[1][0] == 8.0f);
    assert(texture_state.border_inv_real_size[1][0] == 1.0f / 16.0f);
    texture_state.tex_cubemap[1] = true;
    GString *cube_adjustment = g_string_new(NULL);
    d3d11_ps_append_border_adjustment(cube_adjustment, &texture_state, 1, "pT1");
    assert(cube_adjustment->len == 0);
    g_string_free(cube_adjustment, TRUE);

    for (unsigned int kernel = CONVOLUTION_FILTER_QUINCUNX;
         kernel <= CONVOLUTION_FILTER_GAUSSIAN; kernel++) {{
        PshState state = {{ 0 }};
        state.conv_tex[0] = (enum ConvolutionFilter)kernel;
        state.dim_tex[0] = 2;
        if (kernel == CONVOLUTION_FILTER_GAUSSIAN) {{
            state.border_logical_size[0][0] = 8.0f;
            state.border_logical_size[0][1] = 4.0f;
            state.border_inv_real_size[0][0] = 1.0f / 16.0f;
            state.border_inv_real_size[0][1] = 1.0f / 8.0f;
        }}
        GString *source = g_string_new(NULL);
        assert(d3d11_ps_append_convolution_filter(source, &state, 0, NULL));
        assert(strstr(source->str, "tex0.GetDimensions(convolutionWidth0,"));
        assert(strstr(source->str, "convolutionBase0=pT0.xy*texCoordScale[0].xy"));
        assert(strstr(source->str, "(convolutionBase0+float2(-1.0,-1.0)*convolutionTexel0)/pT0.w"));
        assert(strstr(source->str, "(4.0/16.0)"));
        unsigned int sample_count = 0;
        const char *cursor = source->str;
        while ((cursor = strstr(cursor, "T0+=NV2A_SAMPLE2(tex0,samp0,0,"))) {{
            sample_count++;
            cursor++;
        }}
        assert(sample_count == 9);
        if (kernel == CONVOLUTION_FILTER_GAUSSIAN) {{
            assert(strstr(source->str, "pT0.xyz=(pT0.xyz*float3(8,4,0)"));
            puts("--HLSL-BEGIN--");
            fputs(source->str, stdout);
            puts("--HLSL-END--");
        }}
        g_string_free(source, TRUE);
    }}
    PshState invalid = {{ 0 }};
    invalid.dim_tex[0] = 3;
    invalid.conv_tex[0] = CONVOLUTION_FILTER_GAUSSIAN;
    Error *error = NULL;
    GString *source = g_string_new(NULL);
    assert(!d3d11_ps_append_convolution_filter(source, &invalid, 0, &error));
    assert(error && strstr(error->message, "2D texture"));
    free(error);
    error = NULL;
    invalid.dim_tex[0] = 2;
    invalid.tex_cubemap[0] = true;
    assert(!d3d11_ps_append_convolution_filter(source, &invalid, 0, &error));
    assert(error && strstr(error->message, "2D texture"));
    free(error);
    g_string_free(source, TRUE);

    assert(d3d11_filter(1, 1) == 0x00);
    assert(d3d11_filter(2, 1) == 0x10);
    assert(d3d11_filter(1, 2) == 0x04);
    assert(d3d11_filter(2, 2) == 0x14);
    assert(d3d11_filter(5, 2) == 0x05);
    assert(d3d11_filter(6, 4) == 0x15);
    assert(d3d11_filter(7, 2) == 0x14);
    for (unsigned int filter = 1; filter <= 7; filter++) {{
        assert(!d3d11_mipmap_enabled(true, filter));
    }}
    assert(!d3d11_mipmap_enabled(false, 1));
    assert(!d3d11_mipmap_enabled(false, 2));
    assert(d3d11_mipmap_enabled(false, 3));
    assert(d3d11_mipmap_enabled(false, 4));
    assert(d3d11_mipmap_enabled(false, 5));
    assert(d3d11_mipmap_enabled(false, 6));
    assert(!d3d11_mipmap_enabled(false, 7));
    assert(d3d11_effective_mip_levels(true, 4) == 1);
    assert(d3d11_effective_mip_levels(true, 0) == 1);
    assert(d3d11_effective_min_mipmap_level(true, 3) == 0);
    assert(d3d11_effective_mip_levels(false, 4) == 4);
    assert(d3d11_effective_min_mipmap_level(false, 3) == 3);
    assert(d3d11_address_mode(5) == D3D11_TEXTURE_ADDRESS_CLAMP);
    assert(d3d11_address_mode(0) == D3D11_TEXTURE_ADDRESS_CLAMP);
    assert(d3d11_address_mode(2) == D3D11_TEXTURE_ADDRESS_MIRROR);
    assert(d3d11_address_mode(4) == D3D11_TEXTURE_ADDRESS_BORDER);
    assert(d3d11_address_mode(3) == D3D11_TEXTURE_ADDRESS_CLAMP);
    assert(d3d11_address_mode(1) == D3D11_TEXTURE_ADDRESS_WRAP);
    assert(d3d11_address_mode(6) == D3D11_TEXTURE_ADDRESS_CLAMP);
    return 0;
}}
"""

    with tempfile.TemporaryDirectory(prefix="d3d11-filter-test-") as tmp:
        tmpdir = Path(tmp)
        c_file = tmpdir / "test.c"
        exe = tmpdir / "test"
        c_file.write_text(c_source, encoding="utf-8")
        pkg_cflags = shlex.split(
            subprocess.check_output(["pkg-config", "--cflags", "glib-2.0"], text=True)
        )
        pkg_libs = shlex.split(
            subprocess.check_output(["pkg-config", "--libs", "glib-2.0"], text=True)
        )
        subprocess.run(
            [os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             *pkg_cflags, str(c_file), "-o", str(exe), *pkg_libs],
            check=True,
        )
        output = subprocess.check_output([str(exe)], text=True)

        update_sampler = extract_function(
            texture_text, "static bool d3d11_update_sampler("
        )
        assert ".MaxLOD = mipmap_enabled ? D3D11_FLOAT32_MAX : 0" in update_sampler
        assert "d3d11_mipmap_enabled(" in update_sampler
        assert "kelvin_color_format_info_map[shape.color_format].linear" in update_sampler
        assert ".Filter = max_anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC" in update_sampler
        assert ".MaxAnisotropy = MIN(max_anisotropy, 16)" in update_sampler
        assert ".MipLODBias = pgraph_convert_lod_bias_to_float(" in update_sampler
        assert "d3d11_address_mode(\n            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU))" in update_sampler
        assert "d3d11_address_mode(\n            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV))" in update_sampler
        assert "d3d11_address_mode(\n            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP))" in update_sampler
        psh_text = (ROOT / "hw/xbox/nv2a/pgraph/glsl/psh.c").read_text(
            encoding="utf-8"
        )
        gl_constants = (ROOT / "hw/xbox/nv2a/pgraph/gl/constants.h").read_text(
            encoding="utf-8"
        )
        assert "GL_CLAMP_TO_EDGE, /* Approximate GL_CLAMP */" in gl_constants
        assert "NV_PGRAPH_TEXADDRESS0_ADDRU_CLAMP_OGL" in psh_text
        assert "!cubemap && GET_MASK(address" in psh_text
        for axis in range(3):
            assert f"state->clamp_ogl[i][{axis}]" in psh_text
        for register_axis in ("ADDRU", "ADDRV", "ADDRP"):
            assert f"NV_PGRAPH_TEXADDRESS0_{register_axis})" in psh_text

        create_texture = extract_function(
            texture_text, "static bool d3d11_create_texture("
        )
        assert "d3d11_effective_mip_levels(linear_format, shape.levels)" in create_texture
        assert "d3d11_effective_min_mipmap_level(" in create_texture
        assert "MostDetailedMip = min_mipmap_level" in create_texture
        assert "MipLevels = levels - min_mipmap_level" in create_texture
        for channel in range(3):
            assert f"rgba[i * 4 + {channel}] = pixel[{channel}];" in texture_text
        assert "rgba[i * 4 + 3] = 127;" in texture_text
        assert "d3d11_encode_biased_snorm_byte" not in texture_text

        warp_harness = (ROOT / "tests/unit/test-d3d11-shaders.c").read_text(
            encoding="utf-8"
        )
        assert "static void test_convolution_pixel_output(" in warp_harness
        assert "test_convolution_pixel_output(device);" in warp_harness
        assert "static void test_clamp_ogl_pixel_output(" in warp_harness
        assert "test_clamp_ogl_pixel_output(device);" in warp_harness
        for expected in (
            "w=1 -> alpha 64",
            "w=1 -> alpha 32",
            "w=1 -> alpha 16",
            "w=2 -> alpha 143",
            "w=2 -> alpha 24",
            "w=2 -> alpha 4",
            "CLAMP_OGL linear u<0 matches edge texel",
            "CLAMP_OGL linear u=0 matches edge texel",
            "CLAMP_OGL linear u=1 matches edge texel",
            "CLAMP_OGL linear u>1 matches edge texel",
            "CLAMP_OGL nearest u<0 clamps to edge texel",
            "CLAMP_OGL nearest u>1 clamps to edge texel",
        ):
            assert expected in warp_harness

        if shutil.which("glslangValidator"):
            helper_source = output.split("--HLSL-HELPERS-BEGIN--\n", 1)[1].split(
                "--HLSL-HELPERS-END--", 1
            )[0]
            snippet = output.split("--HLSL-BEGIN--\n", 1)[1].split("--HLSL-END--", 1)[0]
            hlsl = f"""
cbuffer Nv2aPixelConstants : register(b0) {{ float4 texCoordScale[4]; }};
{helper_source}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 main(float4 pT0 : TEXCOORD0) : SV_Target {{
    float4 T0 = 0;
{snippet}
    return T0;
}}
"""
            hlsl_file = tmpdir / "test.hlsl"
            spv_file = tmpdir / "test.spv"
            hlsl_file.write_text(hlsl, encoding="utf-8")
            subprocess.run(
                ["glslangValidator", "-D", "-V", "-S", "frag", "-e", "main",
                 "--auto-map-bindings", "-o", str(spv_file), str(hlsl_file)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            print("PASS: extracted CLAMP_OGL and convolution HLSL parsed by glslangValidator")
        else:
            print("SKIP: glslangValidator unavailable; HLSL compiler check omitted")

    print("PASS: production convolution/sampler helpers compiled with GCC -Werror")
    print("PASS: R6G5B5 selects native DXGI SNORM; depth/Y16/other DXGI mappings are retained")
    print("PASS: CLAMP_OGL maps to D3D11 CLAMP, matching the OpenGL edge mode")
    print("PASS: convolution weights/projective offsets/border; LOD, mip and anisotropy sampler policy")


if __name__ == "__main__":
    main()
