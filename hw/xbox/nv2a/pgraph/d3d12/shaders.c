/*
 * Geforce NV2A PGRAPH Direct3D 12 Shader Compiler
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/fast-hash.h"
#include "qemu/qemu-host.h"
#include "qemu/thread.h"
#include <glib/gstdio.h>
#include "spirv_to_dxil.h"
#include "shaders.h"
#include "hw/xbox/nv2a/pgraph/glsl/geom.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "qemu/mstring.h"
#include <glslang/Include/glslang_c_interface.h>
#include <spirv_reflect.h>

void pgraph_vk_init_glsl_compiler(void);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);

#define D3D12_SHADER_CACHE_SIZE 1024
#define D3D12_SHADER_CACHE_MAGIC 0x58444331u
#define D3D12_SHADER_CACHE_VERSION 5
#define D3D12_SHADER_CACHE_MAX_FILE (64u * 1024u * 1024u)

typedef struct D3D12ShaderCacheHeader {
    uint32_t magic;
    uint32_t version;
    LUID adapter_luid;
    uint32_t entry_count;
    uint32_t reserved;
} D3D12ShaderCacheHeader;

typedef struct D3D12ShaderCacheRecord {
    uint64_t hash;
    uint64_t dxil_hash;
    uint32_t stage;
    uint32_t size;
} D3D12ShaderCacheRecord;

typedef bool (*SpirvToDxilFn)(const uint32_t *, size_t,
                              struct dxil_spirv_specialization *, unsigned int,
                              dxil_spirv_shader_stage, const char *,
                              enum dxil_validator_version,
                              const struct dxil_spirv_debug_options *,
                              const struct dxil_spirv_runtime_conf *,
                              const struct dxil_spirv_logger *,
                              struct dxil_spirv_object *);
typedef void (*SpirvToDxilFreeFn)(struct dxil_spirv_object *);
typedef bool (*SpirvToDxilValidateFn)(void *, size_t, char *, size_t);

typedef struct PGRAPHD3D12ShaderCacheEntry {
    uint64_t hash;
    uint64_t last_use;
    unsigned int stage;
    GBytes *dxil;
} PGRAPHD3D12ShaderCacheEntry;

struct PGRAPHD3D12ShaderCache {
    HMODULE module;
    SpirvToDxilFn compile;
    SpirvToDxilFreeFn free_object;
    SpirvToDxilValidateFn validate;
    uint64_t use_counter;
    char *path;
    LUID adapter_luid;
    QemuMutex lock;
    bool dirty;
    PGRAPHD3D12ShaderCacheEntry entries[D3D12_SHADER_CACHE_SIZE];
};

static void d3d12_dxil_log(void *opaque, const char *message)
{
    qemu_host_emit_log(QEMU_HOST_LOG_ERROR, message);
}

static void d3d12_shader_cache_load(PGRAPHD3D12ShaderCache *cache)
{
    gchar *contents = NULL;
    gsize length = 0;
    if (!cache->path ||
        !g_file_get_contents(cache->path, &contents, &length, NULL) ||
        length < sizeof(D3D12ShaderCacheHeader) ||
        length > D3D12_SHADER_CACHE_MAX_FILE) {
        g_free(contents);
        return;
    }

    const uint8_t *cursor = (const uint8_t *)contents;
    const uint8_t *end = cursor + length;
    D3D12ShaderCacheHeader header;
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);
    if (header.magic != D3D12_SHADER_CACHE_MAGIC ||
        header.version != D3D12_SHADER_CACHE_VERSION ||
        memcmp(&header.adapter_luid, &cache->adapter_luid, sizeof(LUID)) != 0 ||
        header.entry_count > D3D12_SHADER_CACHE_SIZE) {
        g_free(contents);
        return;
    }

    unsigned int loaded = 0;
    for (uint32_t i = 0; i < header.entry_count; i++) {
        D3D12ShaderCacheRecord record;
        if ((size_t)(end - cursor) < sizeof(record)) {
            break;
        }
        memcpy(&record, cursor, sizeof(record));
        cursor += sizeof(record);
        if (!record.size || record.size > D3D12_SHADER_CACHE_MAX_FILE ||
            record.stage > DXIL_SPIRV_SHADER_KERNEL ||
            (size_t)(end - cursor) < record.size ||
            fast_hash(cursor, record.size) != record.dxil_hash) {
            break;
        }
        PGRAPHD3D12ShaderCacheEntry *entry = &cache->entries[loaded++];
        entry->hash = record.hash;
        entry->stage = record.stage;
        entry->last_use = ++cache->use_counter;
        entry->dxil = g_bytes_new(cursor, record.size);
        cursor += record.size;
    }
    g_free(contents);
    if (loaded) {
        char *message =
            g_strdup_printf("D3D12: loaded %u persistent DXIL shaders", loaded);
        qemu_host_emit_log(QEMU_HOST_LOG_INFO, message);
        g_free(message);
    }
}

static bool d3d12_shader_cache_save(PGRAPHD3D12ShaderCache *cache)
{
    if (!cache->dirty || !cache->path) {
        return true;
    }
    GByteArray *data = g_byte_array_sized_new(4096);
    D3D12ShaderCacheHeader header = {
        .magic = D3D12_SHADER_CACHE_MAGIC,
        .version = D3D12_SHADER_CACHE_VERSION,
        .adapter_luid = cache->adapter_luid,
    };
    g_byte_array_append(data, (const uint8_t *)&header, sizeof(header));
    for (unsigned int i = 0; i < D3D12_SHADER_CACHE_SIZE; i++) {
        PGRAPHD3D12ShaderCacheEntry *entry = &cache->entries[i];
        if (!entry->dxil) {
            continue;
        }
        gsize size;
        const uint8_t *bytes = g_bytes_get_data(entry->dxil, &size);
        if (size > UINT32_MAX ||
            data->len + sizeof(D3D12ShaderCacheRecord) + size >
                D3D12_SHADER_CACHE_MAX_FILE) {
            continue;
        }
        D3D12ShaderCacheRecord record = {
            .hash = entry->hash,
            .dxil_hash = fast_hash(bytes, size),
            .stage = entry->stage,
            .size = size,
        };
        g_byte_array_append(data, (const uint8_t *)&record, sizeof(record));
        g_byte_array_append(data, bytes, size);
        header.entry_count++;
    }
    memcpy(data->data, &header, sizeof(header));
    char *temporary = g_strdup_printf("%s.tmp", cache->path);
    bool saved = false;
    if (g_file_set_contents(temporary, (const char *)data->data, data->len,
                            NULL)) {
        saved = g_rename(temporary, cache->path) == 0;
    }
    g_remove(temporary);
    g_free(temporary);
    g_byte_array_unref(data);
    return saved;
}

bool pgraph_d3d12_shader_cache_init(PGRAPHD3D12ShaderCache **cache,
                                    ID3D12Device *device, Error **errp)
{
    PGRAPHD3D12ShaderCache *result = g_new0(PGRAPHD3D12ShaderCache, 1);
    qemu_mutex_init(&result->lock);
    device->lpVtbl->GetAdapterLuid(device, &result->adapter_luid);
    char *base_path = qemu_host_dup_pipeline_cache_file();
    if (base_path) {
        result->path = g_strdup_printf("%s.d3d12-dxil", base_path);
        g_free(base_path);
    }

    result->module = LoadPackagedLibrary(L"spirv_to_dxil.dll", 0);
    if (!result->module) {
        result->module = LoadLibraryExW(L"spirv_to_dxil.dll", NULL,
                                        LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    }
    if (!result->module) {
        error_setg_win32(errp, GetLastError(),
                         "D3D12: failed to load spirv_to_dxil.dll");
        qemu_mutex_destroy(&result->lock);
        g_free(result->path);
        g_free(result);
        return false;
    }

    result->compile =
        (SpirvToDxilFn)GetProcAddress(result->module, "spirv_to_dxil");
    result->free_object =
        (SpirvToDxilFreeFn)GetProcAddress(result->module, "spirv_to_dxil_free");
    result->validate = (SpirvToDxilValidateFn)GetProcAddress(
        result->module, "spirv_to_dxil_validate");
    if (!result->compile || !result->free_object || !result->validate) {
        error_setg(errp, "D3D12: invalid spirv_to_dxil.dll exports");
        FreeLibrary(result->module);
        qemu_mutex_destroy(&result->lock);
        g_free(result->path);
        g_free(result);
        return false;
    }

    d3d12_shader_cache_load(result);
    *cache = result;
    qemu_host_emit_log(QEMU_HOST_LOG_INFO,
                       "D3D12: SPIR-V/NIR/DXIL compiler initialized");
    return true;
}

void pgraph_d3d12_shader_cache_finalize(PGRAPHD3D12ShaderCache *cache)
{
    if (!cache) {
        return;
    }
    qemu_mutex_lock(&cache->lock);
    d3d12_shader_cache_save(cache);
    for (unsigned int i = 0; i < D3D12_SHADER_CACHE_SIZE; i++) {
        g_clear_pointer(&cache->entries[i].dxil, g_bytes_unref);
    }
    if (cache->module) {
        FreeLibrary(cache->module);
    }
    qemu_mutex_unlock(&cache->lock);
    qemu_mutex_destroy(&cache->lock);
    g_free(cache->path);
    g_free(cache);
}

void pgraph_d3d12_shader_cache_flush(PGRAPHD3D12ShaderCache *cache)
{
    if (!cache) {
        return;
    }
    qemu_mutex_lock(&cache->lock);
    if (d3d12_shader_cache_save(cache)) {
        cache->dirty = false;
    } else {
        qemu_host_emit_log(QEMU_HOST_LOG_WARNING,
                           "D3D12: persistent DXIL cache write failed");
    }
    qemu_mutex_unlock(&cache->lock);
}

static PGRAPHD3D12ShaderCacheEntry *
d3d12_shader_cache_find(PGRAPHD3D12ShaderCache *cache, uint64_t hash,
                        unsigned int stage)
{
    PGRAPHD3D12ShaderCacheEntry *oldest = &cache->entries[0];
    for (unsigned int i = 0; i < D3D12_SHADER_CACHE_SIZE; i++) {
        PGRAPHD3D12ShaderCacheEntry *entry = &cache->entries[i];
        if (entry->dxil && entry->hash == hash && entry->stage == stage) {
            return entry;
        }
        if (!entry->dxil || entry->last_use < oldest->last_use) {
            oldest = entry;
        }
    }
    return oldest;
}

bool pgraph_d3d12_shader_compile(PGRAPHD3D12ShaderCache *cache,
                                 const uint32_t *spirv, size_t spirv_size,
                                 unsigned int stage,
                                 D3D12_SHADER_BYTECODE *bytecode, Error **errp)
{
    uint64_t hash = fast_hash((const uint8_t *)spirv, spirv_size);
    qemu_mutex_lock(&cache->lock);
    PGRAPHD3D12ShaderCacheEntry *entry =
        d3d12_shader_cache_find(cache, hash, stage);
    if (!entry->dxil || entry->hash != hash || entry->stage != stage) {
        struct dxil_spirv_runtime_conf configuration = {
            .runtime_data_cbv = { .register_space = 31,
                                  .base_shader_register = 0 },
            .push_constant_cbv = { .register_space = 30,
                                   .base_shader_register = 0 },
            .first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO,
            .workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO,
            .declared_read_only_images_as_srvs = true,
            .inferred_read_only_images_as_srvs = true,
            .preserve_generic_io_location_count = 13,
            .shader_model_max = SHADER_MODEL_6_2,
        };
        struct dxil_spirv_debug_options debug_options = { 0 };
        struct dxil_spirv_logger logger = {
            .priv = NULL,
            .log = d3d12_dxil_log,
        };
        struct dxil_spirv_object object = { 0 };
        /* Keep the emitted container compatible with the inbox D3D12
         * validator used by UWP and Xbox.  Validator 1.7 is newer than the
         * runtime present on part of the supported UWP device family; the
         * compiler still succeeds there, but CreateGraphicsPipelineState()
         * rejects every resulting shader with E_INVALIDARG.  Shader model
         * 6.2 only requires validator 1.4 and Mesa uses the same conservative
         * floor for portable D3D12 output. */
        if (!cache->compile(spirv, spirv_size / sizeof(uint32_t), NULL, 0,
                            (dxil_spirv_shader_stage)stage, "main",
                            DXIL_VALIDATOR_1_4, &debug_options, &configuration,
                            &logger, &object)) {
            error_setg(errp, "D3D12: SPIR-V to DXIL compilation failed");
            qemu_mutex_unlock(&cache->lock);
            return false;
        }

        char validation_error[2048] = { 0 };
        if (!cache->validate(object.binary.buffer, object.binary.size,
                             validation_error, sizeof(validation_error))) {
            error_setg(errp, "D3D12: generated DXIL is invalid: %s",
                       validation_error[0] ? validation_error :
                                             "unknown validator error");
            cache->free_object(&object);
            qemu_mutex_unlock(&cache->lock);
            return false;
        }

        g_clear_pointer(&entry->dxil, g_bytes_unref);
        entry->dxil = g_bytes_new(object.binary.buffer, object.binary.size);
        entry->hash = hash;
        entry->stage = stage;
        cache->dirty = true;
        cache->free_object(&object);
        /* Persist validated DXIL immediately. A renderer failure can prevent
         * normal shutdown, and delaying this until finalize() used to discard
         * the only artifacts capable of diagnosing a rejected PSO. */
        if (d3d12_shader_cache_save(cache)) {
            cache->dirty = false;
        } else {
            qemu_host_emit_log(QEMU_HOST_LOG_WARNING,
                               "D3D12: immediate DXIL cache write failed");
        }
    }

    entry->last_use = ++cache->use_counter;
    bytecode->pShaderBytecode =
        g_bytes_get_data(entry->dxil, &bytecode->BytecodeLength);
    qemu_mutex_unlock(&cache->lock);
    return true;
}

bool pgraph_d3d12_shader_compile_bytes(PGRAPHD3D12ShaderCache *cache,
                                       const uint32_t *spirv,
                                       size_t spirv_size, unsigned int stage,
                                       GBytes **dxil, Error **errp)
{
    D3D12_SHADER_BYTECODE bytecode;
    if (!pgraph_d3d12_shader_compile(cache, spirv, spirv_size, stage,
                                     &bytecode, errp)) {
        return false;
    }
    *dxil = g_bytes_new(bytecode.pShaderBytecode, bytecode.BytecodeLength);
    return true;
}

static void d3d12_uniform_layout_clear(PGRAPHD3D12UniformLayout *layout)
{
    for (size_t i = 0; i < layout->member_count; i++) {
        g_free(layout->members[i].name);
    }
    g_free(layout->members);
    g_free(layout->data);
    memset(layout, 0, sizeof(*layout));
}

static bool d3d12_uniform_layout_from_spirv(
    const GByteArray *spirv, unsigned int binding_index,
    PGRAPHD3D12UniformLayout *layout, Error **errp)
{
    SpvReflectShaderModule module;
    SpvReflectResult result = spvReflectCreateShaderModule(
        spirv->len, spirv->data, &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        error_setg(errp, "D3D12: SPIR-V reflection failed (%d)", result);
        return false;
    }
    uint32_t count = 0;
    result = spvReflectEnumerateDescriptorBindings(&module, &count, NULL);
    SpvReflectDescriptorBinding **bindings = g_new0(
        SpvReflectDescriptorBinding *, count);
    if (result == SPV_REFLECT_RESULT_SUCCESS) {
        result = spvReflectEnumerateDescriptorBindings(&module, &count,
                                                       bindings);
    }
    const SpvReflectBlockVariable *block = NULL;
    if (result == SPV_REFLECT_RESULT_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (bindings[i]->binding == binding_index &&
                bindings[i]->set == 0 &&
                bindings[i]->descriptor_type ==
                    SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                block = &bindings[i]->block;
                break;
            }
        }
    }
    if (!block) {
        /* A stage that does not consume any NV2A constants may have its UBO
         * removed by glslang. Keep a valid zero CBV bound; all locations stay
         * absent and no data is copied into it. */
        layout->size = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        layout->data = g_malloc0(layout->size);
        g_free(bindings);
        spvReflectDestroyShaderModule(&module);
        return true;
    }
    layout->member_count = block->member_count;
    layout->members = g_new0(PGRAPHD3D12UniformMember, block->member_count);
    layout->size = block->size;
    layout->data = g_malloc0(layout->size);
    for (uint32_t i = 0; i < block->member_count; i++) {
        const SpvReflectBlockVariable *source = &block->members[i];
        PGRAPHD3D12UniformMember *member = &layout->members[i];
        member->name = g_strdup(source->name);
        member->offset = source->offset;
        member->size = source->size;
        member->array_stride = source->array.stride;
        member->matrix_stride = source->numeric.matrix.stride;
        member->array_count = 1;
        for (uint32_t n = 0; n < source->array.dims_count; n++) {
            member->array_count *= source->array.dims[n];
        }
        member->matrix_columns =
            MAX(source->numeric.matrix.column_count, 1U);
        member->vector_components =
            MAX(source->numeric.vector.component_count, 1U);
    }
    g_free(bindings);
    spvReflectDestroyShaderModule(&module);
    return true;
}

static int d3d12_uniform_find(const PGRAPHD3D12UniformLayout *layout,
                              const char *name)
{
    for (size_t i = 0; i < layout->member_count; i++) {
        if (!strcmp(layout->members[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static void d3d12_copy_uniform(PGRAPHD3D12UniformLayout *layout, int location,
                               const void *source, const UniformInfo *info)
{
    if (location < 0) {
        return;
    }
    PGRAPHD3D12UniformMember *member = &layout->members[location];
    if (member->offset > layout->size || member->size > layout->size -
                                                       member->offset) {
        return;
    }
    const uint8_t *input = source;
    uint8_t *output = layout->data + member->offset;
    size_t scalar_count = info->size / sizeof(uint32_t);
    size_t columns = member->matrix_columns;
    size_t components = scalar_count / columns;
    size_t array_stride = member->array_stride ? member->array_stride :
                                                  member->size;
    size_t matrix_stride = member->matrix_stride ? member->matrix_stride :
                                                    components * 4;
    size_t elements = MIN((size_t)info->count,
                          (size_t)MAX(member->array_count, 1U));
    for (size_t element = 0; element < elements; element++) {
        for (size_t column = 0; column < columns; column++) {
            size_t destination_offset =
                element * array_stride + column * matrix_stride;
            size_t copy_size = components * 4;
            if (destination_offset > member->size ||
                copy_size > member->size - destination_offset) {
                return;
            }
            memcpy(output + destination_offset,
                   input + (element * scalar_count +
                            column * components) * 4,
                   copy_size);
        }
    }
}

static bool d3d12_compile_generated_shader(
    PGRAPHD3D12ShaderCache *cache, glslang_stage_t glsl_stage,
    unsigned int dxil_stage, MString *source, GBytes **dxil,
    PGRAPHD3D12UniformLayout *layout, int uniform_binding, Error **errp)
{
    GByteArray *spirv = pgraph_vk_compile_glsl_to_spv(
        glsl_stage, mstring_get_str(source));
    mstring_unref(source);
    if (!spirv) {
        error_setg(errp, "D3D12: NV2A GLSL to SPIR-V compilation failed");
        return false;
    }
    bool ok = pgraph_d3d12_shader_compile_bytes(
        cache, (const uint32_t *)spirv->data, spirv->len, dxil_stage, dxil,
        errp);
    if (ok && layout) {
        ok = d3d12_uniform_layout_from_spirv(spirv, uniform_binding, layout,
                                             errp);
    }
    g_byte_array_unref(spirv);
    return ok;
}

bool pgraph_d3d12_shader_binding_create(PGRAPHD3D12ShaderCache *cache,
                                        const ShaderState *state,
                                        PGRAPHD3D12ShaderBinding **binding,
                                        Error **errp)
{
    PGRAPHD3D12ShaderBinding *result =
        g_new0(PGRAPHD3D12ShaderBinding, 1);
    result->state = *state;
    GenVshGlslOptions vsh_options = {
        .vulkan = true,
        .prefix_outputs = pgraph_glsl_need_geom(&state->geom),
        .use_push_constants_for_uniform_attrs = false,
        .ubo_binding = 0,
    };
    GenPshGlslOptions psh_options = {
        .vulkan = true,
        .ubo_binding = 1,
        .tex_binding = 2,
    };
    if (!d3d12_compile_generated_shader(
            cache, GLSLANG_STAGE_VERTEX, DXIL_SPIRV_SHADER_VERTEX,
            pgraph_glsl_gen_vsh(&state->vsh, vsh_options),
            &result->vertex_dxil, &result->vertex_uniforms, 0, errp) ||
        !d3d12_compile_generated_shader(
            cache, GLSLANG_STAGE_FRAGMENT, DXIL_SPIRV_SHADER_FRAGMENT,
            pgraph_glsl_gen_psh(&state->psh, psh_options),
            &result->pixel_dxil, &result->pixel_uniforms, 1, errp)) {
        pgraph_d3d12_shader_binding_destroy(result);
        return false;
    }
    result->has_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (result->has_geometry_shader) {
        GenGeomGlslOptions options = {
            .vulkan = true,
            .write_point_size =
                state->geom.polygon_front_mode == POLY_MODE_POINT,
        };
        if (!d3d12_compile_generated_shader(
                cache, GLSLANG_STAGE_GEOMETRY, DXIL_SPIRV_SHADER_GEOMETRY,
                pgraph_glsl_gen_geom(&state->geom, options),
                &result->geometry_dxil, NULL, 0, errp)) {
            pgraph_d3d12_shader_binding_destroy(result);
            return false;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(result->vertex_locs); i++) {
        result->vertex_locs[i] = d3d12_uniform_find(
            &result->vertex_uniforms, VshUniformInfo[i].name);
    }
    for (size_t i = 0; i < ARRAY_SIZE(result->pixel_locs); i++) {
        result->pixel_locs[i] = d3d12_uniform_find(
            &result->pixel_uniforms, PshUniformInfo[i].name);
    }
    *binding = result;
    return true;
}

void pgraph_d3d12_shader_binding_destroy(PGRAPHD3D12ShaderBinding *binding)
{
    if (!binding) {
        return;
    }
    g_clear_pointer(&binding->vertex_dxil, g_bytes_unref);
    g_clear_pointer(&binding->geometry_dxil, g_bytes_unref);
    g_clear_pointer(&binding->pixel_dxil, g_bytes_unref);
    d3d12_uniform_layout_clear(&binding->vertex_uniforms);
    d3d12_uniform_layout_clear(&binding->pixel_uniforms);
    g_free(binding);
}

bool pgraph_d3d12_shader_binding_update_uniforms(
    PGRAPHD3D12ShaderBinding *binding, PGRAPHState *pg, Error **errp)
{
    (void)errp;
    VshUniformValues vertex_values;
    PshUniformValues pixel_values;
    memset(&vertex_values, 0, sizeof(vertex_values));
    memset(&pixel_values, 0, sizeof(pixel_values));
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                       binding->vertex_locs, &vertex_values);
    pgraph_glsl_set_psh_uniform_values(pg, binding->pixel_locs,
                                       &pixel_values);
    for (unsigned int i = 0; i < ARRAY_SIZE(pixel_values.texScale); i++) {
        pixel_values.texScale[i] = 1.0f;
    }
    for (size_t i = 0; i < ARRAY_SIZE(binding->vertex_locs); i++) {
        d3d12_copy_uniform(&binding->vertex_uniforms,
                           binding->vertex_locs[i],
                           (uint8_t *)&vertex_values +
                               VshUniformInfo[i].val_offs,
                           &VshUniformInfo[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(binding->pixel_locs); i++) {
        d3d12_copy_uniform(&binding->pixel_uniforms, binding->pixel_locs[i],
                           (uint8_t *)&pixel_values +
                               PshUniformInfo[i].val_offs,
                           &PshUniformInfo[i]);
    }
    return true;
}
