#!/usr/bin/env python3
"""Compile and test the actual D3D11 custom-depth primitive selector."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS = ROOT / "hw/xbox/nv2a/pgraph/d3d11/shaders.c"
REGS = ROOT / "hw/xbox/nv2a/pgraph/vsh_regs.h"


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
    source = SHADERS.read_text(encoding="utf-8")
    registers = REGS.read_text(encoding="utf-8")
    enum_start = registers.index("enum ShaderPrimitiveMode {")
    enum_end = registers.index("};", enum_start) + 2
    primitive_enum = registers[enum_start:enum_end]
    polygon_enum_start = registers.index("enum ShaderPolygonMode {")
    polygon_enum_end = registers.index("};", polygon_enum_start) + 2
    polygon_enum = registers[polygon_enum_start:polygon_enum_end]
    selector = extract_function(
        source, "static bool d3d11_primitive_supports_custom_depth(")
    polygon_mode_supported = extract_function(
        source, "static bool d3d11_polygon_mode_supported(")
    binding = source[source.index("bool pgraph_d3d11_bind_pixel_shader("):]
    assert "d3d11_primitive_supports_custom_depth(pg->primitive_mode)" in binding
    assert "r->shaders->quad_adjacency_geometry =" in binding
    assert "custom_depth &&" in binding
    assert "polygon_fill" not in selector
    assert binding.index("d3d11_polygon_mode_supported(") < binding.index(
        "pgraph_glsl_set_psh_state(")
    assert "d3d11_get_raster_geometry_shader(pg, &state, errp)" in binding
    assert "pgraph_d3d11_needs_raster_geometry(pg)" in binding
    assert "d3d11_polygon_mode_supported(pg->primitive_mode, front_mode)" in binding
    assert "invalid primitive or polygon mode" in binding
    assert "return front == back ? front : POLY_MODE_FILL" not in source
    draw = (ROOT / "hw/xbox/nv2a/pgraph/d3d11/draw.c").read_text()
    assert "if (r->shaders->quad_adjacency_geometry)" in draw
    assert "D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ" in draw
    assert "D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ" in draw

    test = r"""
#include <assert.h>
#include <stdbool.h>
""" + primitive_enum + "\n" + polygon_enum + "\n" + selector + "\n" + polygon_mode_supported + r"""
int main(void)
{
    static const enum ShaderPrimitiveMode supported[] = {
        PRIM_TYPE_POINTS, PRIM_TYPE_LINES, PRIM_TYPE_LINE_LOOP,
        PRIM_TYPE_LINE_STRIP, PRIM_TYPE_TRIANGLES,
        PRIM_TYPE_TRIANGLE_STRIP, PRIM_TYPE_TRIANGLE_FAN,
        PRIM_TYPE_QUADS, PRIM_TYPE_QUAD_STRIP, PRIM_TYPE_POLYGON,
    };
    for (unsigned int i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        assert(d3d11_primitive_supports_custom_depth(supported[i]));
    }
    assert(!d3d11_primitive_supports_custom_depth(PRIM_TYPE_INVALID));
    assert(!d3d11_primitive_supports_custom_depth(
        (enum ShaderPrimitiveMode)99));
    assert(d3d11_polygon_mode_supported(PRIM_TYPE_TRIANGLES, POLY_MODE_POINT));
    assert(d3d11_polygon_mode_supported(PRIM_TYPE_QUADS, POLY_MODE_POINT));
    assert(d3d11_polygon_mode_supported(PRIM_TYPE_POLYGON, POLY_MODE_POINT));
    assert(!d3d11_polygon_mode_supported(PRIM_TYPE_POLYGON, 99));
    assert(d3d11_polygon_mode_supported(PRIM_TYPE_POLYGON, POLY_MODE_LINE));
    assert(d3d11_polygon_mode_supported(PRIM_TYPE_POLYGON, POLY_MODE_FILL));
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-depth-selector-") as tmp:
        c_file = pathlib.Path(tmp) / "depth-selector.c"
        executable = pathlib.Path(tmp) / "depth-selector"
        c_file.write_text(test, encoding="utf-8")
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file),
             "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: depth selector accepts polygon POINT, routes mixed face modes to GS and rejects invalid modes")


if __name__ == "__main__":
    main()
