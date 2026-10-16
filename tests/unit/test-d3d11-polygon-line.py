#!/usr/bin/env python3
"""Test D3D11 quad polygon LINE/POINT routing and generated GS source."""
import pathlib
import re
import shutil
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
DRAW = ROOT / "hw/xbox/nv2a/pgraph/d3d11/draw.c"
SHADERS = ROOT / "hw/xbox/nv2a/pgraph/d3d11/shaders.c"
SURFACES = ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c"
REGS = ROOT / "hw/xbox/nv2a/pgraph/vsh_regs.h"


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "normal"
    i = opening
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "normal":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "/":
                state = "line-comment"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block-comment"
                i += 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
        elif state in ("string", "char"):
            if ch == "\\":
                i += 1
            elif (state == "string" and ch == '"') or (
                    state == "char" and ch == "'"):
                state = "normal"
        elif state == "line-comment" and ch == "\n":
            state = "normal"
        elif state == "block-comment" and ch == "*" and nxt == "/":
            state = "normal"
            i += 1
        i += 1
    raise ValueError(f"unterminated function: {signature}")


def extract_enum(source, name):
    start = source.index(f"enum {name} {{")
    end = source.index("};", start) + 2
    return source[start:end]


def main():
    draw = DRAW.read_text(encoding="utf-8")
    gl_draw = (ROOT / "hw/xbox/nv2a/pgraph/gl/draw.c").read_text(
        encoding="utf-8")
    gl_psh = (ROOT / "hw/xbox/nv2a/pgraph/glsl/psh.c").read_text(
        encoding="utf-8")
    vk_draw = (ROOT / "hw/xbox/nv2a/pgraph/vk/draw.c").read_text(
        encoding="utf-8")
    shader_source = SHADERS.read_text(encoding="utf-8")
    surfaces = SURFACES.read_text(encoding="utf-8")
    registers = REGS.read_text(encoding="utf-8")
    antialiased_line_enable = extract_function(
        draw, "static bool d3d11_antialiased_line_enable(")
    point_culling = extract_function(
        draw, "static bool d3d11_primitive_uses_face_culling(")
    rasterizer_update = extract_function(
        draw, "bool pgraph_d3d11_update_rasterizer(")
    assert "if (!anti_aliasing" in gl_draw and "GL_LINE_SMOOTH" in gl_draw
    assert "NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE" in gl_draw
    assert "glEnable(GL_POLYGON_SMOOTH)" in gl_draw
    assert "glDisable(GL_POLYGON_SMOOTH)" in gl_draw
    assert "GL_POINT_SMOOTH" not in gl_draw
    assert "NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE" in gl_psh
    assert "vec4 pT3 = vec4(gl_PointCoord, 1.0, 1.0)" in gl_psh
    assert "float2 pointCoord:TEXCOORD11;" in shader_source
    assert "float4 pT3=float4(input.pointCoord,1.0,1.0);" in shader_source
    assert "state.point_sprite = state.point_sprite &&" in shader_source
    assert "pg->primitive_mode == PRIM_TYPE_POINTS;" in shader_source
    assert "VK_PRIMITIVE_TOPOLOGY_POINT_LIST" in vk_draw
    assert "FIXME: point size" in vk_draw
    assert "bool anti_aliasing = !!GET_MASK(" in rasterizer_update
    assert "pgraph_d3d11_needs_raster_geometry(pg)" in rasterizer_update
    assert "d3d11_ps_raster_coverage(source, custom_depth)" in shader_source
    assert "o.coverage=float4(edges,2)" in shader_source
    assert ".MultisampleEnable = anti_aliasing" in rasterizer_update
    assert ".AntialiasedLineEnable = !raster_geometry && d3d11_antialiased_line_enable(" \
        in rasterizer_update
    assert "d3d11_polygon_mode(pg) != POLY_MODE_FILL" in shader_source
    assert "custom_depth_uses_geometry_shader || quad_nonfill_mode" in shader_source
    assert "triangle_point_mode || triangle_line_mode || polygon_line_mode" in shader_source
    assert "d3d11_triangle_primitive(pg->primitive_mode)" in shader_source
    assert "d3d11_triangle_line_primitive(pg->primitive_mode)" in shader_source
    assert "r->shaders->polygon_line_geometry = polygon_line_mode" in shader_source
    assert "mode == PRIM_TYPE_POLYGON && polygon_line" in draw
    assert "D3D11_PRIMITIVE_TOPOLOGY_LINELIST" in draw
    assert "r->shaders->quad_adjacency_geometry =" in shader_source
    assert "if (r->shaders->quad_adjacency_geometry)" in draw
    assert "D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ" in draw
    assert "D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ" in draw
    assert "pgraph_d3d11_needs_wide_lines(" in draw
    assert "wide_lines || point_primitive || polygon_point_mode" in draw
    assert "polygon_point_mode = pg->primitive_mode >= PRIM_TYPE_TRIANGLES" in draw
    assert "back_polygon_mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT" in draw
    assert "if (pg->primitive_mode == PRIM_TYPE_POINTS)" in draw
    assert "return D3D11_CULL_NONE" in draw
    assert "cull_mode = D3D11_CULL_NONE" in draw
    assert "polygon_point_mode)" in point_culling
    assert "skip_draw = false;" in rasterizer_update
    assert "cull_mode = D3D11_CULL_NONE;" in rasterizer_update
    assert "pgraph_apply_scaling_factor(pg, &viewport_width, &viewport_height)" \
        in shader_source
    assert "constants.line_params[2] = MAX(1U, pg->surface_scale_factor)" \
        in shader_source
    assert "ID3D11DeviceContext_GSSetConstantBuffers" in shader_source
    assert "D3D11_DEPTH_GS_VARIANTS_PER_WIDTH" in shader_source
    assert "bool wide_line_mode = pgraph_d3d11_needs_wide_lines(" \
        in shader_source
    assert "wide_line_mode;" in shader_source
    assert "bool wide_lines = pgraph_d3d11_needs_wide_lines(" \
        in shader_source
    assert "base_variant +" in shader_source
    assert "viewport_height - y - height" in surfaces
    assert "pgraph_d3d11_gl_scissor_rect(" in draw
    assert "viewport_height, x, y, width, height);" in draw

    helpers = [
        extract_function(surfaces, "static LONG d3d11_scissor_coordinate("),
        extract_function(surfaces,
                         "D3D11_RECT pgraph_d3d11_gl_scissor_rect("),
        extract_function(draw, "static void G_GNUC_UNUSED d3d11_append_sequence_indices("),
        antialiased_line_enable,
        point_culling,
        extract_function(shader_source,
                         "static void d3d11_append_quad_depth_line("),
        extract_function(shader_source,
                         "static void d3d11_append_quad_depth_point("),
        extract_function(shader_source,
                         "static void d3d11_append_quad_depth_triangle("),
        extract_function(shader_source,
                         "static void d3d11_append_depth_geometry_vertex_helper("),
        extract_function(shader_source,
                         "static void d3d11_append_point_geometry_vertex_helper("),
        extract_function(shader_source,
                         "static bool d3d11_triangle_line_primitive("),
        extract_function(shader_source,
                         "static unsigned int d3d11_depth_geometry_variant_for("),
        extract_function(shader_source,
                         "static bool d3d11_triangle_primitive("),
        extract_function(shader_source,
                         "bool pgraph_d3d11_needs_wide_lines("),
        extract_function(shader_source,
                         "static char *d3d11_depth_geometry_shader_source("),
    ]
    enum_declarations = "\n".join((
        extract_enum(registers, "ShaderPrimitiveMode"),
        extract_enum(registers, "ShaderPolygonMode"),
    ))
    c_program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
typedef int32_t LONG;
typedef struct D3D11_RECT {
    LONG left, top, right, bottom;
} D3D11_RECT;
#define D3D11_VERTEX_CONSTANT_BUFFER_SIZE \
    ((192 + 2 + 4 * 8 + 20 + 26 + 52 + 4) * 4 * sizeof(float))
#define PGRAPHD3D11_POINT_GS_VARIANT 84
#define PGRAPHD3D11_POLYGON_POINT_GS_VARIANT 85

""" + enum_declarations + "\n" + "\n".join(helpers) + r"""
int main(void)
{
    assert(!d3d11_antialiased_line_enable(false, false));
    assert(d3d11_antialiased_line_enable(false, true));
    assert(!d3d11_antialiased_line_enable(true, false));
    assert(!d3d11_antialiased_line_enable(true, true));
    D3D11_RECT scissor =
        pgraph_d3d11_gl_scissor_rect(1080, 100, 200, 300, 400);
    assert(scissor.left == 100 && scissor.top == 480 &&
           scissor.right == 400 && scissor.bottom == 880);
    scissor = pgraph_d3d11_gl_scissor_rect(1080, 0, 0, 1920, 100);
    assert(scissor.top == 980 && scissor.bottom == 1080);
    scissor = pgraph_d3d11_gl_scissor_rect(1080, 0, 1100, 10, 20);
    assert(scissor.top == -40 && scissor.bottom == -20);
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINES, POLY_MODE_FILL, 1));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINES, POLY_MODE_FILL, 2));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINE_LOOP, POLY_MODE_POINT, 4));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINE_STRIP, POLY_MODE_FILL, 2));
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_POINTS, POLY_MODE_LINE, 4));
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_TRIANGLES, POLY_MODE_LINE, 1));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_TRIANGLES, POLY_MODE_LINE, 2));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_QUADS, POLY_MODE_LINE, 2));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_POLYGON, POLY_MODE_LINE, 3));
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_QUAD_STRIP, POLY_MODE_FILL, 3));
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_FILL) == 8);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_LINE) == 16);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_POINT) == 20);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, true, true, POLY_MODE_POINT) == 23);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, false, true, POLY_MODE_FILL) == 0);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, false, true, POLY_MODE_POINT) == 24);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_FAN, false, false, POLY_MODE_POINT) == 25);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_STRIP, true, false, POLY_MODE_POINT) == 31);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, false, true, POLY_MODE_LINE) == 32);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_FAN, false, false, POLY_MODE_LINE) == 33);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_STRIP, true, false, POLY_MODE_LINE) == 39);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_POLYGON, false, true, POLY_MODE_LINE) == 40);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_POLYGON, true, false, POLY_MODE_LINE) == 41);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_POINTS, false, true, POLY_MODE_FILL) ==
           PGRAPHD3D11_POINT_GS_VARIANT);
    assert(!d3d11_primitive_uses_face_culling(
        PRIM_TYPE_TRIANGLES, false, true));
    assert(!d3d11_primitive_uses_face_culling(
        PRIM_TYPE_QUADS, false, true));
    assert(d3d11_primitive_uses_face_culling(
        PRIM_TYPE_TRIANGLES, false, false));
    assert(d3d11_primitive_uses_face_culling(
        PRIM_TYPE_QUADS, false, false));
    assert(!d3d11_primitive_uses_face_culling(
        PRIM_TYPE_LINES, false, false));
    static const uint32_t polygon_indices[] = {4, 7, 1, 9};
    static const uint32_t polygon_edges[] = {4, 7, 7, 1, 1, 9, 9, 4};
    GArray *edges = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    d3d11_append_sequence_indices(edges, PRIM_TYPE_POLYGON,
                                  polygon_indices, G_N_ELEMENTS(polygon_indices),
                                  true, true);
    assert(edges->len == G_N_ELEMENTS(polygon_edges));
    assert(memcmp(edges->data, polygon_edges, sizeof(polygon_edges)) == 0);
    g_array_set_size(edges, 0);
    d3d11_append_sequence_indices(edges, PRIM_TYPE_POLYGON,
                                  polygon_indices, G_N_ELEMENTS(polygon_indices),
                                  true, false);
    assert(edges->len == 6);
    g_array_set_size(edges, 0);
    d3d11_append_sequence_indices(edges, PRIM_TYPE_POLYGON,
                                  polygon_indices, 1, true, true);
    assert(edges->len == 0);
    g_array_unref(edges);
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        char *polygon_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_POLYGON, POLY_MODE_LINE, false);
        assert(polygon_line && strstr(polygon_line, "line GSIn input[2]"));
        assert(strstr(polygon_line, "LineStream<GSOut> stream"));
        assert(strstr(polygon_line, "[maxvertexcount(2)]"));
        assert(strstr(polygon_line, "p0,p1,p2,0.0,0.0"));
        assert(strstr(polygon_line, "input[0],input[0]"));
        assert(strstr(polygon_line, "input[1],input[0]"));
        assert(!strstr(polygon_line, "nv2a_tri_mz_perspective(p0,p1,p2)"));
        g_free(polygon_line);
        char *wide_polygon_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_POLYGON, POLY_MODE_LINE, true);
        assert(wide_polygon_line &&
               strstr(wide_polygon_line, "TriangleStream<GSOut> stream"));
        assert(strstr(wide_polygon_line, "[maxvertexcount(4)]"));
        assert(strstr(wide_polygon_line,
                      "nv2aLineParams : packoffset(c327)"));
        assert(strstr(wide_polygon_line, "nv2a_line_offset(input[0].position"));
        assert(strstr(wide_polygon_line, "nv2a_make_wide_line_vertex"));
        g_free(wide_polygon_line);
    }
    assert(d3d11_triangle_primitive(PRIM_TYPE_TRIANGLES));
    assert(d3d11_triangle_primitive(PRIM_TYPE_TRIANGLE_STRIP));
    assert(d3d11_triangle_primitive(PRIM_TYPE_TRIANGLE_FAN));
    assert(d3d11_triangle_primitive(PRIM_TYPE_POLYGON));
    assert(!d3d11_triangle_primitive(PRIM_TYPE_QUADS));
    assert(d3d11_triangle_line_primitive(PRIM_TYPE_TRIANGLES));
    assert(d3d11_triangle_line_primitive(PRIM_TYPE_TRIANGLE_STRIP));
    assert(d3d11_triangle_line_primitive(PRIM_TYPE_TRIANGLE_FAN));
    assert(!d3d11_triangle_line_primitive(PRIM_TYPE_POLYGON));
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        char *quad_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUADS, POLY_MODE_LINE, false);
        char *strip_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUAD_STRIP, POLY_MODE_LINE, false);
        char *quad_point = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUADS, POLY_MODE_POINT, false);
        char *strip_point = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUAD_STRIP, POLY_MODE_POINT, false);
        assert(quad_line && strstr(quad_line, "LineStream<GSOut> stream"));
        assert(strip_line && strstr(strip_line, "LineStream<GSOut> stream"));
        assert(strstr(quad_line, "[maxvertexcount(8)]"));
        assert(strstr(strip_line, "[maxvertexcount(8)]"));
        assert(quad_point && strstr(quad_point, "TriangleStream<GSOut> stream"));
        assert(strip_point && strstr(strip_point, "TriangleStream<GSOut> stream"));
        assert(strstr(quad_point, "[maxvertexcount(16)]"));
        assert(strstr(strip_point, "[maxvertexcount(16)]"));
        assert(strstr(quad_point, "nv2a_make_point_vertex"));
        assert(strstr(strip_point, "nv2a_make_point_vertex"));
        assert(strstr(quad_point, "pointSize/viewport"));
        assert(strstr(strip_point, "pointSize/viewport"));
        assert(!strstr(quad_point, "PointStream<GSOut> stream"));
        assert(!strstr(strip_point, "PointStream<GSOut> stream"));
        assert(!strstr(quad_point, "LineStream<GSOut> stream"));
        assert(!strstr(strip_point, "LineStream<GSOut> stream"));
        assert(strstr(quad_point, "stream.Append("));
        assert(strstr(strip_point, "stream.Append("));
        assert(strstr(quad_point, "o.pointSize=vertex.pointSize"));
        assert(strstr(strip_point, "o.pointSize=vertex.pointSize"));
        char *wide_quad_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUADS, POLY_MODE_LINE, true);
        char *wide_strip_line = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_QUAD_STRIP, POLY_MODE_LINE, true);
        assert(wide_quad_line &&
               strstr(wide_quad_line, "TriangleStream<GSOut> stream"));
        assert(wide_strip_line &&
               strstr(wide_strip_line, "TriangleStream<GSOut> stream"));
        assert(strstr(wide_quad_line, "[maxvertexcount(16)]"));
        assert(strstr(wide_strip_line, "[maxvertexcount(16)]"));
        assert(strstr(wide_quad_line, "lineOffset3"));
        assert(strstr(wide_strip_line, "nv2a_make_wide_line_vertex"));
        assert(strstr(quad_point,
                      "input[0].depthPos0,input[0].depthPos0,input[0].depthPos0"));
        if (smooth) {
            assert(strstr(quad_line, "o.diffuse=vertex.diffuse"));
            assert(strstr(strip_line, "o.diffuse=vertex.diffuse"));
            assert(strstr(quad_point, "o.diffuse=vertex.diffuse"));
            assert(strstr(strip_point, "o.diffuse=vertex.diffuse"));
        } else {
            assert(strstr(quad_line, "o.diffuse=flatVertex.diffuse"));
            assert(strstr(strip_line, "o.diffuse=flatVertex.diffuse"));
            assert(strstr(quad_point, "o.diffuse=flatVertex.diffuse"));
            assert(strstr(strip_point, "o.diffuse=flatVertex.diffuse"));
        }
        const char *names[] = {"QUADS_LINE", "QUAD_STRIP_LINE",
                               "QUADS_POINT", "QUAD_STRIP_POINT",
                               "WIDE_QUADS_LINE", "WIDE_QUAD_STRIP_LINE"};
        char *sources[] = {quad_line, strip_line, quad_point, strip_point,
                           wide_quad_line, wide_strip_line};
        for (unsigned int i = 0; i < G_N_ELEMENTS(sources); i++) {
            printf("%s_%s\n", smooth ? "SMOOTH" : "FLAT", names[i]);
            puts(sources[i]);
            g_free(sources[i]);
        }
    }
    static const enum ShaderPrimitiveMode triangles[] = {
        PRIM_TYPE_TRIANGLES, PRIM_TYPE_TRIANGLE_STRIP,
        PRIM_TYPE_TRIANGLE_FAN, PRIM_TYPE_POLYGON,
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            for (unsigned int i = 0; i < G_N_ELEMENTS(triangles); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangles[i], POLY_MODE_POINT,
                    false);
                assert(source && strstr(source, "TriangleStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(12)]"));
                assert(!strstr(source, "PointStream<GSOut> stream"));
                assert(strstr(source, "nv2a_make_point_vertex(v,"));
                assert(strstr(source, "stream.RestartStrip()"));
                assert(strstr(source, "for(uint i=0;i<3;i++)"));
                assert(strstr(source,
                              "v.depthPos0=input[i].depthPos0; "
                              "v.depthPos1=input[i].depthPos0; "
                              "v.depthPos2=input[i].depthPos0;"));
                if (smooth) {
                    assert(strstr(source, "v.diffuse=input[i].diffuse"));
                } else {
                    assert(strstr(source, "v.diffuse=flatDiffuse"));
                }
                if (provoking == 0) {
                    assert(strstr(source, "uint p=0u"));
                } else if (provoking == 1) {
                    assert(strstr(source, "uint p=1u"));
                } else if (provoking == 2) {
                    assert(strstr(source, "uint p=2u"));
                } else {
                    assert(strstr(source, "uint p=(2u-(primitiveId&1u))"));
                }
                g_free(source);
            }
        }
    }
    static const enum ShaderPrimitiveMode triangle_lines[] = {
        PRIM_TYPE_TRIANGLES, PRIM_TYPE_TRIANGLE_STRIP,
        PRIM_TYPE_TRIANGLE_FAN,
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            for (unsigned int i = 0; i < G_N_ELEMENTS(triangle_lines); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangle_lines[i], POLY_MODE_LINE,
                    false);
                assert(source && strstr(source, "LineStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(6)]"));
                assert(!strstr(source, "TriangleStream<GSOut> stream"));
                assert(!strstr(source, "PointStream<GSOut> stream"));
                assert(strstr(source, "for(uint i=0;i<3;i++)"));
                assert(strstr(source,
                              "float4 lp2=float4(lp0.xy+float2(-delta.y,delta.x),lp0.zw)"));
                assert(strstr(source, "stream.RestartStrip()"));
                if (smooth) {
                    assert(strstr(source, "o.diffuse=vertex.diffuse"));
                } else {
                    assert(strstr(source, "o.diffuse=flatVertex.diffuse"));
                }
                if (provoking == 0) {
                    assert(strstr(source, "uint p=0u"));
                } else if (provoking == 1) {
                    assert(strstr(source, "uint p=1u"));
                } else if (provoking == 2) {
                    assert(strstr(source, "uint p=2u"));
                } else {
                    assert(strstr(source, "uint p=(2u-(primitiveId&1u))"));
                }
                g_free(source);
                char *wide_source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangle_lines[i],
                    POLY_MODE_LINE, true);
                assert(wide_source &&
                       strstr(wide_source, "TriangleStream<GSOut> stream"));
                assert(strstr(wide_source, "[maxvertexcount(12)]"));
                assert(strstr(wide_source, "nv2a_line_offset(input[i].position"));
                assert(strstr(wide_source, "nv2a_make_wide_line_vertex"));
                g_free(wide_source);
            }
        }
    }
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        char *wide_lines = d3d11_depth_geometry_shader_source(
            smooth != 0, 1, PRIM_TYPE_LINES, POLY_MODE_FILL, true);
        assert(wide_lines && strstr(wide_lines, "TriangleStream<GSOut> stream"));
        assert(strstr(wide_lines, "[maxvertexcount(4)]"));
        assert(strstr(wide_lines, "nv2a_line_offset(input[0].position"));
        g_free(wide_lines);
    }
    char *points = d3d11_depth_geometry_shader_source(
        true, 2, PRIM_TYPE_POINTS, POLY_MODE_FILL, false);
    assert(points && strstr(points, "point GSIn input[1]"));
    assert(strstr(points, "TriangleStream<GSOut> stream"));
    assert(strstr(points, "[maxvertexcount(4)]"));
    assert(strstr(points, "pointSize/viewport"));
    assert(strstr(points, "float2 pointCoord:TEXCOORD11"));
    assert(strstr(points, "float2(0.0,0.0)"));
    assert(strstr(points, "float2(1.0,1.0)"));
    assert(strstr(points,
                  "pointVertex.backDiffuse=pointVertex.diffuse"));
    assert(strstr(points, "stream.RestartStrip()"));
    char *triangle_point = d3d11_depth_geometry_shader_source(
        false, 2, PRIM_TYPE_TRIANGLES, POLY_MODE_POINT, false);
    puts("TRIANGLE_POINT_BEGIN");
    puts(triangle_point);
    puts("TRIANGLE_POINT_END");
    g_free(triangle_point);
    puts("DIRECT_POINTS_BEGIN");
    puts(points);
    puts("DIRECT_POINTS_END");
    g_free(points);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-polygon-mode-") as tmp:
        c_file = pathlib.Path(tmp) / "polygon-mode-hlsl.c"
        executable = pathlib.Path(tmp) / "polygon-mode-hlsl"
        c_file.write_text(c_program, encoding="utf-8")
        glib_flags = shlex.split(
            subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "glib-2.0"],
                text=True))
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wformat=2", str(c_file), "-o", str(executable),
             *glib_flags], check=True)
        output = subprocess.check_output([str(executable)], text=True)

    expected = {
        "FLAT_QUADS_LINE": [0, 1, 1, 2, 2, 3, 3, 0],
        "SMOOTH_QUADS_LINE": [0, 1, 1, 2, 2, 3, 3, 0],
        "FLAT_QUAD_STRIP_LINE": [0, 1, 1, 3, 3, 2, 2, 0],
        "SMOOTH_QUAD_STRIP_LINE": [0, 1, 1, 3, 3, 2, 2, 0],
        "FLAT_QUADS_POINT": [0, 1, 2, 3],
        "SMOOTH_QUADS_POINT": [0, 1, 2, 3],
        "FLAT_QUAD_STRIP_POINT": [0, 1, 2, 3],
        "SMOOTH_QUAD_STRIP_POINT": [0, 1, 2, 3],
    }
    for marker, expected_vertices in expected.items():
        source = output.split(marker + "\n", 1)[1]
        source = source.split("\nFLAT_", 1)[0].split("\nSMOOTH_", 1)[0]
        if marker.endswith("POINT"):
            vertices = [int(value) for value in re.findall(
                r"GSOut pointVertex\d+=nv2a_make_quad_vertex\(input\[(\d+)\],input\[3\],",
                source)]
        else:
            vertices = [int(value) for value in re.findall(
                r"stream\.Append\(nv2a_make_quad_vertex\(input\[(\d+)\],input\[3\],",
                source)]
        assert vertices == expected_vertices, (marker, vertices)
        if marker.endswith("LINE"):
            assert source.count("stream.RestartStrip();") == 4
        else:
            assert "TriangleStream<GSOut> stream" in source
            assert "PointStream<GSOut> stream" not in source
            assert "[maxvertexcount(16)]" in source
            assert source.count("nv2a_make_point_vertex(pointVertex") == 16
            assert source.count("stream.RestartStrip();") == 4
    print("PASS: polygon LINE boundary, triangle/quad LINE/POINT GS, point-sprite mapping and analytic smoothing routing")
    point_source = output.split("DIRECT_POINTS_BEGIN\n", 1)[1].split(
        "\nDIRECT_POINTS_END", 1)[0]
    triangle_point_source = output.split("TRIANGLE_POINT_BEGIN\n", 1)[1].split(
        "\nTRIANGLE_POINT_END", 1)[0]
    if shutil.which("glslangValidator"):
        with tempfile.TemporaryDirectory(prefix="xemu-d3d11-point-gs-") as tmp:
            tmp = pathlib.Path(tmp)
            geometry_sources = {
                "point-list": point_source,
                "triangle-point": triangle_point_source,
            }
            for marker in ("FLAT_QUADS_POINT", "SMOOTH_QUADS_POINT",
                           "FLAT_QUAD_STRIP_POINT",
                           "SMOOTH_QUAD_STRIP_POINT"):
                generated = output.split(marker + "\n", 1)[1]
                geometry_sources[marker.lower().replace("_", "-")] = \
                    re.split(r"\n(?:FLAT|SMOOTH)_[A-Z_]+\n|\nTRIANGLE_POINT_BEGIN\n",
                             generated, maxsplit=1)[0]
            for name, geometry_source in geometry_sources.items():
                hlsl = tmp / f"{name}.hlsl"
                bytecode = tmp / f"{name}.spv"
                hlsl.write_text(geometry_source, encoding="utf-8")
                subprocess.run(
                    ["glslangValidator", "-D", "-V", "-S", "geom", "-e",
                     "main", "-o", str(bytecode), str(hlsl)], check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print("PASS: direct points and polygon POINT HLSL parsed for triangles, quads and quad strips")


if __name__ == "__main__":
    main()
