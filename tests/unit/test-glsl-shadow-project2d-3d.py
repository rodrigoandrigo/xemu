#!/usr/bin/env python3
"""Guard the GLSL/D3D11 PROJECT2D shadow-volume coordinate contract."""

import pathlib
import shlex
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
GLSL = ROOT / "hw/xbox/nv2a/pgraph/glsl/psh.c"
D3D11 = ROOT / "hw/xbox/nv2a/pgraph/d3d11/shaders.c"


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


def validate_glsl_if_available():
    compiler = shutil.which("glslangValidator")
    if not compiler:
        return False
    shader = r"""#version 430 core
uniform sampler3D texSamp0;
uniform float texScale0;
vec2 norm0(vec2 coord) {
    return coord / (textureSize(texSamp0, 0).xy / texScale0);
}
vec4 norm0(vec4 coord) {
    return vec4(norm0(coord.xy), 0, coord.w);
}
layout(location=0) out vec4 result;
void main() {
    vec4 pT0 = vec4(0.5, 0.5, 0.0, 1.0);
    result = textureProj(texSamp0, norm0(vec4(pT0.xy, 0.0, pT0.w)));
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-glsl-shadow-volume-") as tmp:
        path = pathlib.Path(tmp) / "shadow-volume.frag"
        path.write_text(shader, encoding="utf-8")
        subprocess.run([compiler, "-S", "frag", str(path)], check=True,
                       capture_output=True, text=True)
    return True


def main():
    glsl = GLSL.read_text(encoding="utf-8")
    d3d11 = D3D11.read_text(encoding="utf-8")

    # PROJECT2D on a 3D texture uses the z=0 slice in the ordinary GLSL path.
    # The shadow helper must use the sampler3D vec4 textureProj overload too.
    assert "bool project2d_3d = !compare_z && ps->state->dim_tex[i] == 3;" in glsl
    assert "textureProj(texSamp%d, %s(vec4(pT%d.xy, " in glsl
    assert "0.0, pT%d.w)));\\n" in glsl
    assert "vec4(pT%d.xy, 0.0, pT%d.w)" in glsl
    assert "textureProj(texSamp%d, %s(pT%d.xyw))" in glsl
    assert "textureSize(texSamp%d, 0)%s / texScale[%d]" in glsl
    assert 'ps->state->dim_tex[i] == 3 ? ".xy" : ""' in glsl

    # D3D11 samples the same projected xy coordinate at the same fixed slice.
    assert "bool supported_dimension =" in d3d11
    assert "state->dim_tex[stage] == 2 || state->dim_tex[stage] == 3" in d3d11
    assert "float3(pT%u.xy/pT%u.w*" in d3d11
    assert "texCoordScale[%u].xy,0)" in d3d11

    helper = extract_function(
        d3d11, "static bool d3d11_ps_append_shadow_stage(")
    preamble = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "hw/xbox/nv2a/pgraph/psh_regs.h"
typedef struct PshState {
    bool tex_cubemap[4];
    bool tex_x8y24[4];
    bool tex_depth_float[4];
    int dim_tex[4];
    enum PshShadowDepthFunc shadow_depth_func;
} PshState;
typedef struct Error { char message[256]; } Error;
static void error_setg(Error **errp, const char *format, ...)
{
    if (!errp) return;
    *errp = g_new0(Error, 1);
    va_list args;
    va_start(args, format);
    vsnprintf((*errp)->message, sizeof((*errp)->message), format, args);
    va_end(args);
}
"""
    harness = r"""
int main(void)
{
    PshState state = { .dim_tex = { 3 },
                       .shadow_depth_func = SHADOW_DEPTH_FUNC_LEQUAL };
    GString *source = g_string_new(NULL);
    assert(d3d11_ps_append_shadow_stage(
        source, &state, 0, PS_TEXTUREMODES_PROJECT2D, NULL));
    assert(strstr(source->str,
                  "NV2A_SAMPLE3(tex0,samp0,0,float3(pT0.xy/pT0.w*"
                  "texCoordScale[0].xy,0))"));
    g_string_free(source, TRUE);

    state.tex_cubemap[0] = true;
    Error *error = NULL;
    source = g_string_new(NULL);
    assert(!d3d11_ps_append_shadow_stage(
        source, &state, 0, PS_TEXTUREMODES_PROJECT2D, &error));
    assert(error && strstr(error->message, "unsupported shadow-map mode"));
    g_free(error);
    g_string_free(source, TRUE);

    state.tex_cubemap[0] = false;
    assert(!d3d11_ps_append_shadow_stage(
        source = g_string_new(NULL), &state, 0, PS_TEXTUREMODES_PROJECT3D,
        &error));
    assert(error && strstr(error->message, "unsupported shadow-map mode"));
    g_free(error);
    g_string_free(source, TRUE);
    return 0;
}
"""
    cflags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-shadow-volume-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "shadow-volume.c"
        executable = tmp / "shadow-volume"
        c_file.write_text(preamble + helper + harness, encoding="utf-8")
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT}", str(c_file), "-o", str(executable), *cflags],
            check=True)
        subprocess.run([str(executable)], check=True)

    glsl_compiled = validate_glsl_if_available()
    detail = "; GLSL overload compiled" if glsl_compiled else "; glslang unavailable"
    print("PASS: GLSL reference and real D3D11 helper agree on PROJECT2D volume z=0"
          + detail)


if __name__ == "__main__":
    main()
