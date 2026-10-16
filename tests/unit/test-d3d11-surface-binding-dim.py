#!/usr/bin/env python3
"""Validate D3D11 surface binding dimensions consumed by draw and shaders."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"
DRAW = ROOT / "hw/xbox/nv2a/pgraph/d3d11/draw.c"
SHADERS = ROOT / "hw/xbox/nv2a/pgraph/d3d11/shaders.c"


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
    draw_source = DRAW.read_text(encoding="utf-8")
    shader_source = SHADERS.read_text(encoding="utf-8")
    dimensions = extract_function(
        source, "static void d3d11_surface_dimensions(")
    binding_update = extract_function(
        source, "static void d3d11_update_surface_binding_dim(")
    target_update = extract_function(
        source, "bool pgraph_d3d11_render_targets_update(")

    call = "d3d11_update_surface_binding_dim(pg);"
    assert call in target_update
    assert target_update.index(call) < target_update.index("pg->draw_time++;")
    assert "pg->surface_binding_dim.width = guest_width;" in binding_update
    assert "pg->surface_binding_dim.height = guest_height;" in binding_update
    for field in ("clip_x", "clip_y", "clip_width", "clip_height"):
        assert f"surface_binding_dim.{field} =" in binding_update
    assert "pgraph_apply_scaling_factor(pg, &viewport_width, &viewport_height);" \
        in draw_source
    assert "pgraph_apply_scaling_factor(pg, &viewport_width, &viewport_height);" \
        in shader_source

    fixture = r"""
#include <assert.h>
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
    struct {
        int clip_x, clip_width, clip_y, clip_height;
        int width, height;
    } surface_binding_dim;
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
""" + dimensions + "\n\n" + binding_update + r"""
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
    uint32_t guest_width, guest_height, target_width, target_height;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height,
                             &target_width, &target_height);
    d3d11_update_surface_binding_dim(&pg);
    assert(guest_width == 645 && guest_height == 487);
    assert(target_width == 1935 && target_height == 1461);
    assert(pg.surface_binding_dim.width == 645);
    assert(pg.surface_binding_dim.height == 487);
    assert(pg.surface_binding_dim.clip_x == 5);
    assert(pg.surface_binding_dim.clip_y == 7);
    assert(pg.surface_binding_dim.clip_width == 320);
    assert(pg.surface_binding_dim.clip_height == 240);

    pg.surface_type = NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
    pg.surface_shape.log_width = 8;
    pg.surface_shape.log_height = 7;
    pg.surface_shape.clip_x = 2;
    pg.surface_shape.clip_y = 3;
    pg.surface_shape.clip_width = 128;
    pg.surface_shape.clip_height = 64;
    d3d11_surface_dimensions(&pg, &guest_width, &guest_height,
                             &target_width, &target_height);
    d3d11_update_surface_binding_dim(&pg);
    assert(guest_width == 512 && guest_height == 256);
    assert(target_width == 1536 && target_height == 768);
    assert(pg.surface_binding_dim.width == 512);
    assert(pg.surface_binding_dim.height == 256);
    assert(pg.surface_binding_dim.clip_x == 2);
    assert(pg.surface_binding_dim.clip_y == 3);
    assert(pg.surface_binding_dim.clip_width == 128);
    assert(pg.surface_binding_dim.clip_height == 64);
    return 0;
}
"""

    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-surface-binding-") as tmp:
        c_file = pathlib.Path(tmp) / "surface-binding-dim.c"
        c_file.write_text(fixture, encoding="utf-8")
        executable = pathlib.Path(tmp) / "surface-binding-dim"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    print("D3D11 surface binding dimensions: PASS (GCC -Werror)")


if __name__ == "__main__":
    main()
