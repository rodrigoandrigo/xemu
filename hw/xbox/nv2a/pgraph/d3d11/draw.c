/*
 * GeForce NV2A PGRAPH Direct3D 11 draw path
 *
 * Portions of the primitive conversion logic are derived from the
 * Cxbx-Reloaded IndexBufferConvert implementation.
 *
 * This file is part of the Cxbx project.
 *
 * Cxbx and Cxbe are free software; you can redistribute them and/or modify
 * them under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * Modified for xemu's LLE PGRAPH D3D11 renderer on 2026-09-26.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "draw.h"

bool pgraph_d3d11_draw_init(PGRAPHD3D11State *r, Error **errp)
{
    (void)errp;
    r->draw = g_new0(PGRAPHD3D11DrawState, 1);
    return true;
}

void pgraph_d3d11_draw_finalize(PGRAPHD3D11State *r)
{
    if (!r->draw) {
        return;
    }
    if (r->draw->rasterizer_state) {
        ID3D11RasterizerState_Release(r->draw->rasterizer_state);
    }
    if (r->draw->vertex_buffer) {
        ID3D11Buffer_Release(r->draw->vertex_buffer);
    }
    if (r->draw->index_buffer) {
        ID3D11Buffer_Release(r->draw->index_buffer);
    }
    g_free(r->draw);
    r->draw = NULL;
}

void pgraph_d3d11_draw_trim(PGRAPHD3D11State *r)
{
    if (!r->draw) {
        return;
    }
    if (r->draw->rasterizer_state) {
        ID3D11RasterizerState_Release(r->draw->rasterizer_state);
        r->draw->rasterizer_state = NULL;
    }
    if (r->draw->vertex_buffer) {
        ID3D11Buffer_Release(r->draw->vertex_buffer);
        r->draw->vertex_buffer = NULL;
    }
    if (r->draw->index_buffer) {
        ID3D11Buffer_Release(r->draw->index_buffer);
        r->draw->index_buffer = NULL;
    }
    r->draw->rasterizer_state_valid = false;
    r->draw->vertex_buffer_size = 0;
    r->draw->index_buffer_size = 0;
    r->draw->prepared = false;
}

static D3D11_CULL_MODE d3d11_cull_mode(PGRAPHState *pg, bool *skip_draw)
{
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    *skip_draw = false;
    if (!(raster & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
        return D3D11_CULL_NONE;
    }
    switch (GET_MASK(raster, NV_PGRAPH_SETUPRASTER_CULLCTRL)) {
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT:
        return D3D11_CULL_FRONT;
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK:
        return D3D11_CULL_BACK;
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK:
        *skip_draw = true;
        return D3D11_CULL_NONE;
    default:
        return D3D11_CULL_NONE;
    }
}

static D3D11_FILL_MODE d3d11_fill_mode(uint32_t raster)
{
    unsigned int front = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    unsigned int back = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    if (front == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL &&
        back == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL) {
        return D3D11_FILL_SOLID;
    }
    /* D3D11 exposes one fill mode for both faces and has no point polygon
     * mode. Wireframe is the closest native representation when either NV2A
     * face requests LINE or POINT. */
    return D3D11_FILL_WIREFRAME;
}

static bool d3d11_polygon_offset_enabled(PGRAPHState *pg, uint32_t raster)
{
    if (pg->primitive_mode < PRIM_TYPE_TRIANGLES) {
        return false;
    }
    unsigned int mode = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    return (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL &&
            (raster & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE)) ||
           (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE &&
            (raster & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)) ||
           (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT &&
            (raster & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE));
}

static bool d3d11_finite_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

static void G_GNUC_UNUSED d3d11_append_sequence_indices(
    GArray *indices, enum ShaderPrimitiveMode mode, const uint32_t *source,
    uint32_t count, bool clockwise)
{
#define APPEND_INDEX(v)                 \
    do {                                \
        uint32_t n = (v);               \
        g_array_append_val(indices, n); \
    } while (0)
    if (mode == PRIM_TYPE_LINE_LOOP) {
        for (uint32_t i = 1; i < count; i++) {
            APPEND_INDEX(source[i - 1]);
            APPEND_INDEX(source[i]);
        }
        if (count > 1) {
            APPEND_INDEX(source[count - 1]);
            APPEND_INDEX(source[0]);
        }
    } else if (mode == PRIM_TYPE_TRIANGLE_FAN || mode == PRIM_TYPE_POLYGON) {
        for (uint32_t i = 2; i < count; i++) {
            APPEND_INDEX(source[0]);
            APPEND_INDEX(source[i - 1]);
            APPEND_INDEX(source[i]);
        }
    } else if (mode == PRIM_TYPE_QUADS) {
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            uint32_t a = source[i], b = source[i + 1];
            uint32_t c = source[i + 2], d = source[i + 3];
            APPEND_INDEX(a);
            APPEND_INDEX(clockwise ? b : d);
            APPEND_INDEX(clockwise ? d : b);
            APPEND_INDEX(b);
            APPEND_INDEX(clockwise ? c : d);
            APPEND_INDEX(clockwise ? d : c);
        }
    } else if (mode == PRIM_TYPE_QUAD_STRIP) {
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            uint32_t a = source[i], b = source[i + 1];
            uint32_t c = source[i + 3], d = source[i + 2];
            APPEND_INDEX(a);
            APPEND_INDEX(clockwise ? b : d);
            APPEND_INDEX(clockwise ? d : b);
            APPEND_INDEX(b);
            APPEND_INDEX(clockwise ? c : d);
            APPEND_INDEX(clockwise ? d : c);
        }
    } else {
        g_array_append_vals(indices, source, count);
    }
#undef APPEND_INDEX
}

bool pgraph_d3d11_update_rasterizer(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11DrawState *draw = r->draw;
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    bool skip_draw;
    float depth_bias = 0.0f, slope_bias = 0.0f;
    if (d3d11_polygon_offset_enabled(pg, raster)) {
        uint32_t bias_bits = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        uint32_t slope_bits = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
        memcpy(&depth_bias, &bias_bits, sizeof(depth_bias));
        memcpy(&slope_bias, &slope_bits, sizeof(slope_bias));
    }
    int32_t integer_depth_bias = 0;
    if (d3d11_finite_float(depth_bias)) {
        integer_depth_bias =
            depth_bias >= INT32_MAX ?
                INT32_MAX :
            depth_bias <= INT32_MIN ?
                INT32_MIN :
                (int32_t)(depth_bias + (depth_bias >= 0 ? 0.5f : -0.5f));
    }
    D3D11_RASTERIZER_DESC desc = {
        .FillMode = d3d11_fill_mode(raster),
        .CullMode = d3d11_cull_mode(pg, &skip_draw),
        /* The PGRAPH clip-space Y direction is inverted relative to D3D11. */
        .FrontCounterClockwise = !(raster & NV_PGRAPH_SETUPRASTER_FRONTFACE),
        .DepthBias = integer_depth_bias,
        .DepthBiasClamp = 0.0f,
        .SlopeScaledDepthBias =
            d3d11_finite_float(slope_bias) ? slope_bias : 0.0f,
        .DepthClipEnable = FALSE,
        .ScissorEnable = TRUE,
        .MultisampleEnable =
            !!GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
                       NV_PGRAPH_ANTIALIASING_ENABLE),
        .AntialiasedLineEnable =
            !!(raster & NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE),
    };
    draw->skip_draw = skip_draw;
    if (!draw->rasterizer_state_valid ||
        memcmp(&draw->rasterizer_desc, &desc, sizeof(desc))) {
        ID3D11RasterizerState *state = NULL;
        HRESULT hr =
            ID3D11Device_CreateRasterizerState(r->device, &desc, &state);
        if (FAILED(hr)) {
            error_setg(errp,
                       "D3D11: rasterizer-state creation failed "
                       "(HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
        if (draw->rasterizer_state) {
            ID3D11RasterizerState_Release(draw->rasterizer_state);
        }
        draw->rasterizer_state = state;
        draw->rasterizer_desc = desc;
        draw->rasterizer_state_valid = true;
    }
    ID3D11DeviceContext_RSSetState(r->context, draw->rasterizer_state);

    unsigned int viewport_width = pg->surface_binding_dim.width;
    unsigned int viewport_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &viewport_width, &viewport_height);
    D3D11_VIEWPORT viewport = {
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = MAX(1, viewport_width),
        .Height = MAX(1, viewport_height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    ID3D11DeviceContext_RSSetViewports(r->context, 1, &viewport);

    unsigned int x = pg->surface_shape.clip_x;
    unsigned int y = pg->surface_shape.clip_y;
    unsigned int width = pg->surface_shape.clip_width;
    unsigned int height = pg->surface_shape.clip_height;
    pgraph_apply_anti_aliasing_factor(pg, &x, &y);
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    pgraph_apply_scaling_factor(pg, &x, &y);
    pgraph_apply_scaling_factor(pg, &width, &height);
    D3D11_RECT scissor = {
        .left = x,
        .top = y,
        .right = x + width,
        .bottom = y + height,
    };
    ID3D11DeviceContext_RSSetScissorRects(r->context, 1, &scissor);
    return true;
}

typedef struct D3D11PackedVertex {
    float attributes[NV2A_VERTEXSHADER_ATTRIBUTES][4];
} D3D11PackedVertex;

typedef enum D3D11VertexSource {
    D3D11_VERTEX_SOURCE_ARRAY,
    D3D11_VERTEX_SOURCE_INLINE_ARRAY,
    D3D11_VERTEX_SOURCE_INLINE_BUFFER,
} D3D11VertexSource;

static bool d3d11_ensure_dynamic_buffer(PGRAPHD3D11State *r,
                                        ID3D11Buffer **buffer, size_t *capacity,
                                        size_t required, UINT bind_flags,
                                        Error **errp)
{
    if (*buffer && *capacity >= required) {
        return true;
    }
    if (required > UINT32_MAX) {
        error_setg(errp, "D3D11: geometry buffer exceeds the D3D11 limit");
        return false;
    }
    size_t new_capacity = MAX((size_t)4096, *capacity);
    while (new_capacity < required) {
        if (new_capacity > UINT32_MAX / 2) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }
    D3D11_BUFFER_DESC desc = {
        .ByteWidth = new_capacity,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = bind_flags,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    ID3D11Buffer *new_buffer = NULL;
    HRESULT hr = ID3D11Device_CreateBuffer(r->device, &desc, NULL, &new_buffer);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: geometry-buffer creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    if (*buffer) {
        ID3D11Buffer_Release(*buffer);
    }
    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

static bool d3d11_ensure_input_layout(PGRAPHD3D11State *r, Error **errp)
{
    PGRAPHD3D11VertexShader *shader = r->shaders->vertex;
    if (shader->input_layout) {
        ID3D11DeviceContext_IASetInputLayout(r->context, shader->input_layout);
        return true;
    }
    D3D11_INPUT_ELEMENT_DESC elements[NV2A_VERTEXSHADER_ATTRIBUTES];
    for (unsigned int i = 0; i < ARRAY_SIZE(elements); i++) {
        elements[i] = (D3D11_INPUT_ELEMENT_DESC){
            .SemanticName = "ATTRIBUTE",
            .SemanticIndex = i,
            .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
            .InputSlot = 0,
            .AlignedByteOffset = i * sizeof(float) * 4,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
        };
    }
    HRESULT hr = ID3D11Device_CreateInputLayout(
        r->device, elements, ARRAY_SIZE(elements),
        ID3D10Blob_GetBufferPointer(shader->bytecode),
        ID3D10Blob_GetBufferSize(shader->bytecode), &shader->input_layout);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: input-layout creation failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    ID3D11DeviceContext_IASetInputLayout(r->context, shader->input_layout);
    return true;
}

static void d3d11_decode_attribute(const VertexAttribute *attr,
                                   const uint8_t *data, float value[4])
{
    VertexAttribute decoded = *attr;
    pgraph_update_inline_value(&decoded, data);
    memcpy(value, decoded.inline_value, sizeof(decoded.inline_value));
    if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D &&
        attr->count >= 3) {
        float red = value[2];
        value[2] = value[0];
        value[0] = red;
    }
}

static bool d3d11_pack_vertices(NV2AState *d, D3D11VertexSource source,
                                uint32_t first, uint32_t count,
                                D3D11PackedVertex *vertices, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    size_t inline_stride = 0;
    if (source == D3D11_VERTEX_SOURCE_INLINE_ARRAY) {
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (!attr->count) {
                continue;
            }
            inline_stride = ROUND_UP(inline_stride, attr->size);
            attr->inline_array_offset = inline_stride;
            inline_stride += attr->size * attr->count;
            inline_stride = ROUND_UP(inline_stride, attr->size);
        }
        if (!inline_stride ||
            (uint64_t)(first + count) * inline_stride >
                (uint64_t)pg->inline_array_length * sizeof(uint32_t)) {
            error_setg(errp, "D3D11: invalid NV2A inline vertex array");
            return false;
        }
    }

    for (unsigned int attr_index = 0; attr_index < NV2A_VERTEXSHADER_ATTRIBUTES;
         attr_index++) {
        VertexAttribute *attr = &pg->vertex_attributes[attr_index];
        if (!attr->count || (source == D3D11_VERTEX_SOURCE_INLINE_BUFFER &&
                             !attr->inline_buffer_populated)) {
            for (uint32_t vertex = 0; vertex < count; vertex++) {
                memcpy(vertices[vertex].attributes[attr_index],
                       attr->inline_value, sizeof(attr->inline_value));
            }
            continue;
        }
        if (source == D3D11_VERTEX_SOURCE_INLINE_BUFFER) {
            for (uint32_t vertex = 0; vertex < count; vertex++) {
                memcpy(vertices[vertex].attributes[attr_index],
                       &attr->inline_buffer[(first + vertex) * 4],
                       sizeof(attr->inline_value));
            }
            continue;
        }

        const uint8_t *base;
        size_t available;
        size_t stride;
        if (source == D3D11_VERTEX_SOURCE_INLINE_ARRAY) {
            base =
                (const uint8_t *)pg->inline_array + attr->inline_array_offset;
            available = pg->inline_array_length * sizeof(uint32_t) -
                        attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            base = nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            if (!base || attr->offset >= dma_len) {
                error_setg(errp, "D3D11: vertex attribute %u is outside DMA",
                           attr_index);
                return false;
            }
            base += attr->offset;
            available = dma_len - attr->offset;
            stride = attr->stride;
        }
        size_t element_size = attr->size * attr->count;
        if (!stride) {
            if (element_size > available) {
                error_setg(errp,
                           "D3D11: constant vertex attribute %u is "
                           "outside DMA",
                           attr_index);
                return false;
            }
            float value[4];
            d3d11_decode_attribute(attr, base, value);
            for (uint32_t vertex = 0; vertex < count; vertex++) {
                memcpy(vertices[vertex].attributes[attr_index], value,
                       sizeof(value));
            }
            memcpy(attr->inline_value, value, sizeof(value));
            continue;
        }
        uint64_t last_offset = (uint64_t)(first + count - 1) * stride;
        if (last_offset + element_size > available) {
            error_setg(errp,
                       "D3D11: vertex attribute %u exceeds its DMA "
                       "mapping",
                       attr_index);
            return false;
        }
        for (uint32_t vertex = 0; vertex < count; vertex++) {
            d3d11_decode_attribute(attr, base + (first + vertex) * stride,
                                   vertices[vertex].attributes[attr_index]);
        }
        memcpy(attr->inline_value, vertices[count - 1].attributes[attr_index],
               sizeof(attr->inline_value));
    }
    return true;
}

static bool d3d11_upload_vertices(NV2AState *d, D3D11VertexSource source,
                                  uint32_t first, uint32_t count, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    size_t size = count * sizeof(D3D11PackedVertex);
    if (!count || !d3d11_ensure_dynamic_buffer(
                      r, &r->draw->vertex_buffer, &r->draw->vertex_buffer_size,
                      size, D3D11_BIND_VERTEX_BUFFER, errp)) {
        return count == 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(
        r->context, (ID3D11Resource *)r->draw->vertex_buffer, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: vertex-buffer map failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    bool packed =
        d3d11_pack_vertices(d, source, first, count, mapped.pData, errp);
    ID3D11DeviceContext_Unmap(r->context,
                              (ID3D11Resource *)r->draw->vertex_buffer, 0);
    if (!packed) {
        return false;
    }
    UINT stride = sizeof(D3D11PackedVertex), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(
        r->context, 0, 1, &r->draw->vertex_buffer, &stride, &offset);
    return d3d11_ensure_input_layout(r, errp);
}

static bool d3d11_native_topology(enum ShaderPrimitiveMode mode,
                                  D3D11_PRIMITIVE_TOPOLOGY *topology)
{
    switch (mode) {
    case PRIM_TYPE_POINTS:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        return true;
    case PRIM_TYPE_LINES:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        return true;
    case PRIM_TYPE_LINE_STRIP:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        return true;
    case PRIM_TYPE_TRIANGLES:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        return true;
    case PRIM_TYPE_TRIANGLE_STRIP:
        *topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        return true;
    default:
        *topology = mode == PRIM_TYPE_LINE_LOOP ?
                        D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
                        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        return false;
    }
}

static bool d3d11_upload_indices(PGRAPHD3D11State *r, const uint32_t *indices,
                                 uint32_t count, Error **errp)
{
    size_t size = count * sizeof(uint32_t);
    if (!count || !d3d11_ensure_dynamic_buffer(
                      r, &r->draw->index_buffer, &r->draw->index_buffer_size,
                      size, D3D11_BIND_INDEX_BUFFER, errp)) {
        return count == 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(
        r->context, (ID3D11Resource *)r->draw->index_buffer, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: index-buffer map failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    memcpy(mapped.pData, indices, size);
    ID3D11DeviceContext_Unmap(r->context,
                              (ID3D11Resource *)r->draw->index_buffer, 0);
    ID3D11DeviceContext_IASetIndexBuffer(r->context, r->draw->index_buffer,
                                         DXGI_FORMAT_R32_UINT, 0);
    return true;
}

static bool d3d11_emit_draw(NV2AState *d, D3D11VertexSource source,
                            uint32_t first, uint32_t vertex_count,
                            const uint32_t *source_indices,
                            uint32_t source_index_count, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (!vertex_count || r->draw->skip_draw) {
        return true;
    }
    if (!d3d11_upload_vertices(d, source, first, vertex_count, errp)) {
        return false;
    }
    D3D11_PRIMITIVE_TOPOLOGY topology;
    bool native = d3d11_native_topology(pg->primitive_mode, &topology);
    ID3D11DeviceContext_IASetPrimitiveTopology(r->context, topology);
    if (!source_indices && native) {
        ID3D11DeviceContext_Draw(r->context, vertex_count, 0);
        return true;
    }

    GArray *indices = g_array_sized_new(FALSE, FALSE, sizeof(uint32_t),
                                        source_index_count ?: vertex_count);
    if (source_indices) {
        for (uint32_t i = 0; i < source_index_count; i++) {
            uint32_t index = source_indices[i] - first;
            g_array_append_val(indices, index);
        }
    } else {
        for (uint32_t i = 0; i < vertex_count; i++) {
            uint32_t index = i;
            g_array_append_val(indices, index);
        }
    }
    if (!native) {
        GArray *converted = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        d3d11_append_sequence_indices(converted, pg->primitive_mode,
                                      (const uint32_t *)indices->data,
                                      indices->len, true);
        g_array_unref(indices);
        indices = converted;
    }
    bool uploaded = d3d11_upload_indices(r, (const uint32_t *)indices->data,
                                         indices->len, errp);
    if (uploaded && indices->len) {
        ID3D11DeviceContext_DrawIndexed(r->context, indices->len, 0, 0);
    }
    g_array_unref(indices);
    return uploaded;
}

bool pgraph_d3d11_flush_draw(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    if (pg->draw_arrays_length) {
        for (unsigned int i = 0; i < pg->draw_arrays_length; i++) {
            if (!d3d11_emit_draw(d, D3D11_VERTEX_SOURCE_ARRAY,
                                 pg->draw_arrays_start[i],
                                 pg->draw_arrays_count[i], NULL, 0, errp)) {
                return false;
            }
        }
        return true;
    }
    if (pg->inline_elements_length) {
        uint32_t min_element = UINT32_MAX, max_element = 0;
        for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
            min_element = MIN(min_element, pg->inline_elements[i]);
            max_element = MAX(max_element, pg->inline_elements[i]);
        }
        return d3d11_emit_draw(d, D3D11_VERTEX_SOURCE_ARRAY, min_element,
                               max_element - min_element + 1,
                               pg->inline_elements, pg->inline_elements_length,
                               errp);
    }
    if (pg->inline_buffer_length) {
        bool result = d3d11_emit_draw(d, D3D11_VERTEX_SOURCE_INLINE_BUFFER, 0,
                                      pg->inline_buffer_length, NULL, 0, errp);
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->inline_buffer_populated && pg->inline_buffer_length) {
                memcpy(attr->inline_value,
                       &attr->inline_buffer[(pg->inline_buffer_length - 1) * 4],
                       sizeof(attr->inline_value));
                attr->inline_buffer_populated = false;
            }
        }
        return result;
    }
    if (pg->inline_array_length) {
        size_t vertex_size = 0;
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count) {
                vertex_size = ROUND_UP(vertex_size, attr->size);
                vertex_size += attr->size * attr->count;
                vertex_size = ROUND_UP(vertex_size, attr->size);
            }
        }
        if (!vertex_size) {
            error_setg(errp, "D3D11: inline array has no vertex attributes");
            return false;
        }
        uint32_t count =
            pg->inline_array_length * sizeof(uint32_t) / vertex_size;
        return d3d11_emit_draw(d, D3D11_VERTEX_SOURCE_INLINE_ARRAY, 0, count,
                               NULL, 0, errp);
    }
    return true;
}
