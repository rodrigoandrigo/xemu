#!/usr/bin/env python3
"""Rasterize both actual D3D11 vertex epilogues using offscreen WARP."""
import json
import math
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "hw/xbox/nv2a/pgraph/d3d11/shaders.c").read_text()
draw = (root / "hw/xbox/nv2a/pgraph/d3d11/draw.c").read_text()


def extract_c_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


cache_version = int(re.search(
    r"^#define D3D11_SHADER_CACHE_VERSION (\d+)$", source, re.MULTILINE
).group(1))
assert cache_version >= 23
fixed_start = source.index("static char *d3d11_translate_fixed_function_vertex(")
fixed_end = source.index("\nstatic ", fixed_start + 1)
fixed_function = source[fixed_start:fixed_end]
point_size_start = source.index("static void d3d11_append_vsh_point_size(")
point_size_end = source.index(
    "\nstatic void d3d11_append_fixed_function_specular_mix(", point_size_start
)
point_size_helper = source[point_size_start:point_size_end]
assert "case D3D11_TEXGEN_NORMAL:" in fixed_function
assert '"  %s.%c=tNormal.%c;\\n"' in fixed_function
reflection_start = source.index(
    "static void d3d11_append_fixed_function_texgen_vectors(")
reflection_end = source.index(
    "static char *d3d11_translate_fixed_function_vertex(", reflection_start)
reflection = source[reflection_start:reflection_end]
assert '"  { float3 n=tNormal;"' in reflection
assert "reflect(u,n)" in reflection
assert "input.v2.xyz" not in reflection
assert "d3d11_append_fixed_function_texgen_vectors(s, tex," in fixed_function
assert "d3d11_append_vsh_fog_factor(" in fixed_function
assert "d3d11_append_vsh_point_size(" in fixed_function
specular_mix_helper = extract_c_function(
    source, "static void d3d11_append_fixed_function_specular_mix(")
assert "d3d11_append_fixed_function_specular_mix(\n            s, state->specular_enable, state->separate_specular)" \
    in fixed_function
assert '"  oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\\n"' in source
assert '"  oPos.xy = round(oPos.xy * 16.0) / 16.0;\\n"' not in source
assert math.trunc(0.1 * 16.0) / 16.0 == 0.0625
assert math.trunc(-0.1 * 16.0) / 16.0 == -0.0625
assert round(0.1 * 16.0) / 16.0 != 0.0625
assert round(-0.1 * 16.0) / 16.0 != -0.0625
program_start = source.index("static char *d3d11_translate_vertex_program(")
program_end = source.index("typedef enum D3D11TexgenComponentKind", program_start)
assert "d3d11_append_vsh_fog_factor(" in source[program_start:program_end]
assert "d3d11_append_programmable_point_size(" in source[program_start:program_end]
assert source.count("output.fog=fogFactor;") == 2
assert "output.fog=oFog.x" not in source
assert "output.fog=input.v5.x" not in source

kind_start = source.index("typedef enum D3D11TexgenComponentKind {")
kind_end = source.index("} D3D11TexgenComponentKind;", kind_start) + len(
    "} D3D11TexgenComponentKind;")
kind_declaration = source[kind_start:kind_end]
helper_start = source.index(
    "static D3D11TexgenComponentKind d3d11_texgen_component_kind(")
helper_open = source.index("{", helper_start)
depth = 0
for position in range(helper_open, len(source)):
    if source[position] == "{":
        depth += 1
    elif source[position] == "}":
        depth -= 1
        if depth == 0:
            helper_end = position + 1
            break
texgen_helper = source[helper_start:helper_end]
vector_start = source.index(
    "static void d3d11_append_fixed_function_texgen_vectors(")
vector_open = source.index("{", vector_start)
depth = 0
for position in range(vector_open, len(source)):
    if source[position] == "{":
        depth += 1
    elif source[position] == "}":
        depth -= 1
        if depth == 0:
            vector_end = position + 1
            break
texgen_vectors = source[vector_start:vector_end]
assert '"    %s.%c=r.%c*invM+0.5;\\n"' in texgen_vectors
assert '"    %s.%c=r.%c;\\n"' in texgen_vectors
fog_start = source.index("static bool d3d11_append_vsh_fog_factor(")
fog_open = source.index("{", fog_start)
depth = 0
for position in range(fog_open, len(source)):
    if source[position] == "{":
        depth += 1
    elif source[position] == "}":
        depth -= 1
        if depth == 0:
            fog_end = position + 1
            break
fog_helper = source[fog_start:fog_end]
key_start = source.index("static void d3d11_append_programmable_fog_key(")
key_open = source.index("{", key_start)
depth = 0
for position in range(key_open, len(source)):
    if source[position] == "{":
        depth += 1
    elif source[position] == "}":
        depth -= 1
        if depth == 0:
            key_end = position + 1
            break
fog_key_helper = source[key_start:key_end]
vsh_key_helper = extract_c_function(
    source, "static void d3d11_append_programmable_vsh_key(")
color_output_helper = extract_c_function(
    source, "static void d3d11_append_vsh_color_outputs(")
vsh_preamble_start = source.index("static const char d3d11_vsh_preamble[] =")
vsh_preamble_end = source.index(";\n", vsh_preamble_start)
vsh_preamble_expression = source[vsh_preamble_start:vsh_preamble_end]
vsh_preamble = "".join(
    json.loads(literal) for literal in
    re.findall(r'"(?:[^"\\]|\\.)*"', vsh_preamble_expression))
vsh_color_hlsl = re.search(
    r"float4 nv_vsh_color\(float4 c\) \{.*?\n\}", vsh_preamble,
    re.DOTALL).group()
assert "d3d11_append_programmable_vsh_key(" in source
for state_field in ("state.fixed_function.foggen", "state.specular_enable",
                    "state.ignore_specular_alpha",
                    "state.point_params_enable", "state.point_size",
                    "state.surface_scale_factor"):
    assert state_field in source[source.index("bool pgraph_d3d11_bind_vertex_shader("):]
assert source.count("d3d11_append_vsh_color_outputs(s,") == 2
assert "nv_vsh_color" in source and "return saturate(n);" in source
vsh_regs = (root / "hw/xbox/nv2a/pgraph/vsh_regs.h").read_text()
enum_start = vsh_regs.index("enum VshTexgen {")
enum_end = vsh_regs.index("};", enum_start) + 2
texgen_enum = vsh_regs[enum_start:enum_end]
fog_mode_start = vsh_regs.index("enum VshFogMode {")
fog_mode_end = vsh_regs.index("};", fog_mode_start) + 2
fog_mode_enum = vsh_regs[fog_mode_start:fog_mode_end]
foggen_start = vsh_regs.index("enum VshFoggen {")
foggen_end = vsh_regs.index("};", foggen_start) + 2
foggen_enum = vsh_regs[foggen_start:foggen_end]
nv2a_regs = (root / "hw/xbox/nv2a/nv2a_regs.h").read_text()
fog_register = int(re.search(
    r"^#define NV_IGRAPH_XF_XFCTX_FOG\s+(0x[0-9a-fA-F]+|[0-9]+)",
    nv2a_regs, re.MULTILINE).group(1), 0)
texgen_test = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
''' + texgen_enum + "\n" + kind_declaration + "\n" + texgen_helper + r'''
''' + texgen_vectors + r'''
int main(void)
{
    for (unsigned int component = 0; component < 4; component++) {
        assert(d3d11_texgen_component_kind(TEXGEN_DISABLE, component) ==
               D3D11_TEXGEN_INPUT);
        assert(d3d11_texgen_component_kind(TEXGEN_EYE_LINEAR, component) ==
               D3D11_TEXGEN_EYE_LINEAR);
        assert(d3d11_texgen_component_kind(TEXGEN_OBJECT_LINEAR, component) ==
               D3D11_TEXGEN_OBJECT_LINEAR);
        assert(d3d11_texgen_component_kind(TEXGEN_NORMAL_MAP, component) ==
               (component < 3 ? D3D11_TEXGEN_NORMAL : D3D11_TEXGEN_INVALID));
        assert(d3d11_texgen_component_kind(TEXGEN_SPHERE_MAP, component) ==
               (component < 2 ? D3D11_TEXGEN_SPHERE : D3D11_TEXGEN_INVALID));
        assert(d3d11_texgen_component_kind(TEXGEN_REFLECTION_MAP, component) ==
               (component < 3 ? D3D11_TEXGEN_REFLECTION :
                                D3D11_TEXGEN_INVALID));
    }
    assert(d3d11_texgen_component_kind(99, 0) == D3D11_TEXGEN_INVALID);
    enum VshTexgen mixed[4] = {
        TEXGEN_SPHERE_MAP, TEXGEN_DISABLE, TEXGEN_REFLECTION_MAP, TEXGEN_DISABLE,
    };
    GString *generated = g_string_new(NULL);
    d3d11_append_fixed_function_texgen_vectors(generated, "oT0", mixed);
    assert(strstr(generated->str, "oT0.x=r.x*invM+0.5;"));
    assert(strstr(generated->str, "oT0.z=r.z;"));
    assert(!strstr(generated->str, "oT0.y="));
    assert(!strstr(generated->str, "oT0.w="));
    g_string_free(generated, TRUE);

    enum VshTexgen sphere_only[4] = {
        TEXGEN_SPHERE_MAP, TEXGEN_DISABLE, TEXGEN_DISABLE, TEXGEN_DISABLE,
    };
    generated = g_string_new(NULL);
    d3d11_append_fixed_function_texgen_vectors(generated, "oT1", sphere_only);
    assert(strstr(generated->str, "oT1.x=r.x*invM+0.5;"));
    assert(!strstr(generated->str, "oT1.y="));
    assert(!strstr(generated->str, "oT1.z="));
    g_string_free(generated, TRUE);

    enum VshTexgen reflection_only[4] = {
        TEXGEN_DISABLE, TEXGEN_DISABLE, TEXGEN_REFLECTION_MAP, TEXGEN_DISABLE,
    };
    generated = g_string_new(NULL);
    d3d11_append_fixed_function_texgen_vectors(
        generated, "oT2", reflection_only);
    assert(strstr(generated->str, "oT2.z=r.z;"));
    assert(!strstr(generated->str, "oT2.x="));
    assert(!strstr(generated->str, "oT2.y="));
    assert(!strstr(generated->str, "oT2.w="));
    g_string_free(generated, TRUE);

    puts("PASS: per-component fixed-function texgen modes");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-texgen-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "test-texgen.c").write_text(texgen_test)
    exe = tmp / "test-texgen"
    glib_flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(tmp / 'test-texgen.c'), '-o', str(exe),
                    *glib_flags], check=True)
    subprocess.run([str(exe)], check=True)

point_size_test = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
''' + point_size_helper + r'''
int main(void)
{
    GString *source = g_string_new(NULL);
    d3d11_append_vsh_point_size(source, true, 9.0f, 2, 24);
    assert(strstr(source->str, "length(xf_mul4(position,24))"));
    assert(strstr(source->str, "min(pointParams1.w,63.875)"));
    assert(strstr(source->str, "min(pointParams0.w+ptMinSize,63.875)"));
    assert(strstr(source->str,
                  "pointParams0.x+pointParams0.y*d_e+pointParams0.z*d_e*d_e"));
    assert(strstr(source->str, "pointSizeAttenuated*pointParams0.w+pointParams1.w"));
    assert(strstr(source->str, "ptMinSize,ptMaxSize)*2.0"));
    puts("--POINT-SIZE-BEGIN--");
    fputs(source->str, stdout);
    puts("--POINT-SIZE-END--");
    g_string_free(source, TRUE);

    source = g_string_new(NULL);
    d3d11_append_vsh_point_size(source, false, 0.5f, 2, 24);
    assert(strstr(source->str, "output.pointSize=2.000000f;"));
    assert(!strstr(source->str, "pointParams0"));
    g_string_free(source, TRUE);

    source = g_string_new(NULL);
    d3d11_append_programmable_point_size(source, false, 0.0f, 3);
    assert(strstr(source->str, "output.pointSize=3.000000f;"));
    g_string_free(source, TRUE);
    source = g_string_new(NULL);
    d3d11_append_programmable_point_size(source, true, 8.0f, 3);
    assert(strstr(source->str, "output.pointSize=oPts.x;"));
    assert(!strstr(source->str, "3.000000f"));
    g_string_free(source, TRUE);
    puts("PASS: fixed-function point-size attenuation and constant fallback");
    puts("PASS: programmable point-size output/fallback matches OpenGL routing");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-point-size-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "test-point-size.c").write_text(point_size_test)
    exe = tmp / "test-point-size"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(tmp / "test-point-size.c"), "-o", str(exe),
                    *glib_flags], check=True)
    point_output = subprocess.check_output([str(exe)], text=True)
    print("PASS: fixed-function point-size attenuation and constant fallback")
    if shutil.which("glslangValidator"):
        snippet = point_output.split("--POINT-SIZE-BEGIN--\n", 1)[1].split(
            "--POINT-SIZE-END--", 1
        )[0]
        hlsl = f"""
cbuffer Nv2aVertexConstants : register(b0) {{
    float4 C[192]; float4 pointParams0; float4 pointParams1;
}};
float4 xf_mul4(float4 v, uint base) {{
    return float4(dot(v,C[base]), dot(v,C[base+1]),
                  dot(v,C[base+2]), dot(v,C[base+3]));
}}
struct VSIn {{ float4 position : POSITION; }};
struct VSOut {{ float4 position : SV_Position; float pointSize : TEXCOORD12; }};
VSOut main(VSIn input) {{
    float4 position=input.position; VSOut output;
{snippet}
    output.position=float4(0,0,0,1); return output;
}}
"""
        hlsl_file = tmp / "test-point-size.hlsl"
        spv_file = tmp / "test-point-size.spv"
        hlsl_file.write_text(hlsl)
        subprocess.run(
            ["glslangValidator", "-D", "-V", "-S", "vert", "-e", "main",
             "--auto-map-bindings", "-o", str(spv_file), str(hlsl_file)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        print("PASS: generated point-size HLSL parsed by glslangValidator")
    else:
        print("SKIP: glslangValidator unavailable; point-size HLSL check omitted")

color_output_test = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
''' + color_output_helper + r'''
int main(void)
{
    GString *source = g_string_new(NULL);
    d3d11_append_vsh_color_outputs(source, "oD0", "oD1", "oB0", "oB1",
                                   true, true);
    assert(strstr(source->str, "output.diffuse=nv_vsh_color(oD0)"));
    assert(strstr(source->str, "output.backDiffuse=nv_vsh_color(oB0)"));
    assert(strstr(source->str, "output.specular=nv_vsh_color(oD1)"));
    assert(strstr(source->str, "output.backSpecular=nv_vsh_color(oB1)"));
    assert(strstr(source->str, "output.specular.w=1.0"));
    assert(strstr(source->str, "output.backSpecular.w=1.0"));
    puts("--VSH-COLOR-BEGIN--");
    fputs(source->str, stdout);
    puts("--VSH-COLOR-END--");
    g_string_free(source, TRUE);

    source = g_string_new(NULL);
    d3d11_append_vsh_color_outputs(source, "oD0", "oD1", "oB0", "oB1",
                                   false, false);
    assert(strstr(source->str, "output.specular=float4(0,0,0,1)"));
    assert(strstr(source->str, "output.backSpecular=float4(0,0,0,1)"));
    assert(!strstr(source->str, "output.specular=nv_vsh_color(oD1)"));
    g_string_free(source, TRUE);

    source = g_string_new(NULL);
    d3d11_append_vsh_color_outputs(source, "oD0", "oD1", "oB0", "oB1",
                                   true, false);
    assert(strstr(source->str, "output.specular=nv_vsh_color(oD1)"));
    assert(!strstr(source->str, "output.specular.w=1.0"));
    g_string_free(source, TRUE);
    puts("PASS: VSH color clamp, specular enable and ignored-alpha outputs");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-vsh-colors-") as directory:
    tmp = pathlib.Path(directory)
    cfile = tmp / "test-vsh-colors.c"
    exe = tmp / "test-vsh-colors"
    cfile.write_text(color_output_test, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(cfile), "-o", str(exe), *glib_flags], check=True)
    color_output = subprocess.check_output([str(exe)], text=True)
    print("PASS: VSH color clamp, specular enable and ignored-alpha outputs")
    if shutil.which("glslangValidator"):
        snippet = color_output.split("--VSH-COLOR-BEGIN--\n", 1)[1].split(
            "--VSH-COLOR-END--", 1)[0]
        hlsl = f"""
{vsh_color_hlsl}
struct VSOut {{
    float4 position : SV_Position;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float4 backDiffuse : TEXCOORD4;
    float4 backSpecular : TEXCOORD5;
}};
VSOut main() {{
    float4 oD0=float4(-0.25,0.5,1.25,asfloat(0x7fc00000));
    float4 oD1=float4(0.1,0.2,0.3,0.4);
    float4 oB0=float4(0.4,0.3,0.2,0.1);
    float4 oB1=float4(0.5,0.6,0.7,0.8);
    VSOut output; output.position=float4(0,0,0,1);
{snippet}
    return output;
}}
"""
        hlsl_file = tmp / "test-vsh-colors.hlsl"
        spv_file = tmp / "test-vsh-colors.spv"
        hlsl_file.write_text(hlsl, encoding="utf-8")
        subprocess.run(
            ["glslangValidator", "-D", "-V", "-S", "vert", "-e", "main",
             "--auto-map-bindings", "-o", str(spv_file), str(hlsl_file)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True,
        )
        print("PASS: generated color-output HLSL parsed by glslangValidator")
    else:
        print("SKIP: glslangValidator unavailable; color-output HLSL check omitted")

specular_mix_test = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>
''' + specular_mix_helper + r'''
static void check_mix(bool specular_enable, bool separate_specular,
                      bool expected_mix)
{
    GString *source = g_string_new(NULL);
    d3d11_append_fixed_function_specular_mix(
        source, specular_enable, separate_specular);
    assert((strstr(source->str, "litDiffuse.rgb+=litSpecular.rgb;") != NULL) ==
           expected_mix);
    g_string_free(source, TRUE);
}
int main(void)
{
    check_mix(false, false, false);
    check_mix(false, true, false);
    check_mix(true, true, false);
    check_mix(true, false, true);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-vsh-specular-mix-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "test-specular-mix.c").write_text(specular_mix_test)
    exe = tmp / "test-specular-mix"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(tmp / "test-specular-mix.c"), "-o", str(exe),
                    *glib_flags], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: fixed-function specular only mixes when enabled and not separate")

fog_test = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
''' + (
    f"#define NV_IGRAPH_XF_XFCTX_FOG {fog_register}\n" + fog_mode_enum + "\n" +
    foggen_enum + "\n" + fog_helper + "\n" + fog_key_helper + "\n" +
    vsh_key_helper
) + r'''
static void check_fog(bool enabled, enum VshFogMode mode,
                      enum VshFoggen gen, bool fixed_function,
                      const char *expected, const char *absent)
{
    GString *source = g_string_new(NULL);
    assert(d3d11_append_vsh_fog_factor(source, enabled, mode, gen,
                                       fixed_function));
    assert(strstr(source->str, expected));
    if (absent) {
        assert(!strstr(source->str, absent));
    }
    g_string_free(source, TRUE);
}

static void write_fog_fragment(FILE *file, bool enabled,
                               enum VshFogMode mode, enum VshFoggen gen,
                               bool fixed_function)
{
    GString *source = g_string_new(NULL);
    assert(d3d11_append_vsh_fog_factor(source, enabled, mode, gen,
                                       fixed_function));
    assert(fprintf(file, "%zu\n", source->len) > 0);
    assert(fwrite(source->str, 1, source->len, file) == source->len);
    assert(fputc('\n', file) != EOF);
    g_string_free(source, TRUE);
}

static void check_fog_key(bool enabled_a, enum VshFogMode mode_a,
                          bool enabled_b, enum VshFogMode mode_b,
                          bool expected_equal)
{
    GByteArray *a = g_byte_array_new();
    GByteArray *b = g_byte_array_new();
    d3d11_append_programmable_fog_key(a, enabled_a, mode_a);
    d3d11_append_programmable_fog_key(b, enabled_b, mode_b);
    assert(a->len == sizeof(uint8_t) + sizeof(uint32_t));
    assert(b->len == a->len);
    assert((memcmp(a->data, b->data, a->len) == 0) == expected_equal);
    g_byte_array_free(a, TRUE);
    g_byte_array_free(b, TRUE);
}

static void check_vsh_key(bool specular_a, bool ignore_alpha_a,
                          bool point_params_a, float point_size_a,
                          unsigned int scale_a, bool specular_b,
                          bool ignore_alpha_b, bool point_params_b,
                          float point_size_b, unsigned int scale_b,
                          bool expected_equal)
{
    GByteArray *a = g_byte_array_new();
    GByteArray *b = g_byte_array_new();
    d3d11_append_programmable_vsh_key(
        a, true, FOG_MODE_LINEAR, FOGGEN_FOG_X, specular_a, ignore_alpha_a,
        point_params_a, point_size_a, scale_a);
    d3d11_append_programmable_vsh_key(
        b, true, FOG_MODE_LINEAR, FOGGEN_FOG_X, specular_b, ignore_alpha_b,
        point_params_b, point_size_b, scale_b);
    assert(a->len == sizeof(uint8_t) + 2 * sizeof(uint32_t) +
                      3 * sizeof(uint8_t) + sizeof(float) +
                      sizeof(unsigned int));
    assert(b->len == a->len);
    assert((memcmp(a->data, b->data, a->len) == 0) == expected_equal);
    g_byte_array_free(a, TRUE);
    g_byte_array_free(b, TRUE);
}

static void check_vsh_foggen_key(enum VshFoggen foggen_a,
                                 enum VshFoggen foggen_b,
                                 bool expected_equal)
{
    GByteArray *a = g_byte_array_new();
    GByteArray *b = g_byte_array_new();
    d3d11_append_programmable_vsh_key(
        a, true, FOG_MODE_LINEAR, foggen_a, false, false, false, 1.0f, 1);
    d3d11_append_programmable_vsh_key(
        b, true, FOG_MODE_LINEAR, foggen_b, false, false, false, 1.0f, 1);
    assert(a->len == b->len);
    assert((memcmp(a->data, b->data, a->len) == 0) == expected_equal);
    g_byte_array_free(a, TRUE);
    g_byte_array_free(b, TRUE);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char planar_expression[128];
    snprintf(planar_expression, sizeof(planar_expression),
             "fogDistance=dot(C[%u].xyz,tPosition.xyz)+C[%u].w;",
             NV_IGRAPH_XF_XFCTX_FOG, NV_IGRAPH_XF_XFCTX_FOG);
    check_fog(false, FOG_MODE_LINEAR, FOGGEN_FOG_X, true,
              "float fogFactor=1.0;", "fogDistance=");
    check_fog(true, FOG_MODE_LINEAR, FOGGEN_PLANAR, true,
              planar_expression,
              "input.v5.x");
    check_fog(true, FOG_MODE_LINEAR, FOGGEN_PLANAR, true,
              "fogFactor=fogParam.x+fogDistance*fogParam.y;", NULL);
    check_fog(true, FOG_MODE_LINEAR, FOGGEN_PLANAR, true,
              "fogFactor-=1.0;", NULL);
    check_fog(true, FOG_MODE_LINEAR, FOGGEN_PLANAR, true,
              "if (isinf(fogDistance)) { fogFactor=1.0; }", NULL);
    check_fog(true, FOG_MODE_LINEAR, FOGGEN_PLANAR, true,
              "isnan(fogFactor) ? 1.0 : fogFactor", NULL);
    check_fog(true, FOG_MODE_EXP, FOGGEN_SPEC_ALPHA, true,
              "fogDistance=clamp(specular.a,0.0,1.0);", NULL);
    check_fog(true, FOG_MODE_EXP, FOGGEN_SPEC_ALPHA, true,
              "exp2(fogDistance*fogParam.y*16.0)", NULL);
    check_fog(true, FOG_MODE_EXP_ABS, FOGGEN_FOG_X, true,
              "fogFactor=abs(fogFactor);", NULL);
    check_fog(true, FOG_MODE_EXP_ABS, FOGGEN_FOG_X, true,
              "if (isinf(fogDistance)) { fogFactor=0.0; }", NULL);
    check_fog(true, FOG_MODE_EXP2, FOGGEN_PLANAR, true,
              "fogDistance*fogDistance*fogParam.y*fogParam.y*32.0", NULL);
    check_fog(true, FOG_MODE_EXP2_ABS, FOGGEN_RADIAL, true,
              "fogDistance=length(tPosition.xyz);", NULL);
    check_fog(true, FOG_MODE_EXP2_ABS, FOGGEN_RADIAL, true,
              "exp2(-fogDistance*fogDistance*fogParam.y*fogParam.y*32.0)",
              NULL);
    check_fog(true, FOG_MODE_EXP2_ABS, FOGGEN_RADIAL, true,
              "fogFactor=abs(fogFactor);", NULL);
    check_fog(true, FOG_MODE_LINEAR_ABS, FOGGEN_ABS_PLANAR, true,
              "fogDistance=abs(fogDistance);", NULL);
    check_fog(true, FOG_MODE_LINEAR_ABS, FOGGEN_FOG_X, true,
              "fogDistance=input.v5.x;", NULL);
    check_fog(true, FOG_MODE_EXP, FOGGEN_PLANAR, false,
              "fogDistance=oFog.x;", "input.v5.x");
    GString *invalid = g_string_new(NULL);
    assert(!d3d11_append_vsh_fog_factor(
        invalid, true, FOG_MODE_ERROR2, FOGGEN_FOG_X, false));
    g_string_free(invalid, TRUE);
    check_fog_key(true, FOG_MODE_LINEAR, true, FOG_MODE_LINEAR, true);
    check_fog_key(false, FOG_MODE_LINEAR, true, FOG_MODE_LINEAR, false);
    check_fog_key(true, FOG_MODE_LINEAR, true, FOG_MODE_EXP, false);
    check_vsh_key(false, false, false, 1.0f, 1, false, false, false,
                  1.0f, 1, true);
    check_vsh_foggen_key(FOGGEN_FOG_X, FOGGEN_FOG_X, true);
    check_vsh_foggen_key(FOGGEN_FOG_X, FOGGEN_PLANAR, false);
    check_vsh_key(false, false, false, 1.0f, 1, true, false, false,
                  1.0f, 1, false);
    check_vsh_key(true, false, false, 1.0f, 1, true, true, false,
                  1.0f, 1, false);
    check_vsh_key(true, false, false, 1.0f, 1, true, false, true,
                  1.0f, 1, false);
    check_vsh_key(false, false, false, 1.0f, 1, false, false, false,
                  2.0f, 1, false);
    check_vsh_key(false, false, false, 1.0f, 1, false, false, false,
                  1.0f, 2, false);

    FILE *fragments = fopen(argv[1], "wb");
    assert(fragments);
    write_fog_fragment(fragments, true, FOG_MODE_LINEAR,
                       FOGGEN_SPEC_ALPHA, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP,
                       FOGGEN_PLANAR, true);
    write_fog_fragment(fragments, true, FOG_MODE_LINEAR_ABS,
                       FOGGEN_ABS_PLANAR, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP_ABS,
                       FOGGEN_FOG_X, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP2,
                       FOGGEN_PLANAR, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP2_ABS,
                       FOGGEN_RADIAL, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP,
                       FOGGEN_FOG_X, true);
    write_fog_fragment(fragments, true, FOG_MODE_EXP,
                       FOGGEN_FOG_X, false);
    write_fog_fragment(fragments, false, FOG_MODE_LINEAR,
                       FOGGEN_FOG_X, true);
    assert(fclose(fragments) == 0);
    puts("PASS: per-vertex fog generation and mode conversion");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-fog-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "test-fog.c").write_text(fog_test)
    exe = tmp / "test-fog"
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(tmp / 'test-fog.c'), '-o', str(exe),
                    *glib_flags], check=True)
    fragment_file = tmp / "fog-fragments.bin"
    subprocess.run([str(exe), str(fragment_file)], check=True)
    fragment_bytes = fragment_file.read_bytes()
    fog_fragments = []
    offset = 0
    for _ in range(9):
        newline = fragment_bytes.index(b"\n", offset)
        length = int(fragment_bytes[offset:newline])
        offset = newline + 1
        fog_fragments.append(fragment_bytes[offset:offset + length].decode())
        offset += length
        assert fragment_bytes[offset:offset + 1] == b"\n"
        offset += 1
    assert offset == len(fragment_bytes)

state_selector = extract_c_function(
    draw, "static bool d3d11_state_vertex_index(")
span_selector = extract_c_function(
    draw, "static bool d3d11_indexed_vertex_span(")
state_packer = extract_c_function(draw, "static bool d3d11_pack_vertices(")
state_emitter = extract_c_function(draw, "static bool d3d11_emit_draw(")
assert "source_indices[source_index_count - 1]" in state_selector
assert "vertices[state_vertex - first].attributes[attr_index]" in state_packer
assert "d3d11_state_vertex_index(first, vertex_count, source_indices," \
    in state_emitter
assert "d3d11_upload_vertices(d, source, first, vertex_count, state_vertex," \
    in state_emitter
state_test = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
""" + state_selector + span_selector + r"""
int main(void)
{
    uint32_t state_vertex = UINT32_MAX;
    uint32_t vertex_count = 0;
    const uint32_t unordered_indices[] = { 8, 2, 5, 3 };
    assert(d3d11_state_vertex_index(2, 7, unordered_indices, 4,
                                    &state_vertex));
    assert(state_vertex == 3); /* Not the maximum index, 8. */

    assert(d3d11_state_vertex_index(20, 4, NULL, 0, &state_vertex));
    assert(state_vertex == 23);
    assert(!d3d11_state_vertex_index(4, 5, unordered_indices, 4,
                                     &state_vertex));
    const uint32_t invalid_middle_high[] = { 9, 2, 5, 3 };
    const uint32_t invalid_middle_low[] = { 1, 2, 5, 3 };
    assert(!d3d11_state_vertex_index(2, 7, invalid_middle_high, 4,
                                     &state_vertex));
    assert(!d3d11_state_vertex_index(2, 7, invalid_middle_low, 4,
                                     &state_vertex));
    assert(!d3d11_state_vertex_index(4, 0, NULL, 0, &state_vertex));
    assert(!d3d11_state_vertex_index(0, 4, NULL, 1, &state_vertex));
    assert(!d3d11_state_vertex_index(0, 4, unordered_indices, 0,
                                     &state_vertex));
    assert(d3d11_indexed_vertex_span(2, 8, &vertex_count));
    assert(vertex_count == 7);
    assert(d3d11_indexed_vertex_span(UINT32_MAX, UINT32_MAX, &vertex_count));
    assert(vertex_count == 1);
    assert(!d3d11_indexed_vertex_span(0, UINT32_MAX, &vertex_count));
    assert(!d3d11_indexed_vertex_span(8, 7, &vertex_count));
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="xemu-d3d11-vertex-state-") as directory:
    tmp = pathlib.Path(directory)
    c_file = tmp / "vertex-state.c"
    executable = tmp / "vertex-state"
    c_file.write_text(state_test)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c_file), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: indexed draws persist the last submitted vertex, not the max index")

if "--texgen-only" in sys.argv:
    sys.exit(0)

if sys.platform != "win32":
    print("SKIP: D3D11 WARP vertex-output fixture requires Windows/MSYS2")
    sys.exit(0)

front_face = re.search(r'\.FrontCounterClockwise\s*=\s*([^,]+),', draw).group(1)
front_face = front_face.replace('NV_PGRAPH_SETUPRASTER_FRONTFACE', '1')
epilogues = []
for start in ('"  oPos.xy = trunc(', '"  oPos.w=nv_clamp_away('):
    # The latter identifies the fixed-function output block.
    section = source[source.index(start):]
    section = section[:section.index('"  VSOut output;')]
    lines = [json.loads(match) for match in re.findall(r'"(?:[^"\\]|\\.)*"', section)]
    body = ''.join(lines)
    body = re.sub(r'C\[%u\]', 'float4(0,0,0,0)', body)
    epilogues.append(body)
# Negative control: reproduce the old fixed-function projection.
epilogues.append(epilogues[1].replace('oPos.xy/=oPos.w;', '')
                 .replace('oPos.xy*=oPos.w;', 'oPos.xyz*=oPos.w;'))
with tempfile.TemporaryDirectory(prefix="xemu-vertex-") as directory:
    tmp = pathlib.Path(directory)
    (tmp / "vertex-output-generated.h").write_text(
        'static const char *epilogues[] = {' +
        ','.join(json.dumps(x) for x in epilogues) + '};\n' +
        'static const char *fog_factors[] = {' +
        ','.join(json.dumps(x) for x in fog_fragments) + '};\n' +
        '#define XEMU_FRONT_COUNTER_CLOCKWISE(raster) (' + front_face + ')\n')
    exe = tmp / "test.exe"
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-I' + str(tmp),
                    str(root / 'tests/unit/test-d3d11-vertex-output.c'),
                    '-o', str(exe), '-ld3d11', '-ld3dcompiler'], check=True)
    subprocess.run([str(exe)], check=True)
