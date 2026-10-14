/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/glsl/geom.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"

void pgraph_get_inline_values(PGRAPHState *pg, uint16_t attrs,
                              float values[NV2A_VERTEXSHADER_ATTRIBUTES][4],
                              int *count)
{
    /* Runtime uniform fetching is outside this generator-only diagnostic. */
    abort();
}

int main(void)
{
    VshState vertex = {
        .surface_scale_factor = 1,
        .point_size = 8.0f,
        .is_fixed_function = true,
    };
    GenVshGlslOptions vertex_opts = {
        .vulkan = true,
        .prefix_outputs = true,
        .point_extent = true,
        .ubo_binding = 0,
    };
    MString *vertex_code = pgraph_glsl_gen_vsh(&vertex, vertex_opts);
    g_assert_nonnull(
        strstr(mstring_get_str(vertex_code), "gl_PointSize = 1.0"));
    g_assert_true(g_file_set_contents("points.vert",
                                      mstring_get_str(vertex_code), -1, NULL));
    mstring_unref(vertex_code);
    const enum ShaderPrimitiveMode modes[] = {
        PRIM_TYPE_POINTS,       PRIM_TYPE_TRIANGLES, PRIM_TYPE_TRIANGLE_STRIP,
        PRIM_TYPE_TRIANGLE_FAN, PRIM_TYPE_QUADS,     PRIM_TYPE_QUAD_STRIP,
        PRIM_TYPE_POLYGON,
    };
    for (unsigned mode = 0; mode < ARRAY_SIZE(modes); ++mode) {
        for (unsigned cull = 0; cull < 4; ++cull) {
            for (unsigned ccw = 0; ccw < 2; ++ccw) {
                for (unsigned smooth = 0; smooth < 2; ++smooth) {
                    for (unsigned sprite = 0; sprite < 2; ++sprite) {
                        GeomState state = {
                            .primitive_mode = modes[mode],
                            .polygon_front_mode = POLY_MODE_POINT,
                            .polygon_back_mode = POLY_MODE_POINT,
                            .smooth_shading = smooth,
                            .cull_mode = cull,
                            .front_ccw = ccw,
                        };
                        GenGeomGlslOptions opts = {
                            .vulkan = true,
                            .expand_points = true,
                            .point_sprite = sprite,
                        };
                        MString *code = pgraph_glsl_gen_geom(&state, opts);
                        const char *text = mstring_get_str(code);
                        g_assert_nonnull(strstr(
                            text, modes[mode] == PRIM_TYPE_POINTS ?
                                      "max_vertices = 4" :
                                  modes[mode] == PRIM_TYPE_QUADS ||
                                          modes[mode] == PRIM_TYPE_QUAD_STRIP ?
                                      "max_vertices = 16" :
                                      "max_vertices = 12"));
                        g_assert_true(pgraph_glsl_point_output(&state));
                        g_assert_cmpint(
                            strstr(text, "bool point_polygon_front") != NULL,
                            ==, cull != 0 && modes[mode] != PRIM_TYPE_POINTS);
                        if (modes[mode] == PRIM_TYPE_POLYGON) {
                            g_assert_nonnull(
                                strstr(text, "if (gl_PrimitiveIDIn == 0)"));
                        }
                        g_assert_nonnull(strstr(text, "v_pointExtent[index]"));
                        g_assert_null(strstr(text, "gl_PointSize"));
                        g_assert_cmpint(
                            strstr(text, "vtxT3 = vec4(pointCorners") != NULL,
                            ==, sprite);
                        char filename[64];
                        snprintf(filename, sizeof(filename),
                                 "points-%u-%u-%u-%u-%u.geom", mode, cull, ccw,
                                 smooth, sprite);
                        g_assert_true(
                            g_file_set_contents(filename, text, -1, NULL));
                        mstring_unref(code);
                    }
                }
            }
        }
    }
    return 0;
}
