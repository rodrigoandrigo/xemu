/*
 * Geforce NV2A PGRAPH Direct3D 12 draw path
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/fast-hash.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "spirv_to_dxil.h"
#include <glib/gstdio.h>
#include <d3d12sdklayers.h>

#define D3D12_CANONICAL_VERTEX_STRIDE \
    (NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float))
#define D3D12_SHADER_BINDING_CACHE_SIZE 256
#define D3D12_PIPELINE_CACHE_SIZE 512
#define D3D12_PIPELINE_CACHE_MAGIC 0x58505331u
#define D3D12_PIPELINE_CACHE_VERSION 1
#define D3D12_PIPELINE_CACHE_MAX_FILE (128u * 1024u * 1024u)
#define D3D12_MAX_DRAW_SUBMISSIONS 64

void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);

static GMutex d3d12_glsl_compiler_lock;
static unsigned int d3d12_glsl_compiler_users;

static void d3d12_glsl_compiler_acquire(void)
{
    g_mutex_lock(&d3d12_glsl_compiler_lock);
    if (d3d12_glsl_compiler_users++ == 0) {
        pgraph_vk_init_glsl_compiler();
    }
    g_mutex_unlock(&d3d12_glsl_compiler_lock);
}

static void d3d12_glsl_compiler_release(void)
{
    g_mutex_lock(&d3d12_glsl_compiler_lock);
    if (!d3d12_glsl_compiler_users) {
        g_mutex_unlock(&d3d12_glsl_compiler_lock);
        return;
    }
    if (--d3d12_glsl_compiler_users == 0) {
        pgraph_vk_finalize_glsl_compiler();
    }
    g_mutex_unlock(&d3d12_glsl_compiler_lock);
}

typedef struct PGRAPHD3D12ShaderEntry {
    PGRAPHD3D12ShaderBinding *binding;
    uint64_t last_use;
} PGRAPHD3D12ShaderEntry;

typedef struct PGRAPHD3D12PipelineKey {
    ShaderState shader_state;
    uint32_t regs[8];
    DXGI_FORMAT rtv_format;
    DXGI_FORMAT dsv_format;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
} PGRAPHD3D12PipelineKey;

typedef struct PGRAPHD3D12PipelineEntry {
    PGRAPHD3D12PipelineKey key;
    ID3D12PipelineState *pipeline;
    GBytes *cached_blob;
    uint64_t last_use;
} PGRAPHD3D12PipelineEntry;

typedef struct PGRAPHD3D12DrawRange {
    UINT start_index;
    UINT index_count;
} PGRAPHD3D12DrawRange;

typedef struct PGRAPHD3D12DrawSubmission {
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *vertex_buffer;
    ID3D12Resource *index_buffer;
    ID3D12Resource *vertex_constants;
    ID3D12Resource *pixel_constants;
    ID3D12Resource *runtime_constants;
    ID3D12PipelineState *pipeline;
    uint64_t fence_value;
} PGRAPHD3D12DrawSubmission;

struct PGRAPHD3D12DrawState {
    ID3D12RootSignature *root_signature;
    PGRAPHD3D12ShaderBinding *shader;
    ID3D12PipelineState *pipeline;
    uint64_t use_counter;
    PGRAPHD3D12ShaderEntry shader_cache[D3D12_SHADER_BINDING_CACHE_SIZE];
    PGRAPHD3D12PipelineEntry pipeline_cache[D3D12_PIPELINE_CACHE_SIZE];
    char *pipeline_cache_path;
    LUID adapter_luid;
    bool pipeline_cache_dirty;
    bool glsl_compiler_initialized;
    GQueue submissions;
};

typedef struct D3D12PipelineCacheHeader {
    uint32_t magic;
    uint32_t version;
    LUID adapter_luid;
    uint32_t entry_count;
    uint32_t key_size;
} D3D12PipelineCacheHeader;

typedef struct D3D12PipelineCacheRecord {
    uint64_t key_hash;
    uint64_t blob_hash;
    uint32_t blob_size;
    uint32_t reserved;
    PGRAPHD3D12PipelineKey key;
} D3D12PipelineCacheRecord;

static void d3d12_pipeline_cache_load(PGRAPHD3D12DrawState *draw)
{
    gchar *contents = NULL;
    gsize length = 0;
    if (!draw->pipeline_cache_path ||
        !g_file_get_contents(draw->pipeline_cache_path, &contents, &length,
                             NULL) ||
        length < sizeof(D3D12PipelineCacheHeader) ||
        length > D3D12_PIPELINE_CACHE_MAX_FILE) {
        g_free(contents);
        return;
    }
    const uint8_t *cursor = (const uint8_t *)contents;
    const uint8_t *end = cursor + length;
    D3D12PipelineCacheHeader header;
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);
    if (header.magic != D3D12_PIPELINE_CACHE_MAGIC ||
        header.version != D3D12_PIPELINE_CACHE_VERSION ||
        header.key_size != sizeof(PGRAPHD3D12PipelineKey) ||
        memcmp(&header.adapter_luid, &draw->adapter_luid, sizeof(LUID)) ||
        header.entry_count > D3D12_PIPELINE_CACHE_SIZE) {
        g_free(contents);
        return;
    }
    for (uint32_t i = 0; i < header.entry_count; i++) {
        if ((size_t)(end - cursor) < sizeof(D3D12PipelineCacheRecord)) {
            break;
        }
        D3D12PipelineCacheRecord record;
        memcpy(&record, cursor, sizeof(record));
        cursor += sizeof(record);
        if (!record.blob_size ||
            record.blob_size > D3D12_PIPELINE_CACHE_MAX_FILE ||
            (size_t)(end - cursor) < record.blob_size ||
            record.key_hash !=
                fast_hash((const uint8_t *)&record.key, sizeof(record.key)) ||
            record.blob_hash != fast_hash(cursor, record.blob_size)) {
            break;
        }
        PGRAPHD3D12PipelineEntry *entry = &draw->pipeline_cache[i];
        entry->key = record.key;
        entry->cached_blob = g_bytes_new(cursor, record.blob_size);
        entry->last_use = ++draw->use_counter;
        cursor += record.blob_size;
    }
    g_free(contents);
}

static void d3d12_pipeline_cache_save(PGRAPHD3D12DrawState *draw)
{
    if (!draw->pipeline_cache_dirty || !draw->pipeline_cache_path) {
        return;
    }
    GByteArray *data = g_byte_array_sized_new(4096);
    D3D12PipelineCacheHeader header = {
        .magic = D3D12_PIPELINE_CACHE_MAGIC,
        .version = D3D12_PIPELINE_CACHE_VERSION,
        .adapter_luid = draw->adapter_luid,
        .key_size = sizeof(PGRAPHD3D12PipelineKey),
    };
    g_byte_array_append(data, (const uint8_t *)&header, sizeof(header));
    for (unsigned int i = 0; i < D3D12_PIPELINE_CACHE_SIZE; i++) {
        PGRAPHD3D12PipelineEntry *entry = &draw->pipeline_cache[i];
        if (!entry->cached_blob) {
            continue;
        }
        gsize blob_size;
        const uint8_t *blob = g_bytes_get_data(entry->cached_blob, &blob_size);
        if (blob_size > UINT32_MAX ||
            data->len + sizeof(D3D12PipelineCacheRecord) + blob_size >
                D3D12_PIPELINE_CACHE_MAX_FILE) {
            continue;
        }
        D3D12PipelineCacheRecord record = {
            .key_hash =
                fast_hash((const uint8_t *)&entry->key, sizeof(entry->key)),
            .blob_hash = fast_hash(blob, blob_size),
            .blob_size = blob_size,
            .key = entry->key,
        };
        g_byte_array_append(data, (const uint8_t *)&record, sizeof(record));
        g_byte_array_append(data, blob, blob_size);
        header.entry_count++;
    }
    memcpy(data->data, &header, sizeof(header));
    char *temporary = g_strdup_printf("%s.tmp", draw->pipeline_cache_path);
    if (g_file_set_contents(temporary, (const char *)data->data, data->len,
                            NULL) &&
        g_rename(temporary, draw->pipeline_cache_path) == 0) {
        draw->pipeline_cache_dirty = false;
    }
    g_remove(temporary);
    g_free(temporary);
    g_byte_array_unref(data);
}

static void d3d12_release_submission(PGRAPHD3D12DrawSubmission *submission)
{
    if (submission->pipeline) {
        ID3D12PipelineState_Release(submission->pipeline);
    }
    if (submission->runtime_constants) {
        ID3D12Resource_Release(submission->runtime_constants);
    }
    if (submission->pixel_constants) {
        ID3D12Resource_Release(submission->pixel_constants);
    }
    if (submission->vertex_constants) {
        ID3D12Resource_Release(submission->vertex_constants);
    }
    if (submission->index_buffer) {
        ID3D12Resource_Release(submission->index_buffer);
    }
    if (submission->vertex_buffer) {
        ID3D12Resource_Release(submission->vertex_buffer);
    }
    if (submission->list) {
        ID3D12GraphicsCommandList_Release(submission->list);
    }
    if (submission->allocator) {
        ID3D12CommandAllocator_Release(submission->allocator);
    }
    g_free(submission);
}

bool pgraph_d3d12_draw_collect(PGRAPHD3D12State *r, bool wait, Error **errp)
{
    if (!r || !r->draw) {
        return true;
    }
    while (!g_queue_is_empty(&r->draw->submissions)) {
        PGRAPHD3D12DrawSubmission *submission =
            g_queue_peek_head(&r->draw->submissions);
        uint64_t completed = ID3D12Fence_GetCompletedValue(r->fence);
        if (completed == UINT64_MAX) {
            HRESULT hr = ID3D12Device_GetDeviceRemovedReason(r->device);
            if (SUCCEEDED(hr)) {
                hr = DXGI_ERROR_DEVICE_REMOVED;
            }
            pgraph_d3d12_note_device_error(r, hr,
                                           "draw retirement fence status");
            error_setg(errp, "D3D12: draw retirement failed (HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
        if (completed < submission->fence_value) {
            if (!wait) {
                break;
            }
            HRESULT hr = S_OK;
            qemu_mutex_lock(&r->queue_lock);
            completed = ID3D12Fence_GetCompletedValue(r->fence);
            if (completed == UINT64_MAX) {
                hr = ID3D12Device_GetDeviceRemovedReason(r->device);
                if (SUCCEEDED(hr)) {
                    hr = DXGI_ERROR_DEVICE_REMOVED;
                }
            } else if (completed < submission->fence_value) {
                hr = ID3D12Fence_SetEventOnCompletion(
                    r->fence, submission->fence_value, r->fence_event);
                if (SUCCEEDED(hr) &&
                    WaitForSingleObjectEx(r->fence_event, 5000, FALSE) !=
                        WAIT_OBJECT_0) {
                    hr = ID3D12Device_GetDeviceRemovedReason(r->device);
                    if (SUCCEEDED(hr)) {
                        hr = DXGI_ERROR_DEVICE_HUNG;
                    }
                }
            }
            qemu_mutex_unlock(&r->queue_lock);
            if (FAILED(hr)) {
                pgraph_d3d12_note_device_error(r, hr, "draw retirement");
                error_setg(errp,
                           "D3D12: draw retirement failed (HRESULT 0x%08lx)",
                           (unsigned long)hr);
                return false;
            }
        }
        g_queue_pop_head(&r->draw->submissions);
        d3d12_release_submission(submission);
    }
    return true;
}

static bool d3d12_create_root_signature(PGRAPHD3D12State *r, Error **errp)
{
    D3D12_DESCRIPTOR_RANGE srv_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = NV2A_MAX_TEXTURES,
        .BaseShaderRegister = 2,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    D3D12_DESCRIPTOR_RANGE sampler_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        .NumDescriptors = NV2A_MAX_TEXTURES,
        .BaseShaderRegister = 2,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    D3D12_ROOT_PARAMETER parameters[] = {
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 0 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 1, .RegisterSpace = 0 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 31 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = { .ShaderRegister = 0, .RegisterSpace = 30 },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = 1,
                                 .pDescriptorRanges = &srv_range },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { .NumDescriptorRanges = 1,
                                 .pDescriptorRanges = &sampler_range },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        },
    };
    D3D12_ROOT_SIGNATURE_DESC description = {
        .NumParameters = ARRAY_SIZE(parameters),
        .pParameters = parameters,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS,
    };
    ID3DBlob *blob = NULL;
    ID3DBlob *errors = NULL;
    HRESULT hr = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: root signature serialization failed: %s",
                   errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) :
                            "no diagnostic");
        if (errors) {
            ID3D10Blob_Release(errors);
        }
        return false;
    }
    hr = ID3D12Device_CreateRootSignature(
        r->device, 0, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
        (void **)&r->draw->root_signature);
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: root signature creation failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    return true;
}

bool pgraph_d3d12_draw_init(PGRAPHD3D12State *r, Error **errp)
{
    r->draw = g_new0(PGRAPHD3D12DrawState, 1);
    g_queue_init(&r->draw->submissions);
    r->draw->adapter_luid = ID3D12Device_GetAdapterLuid(r->device);
    char *base_path = qemu_host_dup_pipeline_cache_file();
    if (base_path) {
        r->draw->pipeline_cache_path =
            g_strdup_printf("%s.d3d12-pso", base_path);
        g_free(base_path);
    }
    d3d12_glsl_compiler_acquire();
    r->draw->glsl_compiler_initialized = true;
    if (!d3d12_create_root_signature(r, errp)) {
        pgraph_d3d12_draw_finalize(r);
        return false;
    }
    d3d12_pipeline_cache_load(r->draw);
    return true;
}

void pgraph_d3d12_draw_finalize(PGRAPHD3D12State *r)
{
    if (!r || !r->draw) {
        return;
    }
    d3d12_pipeline_cache_save(r->draw);
    while (!g_queue_is_empty(&r->draw->submissions)) {
        d3d12_release_submission(g_queue_pop_head(&r->draw->submissions));
    }
    for (unsigned int i = 0; i < D3D12_PIPELINE_CACHE_SIZE; i++) {
        if (r->draw->pipeline_cache[i].pipeline) {
            ID3D12PipelineState_Release(r->draw->pipeline_cache[i].pipeline);
        }
        g_clear_pointer(&r->draw->pipeline_cache[i].cached_blob, g_bytes_unref);
    }
    for (unsigned int i = 0; i < D3D12_SHADER_BINDING_CACHE_SIZE; i++) {
        pgraph_d3d12_shader_binding_destroy(r->draw->shader_cache[i].binding);
    }
    if (r->draw->root_signature) {
        ID3D12RootSignature_Release(r->draw->root_signature);
    }
    if (r->draw->glsl_compiler_initialized) {
        d3d12_glsl_compiler_release();
    }
    g_free(r->draw->pipeline_cache_path);
    g_free(r->draw);
    r->draw = NULL;
}

static void d3d12_decode_attribute(const VertexAttribute *attribute,
                                   const uint8_t *source, float output[4])
{
    output[0] = output[1] = output[2] = 0.0f;
    output[3] = 1.0f;
    switch (attribute->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        if (attribute->count == 4) {
            output[0] = source[2] / 255.0f;
            output[1] = source[1] / 255.0f;
            output[2] = source[0] / 255.0f;
            output[3] = source[3] / 255.0f;
            break;
        }
        /* fall through */
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        for (unsigned int i = 0; i < attribute->count; i++) {
            output[i] = source[i] / 255.0f;
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        for (unsigned int i = 0; i < attribute->count; i++) {
            int16_t value;
            memcpy(&value, source + i * 2, sizeof(value));
            output[i] = MAX(-1.0f, value / 32767.0f);
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        memcpy(output, source, attribute->count * sizeof(float));
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        for (unsigned int i = 0; i < attribute->count; i++) {
            int16_t value;
            memcpy(&value, source + i * 2, sizeof(value));
            output[i] = value;
        }
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
        int32_t value;
        memcpy(&value, source, sizeof(value));
        int32_t x = (value << 21) >> 21;
        int32_t y = (value << 10) >> 21;
        int32_t z = value >> 22;
        output[0] = MAX(-1.0f, x / 1023.0f);
        output[1] = MAX(-1.0f, y / 1023.0f);
        output[2] = MAX(-1.0f, z / 511.0f);
        break;
    }
    default:
        break;
    }
}

static bool d3d12_build_vertices(NV2AState *d, uint32_t vertex_count,
                                 bool inline_array, uint32_t inline_stride,
                                 uint8_t **vertices, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    if (!vertex_count || vertex_count > NV2A_MAX_BATCH_LENGTH) {
        error_setg(errp, "D3D12: invalid vertex count %u", vertex_count);
        return false;
    }
    uint8_t *result = g_malloc0_n(vertex_count, D3D12_CANONICAL_VERTEX_STRIDE);
    for (unsigned int attribute_index = 0;
         attribute_index < NV2A_VERTEXSHADER_ATTRIBUTES; attribute_index++) {
        VertexAttribute *attribute = &pg->vertex_attributes[attribute_index];
        const uint8_t *base = NULL;
        size_t available = 0;
        uint32_t stride = 0;
        if (!inline_array && pg->inline_buffer_length &&
            attribute->inline_buffer_populated) {
            base = (const uint8_t *)attribute->inline_buffer;
            available = (size_t)pg->inline_buffer_length * 4 * sizeof(float);
            stride = 4 * sizeof(float);
        } else if (!inline_array && pg->inline_buffer_length) {
            base = NULL;
            available = 0;
            stride = 0;
        } else if (inline_array && attribute->count) {
            if (attribute->inline_array_offset >= pg->inline_array_length * 4) {
                g_free(result);
                error_setg(errp,
                           "D3D12: inline vertex attribute is out of range");
                return false;
            }
            base = (const uint8_t *)pg->inline_array +
                   attribute->inline_array_offset;
            available =
                pg->inline_array_length * 4 - attribute->inline_array_offset;
            stride = inline_stride;
        } else if (attribute->count) {
            hwaddr dma_length = 0;
            base = nv_dma_map(
                d, attribute->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_length);
            if (!base || attribute->offset >= dma_length) {
                g_free(result);
                error_setg(errp, "D3D12: vertex attribute DMA is invalid");
                return false;
            }
            base += attribute->offset;
            available = dma_length - attribute->offset;
            stride = attribute->stride;
        }
        for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
            float *destination =
                (float *)(result +
                          (size_t)vertex * D3D12_CANONICAL_VERTEX_STRIDE) +
                attribute_index * 4;
            if (!attribute->count || !stride) {
                memcpy(destination, attribute->inline_value,
                       sizeof(attribute->inline_value));
            } else {
                uint64_t offset = (uint64_t)vertex * stride;
                uint64_t bytes = (uint64_t)attribute->size * attribute->count;
                if (offset + bytes > available) {
                    g_free(result);
                    error_setg(errp,
                               "D3D12: vertex attribute exceeds DMA range");
                    return false;
                }
                d3d12_decode_attribute(attribute, base + offset, destination);
            }
        }
    }
    *vertices = result;
    return true;
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE
d3d12_topology_type(enum ShaderPrimitiveMode mode)
{
    if (mode == PRIM_TYPE_POINTS) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }
    if (mode == PRIM_TYPE_LINES || mode == PRIM_TYPE_LINE_LOOP ||
        mode == PRIM_TYPE_LINE_STRIP || mode == PRIM_TYPE_QUADS ||
        mode == PRIM_TYPE_QUAD_STRIP) {
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

static D3D_PRIMITIVE_TOPOLOGY d3d12_topology(enum ShaderPrimitiveMode mode)
{
    switch (mode) {
    case PRIM_TYPE_POINTS:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case PRIM_TYPE_LINES:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PRIM_TYPE_LINE_LOOP:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PRIM_TYPE_LINE_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PRIM_TYPE_TRIANGLES:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PRIM_TYPE_QUADS:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case PRIM_TYPE_QUAD_STRIP:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    default:
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

static void d3d12_append_sequence_indices(GArray *indices,
                                          enum ShaderPrimitiveMode mode,
                                          const uint32_t *source,
                                          uint32_t count)
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
    } else {
        g_array_append_vals(indices, source, count);
    }
#undef APPEND_INDEX
}

static bool d3d12_create_upload(PGRAPHD3D12State *r, const void *data,
                                size_t size, ID3D12Resource **resource,
                                Error **errp)
{
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = MAX(size, 1),
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
        (void **)resource);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: draw upload allocation failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    void *mapped = NULL;
    D3D12_RANGE no_read = { 0, 0 };
    hr = ID3D12Resource_Map(*resource, 0, &no_read, &mapped);
    if (FAILED(hr)) {
        ID3D12Resource_Release(*resource);
        *resource = NULL;
        error_setg(errp, "D3D12: draw upload map failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    memcpy(mapped, data, size);
    ID3D12Resource_Unmap(*resource, 0, NULL);
    return true;
}

static bool d3d12_ensure_shader(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    /* The native path expands every NV2A attribute into a canonical float4
     * vertex stream on the CPU.  The generated shader must therefore not
     * repeat GL/Vulkan's packed-attribute conversion or treat constants as
     * descriptor-side uniform attributes. */
    pg->compressed_attrs = 0;
    pg->swizzle_attrs = 0;
    pg->uniform_attrs = 0;
    if (r->draw->shader &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->draw->shader->state)) {
        return pgraph_d3d12_shader_binding_update_uniforms(r->draw->shader, pg,
                                                           errp);
    }
    ShaderState state = pgraph_glsl_get_shader_state(pg);
    if (!r->draw->shader ||
        memcmp(&r->draw->shader->state, &state, sizeof(state))) {
        PGRAPHD3D12ShaderEntry *entry = NULL;
        PGRAPHD3D12ShaderEntry *oldest = &r->draw->shader_cache[0];
        for (unsigned int i = 0; i < D3D12_SHADER_BINDING_CACHE_SIZE; i++) {
            PGRAPHD3D12ShaderEntry *candidate = &r->draw->shader_cache[i];
            if (candidate->binding &&
                !memcmp(&candidate->binding->state, &state, sizeof(state))) {
                entry = candidate;
                break;
            }
            if (!candidate->binding || candidate->last_use < oldest->last_use) {
                oldest = candidate;
            }
        }
        if (!entry) {
            PGRAPHD3D12ShaderBinding *binding = NULL;
            if (!pgraph_d3d12_shader_binding_create(r->shader_cache, &state,
                                                    &binding, errp)) {
                return false;
            }
            entry = oldest;
            /* A PSO retains its shader bytecode internally, so evicting this
             * source binding does not invalidate existing cached PSOs. */
            pgraph_d3d12_shader_binding_destroy(entry->binding);
            entry->binding = binding;
        }
        entry->last_use = ++r->draw->use_counter;
        r->draw->shader = entry->binding;
    }
    return pgraph_d3d12_shader_binding_update_uniforms(r->draw->shader, pg,
                                                       errp);
}

static D3D12_CULL_MODE d3d12_cull_mode(PGRAPHState *pg)
{
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    if (!(raster & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
        return D3D12_CULL_MODE_NONE;
    }
    switch (GET_MASK(raster, NV_PGRAPH_SETUPRASTER_CULLCTRL)) {
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT:
        return D3D12_CULL_MODE_FRONT;
    case NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK:
        return D3D12_CULL_MODE_BACK;
    default:
        return D3D12_CULL_MODE_NONE;
    }
}

static bool d3d12_create_pipeline(PGRAPHState *pg,
                                  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                                  Error **errp)
{
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12ShaderBinding *shader = r->draw->shader;
    static const uint32_t pipeline_registers[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_CONTROL_0,     NV_PGRAPH_CONTROL_1,
        NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,     NV_PGRAPH_SETUPRASTER,
        NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    PGRAPHD3D12PipelineKey key = { 0 };
    key.shader_state = shader->state;
    for (unsigned int i = 0; i < ARRAY_SIZE(pipeline_registers); i++) {
        key.regs[i] = pgraph_reg_r(pg, pipeline_registers[i]);
    }
    key.rtv_format = r->render_targets.color.resource ?
                         r->render_targets.color.format :
                         DXGI_FORMAT_UNKNOWN;
    key.dsv_format = r->render_targets.depth_stencil.resource ?
                         r->render_targets.depth_stencil.format :
                         DXGI_FORMAT_UNKNOWN;
    key.topology_type = topology;
    PGRAPHD3D12PipelineEntry *entry = NULL;
    PGRAPHD3D12PipelineEntry *oldest = &r->draw->pipeline_cache[0];
    for (unsigned int i = 0; i < D3D12_PIPELINE_CACHE_SIZE; i++) {
        PGRAPHD3D12PipelineEntry *candidate = &r->draw->pipeline_cache[i];
        if ((candidate->pipeline || candidate->cached_blob) &&
            !memcmp(&candidate->key, &key, sizeof(key))) {
            entry = candidate;
            break;
        }
        if (!candidate->pipeline || candidate->last_use < oldest->last_use) {
            oldest = candidate;
        }
    }
    if (entry && entry->pipeline) {
        entry->last_use = ++r->draw->use_counter;
        r->draw->pipeline = entry->pipeline;
        return true;
    }
    D3D12_INPUT_ELEMENT_DESC elements[NV2A_VERTEXSHADER_ATTRIBUTES];
    static const char semantic[] = "TEXCOORD";
    for (unsigned int i = 0; i < ARRAY_SIZE(elements); i++) {
        elements[i] = (D3D12_INPUT_ELEMENT_DESC){
            .SemanticName = semantic,
            .SemanticIndex = i,
            .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
            .InputSlot = 0,
            .AlignedByteOffset = i * sizeof(float) * 4,
            .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        };
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description = {
        .pRootSignature = r->draw->root_signature,
        .InputLayout = { elements, ARRAY_SIZE(elements) },
        .SampleMask = UINT_MAX,
        .PrimitiveTopologyType = topology,
        .RasterizerState = {
            .FillMode = D3D12_FILL_MODE_SOLID,
            .CullMode = d3d12_cull_mode(pg),
            .FrontCounterClockwise = !(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                                      NV_PGRAPH_SETUPRASTER_FRONTFACE),
            .DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
            .DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            .SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
            .DepthClipEnable = FALSE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
            .ForcedSampleCount = 0,
            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
        },
    };
    description.VS.pShaderBytecode =
        g_bytes_get_data(shader->vertex_dxil, &description.VS.BytecodeLength);
    description.PS.pShaderBytecode =
        g_bytes_get_data(shader->pixel_dxil, &description.PS.BytecodeLength);
    if (shader->geometry_dxil) {
        description.GS.pShaderBytecode = g_bytes_get_data(
            shader->geometry_dxil, &description.GS.BytecodeLength);
    }
    pgraph_d3d12_configure_pipeline(r, &description);
    gsize cached_size = 0;
    if (entry && entry->cached_blob) {
        description.CachedPSO.pCachedBlob =
            g_bytes_get_data(entry->cached_blob, &cached_size);
        description.CachedPSO.CachedBlobSizeInBytes = cached_size;
    }
    ID3D12PipelineState *pipeline = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(
        r->device, &description, &IID_ID3D12PipelineState, (void **)&pipeline);
    if (FAILED(hr) && description.CachedPSO.pCachedBlob) {
        /* Driver/runtime updates can invalidate an otherwise well-formed
         * cache record. Retry from the canonical descriptor and replace it. */
        description.CachedPSO = (D3D12_CACHED_PIPELINE_STATE){ 0 };
        hr = ID3D12Device_CreateGraphicsPipelineState(r->device, &description,
                                                      &IID_ID3D12PipelineState,
                                                      (void **)&pipeline);
        g_clear_pointer(&entry->cached_blob, g_bytes_unref);
        r->draw->pipeline_cache_dirty = true;
    }
    if (FAILED(hr)) {
        static unsigned int diagnostic_count;
        if (diagnostic_count < 16) {
            ID3D12InfoQueue *info = NULL;
            if (SUCCEEDED(ID3D12Device_QueryInterface(
                    r->device, &IID_ID3D12InfoQueue, (void **)&info))) {
                UINT64 count = ID3D12InfoQueue_GetNumStoredMessages(info);
                UINT64 first = count > 32 ? count - 32 : 0;
                for (UINT64 i = first; i < count; i++) {
                    SIZE_T size = 0;
                    HRESULT message_hr =
                        ID3D12InfoQueue_GetMessage(info, i, NULL, &size);
                    if (message_hr != S_FALSE || !size) {
                        continue;
                    }
                    D3D12_MESSAGE *message = g_malloc(size);
                    if (SUCCEEDED(ID3D12InfoQueue_GetMessage(
                            info, i, message, &size))) {
                        char *text = g_strdup_printf(
                            "D3D12 debug [%u]: %s", message->ID,
                            message->pDescription);
                        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, text);
                        g_free(text);
                    }
                    g_free(message);
                }
                ID3D12InfoQueue_ClearStoredMessages(info);
                ID3D12InfoQueue_Release(info);
            }
            diagnostic_count++;
        }
        pgraph_d3d12_note_device_error(r, hr, "graphics pipeline creation");
        error_setg(errp,
                   "D3D12: graphics pipeline creation failed (0x%08lx; "
                   "topology=%u, RTV=%u, DSV=%u, targets=%u, VS=%zu, "
                   "GS=%zu, PS=%zu, depth=%u, stencil=%u, blend=%u, "
                   "logic=%u)",
                   (unsigned long)hr, description.PrimitiveTopologyType,
                   description.RTVFormats[0], description.DSVFormat,
                   description.NumRenderTargets, description.VS.BytecodeLength,
                   description.GS.BytecodeLength, description.PS.BytecodeLength,
                   description.DepthStencilState.DepthEnable,
                   description.DepthStencilState.StencilEnable,
                   description.BlendState.RenderTarget[0].BlendEnable,
                   description.BlendState.RenderTarget[0].LogicOpEnable);
        return false;
    }
    entry = entry ? entry : oldest;
    if (entry->pipeline) {
        ID3D12PipelineState_Release(entry->pipeline);
    }
    if (memcmp(&entry->key, &key, sizeof(key))) {
        g_clear_pointer(&entry->cached_blob, g_bytes_unref);
    }
    entry->key = key;
    entry->pipeline = pipeline;
    ID3DBlob *cached_blob = NULL;
    if (SUCCEEDED(ID3D12PipelineState_GetCachedBlob(pipeline, &cached_blob))) {
        g_clear_pointer(&entry->cached_blob, g_bytes_unref);
        entry->cached_blob =
            g_bytes_new(ID3D10Blob_GetBufferPointer(cached_blob),
                        ID3D10Blob_GetBufferSize(cached_blob));
        ID3D10Blob_Release(cached_blob);
        r->draw->pipeline_cache_dirty = true;
    }
    entry->last_use = ++r->draw->use_counter;
    r->draw->pipeline = pipeline;
    return true;
}

bool pgraph_d3d12_flush_draw(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    if (!r || !r->draw) {
        error_setg(errp, "D3D12: draw state is unavailable");
        return false;
    }
    if (!pgraph_d3d12_draw_collect(r, false, errp)) {
        return false;
    }
    if (g_queue_get_length(&r->draw->submissions) >=
            D3D12_MAX_DRAW_SUBMISSIONS &&
        !pgraph_d3d12_draw_collect(r, true, errp)) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    if ((raster & NV_PGRAPH_SETUPRASTER_CULLENABLE) &&
        GET_MASK(raster, NV_PGRAPH_SETUPRASTER_CULLCTRL) ==
            NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT_AND_BACK) {
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attribute = &pg->vertex_attributes[i];
            if (attribute->inline_buffer_populated &&
                pg->inline_buffer_length) {
                memcpy(attribute->inline_value,
                       attribute->inline_buffer +
                           (pg->inline_buffer_length - 1) * 4,
                       sizeof(attribute->inline_value));
                attribute->inline_buffer_populated = false;
            }
        }
        return true;
    }
    if (pg->zpass_pixel_count_enable && r->query_count >= r->max_query_count &&
        !pgraph_d3d12_process_reports_now(d, errp)) {
        return false;
    }
    uint32_t max_vertex = 0;
    bool inline_array = false;
    uint32_t inline_stride = 0;
    GArray *indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(PGRAPHD3D12DrawRange));
    enum ShaderPrimitiveMode mode =
        (enum ShaderPrimitiveMode)pg->primitive_mode;
    if (pg->draw_arrays_length) {
        for (unsigned int batch = 0; batch < pg->draw_arrays_length; batch++) {
            uint32_t start = pg->draw_arrays_start[batch];
            uint32_t count = pg->draw_arrays_count[batch];
            uint32_t *sequence = g_new(uint32_t, count);
            for (uint32_t i = 0; i < count; i++) {
                sequence[i] = start + i;
            }
            UINT first_index = indices->len;
            d3d12_append_sequence_indices(indices, mode, sequence, count);
            PGRAPHD3D12DrawRange range = {
                .start_index = first_index,
                .index_count = indices->len - first_index,
            };
            if (range.index_count) {
                g_array_append_val(ranges, range);
            }
            max_vertex = MAX(max_vertex, start + count);
            g_free(sequence);
        }
    } else if (pg->inline_elements_length) {
        UINT first_index = indices->len;
        d3d12_append_sequence_indices(indices, mode, pg->inline_elements,
                                      pg->inline_elements_length);
        PGRAPHD3D12DrawRange range = {
            .start_index = first_index,
            .index_count = indices->len - first_index,
        };
        if (range.index_count)
            g_array_append_val(ranges, range);
        for (unsigned int i = 0; i < pg->inline_elements_length; i++) {
            max_vertex = MAX(max_vertex, pg->inline_elements[i] + 1);
        }
    } else if (pg->inline_buffer_length) {
        uint32_t *sequence = g_new(uint32_t, pg->inline_buffer_length);
        for (uint32_t i = 0; i < pg->inline_buffer_length; i++) {
            sequence[i] = i;
        }
        UINT first_index = indices->len;
        d3d12_append_sequence_indices(indices, mode, sequence,
                                      pg->inline_buffer_length);
        PGRAPHD3D12DrawRange range = {
            .start_index = first_index,
            .index_count = indices->len - first_index,
        };
        if (range.index_count)
            g_array_append_val(ranges, range);
        max_vertex = pg->inline_buffer_length;
        g_free(sequence);
    } else if (pg->inline_array_length) {
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attribute = &pg->vertex_attributes[i];
            if (!attribute->count) {
                continue;
            }
            inline_stride = ROUND_UP(inline_stride, attribute->size);
            attribute->inline_array_offset = inline_stride;
            inline_stride += attribute->size * attribute->count;
            inline_stride = ROUND_UP(inline_stride, attribute->size);
        }
        if (!inline_stride || (pg->inline_array_length * 4) % inline_stride) {
            g_array_unref(indices);
            g_array_unref(ranges);
            error_setg(errp, "D3D12: malformed inline vertex array");
            return false;
        }
        max_vertex = pg->inline_array_length * 4 / inline_stride;
        uint32_t *sequence = g_new(uint32_t, max_vertex);
        for (uint32_t i = 0; i < max_vertex; i++) {
            sequence[i] = i;
        }
        UINT first_index = indices->len;
        d3d12_append_sequence_indices(indices, mode, sequence, max_vertex);
        PGRAPHD3D12DrawRange range = {
            .start_index = first_index,
            .index_count = indices->len - first_index,
        };
        if (range.index_count)
            g_array_append_val(ranges, range);
        inline_array = true;
        g_free(sequence);
    } else {
        g_array_unref(indices);
        g_array_unref(ranges);
        return true;
    }
    if (!max_vertex || !indices->len || !ranges->len) {
        g_array_unref(indices);
        g_array_unref(ranges);
        return true;
    }
    uint8_t *vertices = NULL;
    if (!d3d12_build_vertices(d, max_vertex, inline_array, inline_stride,
                              &vertices, errp) ||
        !d3d12_ensure_shader(pg, errp)) {
        g_free(vertices);
        g_array_unref(indices);
        g_array_unref(ranges);
        return false;
    }
    if (pg->inline_buffer_length) {
        for (unsigned int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attribute = &pg->vertex_attributes[i];
            if (!attribute->inline_buffer_populated) {
                continue;
            }
            memcpy(attribute->inline_value,
                   attribute->inline_buffer +
                       (pg->inline_buffer_length - 1) * 4,
                   sizeof(attribute->inline_value));
            attribute->inline_buffer_populated = false;
        }
    }
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = d3d12_topology_type(mode);
    if (!d3d12_create_pipeline(pg, topology_type, errp)) {
        g_free(vertices);
        g_array_unref(indices);
        g_array_unref(ranges);
        return false;
    }

    ID3D12Resource *vertex_buffer = NULL, *index_buffer = NULL;
    ID3D12Resource *vertex_constants = NULL, *pixel_constants = NULL;
    ID3D12Resource *runtime_constants = NULL;
    uint32_t draw_width =
        r->render_targets.color.resource ?
            r->render_targets.color.width :
        r->render_targets.depth_stencil.resource ?
            r->render_targets.depth_stencil.width :
            pg->surface_shape.clip_x + pg->surface_shape.clip_width;
    uint32_t draw_height =
        r->render_targets.color.resource ?
            r->render_targets.color.height :
        r->render_targets.depth_stencil.resource ?
            r->render_targets.depth_stencil.height :
            pg->surface_shape.clip_y + pg->surface_shape.clip_height;
    if (!r->render_targets.color.resource &&
        !r->render_targets.depth_stencil.resource) {
        pgraph_apply_anti_aliasing_factor(pg, &draw_width, &draw_height);
        pgraph_apply_scaling_factor(pg, &draw_width, &draw_height);
    }
    draw_width = MAX(draw_width, 1U);
    draw_height = MAX(draw_height, 1U);
    struct dxil_spirv_vertex_runtime_data runtime_data = { 0 };
    runtime_data.viewport_width = draw_width;
    runtime_data.viewport_height = draw_height;
    bool ok =
        d3d12_create_upload(r, vertices,
                            (size_t)max_vertex * D3D12_CANONICAL_VERTEX_STRIDE,
                            &vertex_buffer, errp) &&
        d3d12_create_upload(r, indices->data, indices->len * sizeof(uint32_t),
                            &index_buffer, errp) &&
        d3d12_create_upload(r, r->draw->shader->vertex_uniforms.data,
                            r->draw->shader->vertex_uniforms.size,
                            &vertex_constants, errp) &&
        d3d12_create_upload(r, r->draw->shader->pixel_uniforms.data,
                            r->draw->shader->pixel_uniforms.size,
                            &pixel_constants, errp) &&
        d3d12_create_upload(r, &runtime_data, sizeof(runtime_data),
                            &runtime_constants, errp);
    g_free(vertices);
    if (!ok) {
        goto cleanup;
    }
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    HRESULT hr = ID3D12Device_CreateCommandAllocator(
        r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
            r->draw->pipeline, &IID_ID3D12GraphicsCommandList, (void **)&list);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: draw command list creation failed");
        if (allocator)
            ID3D12CommandAllocator_Release(allocator);
        ok = false;
        goto cleanup;
    }
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list,
                                                       r->draw->root_signature);
    ID3D12DescriptorHeap *heaps[] = { r->srv_heap, r->sampler_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, ARRAY_SIZE(heaps),
                                                 heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        list, 0, ID3D12Resource_GetGPUVirtualAddress(vertex_constants));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        list, 1, ID3D12Resource_GetGPUVirtualAddress(pixel_constants));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
        list, 4,
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(r->srv_heap));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
        list, 5,
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
            r->sampler_heap));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        list, 2, ID3D12Resource_GetGPUVirtualAddress(runtime_constants));
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        list, 3, ID3D12Resource_GetGPUVirtualAddress(runtime_constants));
    pgraph_d3d12_bind_render_targets(r, list);
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {
        .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vertex_buffer),
        .SizeInBytes = max_vertex * D3D12_CANONICAL_VERTEX_STRIDE,
        .StrideInBytes = D3D12_CANONICAL_VERTEX_STRIDE,
    };
    D3D12_INDEX_BUFFER_VIEW index_view = {
        .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(index_buffer),
        .SizeInBytes = indices->len * sizeof(uint32_t),
        .Format = DXGI_FORMAT_R32_UINT,
    };
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vertex_view);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &index_view);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list,
                                                     d3d12_topology(mode));
    D3D12_VIEWPORT viewport = {
        .Width = draw_width,
        .Height = draw_height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &viewport);
    unsigned int sx = pg->surface_shape.clip_x;
    unsigned int sy = pg->surface_shape.clip_y;
    unsigned int sw = pg->surface_shape.clip_width;
    unsigned int sh = pg->surface_shape.clip_height;
    pgraph_apply_anti_aliasing_factor(pg, &sx, &sy);
    pgraph_apply_anti_aliasing_factor(pg, &sw, &sh);
    pgraph_apply_scaling_factor(pg, &sx, &sy);
    pgraph_apply_scaling_factor(pg, &sw, &sh);
    D3D12_RECT scissor = { sx, sy, sx + sw, sy + sh };
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
    uint32_t query_count_before_draw = r->query_count;
    if (pg->zpass_pixel_count_enable) {
        pgraph_d3d12_reports_begin_draw(r, list);
    }
    for (unsigned int i = 0; i < ranges->len; i++) {
        const PGRAPHD3D12DrawRange *range =
            &g_array_index(ranges, PGRAPHD3D12DrawRange, i);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(
            list, range->index_count, 1, range->start_index, 0, 0);
    }
    if (pg->zpass_pixel_count_enable) {
        pgraph_d3d12_reports_end_draw(r, list);
    }
    hr = ID3D12GraphicsCommandList_Close(list);
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        uint64_t fence;
        qemu_mutex_lock(&r->queue_lock);
        fence = r->next_fence_value++;
        ID3D12CommandQueue_ExecuteCommandLists(r->queue, 1, lists);
        hr = ID3D12CommandQueue_Signal(r->queue, r->fence, fence);
        qemu_mutex_unlock(&r->queue_lock);
        if (SUCCEEDED(hr)) {
            PGRAPHD3D12DrawSubmission *submission =
                g_new0(PGRAPHD3D12DrawSubmission, 1);
            submission->allocator = allocator;
            submission->list = list;
            submission->vertex_buffer = vertex_buffer;
            submission->index_buffer = index_buffer;
            submission->vertex_constants = vertex_constants;
            submission->pixel_constants = pixel_constants;
            submission->runtime_constants = runtime_constants;
            submission->pipeline = r->draw->pipeline;
            ID3D12PipelineState_AddRef(submission->pipeline);
            submission->fence_value = fence;
            g_queue_push_tail(&r->draw->submissions, submission);
            allocator = NULL;
            list = NULL;
            vertex_buffer = NULL;
            index_buffer = NULL;
            vertex_constants = NULL;
            pixel_constants = NULL;
            runtime_constants = NULL;
            ok = true;
        } else {
            pgraph_d3d12_note_device_error(r, hr, "draw submission");
            error_setg(errp, "D3D12: draw submission failed (0x%08lx)",
                       (unsigned long)hr);
            ok = false;
        }
    } else {
        error_setg(errp, "D3D12: draw command list close failed");
        ok = false;
    }
    if (!ok && r->query_count > query_count_before_draw) {
        r->query_count = query_count_before_draw;
    }
    if (list) {
        ID3D12GraphicsCommandList_Release(list);
    }
    if (allocator) {
        ID3D12CommandAllocator_Release(allocator);
    }
    if (ok) {
        pg->surface_color.draw_dirty |=
            r->render_targets.color.resource && pgraph_color_write_enabled(pg);
        pg->surface_zeta.draw_dirty |=
            r->render_targets.depth_stencil.resource &&
            pgraph_zeta_write_enabled(pg);
        pg->draw_time++;
    }

cleanup:
    if (runtime_constants)
        ID3D12Resource_Release(runtime_constants);
    if (pixel_constants)
        ID3D12Resource_Release(pixel_constants);
    if (vertex_constants)
        ID3D12Resource_Release(vertex_constants);
    if (index_buffer)
        ID3D12Resource_Release(index_buffer);
    if (vertex_buffer)
        ID3D12Resource_Release(vertex_buffer);
    g_array_unref(indices);
    g_array_unref(ranges);
    return ok;
}
