/*
 * Geforce NV2A PGRAPH Direct3D 12 Shader Compiler
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_SHADERS_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_SHADERS_H

#include "qapi/error.h"
#include <d3d12.h>
#include <stdint.h>
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

typedef struct PGRAPHD3D12ShaderCache PGRAPHD3D12ShaderCache;
typedef struct PGRAPHState PGRAPHState;

typedef struct PGRAPHD3D12UniformMember {
    char *name;
    size_t offset;
    size_t size;
    size_t array_stride;
    size_t matrix_stride;
    uint32_t array_count;
    uint32_t matrix_columns;
    uint32_t vector_components;
} PGRAPHD3D12UniformMember;

typedef struct PGRAPHD3D12UniformLayout {
    PGRAPHD3D12UniformMember *members;
    size_t member_count;
    size_t size;
    uint8_t *data;
} PGRAPHD3D12UniformLayout;

typedef struct PGRAPHD3D12ShaderBinding {
    ShaderState state;
    GBytes *vertex_dxil;
    GBytes *geometry_dxil;
    GBytes *pixel_dxil;
    PGRAPHD3D12UniformLayout vertex_uniforms;
    PGRAPHD3D12UniformLayout pixel_uniforms;
    VshUniformLocs vertex_locs;
    PshUniformLocs pixel_locs;
    bool has_geometry_shader;
} PGRAPHD3D12ShaderBinding;

bool pgraph_d3d12_shader_cache_init(PGRAPHD3D12ShaderCache **cache,
                                    ID3D12Device *device, Error **errp);
void pgraph_d3d12_shader_cache_finalize(PGRAPHD3D12ShaderCache *cache);
void pgraph_d3d12_shader_cache_flush(PGRAPHD3D12ShaderCache *cache);
bool pgraph_d3d12_shader_compile(PGRAPHD3D12ShaderCache *cache,
                                 const uint32_t *spirv, size_t spirv_size,
                                 unsigned int stage,
                                 D3D12_SHADER_BYTECODE *bytecode, Error **errp);
bool pgraph_d3d12_shader_compile_bytes(PGRAPHD3D12ShaderCache *cache,
                                       const uint32_t *spirv,
                                       size_t spirv_size, unsigned int stage,
                                       GBytes **dxil, Error **errp);
bool pgraph_d3d12_shader_binding_create(PGRAPHD3D12ShaderCache *cache,
                                        const ShaderState *state,
                                        PGRAPHD3D12ShaderBinding **binding,
                                        Error **errp);
void pgraph_d3d12_shader_binding_destroy(
    PGRAPHD3D12ShaderBinding *binding);
bool pgraph_d3d12_shader_binding_update_uniforms(
    PGRAPHD3D12ShaderBinding *binding, PGRAPHState *pg, Error **errp);

#endif
