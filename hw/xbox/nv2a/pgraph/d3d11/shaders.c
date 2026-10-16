/*
 * GeForce NV2A PGRAPH Direct3D 11 shader translation
 *
 * The NV2A operation mapping in this file is based on the xemu GLSL vertex
 * shader generator and the nv2a_vsh_cpu instruction decoder. The JIT/cache
 * organization was informed by Cxbx-Reloaded's VertexShaderCache.
 *
 * Cxbx-Reloaded is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Modified and rewritten for xemu's LLE PGRAPH renderer on 2026-09-26.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qapi/error.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "nv2a_vsh_disassembler.h"
#include "renderer.h"
#include "ui/xemu-settings.h"
#include <d3dcompiler.h>
#include "shaders.h"

#define D3D11_SHADER_CACHE_VERSION 26
#define D3D11_SHADER_CACHE_MAGIC 0x31485358U /* XSH1 */
#define D3D11_SHADER_CACHE_MAX_BYTECODE (16 * MiB)
#define D3D11_SHADER_MEMORY_CACHE_LIMIT 2048
#define D3D11_SHADER_FAILURE_CACHE_LIMIT 256
#define D3D11_PIXEL_CONSTANT_BUFFER_SIZE 736
#define D3D11_VERTEX_CONSTANT_BUFFER_SIZE                            \
    ((NV2A_VERTEXSHADER_CONSTANTS + 2 + 4 * NV2A_MAX_LIGHTS +        \
      NV2A_LTC1_COUNT + NV2A_LTCTXA_COUNT + NV2A_LTCTXB_COUNT + 4) * \
     4 * sizeof(float))

typedef enum D3D11ShaderStage {
    D3D11_SHADER_STAGE_VERTEX = 1,
    D3D11_SHADER_STAGE_PIXEL = 2,
} D3D11ShaderStage;

typedef struct D3D11ShaderCacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;
    uint64_t key_hash;
    uint32_t key_size;
    uint32_t bytecode_size;
} D3D11ShaderCacheHeader;

static uint64_t d3d11_shader_key_hash(GBytes *key, D3D11ShaderStage stage)
{
    gsize size;
    const uint8_t *data = g_bytes_get_data(key, &size);
    uint64_t hash = fast_hash(data, size);
    return hash ^ ((uint64_t)D3D11_SHADER_CACHE_VERSION << 56) ^
           ((uint64_t)stage << 48);
}

static char *d3d11_shader_cache_path(PGRAPHD3D11ShaderState *shaders,
                                     GBytes *key, D3D11ShaderStage stage)
{
    uint64_t hash = d3d11_shader_key_hash(key, stage);
    return g_strdup_printf(
        "%s/%c-%016" PRIx64 ".dxbc", shaders->cache_directory,
        stage == D3D11_SHADER_STAGE_VERTEX ? 'v' : 'p', hash);
}

static ID3DBlob *d3d11_shader_cache_load(PGRAPHD3D11ShaderState *shaders,
                                         GBytes *key, D3D11ShaderStage stage)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }
    char *path = d3d11_shader_cache_path(shaders, key, stage);
    FILE *file = qemu_fopen(path, "rb");
    if (!file) {
        g_free(path);
        return NULL;
    }
    gsize key_size;
    const void *key_data = g_bytes_get_data(key, &key_size);
    D3D11ShaderCacheHeader header;
    bool valid = fread(&header, sizeof(header), 1, file) == 1 &&
                 header.magic == D3D11_SHADER_CACHE_MAGIC &&
                 header.version == D3D11_SHADER_CACHE_VERSION &&
                 header.stage == stage && header.key_size == key_size &&
                 header.key_hash == d3d11_shader_key_hash(key, stage) &&
                 header.bytecode_size > 0 &&
                 header.bytecode_size <= D3D11_SHADER_CACHE_MAX_BYTECODE;
    uint8_t *stored_key = valid ? g_malloc(key_size) : NULL;
    valid = valid && fread(stored_key, key_size, 1, file) == 1 &&
            memcmp(stored_key, key_data, key_size) == 0;
    ID3DBlob *bytecode = NULL;
    HRESULT hr =
        valid ? D3DCreateBlob(header.bytecode_size, &bytecode) : E_FAIL;
    valid = valid && SUCCEEDED(hr) &&
            fread(ID3D10Blob_GetBufferPointer(bytecode), header.bytecode_size,
                  1, file) == 1 &&
            fgetc(file) == EOF;
    g_free(stored_key);
    fclose(file);
    if (!valid) {
        if (bytecode) {
            ID3D10Blob_Release(bytecode);
            bytecode = NULL;
        }
        qemu_unlink(path);
    }
    g_free(path);
    return bytecode;
}

static void d3d11_shader_cache_store(PGRAPHD3D11ShaderState *shaders,
                                     GBytes *key, D3D11ShaderStage stage,
                                     ID3DBlob *bytecode)
{
    if (!g_config.perf.cache_shaders || !bytecode) {
        return;
    }
    gsize key_size;
    const void *key_data = g_bytes_get_data(key, &key_size);
    SIZE_T bytecode_size = ID3D10Blob_GetBufferSize(bytecode);
    if (key_size > UINT32_MAX ||
        bytecode_size > D3D11_SHADER_CACHE_MAX_BYTECODE) {
        return;
    }
    D3D11ShaderCacheHeader header = {
        .magic = D3D11_SHADER_CACHE_MAGIC,
        .version = D3D11_SHADER_CACHE_VERSION,
        .stage = stage,
        .key_hash = d3d11_shader_key_hash(key, stage),
        .key_size = key_size,
        .bytecode_size = bytecode_size,
    };
    char *path = d3d11_shader_cache_path(shaders, key, stage);
    FILE *file = qemu_fopen(path, "wb");
    if (file) {
        bool written = fwrite(&header, sizeof(header), 1, file) == 1 &&
                       fwrite(key_data, key_size, 1, file) == 1 &&
                       fwrite(ID3D10Blob_GetBufferPointer(bytecode),
                              bytecode_size, 1, file) == 1;
        written = fclose(file) == 0 && written;
        if (!written) {
            qemu_unlink(path);
        }
    }
    g_free(path);
}

static void d3d11_append_programmable_point_size(
    GString *s, bool point_params_enable, float point_size,
    unsigned int surface_scale_factor);

static const char d3d11_vsh_preamble[] =
    "#define NV2A_COLOR_INTERPOLATION\n"
    "cbuffer Nv2aVertexConstants : register(b0) {\n"
    "  float4 C[192];\n"
    "  float4 clipRange;\n"
    "  float2 surfaceSize;\n"
    "  float2 fogParam;\n"
    "  float4 lightInfiniteDirection[8];\n"
    "  float4 lightInfiniteHalfVector[8];\n"
    "  float4 lightLocalAttenuation[8];\n"
    "  float4 lightLocalPosition[8];\n"
    "  float4 ltc1[20];\n"
    "  float4 ltctxa[26];\n"
    "  float4 ltctxb[52];\n"
    "  float4 pointParams0;\n"
    "  float4 pointParams1;\n"
    "  float materialAlpha; float specularPower; float2 _constantPadding;\n"
    "};\n"
    "struct VSIn {\n"
    "  float4 v0 : ATTRIBUTE0; float4 v1 : ATTRIBUTE1;\n"
    "  float4 v2 : ATTRIBUTE2; float4 v3 : ATTRIBUTE3;\n"
    "  float4 v4 : ATTRIBUTE4; float4 v5 : ATTRIBUTE5;\n"
    "  float4 v6 : ATTRIBUTE6; float4 v7 : ATTRIBUTE7;\n"
    "  float4 v8 : ATTRIBUTE8; float4 v9 : ATTRIBUTE9;\n"
    "  float4 v10 : ATTRIBUTE10; float4 v11 : ATTRIBUTE11;\n"
    "  float4 v12 : ATTRIBUTE12; float4 v13 : ATTRIBUTE13;\n"
    "  float4 v14 : ATTRIBUTE14; float4 v15 : ATTRIBUTE15;\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 position : SV_Position;\n"
    "  NV2A_COLOR_INTERPOLATION float4 diffuse : COLOR0;\n"
    "  NV2A_COLOR_INTERPOLATION float4 specular : COLOR1;\n"
    "  NV2A_COLOR_INTERPOLATION float4 backDiffuse : TEXCOORD4;\n"
    "  NV2A_COLOR_INTERPOLATION float4 backSpecular : TEXCOORD5;\n"
    "  float fog : FOG; float pointSize : TEXCOORD12;\n"
    "  float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1;\n"
    "  float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3;\n"
    "  nointerpolation float4 depthPos0 : TEXCOORD6;\n"
    "  nointerpolation float4 depthPos1 : TEXCOORD7;\n"
    "  nointerpolation float4 depthPos2 : TEXCOORD8;\n"
    "  nointerpolation float triMzPerspective : TEXCOORD9;\n"
    "  nointerpolation float triMzAffine : TEXCOORD10;\n"
    "};\n"
    "float4 nv_vsh_color(float4 c) {\n"
    "  float4 n=float4(c.x == c.x ? c.x : 1.0,"
    "c.y == c.y ? c.y : 1.0,c.z == c.z ? c.z : 1.0,"
    "c.w == c.w ? c.w : 1.0);\n"
    "  return saturate(n);\n"
    "}\n"
    "float4 nv_mov(float4 a) { return a; }\n"
    "float4 nv_mul(float4 a, float4 b) {\n"
    "  float4 r = a * b;\n"
    "  if (a.x == 0 || b.x == 0) r.x = 0;\n"
    "  if (a.y == 0 || b.y == 0) r.y = 0;\n"
    "  if (a.z == 0 || b.z == 0) r.z = 0;\n"
    "  if (a.w == 0 || b.w == 0) r.w = 0;\n"
    "  return r;\n"
    "}\n"
    "float4 nv_add(float4 a, float4 b) { return a + b; }\n"
    "float4 nv_mad(float4 a, float4 b, float4 c) { return nv_mul(a,b)+c; }\n"
    "float4 nv_dp3(float4 a, float4 b) { return dot(a.xyz,b.xyz).xxxx; }\n"
    "float4 nv_dph(float4 a, float4 b) { return dot(float4(a.xyz,1),b).xxxx; "
    "}\n"
    "float4 nv_dp4(float4 a, float4 b) { return dot(a,b).xxxx; }\n"
    "float4 nv_dst(float4 a, float4 b) { return float4(1,a.y*b.y,a.z,b.w); }\n"
    "float4 nv_min(float4 a, float4 b) { return min(a,b); }\n"
    "float4 nv_max(float4 a, float4 b) { return max(a,b); }\n"
    "float4 nv_slt(float4 a, float4 b) { return float4(a < b); }\n"
    "float4 nv_sge(float4 a, float4 b) { return float4(a >= b); }\n"
    "float nv_clamp_away(float x) {\n"
    "  return (x > 0 || asuint(x) == 0) ?\n"
    "         clamp(x, asfloat(0x1f800000), asfloat(0x5f800000))\n"
    "                : clamp(x, asfloat(0xdf800000), asfloat(0x9f800000));\n"
    "}\n"
    "float4 nv_rcp(float4 a) { return (1.0/a.x).xxxx; }\n"
    "float4 nv_rcc(float4 a) { return nv_clamp_away(1.0/a.x).xxxx; }\n"
    "float4 nv_rsq(float4 a) {\n"
    "  float x = a.x == 0 ? asfloat(0x7f800000) : rsqrt(abs(a.x));\n"
    "  return x.xxxx;\n"
    "}\n"
    "float4 nv_exp(float4 a) {\n"
    "  float f=floor(a.x); return float4(exp2(f),a.x-f,exp2(a.x),1);\n"
    "}\n"
    "float4 nv_log(float4 a) {\n"
    "  float x=abs(a.x); if (x==0) return "
    "float4(-asfloat(0x7f800000),1,-asfloat(0x7f800000),1);\n"
    "  float f=floor(log2(x)); return float4(f,x/exp2(f),log2(x),1);\n"
    "}\n"
    "float4 nv_lit(float4 a) {\n"
    "  float w=clamp(a.w,-127.99609375,127.99609375);\n"
    "  float x=max(a.x,0), y=max(a.y,0);\n"
    "  return float4(1,x,x>0 ? exp2(w*log2(y)) : 0,1);\n"
    "}\n"
    "float4 xf_mul4(float4 v, uint base) {\n"
    "  return float4(dot(v,C[base]),dot(v,C[base+1]),"
    "dot(v,C[base+2]),dot(v,C[base+3]));\n"
    "}\n"
    "float3 xf_mul3(float4 v, uint base) { return xf_mul4(v,base).xyz; }\n";

static void d3d11_vertex_shader_destroy(gpointer opaque)
{
    PGRAPHD3D11VertexShader *shader = opaque;
    if (shader->input_layout) {
        ID3D11InputLayout_Release(shader->input_layout);
    }
    if (shader->shader) {
        ID3D11VertexShader_Release(shader->shader);
    }
    if (shader->bytecode) {
        ID3D10Blob_Release(shader->bytecode);
    }
    g_free(shader);
}

static void d3d11_pixel_shader_destroy(gpointer opaque)
{
    PGRAPHD3D11PixelShader *shader = opaque;
    if (shader->shader) {
        ID3D11PixelShader_Release(shader->shader);
    }
    if (shader->bytecode) {
        ID3D10Blob_Release(shader->bytecode);
    }
    g_free(shader);
}

static void d3d11_geometry_shader_destroy(gpointer data)
{
    if (data) {
        ID3D11GeometryShader_Release((ID3D11GeometryShader *)data);
    }
}

bool pgraph_d3d11_shaders_init(PGRAPHD3D11State *r, Error **errp)
{
    r->shaders = g_new0(PGRAPHD3D11ShaderState, 1);
    r->shaders->vertex_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref,
        d3d11_vertex_shader_destroy);
    r->shaders->pixel_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref,
        d3d11_pixel_shader_destroy);
    r->shaders->raster_geometry_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref,
        d3d11_geometry_shader_destroy);
    r->shaders->failed_vertex_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref, NULL);
    r->shaders->failed_pixel_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref, NULL);
    r->shaders->cache_directory =
        g_strdup_printf("%sd3d11-shaders", xemu_settings_get_base_path());
    if (g_config.perf.cache_shaders) {
        qemu_mkdir(r->shaders->cache_directory);
    }
    D3D11_BUFFER_DESC desc = {
        .ByteWidth = D3D11_VERTEX_CONSTANT_BUFFER_SIZE,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
    };
    HRESULT hr = ID3D11Device_CreateBuffer(r->device, &desc, NULL,
                                           &r->shaders->vertex_constants);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: vertex constant-buffer creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        pgraph_d3d11_shaders_finalize(r);
        return false;
    }
    desc.ByteWidth = D3D11_PIXEL_CONSTANT_BUFFER_SIZE;
    hr = ID3D11Device_CreateBuffer(r->device, &desc, NULL,
                                   &r->shaders->pixel_constants);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: pixel constant-buffer creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        pgraph_d3d11_shaders_finalize(r);
        return false;
    }
    return true;
}

void pgraph_d3d11_shaders_finalize(PGRAPHD3D11State *r)
{
    if (!r->shaders) {
        return;
    }
    if (r->shaders->vertex_constants) {
        ID3D11Buffer_Release(r->shaders->vertex_constants);
    }
    if (r->shaders->pixel_constants) {
        ID3D11Buffer_Release(r->shaders->pixel_constants);
    }
    for (unsigned int i = 0; i < PGRAPHD3D11_GEOMETRY_SHADER_VARIANTS; i++) {
        if (r->shaders->depth_geometry_shaders[i]) {
            ID3D11GeometryShader_Release(
                r->shaders->depth_geometry_shaders[i]);
        }
    }
    g_hash_table_unref(r->shaders->vertex_cache);
    g_hash_table_unref(r->shaders->pixel_cache);
    g_hash_table_unref(r->shaders->raster_geometry_cache);
    g_hash_table_unref(r->shaders->failed_vertex_cache);
    g_hash_table_unref(r->shaders->failed_pixel_cache);
    g_free(r->shaders->cache_directory);
    g_free(r->shaders);
    r->shaders = NULL;
}

void pgraph_d3d11_shaders_trim(PGRAPHD3D11State *r)
{
    if (!r->shaders) {
        return;
    }
    ID3D11DeviceContext_VSSetShader(r->context, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(r->context, NULL, NULL, 0);
    ID3D11DeviceContext_GSSetShader(r->context, NULL, NULL, 0);
    r->shaders->vertex = NULL;
    r->shaders->pixel = NULL;
    r->shaders->quad_adjacency_geometry = false;
    r->shaders->polygon_line_geometry = false;
    r->shaders->raster_geometry = false;
    g_hash_table_remove_all(r->shaders->raster_geometry_cache);
    for (unsigned int i = 0; i < PGRAPHD3D11_GEOMETRY_SHADER_VARIANTS; i++) {
        if (r->shaders->depth_geometry_shaders[i]) {
            ID3D11GeometryShader_Release(
                r->shaders->depth_geometry_shaders[i]);
            r->shaders->depth_geometry_shaders[i] = NULL;
        }
        r->shaders->depth_geometry_shader_failed[i] = false;
    }
    g_hash_table_remove_all(r->shaders->vertex_cache);
    g_hash_table_remove_all(r->shaders->pixel_cache);
    g_hash_table_remove_all(r->shaders->failed_vertex_cache);
    g_hash_table_remove_all(r->shaders->failed_pixel_cache);
}

static void d3d11_append_swizzle(GString *s, const Nv2aVshInput *input)
{
    static const char component[] = "xyzw";
    g_string_append_c(s, '.');
    for (unsigned int i = 0; i < 4; i++) {
        g_string_append_c(s, component[input->swizzle[i]]);
    }
}

static void d3d11_append_input(GString *s, const Nv2aVshInput *input)
{
    if (input->is_negated) {
        g_string_append(s, "(-");
    }
    switch (input->type) {
    case NV2ART_TEMPORARY:
        g_string_append_printf(s, input->index == 12 ? "oPos" : "r%u",
                               input->index);
        break;
    case NV2ART_INPUT:
        g_string_append_printf(s, "v[%u]", input->index);
        break;
    case NV2ART_CONTEXT:
        if (input->is_relative) {
            g_string_append_printf(s, "C[clamp(%u + a0, 0, 191)]",
                                   input->index);
        } else {
            g_string_append_printf(s, "C[%u]", input->index);
        }
        break;
    default:
        g_string_append(s, "float4(0,0,0,0)");
        break;
    }
    d3d11_append_swizzle(s, input);
    if (input->is_negated) {
        g_string_append_c(s, ')');
    }
}

static const char *d3d11_operation_name(Nv2aVshOpcode opcode)
{
    switch (opcode) {
    case NV2AOP_MOV:
        return "nv_mov";
    case NV2AOP_MUL:
        return "nv_mul";
    case NV2AOP_ADD:
        return "nv_add";
    case NV2AOP_MAD:
        return "nv_mad";
    case NV2AOP_DP3:
        return "nv_dp3";
    case NV2AOP_DPH:
        return "nv_dph";
    case NV2AOP_DP4:
        return "nv_dp4";
    case NV2AOP_DST:
        return "nv_dst";
    case NV2AOP_MIN:
        return "nv_min";
    case NV2AOP_MAX:
        return "nv_max";
    case NV2AOP_SLT:
        return "nv_slt";
    case NV2AOP_SGE:
        return "nv_sge";
    case NV2AOP_RCP:
        return "nv_rcp";
    case NV2AOP_RCC:
        return "nv_rcc";
    case NV2AOP_RSQ:
        return "nv_rsq";
    case NV2AOP_EXP:
        return "nv_exp";
    case NV2AOP_LOG:
        return "nv_log";
    case NV2AOP_LIT:
        return "nv_lit";
    default:
        return NULL;
    }
}

static unsigned int d3d11_operation_input_count(Nv2aVshOpcode opcode)
{
    switch (opcode) {
    case NV2AOP_MOV:
    case NV2AOP_RCP:
    case NV2AOP_RCC:
    case NV2AOP_RSQ:
    case NV2AOP_EXP:
    case NV2AOP_LOG:
    case NV2AOP_LIT:
        return 1;
    case NV2AOP_MAD:
        return 3;
    default:
        return 2;
    }
}

static bool d3d11_append_operation(GString *s, const Nv2aVshOperation *op,
                                   const char *temporary, Error **errp)
{
    if (op->opcode == NV2AOP_NOP) {
        return true;
    }
    if (op->opcode == NV2AOP_ARL) {
        g_string_append_printf(s, "  int %s = (int)floor(", temporary);
        d3d11_append_input(s, &op->inputs[0]);
        g_string_append(s, ".x + 0.001);\n");
        return true;
    }
    const char *name = d3d11_operation_name(op->opcode);
    if (!name) {
        error_setg(errp, "D3D11: unsupported NV2A vertex opcode %u",
                   op->opcode);
        return false;
    }
    g_string_append_printf(s, "  float4 %s = %s(", temporary, name);
    unsigned int count = d3d11_operation_input_count(op->opcode);
    for (unsigned int i = 0; i < count; i++) {
        if (i) {
            g_string_append_c(s, ',');
        }
        d3d11_append_input(s, &op->inputs[i]);
    }
    g_string_append(s, ");\n");
    return true;
}

static const char *d3d11_output_name(const Nv2aVshOutput *output)
{
    static const char *const names[] = {
        "oPos", NULL,  NULL,  "oD0", "oD1", "oFog", "oPts",
        "oB0",  "oB1", "oT0", "oT1", "oT2", "oT3",
    };
    if (output->type == NV2ART_TEMPORARY) {
        return output->index == 12 ? "oPos" : NULL;
    }
    if (output->type == NV2ART_OUTPUT && output->index < ARRAY_SIZE(names)) {
        return names[output->index];
    }
    return NULL;
}

static void d3d11_append_writemask(GString *s, Nv2aVshWritemask mask)
{
    if (mask == NV2AWM_XYZW) {
        return;
    }
    g_string_append_c(s, '.');
    if (mask & NV2AWM_X) {
        g_string_append_c(s, 'x');
    }
    if (mask & NV2AWM_Y) {
        g_string_append_c(s, 'y');
    }
    if (mask & NV2AWM_Z) {
        g_string_append_c(s, 'z');
    }
    if (mask & NV2AWM_W) {
        g_string_append_c(s, 'w');
    }
}

static bool d3d11_append_outputs(GString *s, const Nv2aVshOperation *op,
                                 const char *temporary, Error **errp)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(op->outputs); i++) {
        const Nv2aVshOutput *output = &op->outputs[i];
        if (output->type == NV2ART_NONE) {
            continue;
        }
        if (output->type == NV2ART_ADDRESS || op->opcode == NV2AOP_ARL) {
            g_string_append_printf(s, "  a0 = %s;\n", temporary);
            continue;
        }
        if (output->type == NV2ART_CONTEXT) {
            error_setg(errp, "D3D11: NV2A vertex program writes constant c[%u]",
                       output->index);
            return false;
        }
        if (output->type == NV2ART_OUTPUT && output->index == 5) {
            if (output->writemask) {
                g_string_append_printf(s, "  oFog.x = %s.x;\n", temporary);
            }
            continue;
        }
        const char *name = d3d11_output_name(output);
        char register_name[16];
        if (!name && output->type == NV2ART_TEMPORARY && output->index < 12) {
            snprintf(register_name, sizeof(register_name), "r%u",
                     output->index);
            name = register_name;
        }
        if (!name) {
            error_setg(errp, "D3D11: invalid NV2A vertex output %u:%u",
                       output->type, output->index);
            return false;
        }
        g_string_append_printf(s, "  %s", name);
        d3d11_append_writemask(s, output->writemask);
        g_string_append_printf(s, " = %s", temporary);
        d3d11_append_writemask(s, output->writemask);
        g_string_append(s, ";\n");
    }
    return true;
}

static bool d3d11_append_vsh_fog_factor(GString *s, bool fog_enable,
                                        enum VshFogMode fog_mode,
                                        enum VshFoggen foggen,
                                        bool fixed_function)
{
    if (!fog_enable) {
        g_string_append(s, "  float fogFactor=1.0;\n");
        return true;
    }

    if (!fixed_function) {
        g_string_append(s, "  float fogDistance=oFog.x;\n");
    } else {
        switch (foggen) {
        case FOGGEN_SPEC_ALPHA:
            g_string_append(s,
                            "  float fogDistance=clamp(specular.a,0.0,1.0);\n");
            break;
        case FOGGEN_RADIAL:
            g_string_append(s, "  float fogDistance=length(tPosition.xyz);\n");
            break;
        case FOGGEN_PLANAR:
        case FOGGEN_ABS_PLANAR:
            g_string_append_printf(
                s,
                "  float fogDistance=dot(C[%u].xyz,tPosition.xyz)+C[%u].w;\n",
                NV_IGRAPH_XF_XFCTX_FOG, NV_IGRAPH_XF_XFCTX_FOG);
            if (foggen == FOGGEN_ABS_PLANAR) {
                g_string_append(s, "  fogDistance=abs(fogDistance);\n");
            }
            break;
        case FOGGEN_FOG_X:
            g_string_append(s, "  float fogDistance=input.v5.x;\n");
            break;
        default:
            return false;
        }
    }

    float infinite_fogdistance_result = 0.0f;
    float nan_fogfactor_result = 0.0f;
    switch (fog_mode) {
    case FOG_MODE_LINEAR:
    case FOG_MODE_LINEAR_ABS:
        infinite_fogdistance_result = 1.0f;
        nan_fogfactor_result = 1.0f;
        g_string_append(s, "  float fogFactor=fogParam.x+fogDistance*fogParam.y;\n"
                           "  fogFactor-=1.0;\n");
        break;
    case FOG_MODE_EXP:
        infinite_fogdistance_result = 1.0f;
        nan_fogfactor_result = 1.0f;
        g_string_append(s,
                        "  float fogFactor=fogParam.x+"
                        "exp2(fogDistance*fogParam.y*16.0);\n"
                        "  fogFactor-=1.5;\n");
        break;
    case FOG_MODE_EXP_ABS:
        g_string_append(s,
                        "  float fogFactor=fogParam.x+"
                        "exp2(fogDistance*fogParam.y*16.0);\n"
                        "  fogFactor-=1.5;\n");
        break;
    case FOG_MODE_EXP2:
    case FOG_MODE_EXP2_ABS:
        g_string_append(s,
                        "  float fogFactor=fogParam.x+exp2(-fogDistance*"
                        "fogDistance*fogParam.y*fogParam.y*32.0);\n"
                        "  fogFactor-=1.5;\n");
        break;
    default:
        return false;
    }
    if (fog_mode == FOG_MODE_LINEAR_ABS || fog_mode == FOG_MODE_EXP_ABS ||
        fog_mode == FOG_MODE_EXP2_ABS) {
        g_string_append(s, "  fogFactor=abs(fogFactor);\n");
    }
    g_string_append_printf(
        s,
        "  if (isinf(fogDistance)) { fogFactor=%0.1f; } else {\n"
        "    fogFactor=clamp(isnan(fogFactor) ? %0.1f : fogFactor,"
        "-asfloat(0x7f7fffff),asfloat(0x7f7fffff));\n"
        "  }\n",
        infinite_fogdistance_result, nan_fogfactor_result);
    return true;
}

static void d3d11_append_vsh_color_outputs(GString *s, const char *diffuse,
                                           const char *specular,
                                           const char *back_diffuse,
                                           const char *back_specular,
                                           bool specular_enable,
                                           bool ignore_specular_alpha)
{
    g_string_append_printf(
        s,
        "  output.diffuse=nv_vsh_color(%s); "
        "output.backDiffuse=nv_vsh_color(%s);\n",
        diffuse, back_diffuse);
    if (specular_enable) {
        g_string_append_printf(
            s,
            "  output.specular=nv_vsh_color(%s); "
            "output.backSpecular=nv_vsh_color(%s);\n",
            specular, back_specular);
        if (ignore_specular_alpha) {
            g_string_append(s,
                            "  output.specular.w=1.0; "
                            "output.backSpecular.w=1.0;\n");
        }
    } else {
        g_string_append(s,
                        "  output.specular=float4(0,0,0,1); "
                        "output.backSpecular=float4(0,0,0,1);\n");
    }
}

static char *d3d11_translate_vertex_program(const VshState *state,
                                            Error **errp)
{
    const ProgrammableVshState *program = &state->programmable;
    if (!program->program_length ||
        program->program_length > NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        error_setg(errp, "D3D11: invalid NV2A vertex program length %d",
                   program->program_length);
        return NULL;
    }
    GString *s = g_string_new(d3d11_vsh_preamble);
    g_string_append(s, "VSOut main(VSIn input) {\n  float4 v[16];\n");
    for (unsigned int i = 0; i < 16; i++) {
        g_string_append_printf(s, "  v[%u] = input.v%u;\n", i, i);
    }
    for (unsigned int i = 0; i < 12; i++) {
        g_string_append_printf(s, "  float4 r%u = 0;\n", i);
    }
    g_string_append(s, "  int a0=0; float4 oPos=float4(0,0,0,1);\n"
                       "  float4 oD0=float4(0,0,0,1), oD1=float4(0,0,0,1);\n"
                       "  float4 oB0=float4(0,0,0,1), oB1=float4(0,0,0,1);\n"
                       "  float4 oFog=float4(0,0,0,1), oPts=float4(0,0,0,1);\n"
                       "  float4 oT0=float4(0,0,0,1), oT1=float4(0,0,0,1);\n"
                       "  float4 oT2=float4(0,0,0,1), oT3=float4(0,0,0,1);\n");
    bool final = false;
    for (int i = 0; i < program->program_length; i++) {
        Nv2aVshStep step = { 0 };
        Nv2aVshParseResult result = nv2a_vsh_parse_step(
            &step, (const uint32_t *)&program->program_data[i]);
        if (result != NV2AVPR_SUCCESS) {
            error_setg(errp, "D3D11: invalid NV2A vertex instruction %d (%d)",
                       i, result);
            g_string_free(s, TRUE);
            return NULL;
        }
        char mac[24], ilu[24];
        snprintf(mac, sizeof(mac), "mac_result_%d", i);
        snprintf(ilu, sizeof(ilu), "ilu_result_%d", i);
        g_string_append_printf(s, "  // NV2A instruction %d\n", i);
        if (!d3d11_append_operation(s, &step.mac, mac, errp) ||
            !d3d11_append_operation(s, &step.ilu, ilu, errp) ||
            !d3d11_append_outputs(s, &step.mac, mac, errp) ||
            !d3d11_append_outputs(s, &step.ilu, ilu, errp)) {
            g_string_free(s, TRUE);
            return NULL;
        }
        if (step.is_final) {
            final = true;
            break;
        }
    }
    if (!final) {
        error_setg(errp, "D3D11: NV2A vertex program has no FINAL instruction");
        g_string_free(s, TRUE);
        return NULL;
    }
    if (!d3d11_append_vsh_fog_factor(s, state->fog_enable, state->fog_mode,
                                     state->fixed_function.foggen, false)) {
        error_setg(errp, "D3D11: unsupported vertex fog mode");
        g_string_free(s, TRUE);
        return NULL;
    }
    g_string_append(
        s, "  oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\n"
           "  oPos.w = nv_clamp_away(oPos.w);\n"
           "  float4 depthPosition=float4(oPos.xy,oPos.z,oPos.w);\n"
           "  oPos.xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize; "
           "oPos.y=-oPos.y;\n"
           "  oPos.z = oPos.z / clipRange.y; oPos.xyz *= oPos.w;\n"
           "  VSOut output; output.position=oPos;\n"
           "  output.depthPos0=depthPosition; output.depthPos1=depthPosition;\n"
           "  output.depthPos2=depthPosition; output.triMzPerspective=0;\n"
           "  output.triMzAffine=0;\n");
    d3d11_append_vsh_color_outputs(s, "oD0", "oD1", "oB0", "oB1",
                                   state->specular_enable,
                                   state->ignore_specular_alpha);
    g_string_append(s, "  output.fog=fogFactor;\n");
    d3d11_append_programmable_point_size(
        s, state->point_params_enable, state->point_size,
        state->surface_scale_factor);
    g_string_append(s,
           "  output.tex0=oT0;\n"
           "  output.tex1=oT1; output.tex2=oT2; output.tex3=oT3;\n"
           "  return output;\n}\n");
    return g_string_free(s, FALSE);
}

typedef enum D3D11TexgenComponentKind {
    D3D11_TEXGEN_INPUT,
    D3D11_TEXGEN_EYE_LINEAR,
    D3D11_TEXGEN_OBJECT_LINEAR,
    D3D11_TEXGEN_NORMAL,
    D3D11_TEXGEN_SPHERE,
    D3D11_TEXGEN_REFLECTION,
    D3D11_TEXGEN_INVALID,
} D3D11TexgenComponentKind;

static D3D11TexgenComponentKind d3d11_texgen_component_kind(
    unsigned int mode, unsigned int component)
{
    switch (mode) {
    case TEXGEN_DISABLE:
        return D3D11_TEXGEN_INPUT;
    case TEXGEN_EYE_LINEAR:
        return D3D11_TEXGEN_EYE_LINEAR;
    case TEXGEN_OBJECT_LINEAR:
        return D3D11_TEXGEN_OBJECT_LINEAR;
    case TEXGEN_NORMAL_MAP:
        return component < 3 ? D3D11_TEXGEN_NORMAL : D3D11_TEXGEN_INVALID;
    case TEXGEN_SPHERE_MAP:
        return component < 2 ? D3D11_TEXGEN_SPHERE : D3D11_TEXGEN_INVALID;
    case TEXGEN_REFLECTION_MAP:
        return component < 3 ? D3D11_TEXGEN_REFLECTION :
                               D3D11_TEXGEN_INVALID;
    default:
        return D3D11_TEXGEN_INVALID;
    }
}

static void d3d11_append_fixed_function_texgen_vectors(
    GString *s, const char *tex, const enum VshTexgen modes[4])
{
    bool sphere = false;
    bool reflection = false;
    for (unsigned int component = 0; component < 4; component++) {
        D3D11TexgenComponentKind kind =
            d3d11_texgen_component_kind(modes[component], component);
        sphere |= kind == D3D11_TEXGEN_SPHERE;
        reflection |= kind == D3D11_TEXGEN_REFLECTION;
    }
    if (!sphere && !reflection) {
        return;
    }

    g_string_append(s,
                    "  { float3 n=tNormal;"
                    " float3 u=normalize(tPosition.xyz); float3 r=reflect(u,n);\n");
    if (sphere) {
        g_string_append(s,
                        "    float invM=1.0/(2.0*length(r+"
                        "float3(0,0,1)));\n");
    }
    for (unsigned int component = 0; component < 4; component++) {
        char channel = "xyzw"[component];
        D3D11TexgenComponentKind kind =
            d3d11_texgen_component_kind(modes[component], component);
        if (kind == D3D11_TEXGEN_SPHERE) {
            g_string_append_printf(s, "    %s.%c=r.%c*invM+0.5;\n", tex,
                                   channel, channel);
        } else if (kind == D3D11_TEXGEN_REFLECTION) {
            g_string_append_printf(s, "    %s.%c=r.%c;\n", tex, channel,
                                   channel);
        }
    }
    g_string_append(s, "  }\n");
}

static void d3d11_append_vsh_point_size(GString *s, bool point_params_enable,
                                        float point_size,
                                        unsigned int surface_scale_factor,
                                        unsigned int model_view_matrix)
{
    if (point_params_enable) {
        g_string_append_printf(
            s,
            "  float d_e=length(xf_mul4(position,%u));\n"
            "  float ptMinSize=min(pointParams1.w,63.875);\n"
            "  float ptMaxSize=min(pointParams0.w+ptMinSize,63.875);\n"
            "  float pointSizeAttenuated=1.0/sqrt(pointParams0.x+"
            "pointParams0.y*d_e+pointParams0.z*d_e*d_e)+pointParams1.z;\n"
            "  output.pointSize=clamp(pointSizeAttenuated*pointParams0.w+"
            "pointParams1.w,ptMinSize,ptMaxSize)*%u.0;\n",
            model_view_matrix, surface_scale_factor);
    } else {
        g_string_append_printf(s, "  output.pointSize=%ff;\n",
                               MAX(1.0f, point_size) *
                                   surface_scale_factor);
    }
}

static void d3d11_append_programmable_point_size(
    GString *s, bool point_params_enable, float point_size,
    unsigned int surface_scale_factor)
{
    if (point_params_enable) {
        g_string_append(s, "  output.pointSize=oPts.x;\n");
    } else {
        if (point_size <= 0.0f) {
            point_size = 1.0f;
        }
        g_string_append_printf(s, "  output.pointSize=%ff;\n",
                               point_size * surface_scale_factor);
    }
}

static void d3d11_append_fixed_function_specular_mix(GString *s,
                                                      bool specular_enable,
                                                      bool separate_specular)
{
    if (specular_enable && !separate_specular) {
        g_string_append(s, "  litDiffuse.rgb+=litSpecular.rgb;"
                           " litSpecular=specular;\n");
    }
}

static char *d3d11_translate_fixed_function_vertex(const VshState *state,
                                                   Error **errp)
{
    const FixedFunctionVshState *ff = &state->fixed_function;
    GString *s = g_string_new(d3d11_vsh_preamble);

    /*
     * The NV2A fixed-function transform consumes the same XF context vectors
     * exposed through C[] as the programmable path.  Keep the initial D3D11
     * implementation deliberately expressed in terms of those verified xemu
     * constants instead of reconstructing desktop D3D fixed-function state.
     */
    g_string_append_printf(
        s, "VSOut main(VSIn input) {\n"
           "  float4 position=input.v0, diffuse=input.v3, specular=input.v4;\n"
           "  float4 backDiffuse=input.v7, backSpecular=input.v8;\n"
           "  float4 oT0=input.v9, oT1=input.v10;\n"
           "  float4 oT2=input.v11, oT3=input.v12;\n"
           "  float4 tPosition=0; float3 tNormal=0;\n");

    unsigned int skin_count = 0;
    bool generated_last_weight = false;
    switch (ff->skinning) {
    case SKINNING_OFF:
        break;
    case SKINNING_1WEIGHTS:
        skin_count = 2;
        generated_last_weight = true;
        break;
    case SKINNING_2WEIGHTS2MATRICES:
        skin_count = 2;
        break;
    case SKINNING_2WEIGHTS:
        skin_count = 3;
        generated_last_weight = true;
        break;
    case SKINNING_3WEIGHTS3MATRICES:
        skin_count = 3;
        break;
    case SKINNING_3WEIGHTS:
        skin_count = 4;
        generated_last_weight = true;
        break;
    case SKINNING_4WEIGHTS4MATRICES:
        skin_count = 4;
        break;
    default:
        error_setg(errp, "D3D11: invalid fixed-function skinning mode %u",
                   ff->skinning);
        g_string_free(s, TRUE);
        return NULL;
    }
    if (!skin_count) {
        g_string_append_printf(s,
                               "  tPosition=xf_mul4(position,%u);"
                               " tNormal=xf_mul3(float4(input.v2.xyz,0),%u);\n",
                               NV_IGRAPH_XF_XFCTX_MMAT0,
                               NV_IGRAPH_XF_XFCTX_IMMAT0);
    } else {
        g_string_append(s, "  float remainingWeight=1.0;\n");
        for (unsigned int i = 0; i < skin_count; i++) {
            char component = "xyzw"[i];
            if (generated_last_weight && i == skin_count - 1) {
                g_string_append(s, "  float skinWeight=remainingWeight;\n");
            } else {
                g_string_append_printf(s,
                                       "  float skinWeight%u=input.v1.%c;"
                                       " remainingWeight-=skinWeight%u;\n",
                                       i, component, i);
            }
            const char *weight_name =
                generated_last_weight && i == skin_count - 1 ? "skinWeight" :
                                                               NULL;
            if (weight_name) {
                g_string_append_printf(
                    s,
                    "  tPosition+=xf_mul4(position,%u)*%s;"
                    " tNormal+=xf_mul3(float4(input.v2.xyz,0),%u)*%s;\n",
                    NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8, weight_name,
                    NV_IGRAPH_XF_XFCTX_IMMAT0 + i * 8, weight_name);
            } else {
                g_string_append_printf(
                    s,
                    "  tPosition+=xf_mul4(position,%u)*skinWeight%u;"
                    " tNormal+=xf_mul3(float4(input.v2.xyz,0),%u)*"
                    "skinWeight%u;\n",
                    NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8, i,
                    NV_IGRAPH_XF_XFCTX_IMMAT0 + i * 8, i);
            }
        }
    }
    if (ff->normalization) {
        g_string_append(s, "  tNormal=normalize(tNormal);\n");
    }

    for (unsigned int stage = 0; stage < NV2A_MAX_TEXTURES; stage++) {
        const char *tex = stage == 0 ? "oT0" :
                          stage == 1 ? "oT1" :
                          stage == 2 ? "oT2" :
                                       "oT3";
        const char *input = stage == 0 ? "input.v9" :
                            stage == 1 ? "input.v10" :
                            stage == 2 ? "input.v11" :
                                         "input.v12";
        unsigned int plane = NV_IGRAPH_XF_XFCTX_TG0MAT + stage * 8;
        for (unsigned int component = 0; component < 4; component++) {
            char channel = "xyzw"[component];
            unsigned int mode = ff->texgen[stage][component];
            D3D11TexgenComponentKind kind =
                d3d11_texgen_component_kind(mode, component);
            switch (kind) {
            case D3D11_TEXGEN_INPUT:
                break;
            case D3D11_TEXGEN_EYE_LINEAR:
                g_string_append_printf(s, "  %s.%c=dot(tPosition,C[%u]);\n",
                                       tex, channel, plane + component);
                break;
            case D3D11_TEXGEN_OBJECT_LINEAR:
                g_string_append_printf(s, "  %s.%c=dot(position,C[%u]);\n", tex,
                                       channel, plane + component);
                break;
            case D3D11_TEXGEN_NORMAL:
                g_string_append_printf(s, "  %s.%c=tNormal.%c;\n", tex,
                                       channel, channel);
                break;
            case D3D11_TEXGEN_SPHERE:
            case D3D11_TEXGEN_REFLECTION:
                break;
            default:
                error_setg(errp,
                           "D3D11: unsupported fixed-function texgen %u "
                           "on channel %u",
                           mode, component);
                g_string_free(s, TRUE);
                return NULL;
            }
        }
        d3d11_append_fixed_function_texgen_vectors(s, tex,
                                                   ff->texgen[stage]);
        if (ff->texture_matrix_enable[stage]) {
            unsigned int matrix = NV_IGRAPH_XF_XFCTX_T0MAT + stage * 8;
            g_string_append_printf(s,
                                   "  %s=float4(dot(%s,C[%u]),dot(%s,C[%u]),"
                                   "dot(%s,C[%u]),dot(%s,C[%u]));\n",
                                   tex, tex, matrix + 0, tex, matrix + 1, tex,
                                   matrix + 2, tex, matrix + 3);
        }
        (void)input;
    }

    if (ff->lighting) {
        const char *alpha =
            ff->diffuse_src == MATERIAL_COLOR_SRC_MATERIAL ? "materialAlpha" :
            ff->diffuse_src == MATERIAL_COLOR_SRC_SPECULAR ? "specular.a" :
                                                             "diffuse.a";
        const char *ambient =
            ff->ambient_src == MATERIAL_COLOR_SRC_DIFFUSE  ? "diffuse.rgb" :
            ff->ambient_src == MATERIAL_COLOR_SRC_SPECULAR ? "specular.rgb" :
                                                             "ltctxa[17].rgb";
        const char *emission =
            ff->emission_src == MATERIAL_COLOR_SRC_DIFFUSE  ? "diffuse.rgb" :
            ff->emission_src == MATERIAL_COLOR_SRC_SPECULAR ? "specular.rgb" :
                                                              "ltctxa[17].rgb";
        g_string_append_printf(
            s,
            "  float4 litDiffuse=float4(%s,%s);"
            " litDiffuse.rgb*=ltctxa[%u].rgb; litDiffuse.rgb+=%s;\n"
            "  float4 litSpecular=float4(0,0,0,specular.a);\n",
            ambient, alpha, NV_IGRAPH_XF_LTCTXA_CM_COL, emission);
        if (ff->local_eye) {
            g_string_append_printf(
                s,
                "  float3 eyeVector=normalize(C[%u].xyz/C[%u].w-"
                "tPosition.xyz/tPosition.w);\n",
                NV_IGRAPH_XF_XFCTX_EYEP, NV_IGRAPH_XF_XFCTX_EYEP);
        } else {
            g_string_append(s, "  float3 eyeVector=float3(0,0,0);\n");
        }
        for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            if (ff->light[i] == LIGHT_OFF) {
                continue;
            }
            g_string_append_printf(s, "  { // NV2A light %u\n", i);
            if (ff->light[i] == LIGHT_INFINITE) {
                g_string_append_printf(
                    s,
                    "    float attenuation=1.0;"
                    " float3 "
                    "lightVector=normalize(lightInfiniteDirection[%u].xyz);\n",
                    i);
                if (ff->local_eye) {
                    g_string_append(
                        s, "    float3 "
                           "halfVector=normalize(lightVector+eyeVector);\n");
                } else {
                    g_string_append_printf(
                        s,
                        "    float3 "
                        "halfVector=lightInfiniteHalfVector[%u].xyz;\n",
                        i);
                }
            } else {
                g_string_append_printf(
                    s,
                    "    float3 delta=lightLocalPosition[%u].xyz-"
                    "tPosition.xyz/tPosition.w; float "
                    "distanceToLight=length(delta);\n"
                    "    float3 lightVector=normalize(delta);"
                    " float attenuation=distanceToLight<=ltc1[%u].x ?"
                    " 1.0/(lightLocalAttenuation[%u].x+"
                    "lightLocalAttenuation[%u].y*distanceToLight+"
                    "lightLocalAttenuation[%u].z*distanceToLight*"
                    "distanceToLight) : 0.0;\n"
                    "    float3 halfVector=normalize(lightVector+eyeVector);\n",
                    i, NV_IGRAPH_XF_LTC1_r0 + i, i, i, i);
                if (ff->light[i] == LIGHT_SPOT) {
                    g_string_append_printf(
                        s,
                        "    float4 spot=ltctxa[%u]; float invScale=1.0/"
                        "length(spot.xyz); float rho=invScale*dot(spot.xyz,"
                        "lightVector); float outer=-invScale*spot.w;"
                        " float inner=invScale+outer; attenuation*=rho>inner ?"
                        " 1.0 : (rho<=outer ? 0.0 : dot(spot.xyz,lightVector)+"
                        "spot.w);\n",
                        NV_IGRAPH_XF_LTCTXA_L0_SPT + i * 2);
                }
            }
            const char *diffuse_factor =
                ff->diffuse_src == MATERIAL_COLOR_SRC_DIFFUSE ?
                    "diffuse.rgb*" :
                ff->diffuse_src == MATERIAL_COLOR_SRC_SPECULAR ?
                    "specular.rgb*" :
                    "";
            const char *specular_factor =
                ff->specular_src == MATERIAL_COLOR_SRC_DIFFUSE ?
                    "diffuse.rgb*" :
                ff->specular_src == MATERIAL_COLOR_SRC_SPECULAR ?
                    "specular.rgb*" :
                    "";
            g_string_append_printf(
                s,
                "    float ndl=max(0.0,dot(tNormal,lightVector));"
                " float ndh=max(0.0,dot(tNormal,halfVector));\n"
                "    float power=(ndl==0.0||ndh==0.0) ? 0.0 :"
                " pow(ndh,specularPower);\n"
                "    litDiffuse.rgb+=ltctxb[%u].rgb*attenuation;\n"
                "    litDiffuse.rgb+=%sltctxb[%u].rgb*attenuation*ndl;\n"
                "    litSpecular.rgb+=%sltctxb[%u].rgb*attenuation*power;\n"
                "  }\n",
                NV_IGRAPH_XF_LTCTXB_L0_AMB + i * 6, diffuse_factor,
                NV_IGRAPH_XF_LTCTXB_L0_DIF + i * 6, specular_factor,
                NV_IGRAPH_XF_LTCTXB_L0_SPC + i * 6);
        }
        d3d11_append_fixed_function_specular_mix(
            s, state->specular_enable, state->separate_specular);
        g_string_append(s, "  diffuse=litDiffuse; specular=litSpecular;\n");
    }

    if (!skin_count) {
        g_string_append(s, "  tPosition=position;\n");
    }
    if (!d3d11_append_vsh_fog_factor(
            s, state->fog_enable, state->fog_mode, ff->foggen, true)) {
        error_setg(errp, "D3D11: unsupported fixed-function fog mode");
        g_string_free(s, TRUE);
        return NULL;
    }
    g_string_append_printf(s, "  float4 oPos=xf_mul4(tPosition,%u);\n",
                           NV_IGRAPH_XF_XFCTX_CMAT0);

    g_string_append_printf(
        s,
        "  oPos.w=nv_clamp_away(oPos.w);\n"
        "  oPos.xy/=oPos.w;\n"
        "  oPos.xy+=C[%u].xy;\n"
        "  oPos.xy=trunc(oPos.xy*16.0)/16.0;\n"
        "  float4 depthPosition=float4(oPos.xy,oPos.z/oPos.w,oPos.w);\n"
        "  oPos.xy=(2.0*oPos.xy-surfaceSize)/surfaceSize; oPos.y=-oPos.y;\n"
        "  oPos.z=oPos.z/clipRange.y; oPos.xy*=oPos.w;\n"
        "  VSOut output; output.position=oPos;\n"
        "  output.depthPos0=depthPosition; output.depthPos1=depthPosition;\n"
        "  output.depthPos2=depthPosition; output.triMzPerspective=0;\n"
        "  output.triMzAffine=0;\n",
        NV_IGRAPH_XF_XFCTX_VPOFF);
    d3d11_append_vsh_color_outputs(s, "diffuse", "specular", "backDiffuse",
                                   "backSpecular", state->specular_enable,
                                   state->ignore_specular_alpha);
    g_string_append(s, "  output.fog=fogFactor;\n");
    d3d11_append_vsh_point_size(
        s, state->point_params_enable, state->point_size,
        state->surface_scale_factor, NV_IGRAPH_XF_XFCTX_MMAT0);
    g_string_append(s,
                    "  output.tex0=oT0; output.tex1=oT1;\n"
                    "  output.tex2=oT2; output.tex3=oT3; return output;\n}\n");
    return g_string_free(s, FALSE);
}

static const char *d3d11_shader_profile(const PGRAPHD3D11State *r,
                                        bool vertex)
{
    if (r->feature_level >= D3D_FEATURE_LEVEL_11_0) {
        return vertex ? "vs_5_0" : "ps_5_0";
    }
    if (r->feature_level >= D3D_FEATURE_LEVEL_10_1) {
        return vertex ? "vs_4_1" : "ps_4_1";
    }
    if (r->feature_level >= D3D_FEATURE_LEVEL_10_0) {
        return vertex ? "vs_4_0" : "ps_4_0";
    }
    return NULL;
}

static PGRAPHD3D11VertexShader *d3d11_compile_vertex_shader(PGRAPHD3D11State *r,
                                                            const char *source,
                                                            Error **errp)
{
    ID3DBlob *bytecode = NULL, *errors = NULL;
    const char *profile = d3d11_shader_profile(r, true);
    if (!profile) {
        error_setg(errp, "D3D11: unsupported shader feature level 0x%x",
                   r->feature_level);
        return NULL;
    }
    HRESULT hr = D3DCompile2(
        source, strlen(source), "nv2a_vertex.hlsl", NULL, NULL, "main",
        profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, 0, NULL, 0, &bytecode, &errors);
    if (FAILED(hr)) {
        const char *message = errors ? ID3D10Blob_GetBufferPointer(errors) :
                                       "no compiler diagnostics";
        error_setg(errp,
                   "D3D11: NV2A vertex shader compilation failed "
                   "(HRESULT 0x%08lx): %s",
                   (unsigned long)hr, message);
        if (errors) {
            ID3D10Blob_Release(errors);
        }
        return NULL;
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    PGRAPHD3D11VertexShader *shader = g_new0(PGRAPHD3D11VertexShader, 1);
    shader->bytecode = bytecode;
    hr = ID3D11Device_CreateVertexShader(
        r->device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader->shader);
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: CreateVertexShader failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        d3d11_vertex_shader_destroy(shader);
        return NULL;
    }
    return shader;
}

static PGRAPHD3D11VertexShader *
d3d11_create_cached_vertex_shader(PGRAPHD3D11State *r, GBytes *key)
{
    ID3DBlob *bytecode =
        d3d11_shader_cache_load(r->shaders, key, D3D11_SHADER_STAGE_VERTEX);
    if (!bytecode) {
        return NULL;
    }
    PGRAPHD3D11VertexShader *shader = g_new0(PGRAPHD3D11VertexShader, 1);
    shader->bytecode = bytecode;
    HRESULT hr = ID3D11Device_CreateVertexShader(
        r->device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader->shader);
    if (FAILED(hr)) {
        char *path =
            d3d11_shader_cache_path(r->shaders, key, D3D11_SHADER_STAGE_VERTEX);
        qemu_unlink(path);
        g_free(path);
        d3d11_vertex_shader_destroy(shader);
        return NULL;
    }
    return shader;
}

static void d3d11_trim_vertex_cache(PGRAPHD3D11ShaderState *shaders)
{
    if (g_hash_table_size(shaders->vertex_cache) <
        D3D11_SHADER_MEMORY_CACHE_LIMIT) {
        return;
    }
    GHashTableIter iter;
    gpointer key, value, oldest_key = NULL;
    uint64_t oldest_use = UINT64_MAX;
    g_hash_table_iter_init(&iter, shaders->vertex_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PGRAPHD3D11VertexShader *shader = value;
        if (shader != shaders->vertex && shader->last_used < oldest_use) {
            oldest_key = key;
            oldest_use = shader->last_used;
        }
    }
    if (oldest_key) {
        g_hash_table_remove(shaders->vertex_cache, oldest_key);
    }
}

typedef struct PGRAPHD3D11VertexConstants {
    float c[NV2A_VERTEXSHADER_CONSTANTS][4];
    float clip_range[4];
    float surface_size[2];
    float fog_param[2];
    float light_infinite_direction[NV2A_MAX_LIGHTS][4];
    float light_infinite_half_vector[NV2A_MAX_LIGHTS][4];
    float light_local_attenuation[NV2A_MAX_LIGHTS][4];
    float light_local_position[NV2A_MAX_LIGHTS][4];
    float ltc1[NV2A_LTC1_COUNT][4];
    float ltctxa[NV2A_LTCTXA_COUNT][4];
    float ltctxb[NV2A_LTCTXB_COUNT][4];
    float point_params[8];
    float material_alpha;
    float specular_power;
    float padding[2];
    float line_params[4];
} PGRAPHD3D11VertexConstants;

static bool d3d11_update_vertex_constants(PGRAPHState *pg,
                                          const VshState *state, Error **errp)
{
    QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D11VertexConstants) !=
                      D3D11_VERTEX_CONSTANT_BUFFER_SIZE);
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    VshUniformLocs locs;
    for (unsigned int i = 0; i < ARRAY_SIZE(locs); i++) {
        locs[i] = -1;
    }
    locs[VshUniform_c] = 0;
    locs[VshUniform_clipRange] = 0;
    locs[VshUniform_surfaceSize] = 0;
    locs[VshUniform_fogParam] = 0;
    locs[VshUniform_lightInfiniteDirection] = 0;
    locs[VshUniform_lightInfiniteHalfVector] = 0;
    locs[VshUniform_lightLocalAttenuation] = 0;
    locs[VshUniform_lightLocalPosition] = 0;
    locs[VshUniform_ltc1] = 0;
    locs[VshUniform_ltctxa] = 0;
    locs[VshUniform_ltctxb] = 0;
    locs[VshUniform_material_alpha] = 0;
    locs[VshUniform_pointParams] = 0;
    locs[VshUniform_specularPower] = 0;
    VshUniformValues values = { 0 };
    pgraph_glsl_set_vsh_uniform_values(pg, state, locs, &values);
    PGRAPHD3D11VertexConstants constants = { 0 };
    memcpy(constants.c, values.c, sizeof(constants.c));
    memcpy(constants.clip_range, values.clipRange[0],
           sizeof(constants.clip_range));
    memcpy(constants.surface_size, values.surfaceSize[0],
           sizeof(constants.surface_size));
    memcpy(constants.fog_param, values.fogParam[0],
           sizeof(constants.fog_param));
    for (unsigned int i = 0; i < NV2A_MAX_LIGHTS; i++) {
        memcpy(constants.light_infinite_direction[i],
               values.lightInfiniteDirection[i], 3 * sizeof(float));
        memcpy(constants.light_infinite_half_vector[i],
               values.lightInfiniteHalfVector[i], 3 * sizeof(float));
        memcpy(constants.light_local_attenuation[i],
               values.lightLocalAttenuation[i], 3 * sizeof(float));
        memcpy(constants.light_local_position[i], values.lightLocalPosition[i],
               3 * sizeof(float));
    }
    memcpy(constants.ltc1, values.ltc1, sizeof(constants.ltc1));
    memcpy(constants.ltctxa, values.ltctxa, sizeof(constants.ltctxa));
    memcpy(constants.ltctxb, values.ltctxb, sizeof(constants.ltctxb));
    memcpy(constants.point_params, values.pointParams,
           sizeof(constants.point_params));
    constants.material_alpha = values.material_alpha[0];
    constants.specular_power = values.specularPower[0];
    unsigned int viewport_width = pg->surface_binding_dim.width;
    unsigned int viewport_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &viewport_width, &viewport_height);
    constants.line_params[0] = MAX(1U, viewport_width);
    constants.line_params[1] = MAX(1U, viewport_height);
    constants.line_params[2] = MAX(1U, pg->surface_scale_factor);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(
        r->context, (ID3D11Resource *)r->shaders->vertex_constants, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: vertex constant-buffer update failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ID3D11DeviceContext_Unmap(
        r->context, (ID3D11Resource *)r->shaders->vertex_constants, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(r->context, 0, 1,
                                             &r->shaders->vertex_constants);
    ID3D11DeviceContext_GSSetConstantBuffers(
        r->context, 0, 1, &r->shaders->vertex_constants);
    return true;
}

static void d3d11_append_programmable_fog_key(GByteArray *key_data,
                                               bool fog_enable,
                                               enum VshFogMode fog_mode)
{
    const uint8_t fog_enabled_key = fog_enable ? 1 : 0;
    const uint32_t fog_mode_key = (uint32_t)fog_mode;
    g_byte_array_append(key_data, &fog_enabled_key, sizeof(fog_enabled_key));
    g_byte_array_append(key_data, (const uint8_t *)&fog_mode_key,
                        sizeof(fog_mode_key));
}

static void d3d11_append_programmable_vsh_key(
    GByteArray *key_data, bool fog_enable, enum VshFogMode fog_mode,
    enum VshFoggen foggen, bool specular_enable, bool ignore_specular_alpha,
    bool point_params_enable, float point_size,
    unsigned int surface_scale_factor)
{
    const uint32_t foggen_key = (uint32_t)foggen;
    const uint8_t codegen_flags[] = {
        specular_enable ? 1 : 0,
        ignore_specular_alpha ? 1 : 0,
        point_params_enable ? 1 : 0,
    };
    d3d11_append_programmable_fog_key(key_data, fog_enable, fog_mode);
    g_byte_array_append(key_data, (const uint8_t *)&foggen_key,
                        sizeof(foggen_key));
    g_byte_array_append(key_data, codegen_flags, sizeof(codegen_flags));
    g_byte_array_append(key_data, (const uint8_t *)&point_size,
                        sizeof(point_size));
    g_byte_array_append(key_data, (const uint8_t *)&surface_scale_factor,
                        sizeof(surface_scale_factor));
}

bool pgraph_d3d11_bind_vertex_shader(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    VshState state = { 0 };
    pgraph_glsl_set_vsh_state(pg, &state);
    const ProgrammableVshState *program = &state.programmable;
    GByteArray *key_data;
    if (state.is_fixed_function) {
        key_data = g_byte_array_sized_new(sizeof(state));
        g_byte_array_append(key_data, (const uint8_t *)&state, sizeof(state));
    } else {
        key_data = g_byte_array_sized_new(sizeof(program->program_length) +
                                          program->program_length *
                                              sizeof(program->program_data[0]) +
                                          sizeof(uint8_t) + sizeof(uint32_t) +
                                          sizeof(uint32_t) +
                                          3 * sizeof(uint8_t) + sizeof(float) +
                                          sizeof(state.surface_scale_factor));
        g_byte_array_append(key_data, (const uint8_t *)&program->program_length,
                            sizeof(program->program_length));
        g_byte_array_append(key_data, (const uint8_t *)program->program_data,
                            program->program_length *
                                sizeof(program->program_data[0]));
        d3d11_append_programmable_vsh_key(
            key_data, state.fog_enable, state.fog_mode,
            state.fixed_function.foggen, state.specular_enable,
            state.ignore_specular_alpha, state.point_params_enable,
            state.point_size, state.surface_scale_factor);
    }
    GBytes *key = g_byte_array_free_to_bytes(key_data);
    PGRAPHD3D11VertexShader *shader =
        g_hash_table_lookup(r->shaders->vertex_cache, key);
    if (!shader) {
        if (g_hash_table_contains(r->shaders->failed_vertex_cache, key)) {
            error_setg(errp, "D3D11: cached NV2A vertex shader failure");
            g_bytes_unref(key);
            return false;
        }
        shader = d3d11_create_cached_vertex_shader(r, key);
    }
    if (!shader) {
        char *source = state.is_fixed_function ?
                           d3d11_translate_fixed_function_vertex(&state, errp) :
                           d3d11_translate_vertex_program(&state, errp);
        if (!source) {
            if (g_hash_table_size(r->shaders->failed_vertex_cache) >=
                D3D11_SHADER_FAILURE_CACHE_LIMIT) {
                g_hash_table_remove_all(r->shaders->failed_vertex_cache);
            }
            g_hash_table_add(r->shaders->failed_vertex_cache, g_bytes_ref(key));
            g_bytes_unref(key);
            return false;
        }
        shader = d3d11_compile_vertex_shader(r, source, errp);
        g_free(source);
        if (!shader) {
            if (g_hash_table_size(r->shaders->failed_vertex_cache) >=
                D3D11_SHADER_FAILURE_CACHE_LIMIT) {
                g_hash_table_remove_all(r->shaders->failed_vertex_cache);
            }
            g_hash_table_add(r->shaders->failed_vertex_cache, g_bytes_ref(key));
            g_bytes_unref(key);
            return false;
        }
        d3d11_shader_cache_store(r->shaders, key, D3D11_SHADER_STAGE_VERTEX,
                                 shader->bytecode);
        d3d11_trim_vertex_cache(r->shaders);
        g_hash_table_insert(r->shaders->vertex_cache, key, shader);
    } else {
        if (!g_hash_table_lookup(r->shaders->vertex_cache, key)) {
            d3d11_trim_vertex_cache(r->shaders);
            g_hash_table_insert(r->shaders->vertex_cache, key, shader);
        } else {
            g_bytes_unref(key);
        }
    }
    shader->last_used = ++r->shaders->use_serial;
    r->shaders->vertex = shader;
    if (!d3d11_update_vertex_constants(pg, &state, errp)) {
        return false;
    }
    ID3D11DeviceContext_VSSetShader(r->context, shader->shader, NULL, 0);
    return true;
}

static const char d3d11_psh_preamble[] =
    "cbuffer Nv2aPixelConstants : register(b0) {\n"
    "  float4 K[18]; float4 fogColor; int alphaRef; float3 eyeVector;\n"
    "  float4 bumpMat[4]; float4 bumpParams[4]; float4 clipRange;\n"
    "  uint4 colorKey; uint4 colorKeyMask; int4 clipRegion[8];\n"
    "  float4 texScale; float4 texCoordScale[4]; float2 surfaceScale; float "
    "depthOffset;\n"
    "  float depthFactor; float4 blendColor;\n"
    "};\n"
    "struct PSIn {\n"
    "  float4 position : SV_Position;\n"
    "  float4 diffuse : COLOR0; float4 specular : COLOR1;\n"
    "  float4 backDiffuse : TEXCOORD4; float4 backSpecular : TEXCOORD5;\n"
    "  float fog : FOG; float pointSize : TEXCOORD12;\n"
    "  float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1;\n"
    "  float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3;\n"
    "  nointerpolation float4 depthPos0 : TEXCOORD6;\n"
    "  nointerpolation float4 depthPos1 : TEXCOORD7;\n"
    "  nointerpolation float4 depthPos2 : TEXCOORD8;\n"
    "  nointerpolation float triMzPerspective : TEXCOORD9;\n"
    "  nointerpolation float triMzAffine : TEXCOORD10;\n"
    "  float2 pointCoord : TEXCOORD11;\n"
    "  bool frontFace : SV_IsFrontFace;\n"
    "};\n"
    "float nv_mul1(float a,float b) { return (a==0||b==0)?0:a*b; }\n"
    "float3 nv_mul3(float3 a,float3 b) {\n"
    "  float3 r=a*b; if(a.x==0||b.x==0)r.x=0;\n"
    "  if(a.y==0||b.y==0)r.y=0; if(a.z==0||b.z==0)r.z=0; return r;\n"
    "}\n"
    "bool nv_color_key_match(float4 texel,uint key,uint mask) {\n"
    "  uint4 c=(uint4)(texel*255.0+0.5);\n"
    "  uint value=(c.a<<24)|(c.r<<16)|(c.g<<8)|c.b;\n"
    "  return (value&mask)==(key&mask);\n"
    "}\n";

static void d3d11_append_quad_depth_line(GString *source,
                                         bool wide_lines,
                                         unsigned int edge,
                                         unsigned int p0,
                                         unsigned int p1,
                                         unsigned int p2,
                                         unsigned int v0,
                                         unsigned int v1)
{
    if (wide_lines) {
        g_string_append_printf(
            source,
            "  float4 p%u0=input[%u].depthPos0; "
            "float4 p%u1=input[%u].depthPos0; "
            "float4 p%u2=input[%u].depthPos0;\n"
            "  float mzP%u=nv2a_tri_mz_perspective(p%u0,p%u1,p%u2);\n"
            "  float mzA%u=nv2a_tri_mz_affine(p%u0,p%u1,p%u2);\n"
            "  float4 lineOffset%u=nv2a_line_offset(input[%u].position,"
            "input[%u].position);\n"
            "  stream.Append(nv2a_make_wide_line_vertex(input[%u],input[3],"
            "p%u0,p%u1,p%u2,mzP%u,mzA%u,lineOffset%u,1.0,-1.0));\n"
            "  stream.Append(nv2a_make_wide_line_vertex(input[%u],input[3],"
            "p%u0,p%u1,p%u2,mzP%u,mzA%u,lineOffset%u,-1.0,-1.0));\n"
            "  stream.Append(nv2a_make_wide_line_vertex(input[%u],input[3],"
            "p%u0,p%u1,p%u2,mzP%u,mzA%u,lineOffset%u,1.0,1.0));\n"
            "  stream.Append(nv2a_make_wide_line_vertex(input[%u],input[3],"
            "p%u0,p%u1,p%u2,mzP%u,mzA%u,lineOffset%u,-1.0,1.0));\n"
            "  stream.RestartStrip();\n",
            edge, p0, edge, p1, edge, p2,
            edge, edge, edge, edge,
            edge, edge, edge, edge,
            edge, v0, v1,
            v0, edge, edge, edge, edge, edge, edge,
            v0, edge, edge, edge, edge, edge, edge,
            v1, edge, edge, edge, edge, edge, edge,
            v1, edge, edge, edge, edge, edge, edge);
        return;
    }
    g_string_append_printf(
        source,
        "  float4 p%u0=input[%u].depthPos0; "
        "float4 p%u1=input[%u].depthPos0; "
        "float4 p%u2=input[%u].depthPos0;\n"
        "  float mzP%u=nv2a_tri_mz_perspective(p%u0,p%u1,p%u2);\n"
        "  float mzA%u=nv2a_tri_mz_affine(p%u0,p%u1,p%u2);\n"
        "  stream.Append(nv2a_make_quad_vertex(input[%u],input[3],"
        "p%u0,p%u1,p%u2,mzP%u,mzA%u));\n"
        "  stream.Append(nv2a_make_quad_vertex(input[%u],input[3],"
        "p%u0,p%u1,p%u2,mzP%u,mzA%u));\n"
        "  stream.RestartStrip();\n",
        edge, p0, edge, p1, edge, p2,
        edge, edge, edge, edge,
        edge, edge, edge, edge,
        v0, edge, edge, edge, edge, edge,
        v1, edge, edge, edge, edge, edge);
}

static void d3d11_append_quad_depth_point(GString *source,
                                          unsigned int point,
                                          unsigned int p0,
                                          unsigned int p1,
                                          unsigned int p2,
                                          unsigned int vertex)
{
    g_string_append_printf(
        source,
        "  float4 p%u0=input[%u].depthPos0; "
        "float4 p%u1=input[%u].depthPos0; "
        "float4 p%u2=input[%u].depthPos0;\n"
        "  float mzP%u=nv2a_tri_mz_perspective(p%u0,p%u1,p%u2);\n"
        "  float mzA%u=nv2a_tri_mz_affine(p%u0,p%u1,p%u2);\n"
        "  GSOut pointVertex%u=nv2a_make_quad_vertex(input[%u],input[3],"
        "input[%u].depthPos0,input[%u].depthPos0,input[%u].depthPos0,"
        "mzP%u,mzA%u);\n"
        "  stream.Append(nv2a_make_point_vertex(pointVertex%u,"
        "float2(-1.0,1.0),float2(0.0,0.0)));\n"
        "  stream.Append(nv2a_make_point_vertex(pointVertex%u,"
        "float2(1.0,1.0),float2(1.0,0.0)));\n"
        "  stream.Append(nv2a_make_point_vertex(pointVertex%u,"
        "float2(-1.0,-1.0),float2(0.0,1.0)));\n"
        "  stream.Append(nv2a_make_point_vertex(pointVertex%u,"
        "float2(1.0,-1.0),float2(1.0,1.0)));\n"
        "  stream.RestartStrip();\n",
        point, p0, point, p1, point, p2,
        point, point, point, point,
        point, point, point, point,
        point, point, vertex, vertex, vertex, point, point, point,
        point, point, point);
}

static void d3d11_append_quad_depth_triangle(GString *source,
                                              unsigned int triangle,
                                              unsigned int p0,
                                              unsigned int p1,
                                              unsigned int p2,
                                              unsigned int v0,
                                              unsigned int v1,
                                              unsigned int v2)
{
    g_string_append_printf(
        source,
        "  float4 p%u0=input[%u].depthPos0; "
        "float4 p%u1=input[%u].depthPos0; "
        "float4 p%u2=input[%u].depthPos0;\n"
        "  float mzP%u=nv2a_tri_mz_perspective(p%u0,p%u1,p%u2);\n"
        "  float mzA%u=nv2a_tri_mz_affine(p%u0,p%u1,p%u2);\n"
        "  stream.Append(nv2a_make_quad_vertex(input[%u],input[3],"
        "p%u0,p%u1,p%u2,mzP%u,mzA%u));\n"
        "  stream.Append(nv2a_make_quad_vertex(input[%u],input[3],"
        "p%u0,p%u1,p%u2,mzP%u,mzA%u));\n"
        "  stream.Append(nv2a_make_quad_vertex(input[%u],input[3],"
        "p%u0,p%u1,p%u2,mzP%u,mzA%u));\n"
        "  stream.RestartStrip();\n",
        triangle, p0, triangle, p1, triangle, p2,
        triangle, triangle, triangle, triangle,
        triangle, triangle, triangle, triangle,
        v0, triangle, triangle, triangle, triangle, triangle,
        v1, triangle, triangle, triangle, triangle, triangle,
        v2, triangle, triangle, triangle, triangle, triangle);
}

static void d3d11_append_depth_geometry_vertex_helper(GString *source,
                                                      bool smooth_shading,
                                                      bool wide_lines)
{
    g_string_append(
        source,
        "GSOut nv2a_make_quad_vertex(GSIn vertex,GSIn flatVertex,"
        "float4 p0,float4 p1,float4 p2,float mzP,float mzA){\n"
        "  GSOut o; o.position=vertex.position;\n");
    if (smooth_shading) {
        g_string_append(
            source,
            "  o.diffuse=vertex.diffuse; o.specular=vertex.specular;\n"
            "  o.backDiffuse=vertex.backDiffuse; "
            "o.backSpecular=vertex.backSpecular;\n");
    } else {
        g_string_append(
            source,
            "  o.diffuse=flatVertex.diffuse; "
            "o.specular=flatVertex.specular;\n"
            "  o.backDiffuse=flatVertex.backDiffuse; "
            "o.backSpecular=flatVertex.backSpecular;\n");
    }
    g_string_append(
        source,
        "  o.fog=vertex.fog; o.pointSize=vertex.pointSize;\n"
        "  o.tex0=vertex.tex0; o.tex1=vertex.tex1;\n"
        "  o.tex2=vertex.tex2; o.tex3=vertex.tex3;\n"
        "  o.pointCoord=float2(0.0,0.0);\n"
        "  o.depthPos0=p0; o.depthPos1=p1; o.depthPos2=p2;\n"
        "  o.triMzPerspective=mzP; o.triMzAffine=mzA; return o;\n"
        "}\n");
    if (wide_lines) {
        g_string_append_printf(
            source,
            "cbuffer Nv2aGeometryConstants : register(b0) {\n"
            "  float4 nv2aLineParams : packoffset(c%u);\n"
            "};\n",
            (unsigned int)(D3D11_VERTEX_CONSTANT_BUFFER_SIZE /
                               (4 * sizeof(float)) -
                           1));
        g_string_append(
            source,
            "float4 nv2a_line_offset(float4 a,float4 b){\n"
            "  float2 viewport=max(nv2aLineParams.xy,float2(1.0,1.0));\n"
            "  float2 delta=(b.xy/b.w-a.xy/a.w)*viewport;\n"
            "  float length2=dot(delta,delta);\n"
            "  float2 fallback=nv2aLineParams.z/viewport;\n"
            "  if(abs(a.w)<=1e-20||abs(b.w)<=1e-20||"
            "isnan(length2)||isinf(length2)||length2<=1e-12)\n"
            "    return float4(0.0,fallback.y,fallback.x,0.0);\n"
            "  float2 normal=float2(-delta.y,delta.x)*rsqrt(length2);\n"
            "  return float4(normal*nv2aLineParams.z/viewport,0.0,0.0);\n"
            "}\n"
            "GSOut nv2a_make_wide_line_vertex(GSIn vertex,GSIn flatVertex,"
            "float4 p0,float4 p1,float4 p2,float mzP,float mzA,"
            "float4 offset,float side,float endSide){\n"
            "  GSOut o=nv2a_make_quad_vertex(vertex,flatVertex,"
            "p0,p1,p2,mzP,mzA);\n"
            "  o.position.xy+=(offset.xy*side+offset.zw*endSide)*"
            "o.position.w; return o;\n"
            "}\n");
    }
}

static void d3d11_append_point_geometry_vertex_helper(GString *source)
{
    g_string_append_printf(
        source,
        "cbuffer Nv2aPointGeometryConstants : register(b0) {\n"
        "  float4 nv2aPointViewport : packoffset(c%u);\n"
        "};\n"
        "GSOut nv2a_make_point_vertex(GSOut vertex,float2 corner,float2 coord){\n"
        "  float2 viewport=max(nv2aPointViewport.xy,float2(1.0,1.0));\n"
        "  float pointSize=max(vertex.pointSize,1.0);\n"
        "  vertex.position.xy+=corner*(pointSize/viewport)*vertex.position.w;\n"
        "  vertex.pointCoord=coord; return vertex;\n"
        "}\n",
        (unsigned int)(D3D11_VERTEX_CONSTANT_BUFFER_SIZE /
                           (4 * sizeof(float)) -
                       1));
}

static char *d3d11_depth_geometry_shader_source(
    bool smooth_shading, unsigned int provoking_mode,
    enum ShaderPrimitiveMode primitive, enum ShaderPolygonMode polygon_mode,
    bool wide_lines)
{
    const char *provoking = provoking_mode == 0 ? "0u" :
                            provoking_mode == 1 ? "1u" :
                            provoking_mode == 2 ? "2u" :
                                                  "(2u-(primitiveId&1u))";
    const char *line_provoking =
        primitive == PRIM_TYPE_POLYGON && polygon_mode == POLY_MODE_LINE ?
            "0" : provoking_mode == 0 ? "0" : "1";
    GString *source = g_string_new("");
    g_string_append(
        source,
        "struct GSIn {\n"
        "  float4 position:SV_Position; float4 diffuse:COLOR0;\n"
        "  float4 specular:COLOR1; float4 backDiffuse:TEXCOORD4;\n"
        "  float4 backSpecular:TEXCOORD5; float fog:FOG; float pointSize:TEXCOORD12;\n"
        "  float4 tex0:TEXCOORD0; float4 tex1:TEXCOORD1;\n"
        "  float4 tex2:TEXCOORD2; float4 tex3:TEXCOORD3;\n"
        "  nointerpolation float4 depthPos0:TEXCOORD6;\n"
        "  nointerpolation float4 depthPos1:TEXCOORD7;\n"
        "  nointerpolation float4 depthPos2:TEXCOORD8;\n"
        "  nointerpolation float triMzPerspective:TEXCOORD9;\n"
        "  nointerpolation float triMzAffine:TEXCOORD10;\n"
        "};\n"
        "struct GSOut {\n"
        "  float4 position:SV_Position;\n"
        "  float4 diffuse:COLOR0; float4 specular:COLOR1;\n"
        "  float4 backDiffuse:TEXCOORD4; float4 backSpecular:TEXCOORD5;\n"
        "  float fog:FOG; float pointSize:TEXCOORD12;\n"
        "  float4 tex0:TEXCOORD0; float4 tex1:TEXCOORD1;\n"
        "  float4 tex2:TEXCOORD2; float4 tex3:TEXCOORD3;\n"
        "  nointerpolation float4 depthPos0:TEXCOORD6;\n"
        "  nointerpolation float4 depthPos1:TEXCOORD7;\n"
        "  nointerpolation float4 depthPos2:TEXCOORD8;\n"
        "  nointerpolation float triMzPerspective:TEXCOORD9;\n"
        "  nointerpolation float triMzAffine:TEXCOORD10;\n"
        "  float2 pointCoord:TEXCOORD11;\n"
        "};\n"
        "float nv2a_kahan_det(float a,float b,float c,float d){\n"
        "  precise float cd=c*d; precise float err=mad(-c,d,cd);\n"
        "  precise float value=mad(a,b,-cd)+err; return value;\n"
        "}\n"
        "float nv2a_tri_mz_perspective(float4 p0,float4 p1,float4 p2){\n"
        "  precise float2 m0=p1.xy-p0.xy; precise float2 m1=p2.xy-p0.xy;\n"
        "  precise float2 b=float2(p0.w-p1.w,p0.w-p2.w);\n"
        "  b/=float2(p1.w,p2.w)*p0.w;\n"
        "  float det=nv2a_kahan_det(m0.x,m1.y,m1.x,m0.y);\n"
        "  float dx=nv2a_kahan_det(b.x,m1.y,b.y,m0.y)/det;\n"
        "  float dy=nv2a_kahan_det(b.y,m0.x,b.x,m1.x)/det;\n"
        "  float slope=max(abs(dx),abs(dy));\n"
        "  return (isnan(slope)||isinf(slope))?0.0:slope;\n"
        "}\n"
        "float nv2a_tri_mz_affine(float4 p0,float4 p1,float4 p2){\n"
        "  precise float2 m0=p1.xy-p0.xy; precise float2 m1=p2.xy-p0.xy;\n"
        "  precise float2 b=float2(p1.z-p0.z,p2.z-p0.z);\n"
        "  float det=nv2a_kahan_det(m0.x,m1.y,m1.x,m0.y);\n"
        "  float dx=nv2a_kahan_det(b.x,m1.y,b.y,m0.y)/det;\n"
        "  float dy=nv2a_kahan_det(b.y,m0.x,b.x,m1.x)/det;\n"
        "  float slope=max(abs(dx),abs(dy));\n"
        "  return (isnan(slope)||isinf(slope))?0.0:slope;\n"
        "}\n");
    if (primitive == PRIM_TYPE_POINTS) {
        d3d11_append_depth_geometry_vertex_helper(source, true, false);
        d3d11_append_point_geometry_vertex_helper(source);
        g_string_append(
            source,
            "[maxvertexcount(4)]\n"
            "void main(point GSIn input[1],inout TriangleStream<GSOut> stream){\n"
            "  float4 p=input[0].depthPos0;\n"
            "  GSOut pointVertex=nv2a_make_quad_vertex(input[0],input[0],p,p,p,"
            "0.0,0.0);\n"
            "  pointVertex.backDiffuse=pointVertex.diffuse;"
            " pointVertex.backSpecular=pointVertex.specular;\n"
            "  stream.Append(nv2a_make_point_vertex(pointVertex,float2(-1.0,1.0),"
            "float2(0.0,0.0)));\n"
            "  stream.Append(nv2a_make_point_vertex(pointVertex,float2(1.0,1.0),"
            "float2(1.0,0.0)));\n"
            "  stream.Append(nv2a_make_point_vertex(pointVertex,float2(-1.0,-1.0),"
            "float2(0.0,1.0)));\n"
            "  stream.Append(nv2a_make_point_vertex(pointVertex,float2(1.0,-1.0),"
            "float2(1.0,1.0)));\n"
            "  stream.RestartStrip();\n"
            "}\n");
        return g_string_free(source, FALSE);
    }
    if (primitive == PRIM_TYPE_POLYGON && polygon_mode == POLY_MODE_LINE) {
        d3d11_append_depth_geometry_vertex_helper(source, smooth_shading,
                                                  wide_lines);
        g_string_append_printf(
            source,
            "[maxvertexcount(%u)]\n"
            "void main(line GSIn input[2],"
            "inout %s<GSOut> stream){\n"
            "  float4 p0=input[0].depthPos0; "
            "float4 p1=input[1].depthPos0;\n"
            "  float2 delta=p1.xy-p0.xy;\n"
            "  float4 p2=float4(p0.xy+float2(-delta.y,delta.x),p0.zw);\n",
            wide_lines ? 4 : 2,
            wide_lines ? "TriangleStream" : "LineStream");
        if (wide_lines) {
            g_string_append(
                source,
                "  float4 lineOffset=nv2a_line_offset(input[0].position,"
                "input[1].position);\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[0],input[0],"
                "p0,p1,p2,0.0,0.0,lineOffset,1.0,-1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[0],input[0],"
                "p0,p1,p2,0.0,0.0,lineOffset,-1.0,-1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[1],input[0],"
                "p0,p1,p2,0.0,0.0,lineOffset,1.0,1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[1],input[0],"
                "p0,p1,p2,0.0,0.0,lineOffset,-1.0,1.0));\n"
                "  stream.RestartStrip();\n");
        } else {
            g_string_append(
                source,
                "  stream.Append(nv2a_make_quad_vertex(input[0],input[0],"
                "p0,p1,p2,0.0,0.0));\n"
                "  stream.Append(nv2a_make_quad_vertex(input[1],input[0],"
                "p0,p1,p2,0.0,0.0));\n"
                "  stream.RestartStrip();\n");
        }
        g_string_append(source, "}\n");
        return g_string_free(source, FALSE);
    }
    if (primitive == PRIM_TYPE_LINES ||
        primitive == PRIM_TYPE_LINE_LOOP ||
        primitive == PRIM_TYPE_LINE_STRIP) {
        d3d11_append_depth_geometry_vertex_helper(source, smooth_shading,
                                                  wide_lines);
        g_string_append_printf(
            source,
            "[maxvertexcount(%u)]\n"
            "void main(line GSIn input[2],"
            "inout %s<GSOut> stream){\n"
            "  float4 p0=input[0].depthPos0; "
            "float4 p1=input[1].depthPos0;\n"
            "  float2 delta=p1.xy-p0.xy;\n"
            "  float4 p2=float4(p0.xy+float2(-delta.y,delta.x),p0.zw);\n"
            "  float mzP=nv2a_tri_mz_perspective(p0,p1,p2);\n"
            "  float mzA=nv2a_tri_mz_affine(p0,p1,p2);\n",
            wide_lines ? 4 : 2,
            wide_lines ? "TriangleStream" : "LineStream");
        if (wide_lines) {
            g_string_append_printf(
                source,
                "  float4 lineOffset=nv2a_line_offset(input[0].position,"
                "input[1].position);\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[0],"
                "input[%s],p0,p1,p2,mzP,mzA,lineOffset,1.0,-1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[0],"
                "input[%s],p0,p1,p2,mzP,mzA,lineOffset,-1.0,-1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[1],"
                "input[%s],p0,p1,p2,mzP,mzA,lineOffset,1.0,1.0));\n"
                "  stream.Append(nv2a_make_wide_line_vertex(input[1],"
                "input[%s],p0,p1,p2,mzP,mzA,lineOffset,-1.0,1.0));\n"
                "  stream.RestartStrip();\n",
                line_provoking, line_provoking,
                line_provoking, line_provoking);
        } else {
            g_string_append_printf(
                source,
                "  stream.Append(nv2a_make_quad_vertex(input[0],input[%s],"
                "p0,p1,p2,mzP,mzA));\n"
                "  stream.Append(nv2a_make_quad_vertex(input[1],input[%s],"
                "p0,p1,p2,mzP,mzA));\n"
                "  stream.RestartStrip();\n",
                line_provoking, line_provoking);
        }
        g_string_append(source, "}\n");
        return g_string_free(source, FALSE);
    }
    if (polygon_mode == POLY_MODE_LINE &&
        (primitive == PRIM_TYPE_TRIANGLES ||
         primitive == PRIM_TYPE_TRIANGLE_STRIP ||
         primitive == PRIM_TYPE_TRIANGLE_FAN)) {
        d3d11_append_depth_geometry_vertex_helper(source, smooth_shading,
                                                  wide_lines);
        g_string_append_printf(
            source,
            "[maxvertexcount(%u)]\n"
            "void main(triangle GSIn input[3],uint primitiveId:SV_PrimitiveID,"
            "inout %s<GSOut> stream){\n"
            "  uint p=%s;\n"
            "  float4 p0=input[0].depthPos0; float4 p1=input[1].depthPos0;\n"
            "  float4 p2=input[2].depthPos0;\n"
            "  float mzP=nv2a_tri_mz_perspective(p0,p1,p2);\n"
            "  float mzA=nv2a_tri_mz_affine(p0,p1,p2);\n"
            "  for(uint i=0;i<3;i++){ uint j=(i+1)%%3;\n"
            "    float4 lp0=input[i].depthPos0;\n"
            "    float4 lp1=input[j].depthPos0;\n"
            "    float2 delta=lp1.xy-lp0.xy;\n"
            "    float4 lp2=float4(lp0.xy+float2(-delta.y,delta.x),lp0.zw);\n",
            wide_lines ? 12 : 6,
            wide_lines ? "TriangleStream" : "LineStream",
            provoking);
        if (wide_lines) {
            g_string_append(
                source,
                "    float4 lineOffset=nv2a_line_offset(input[i].position,"
                "input[j].position);\n"
                "    stream.Append(nv2a_make_wide_line_vertex(input[i],input[p],"
                "lp0,lp1,lp2,mzP,mzA,lineOffset,1.0,-1.0));\n"
                "    stream.Append(nv2a_make_wide_line_vertex(input[i],input[p],"
                "lp0,lp1,lp2,mzP,mzA,lineOffset,-1.0,-1.0));\n"
                "    stream.Append(nv2a_make_wide_line_vertex(input[j],input[p],"
                "lp0,lp1,lp2,mzP,mzA,lineOffset,1.0,1.0));\n"
                "    stream.Append(nv2a_make_wide_line_vertex(input[j],input[p],"
                "lp0,lp1,lp2,mzP,mzA,lineOffset,-1.0,1.0));\n"
                "    stream.RestartStrip();\n"
                "  }\n"
                "}\n");
        } else {
            g_string_append(
                source,
                "    stream.Append(nv2a_make_quad_vertex(input[i],input[p],"
                "lp0,lp1,lp2,mzP,mzA));\n"
                "    stream.Append(nv2a_make_quad_vertex(input[j],input[p],"
                "lp0,lp1,lp2,mzP,mzA));\n"
                "    stream.RestartStrip();\n"
                "  }\n"
                "}\n");
        }
        return g_string_free(source, FALSE);
    }
    if (primitive == PRIM_TYPE_QUADS ||
        primitive == PRIM_TYPE_QUAD_STRIP) {
        d3d11_append_depth_geometry_vertex_helper(source, smooth_shading,
                                                  wide_lines);
        bool polygon_line = polygon_mode == POLY_MODE_LINE;
        bool polygon_point = polygon_mode == POLY_MODE_POINT;
        if (polygon_point) {
            d3d11_append_point_geometry_vertex_helper(source);
        }
        unsigned int max_vertices = polygon_line ? (wide_lines ? 16 : 8) :
                                    polygon_point ? 16 : 6;
        const char *stream_type = polygon_line ?
                                      (wide_lines ? "TriangleStream" :
                                                    "LineStream") :
                                  "TriangleStream";
        g_string_append_printf(source, "[maxvertexcount(%u)]\n", max_vertices);
        if (primitive == PRIM_TYPE_QUAD_STRIP) {
            g_string_append_printf(
                source,
                "void main(lineadj GSIn input[4],"
                "uint primitiveId:SV_PrimitiveID,"
                "inout %s<GSOut> stream){\n"
                "  if((primitiveId&1u)!=0u)return;\n",
                stream_type);
        } else {
            g_string_append_printf(
                source,
                "void main(lineadj GSIn input[4],"
                "inout %s<GSOut> stream){\n",
                stream_type);
        }
        if (polygon_line && primitive == PRIM_TYPE_QUADS) {
            /* GL emits only the four perimeter edges; do not expose the 0-2
             * triangulation diagonal as D3D11 wireframe rasterization would. */
            d3d11_append_quad_depth_line(source, wide_lines, 0, 0, 1, 2, 0, 1);
            d3d11_append_quad_depth_line(source, wide_lines, 1, 0, 1, 2, 1, 2);
            d3d11_append_quad_depth_line(source, wide_lines, 2, 0, 2, 3, 2, 3);
            d3d11_append_quad_depth_line(source, wide_lines, 3, 0, 2, 3, 3, 0);
        } else if (polygon_line) {
            /* Match geom.c's quad-strip perimeter and split-depth planes. */
            d3d11_append_quad_depth_line(source, wide_lines, 0, 2, 0, 1, 0, 1);
            d3d11_append_quad_depth_line(source, wide_lines, 1, 2, 1, 3, 1, 3);
            d3d11_append_quad_depth_line(source, wide_lines, 2, 2, 1, 3, 3, 2);
            d3d11_append_quad_depth_line(source, wide_lines, 3, 2, 0, 1, 2, 0);
        } else if (polygon_point && primitive == PRIM_TYPE_QUADS) {
            /* The first triangle owns points 0-2; the second owns point 3. */
            d3d11_append_quad_depth_point(source, 0, 0, 1, 2, 0);
            d3d11_append_quad_depth_point(source, 1, 0, 1, 2, 1);
            d3d11_append_quad_depth_point(source, 2, 0, 1, 2, 2);
            d3d11_append_quad_depth_point(source, 3, 0, 2, 3, 3);
        } else if (polygon_point) {
            /* Match geom.c's quad-strip triangle split and point ordering. */
            d3d11_append_quad_depth_point(source, 0, 2, 0, 1, 0);
            d3d11_append_quad_depth_point(source, 1, 2, 0, 1, 1);
            d3d11_append_quad_depth_point(source, 2, 2, 0, 1, 2);
            d3d11_append_quad_depth_point(source, 3, 2, 1, 3, 3);
        } else if (primitive == PRIM_TYPE_QUADS) {
            /* Match geom.c: the quad is split along 0-2 and uses vertex 3
             * for flat-shaded attributes on both output triangles. */
            d3d11_append_quad_depth_triangle(source, 0, 0, 1, 2, 1, 2, 0);
            g_string_append(source, "\n");
            d3d11_append_quad_depth_triangle(source, 1, 0, 2, 3, 2, 3, 0);
        } else {
            /* Match geom.c's even-primitive handling of GL_LINE_STRIP_ADJACENCY. */
            d3d11_append_quad_depth_triangle(source, 0, 2, 0, 1, 0, 1, 2);
            g_string_append(source, "\n");
            d3d11_append_quad_depth_triangle(source, 1, 2, 1, 3, 2, 1, 3);
        }
        g_string_append(source, "}\n");
        return g_string_free(source, FALSE);
    }
    bool polygon_point = polygon_mode == POLY_MODE_POINT;
    if (polygon_point) {
        d3d11_append_point_geometry_vertex_helper(source);
    }
    g_string_append_printf(
        source,
        "[maxvertexcount(%u)]\n"
        "void main(triangle GSIn input[3],uint primitiveId:SV_PrimitiveID,"
        "inout TriangleStream<GSOut> stream){\n"
        "  uint p=%s;\n"
        "  float4 p0=input[0].depthPos0; float4 p1=input[1].depthPos0;\n"
        "  float4 p2=input[2].depthPos0;\n"
        "  float mzP=nv2a_tri_mz_perspective(p0,p1,p2);\n"
        "  float mzA=nv2a_tri_mz_affine(p0,p1,p2);\n",
        polygon_point ? 12 : 3, provoking);
    if (!smooth_shading) {
        g_string_append(
            source,
            "  float4 flatDiffuse=p==0?input[0].diffuse:"
            "(p==1?input[1].diffuse:input[2].diffuse);\n"
            "  float4 flatSpecular=p==0?input[0].specular:"
            "(p==1?input[1].specular:input[2].specular);\n"
            "  float4 flatBackDiffuse=p==0?input[0].backDiffuse:"
            "(p==1?input[1].backDiffuse:input[2].backDiffuse);\n"
            "  float4 flatBackSpecular=p==0?input[0].backSpecular:"
            "(p==1?input[1].backSpecular:input[2].backSpecular);\n");
    }
    g_string_append(
        source,
        "  for(uint i=0;i<3;i++){ GSOut v=(GSOut)0;\n");
    if (primitive == PRIM_TYPE_POLYGON && polygon_point) {
        /* A fan repeats its first two vertices. Emit each boundary point only
         * once, so alpha blending, stencil and occlusion are not duplicated.
         */
        g_string_append(source, "    if(primitiveId!=0 && i<2) continue;\n");
    }
    g_string_append(source, "    v.position=input[i].position;\n");
    if (smooth_shading) {
        g_string_append(source,
                        "    v.diffuse=input[i].diffuse; v.specular=input[i].specular;\n"
                        "    v.backDiffuse=input[i].backDiffuse;\n"
                        "    v.backSpecular=input[i].backSpecular;\n");
    } else {
        g_string_append(source,
                        "    v.diffuse=flatDiffuse; v.specular=flatSpecular;\n"
                        "    v.backDiffuse=flatBackDiffuse;\n"
                        "    v.backSpecular=flatBackSpecular;\n");
    }
    g_string_append(
        source,
        "    v.fog=input[i].fog; v.pointSize=input[i].pointSize;\n"
        "    v.tex0=input[i].tex0; v.tex1=input[i].tex1;\n"
        "    v.tex2=input[i].tex2; v.tex3=input[i].tex3;\n");
    if (polygon_point) {
        g_string_append(
            source,
            "    v.depthPos0=input[i].depthPos0; "
            "v.depthPos1=input[i].depthPos0; "
            "v.depthPos2=input[i].depthPos0;\n");
    } else {
        g_string_append(
            source,
            "    v.depthPos0=p0; v.depthPos1=p1; v.depthPos2=p2;\n");
    }
    g_string_append(
        source,
        "    v.triMzPerspective=mzP; v.triMzAffine=mzA;\n"
        );
    if (polygon_point) {
        g_string_append(
            source,
            "    stream.Append(nv2a_make_point_vertex(v,float2(-1.0,1.0),"
            "float2(0.0,0.0)));\n"
            "    stream.Append(nv2a_make_point_vertex(v,float2(1.0,1.0),"
            "float2(1.0,0.0)));\n"
            "    stream.Append(nv2a_make_point_vertex(v,float2(-1.0,-1.0),"
            "float2(0.0,1.0)));\n"
            "    stream.Append(nv2a_make_point_vertex(v,float2(1.0,-1.0),"
            "float2(1.0,1.0)));\n"
            "    stream.RestartStrip(); }\n");
    } else {
        g_string_append(source, "    stream.Append(v); }\n"
                        "  stream.RestartStrip();\n");
    }
    g_string_append(source, "}\n");
    return g_string_free(source, FALSE);
}

#define D3D11_PS_REGISTER_MASK 0x0f
#define D3D11_PS_INPUT_MAPPING_MASK 0xe0

static uint32_t d3d11_ps_input_byte(uint32_t word, unsigned int slot)
{
    return (word >> ((3 - slot) * 8)) & 0xff;
}

static const char *d3d11_ps_register_name(uint32_t index)
{
    switch (index & D3D11_PS_REGISTER_MASK) {
    case PS_REGISTER_C0:
        return "C0";
    case PS_REGISTER_C1:
        return "C1";
    case PS_REGISTER_FOG:
        return "FOG";
    case PS_REGISTER_V0:
        return "V0";
    case PS_REGISTER_V1:
        return "V1";
    case PS_REGISTER_T0:
        return "T0";
    case PS_REGISTER_T1:
        return "T1";
    case PS_REGISTER_T2:
        return "T2";
    case PS_REGISTER_T3:
        return "T3";
    case PS_REGISTER_R0:
        return "R0";
    case PS_REGISTER_R1:
        return "R1";
    case PS_REGISTER_V1R0_SUM:
        return "V1R0_SUM";
    case PS_REGISTER_EF_PROD:
        return "EF_PROD";
    default:
        return NULL;
    }
}

static void d3d11_ps_append_input(GString *s, uint32_t input, bool alpha)
{
    uint32_t index = input & D3D11_PS_REGISTER_MASK;
    uint32_t mapping = input & D3D11_PS_INPUT_MAPPING_MASK;
    bool alpha_channel = input & PS_CHANNEL_ALPHA;
    const char *name = d3d11_ps_register_name(index);
    GString *value = g_string_new(NULL);
    if (!name || index == PS_REGISTER_ZERO) {
        g_string_append(value, alpha ? "0.0" : "float3(0,0,0)");
    } else if (alpha) {
        g_string_append_printf(value, "%s.%c", name, alpha_channel ? 'a' : 'b');
    } else if (alpha_channel) {
        g_string_append_printf(value, "%s.aaa", name);
    } else {
        g_string_append_printf(value, "%s.rgb", name);
    }
    const char *zero = alpha ? "0.0" : "float3(0,0,0)";
    const char *one = alpha ? "1.0" : "float3(1,1,1)";
    switch (mapping) {
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        g_string_append_printf(s, "max(%s,%s)", zero, value->str);
        break;
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        g_string_append_printf(s, "(%s-saturate(%s))", one, value->str);
        break;
    case PS_INPUTMAPPING_EXPAND_NORMAL:
        g_string_append_printf(s, "(2*max(%s,%s)-%s)", zero, value->str, one);
        break;
    case PS_INPUTMAPPING_EXPAND_NEGATE:
        g_string_append_printf(s, "(%s-2*max(%s,%s))", one, zero, value->str);
        break;
    case PS_INPUTMAPPING_HALFBIAS_NORMAL:
        g_string_append_printf(s, "(max(%s,%s)-0.5)", zero, value->str);
        break;
    case PS_INPUTMAPPING_HALFBIAS_NEGATE:
        g_string_append_printf(s, "(0.5-max(%s,%s))", zero, value->str);
        break;
    case PS_INPUTMAPPING_SIGNED_IDENTITY:
        g_string_append(s, value->str);
        break;
    case PS_INPUTMAPPING_SIGNED_NEGATE:
        g_string_append_printf(s, "(-%s)", value->str);
        break;
    }
    g_string_free(value, TRUE);
}

static void d3d11_ps_append_mapped_result(GString *s, const char *value,
                                          uint32_t flags)
{
    float scale = 1.0f;
    switch ((flags >> 4) & 3) {
    case 1:
        scale = 2.0f;
        break;
    case 2:
        scale = 4.0f;
        break;
    case 3:
        scale = 0.5f;
        break;
    }
    if (flags & PS_COMBINEROUTPUT_BIAS) {
        g_string_append_printf(s, "clamp(((%s)-0.5)*%.1f,-1.0,1.0)", value,
                               scale);
    } else {
        g_string_append_printf(s, "clamp((%s)*%.1f,-1.0,1.0)", value, scale);
    }
}

static void d3d11_ps_append_rgb_write(GString *s, uint32_t destination,
                                      const char *value, uint32_t flags,
                                      bool blue_to_alpha)
{
    const char *name = d3d11_ps_register_name(destination);
    if (!name || destination == PS_REGISTER_DISCARD) {
        return;
    }
    g_string_append_printf(s, "  %s.rgb=", name);
    d3d11_ps_append_mapped_result(s, value, flags);
    g_string_append(s, ";\n");
    if (blue_to_alpha) {
        g_string_append_printf(s, "  %s.a=%s.b;\n", name, name);
    }
}

static void d3d11_ps_append_alpha_write(GString *s, uint32_t destination,
                                        const char *value, uint32_t flags)
{
    const char *name = d3d11_ps_register_name(destination);
    if (!name || destination == PS_REGISTER_DISCARD) {
        return;
    }
    g_string_append_printf(s, "  %s.a=", name);
    d3d11_ps_append_mapped_result(s, value, flags);
    g_string_append(s, ";\n");
}

static bool d3d11_ps_append_stage(GString *s, const PshState *state,
                                  unsigned int stage, Error **errp)
{
    uint32_t rgb_input = state->rgb_inputs[stage];
    uint32_t alpha_input = state->alpha_inputs[stage];
    uint32_t rgb_output = state->rgb_outputs[stage];
    uint32_t alpha_output = state->alpha_outputs[stage];
    uint32_t rgb_flags = rgb_output >> 12;
    uint32_t alpha_flags = alpha_output >> 12;
    bool unique_c0 =
        (state->combiner_control >> 8) & PS_COMBINERCOUNT_UNIQUE_C0;
    bool unique_c1 =
        (state->combiner_control >> 8) & PS_COMBINERCOUNT_UNIQUE_C1;
    g_string_append_printf(s, "  // Register combiner stage %u\n", stage);
    g_string_append_printf(s, "  C0=K[%u]; C1=K[%u];\n",
                           unique_c0 ? stage * 2 : 0,
                           unique_c1 ? stage * 2 + 1 : 1);
    const char *prefixes[] = { "rgbA", "rgbB", "rgbC", "rgbD" };
    for (unsigned int i = 0; i < 4; i++) {
        g_string_append_printf(s, "  float3 %s_%u=", prefixes[i], stage);
        d3d11_ps_append_input(s, d3d11_ps_input_byte(rgb_input, i), false);
        g_string_append(s, ";\n");
    }
    const char *alpha_prefixes[] = { "aA", "aB", "aC", "aD" };
    for (unsigned int i = 0; i < 4; i++) {
        g_string_append_printf(s, "  float %s_%u=", alpha_prefixes[i], stage);
        d3d11_ps_append_input(s, d3d11_ps_input_byte(alpha_input, i), true);
        g_string_append(s, ";\n");
    }
    if (rgb_flags & PS_COMBINEROUTPUT_AB_DOT_PRODUCT) {
        g_string_append_printf(s,
                               "  float3 rgbAB_%u=dot(rgbA_%u,rgbB_%u).xxx;\n",
                               stage, stage, stage);
    } else {
        g_string_append_printf(s,
                               "  float3 rgbAB_%u=nv_mul3(rgbA_%u,rgbB_%u);\n",
                               stage, stage, stage);
    }
    if (rgb_flags & PS_COMBINEROUTPUT_CD_DOT_PRODUCT) {
        g_string_append_printf(s,
                               "  float3 rgbCD_%u=dot(rgbC_%u,rgbD_%u).xxx;\n",
                               stage, stage, stage);
    } else {
        g_string_append_printf(s,
                               "  float3 rgbCD_%u=nv_mul3(rgbC_%u,rgbD_%u);\n",
                               stage, stage, stage);
    }
    g_string_append_printf(s, "  float aAB_%u=nv_mul1(aA_%u,aB_%u);\n", stage,
                           stage, stage);
    g_string_append_printf(s, "  float aCD_%u=nv_mul1(aC_%u,aD_%u);\n", stage,
                           stage, stage);
    bool mux_msb = (state->combiner_control >> 8) & PS_COMBINERCOUNT_MUX_MSB;
    g_string_append_printf(
        s,
        mux_msb ? "  bool mux_%u=(R0.a>=0.5);\n" :
                  "  bool mux_%u=(((uint)(saturate(R0.a)*255)&1)!=0);\n",
        stage);
    if (!(rgb_flags & (PS_COMBINEROUTPUT_AB_DOT_PRODUCT |
                       PS_COMBINEROUTPUT_CD_DOT_PRODUCT))) {
        g_string_append_printf(
            s,
            (rgb_flags & PS_COMBINEROUTPUT_AB_CD_MUX) ?
                "  float3 rgbSUM_%u=mux_%u?rgbCD_%u:rgbAB_%u;\n" :
                "  float3 rgbSUM_%u=rgbAB_%u+rgbCD_%u;\n",
            stage, stage, stage, stage);
    }
    g_string_append_printf(s,
                           (alpha_flags & PS_COMBINEROUTPUT_AB_CD_MUX) ?
                               "  float aSUM_%u=mux_%u?aCD_%u:aAB_%u;\n" :
                               "  float aSUM_%u=aAB_%u+aCD_%u;\n",
                           stage, stage, stage, stage);

    char value[32];
    snprintf(value, sizeof(value), "rgbAB_%u", stage);
    d3d11_ps_append_rgb_write(s, (rgb_output >> 4) & D3D11_PS_REGISTER_MASK,
                              value, rgb_flags,
                              rgb_flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA);
    snprintf(value, sizeof(value), "rgbCD_%u", stage);
    d3d11_ps_append_rgb_write(s, rgb_output & D3D11_PS_REGISTER_MASK, value,
                              rgb_flags,
                              rgb_flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA);
    if (!(rgb_flags & (PS_COMBINEROUTPUT_AB_DOT_PRODUCT |
                       PS_COMBINEROUTPUT_CD_DOT_PRODUCT))) {
        snprintf(value, sizeof(value), "rgbSUM_%u", stage);
        d3d11_ps_append_rgb_write(s, (rgb_output >> 8) & D3D11_PS_REGISTER_MASK,
                                  value, rgb_flags, false);
    }
    snprintf(value, sizeof(value), "aAB_%u", stage);
    d3d11_ps_append_alpha_write(s, (alpha_output >> 4) & D3D11_PS_REGISTER_MASK,
                                value, alpha_flags);
    snprintf(value, sizeof(value), "aCD_%u", stage);
    d3d11_ps_append_alpha_write(s, alpha_output & D3D11_PS_REGISTER_MASK, value,
                                alpha_flags);
    snprintf(value, sizeof(value), "aSUM_%u", stage);
    d3d11_ps_append_alpha_write(s, (alpha_output >> 8) & D3D11_PS_REGISTER_MASK,
                                value, alpha_flags);
    (void)errp;
    return true;
}

static void d3d11_ps_append_final_input(GString *s, uint32_t input,
                                       bool alpha, bool allow_ef_prod)
{
    if (!allow_ef_prod &&
        (input & D3D11_PS_REGISTER_MASK) == PS_REGISTER_EF_PROD) {
        input &= ~D3D11_PS_REGISTER_MASK;
    }
    d3d11_ps_append_input(s, input, alpha);
}

static bool d3d11_ps_append_alpha_test(GString *s, const PshState *state,
                                       Error **errp)
{
    if (!state->alpha_test || state->alpha_func == ALPHA_FUNC_ALWAYS) {
        return true;
    }
    const char *condition;
    switch (state->alpha_func) {
    case ALPHA_FUNC_NEVER:
        condition = "true";
        break;
    case ALPHA_FUNC_LESS:
        condition = "resultAlpha>=alphaRef";
        break;
    case ALPHA_FUNC_EQUAL:
        condition = "resultAlpha!=alphaRef";
        break;
    case ALPHA_FUNC_LEQUAL:
        condition = "resultAlpha>alphaRef";
        break;
    case ALPHA_FUNC_GREATER:
        condition = "resultAlpha<=alphaRef";
        break;
    case ALPHA_FUNC_NOTEQUAL:
        condition = "resultAlpha==alphaRef";
        break;
    case ALPHA_FUNC_GEQUAL:
        condition = "resultAlpha<alphaRef";
        break;
    default:
        error_setg(errp, "D3D11: invalid NV2A alpha function %u",
                   state->alpha_func);
        return false;
    }
    g_string_append(s, "  int resultAlpha=(int)round(result.a*255.0);\n");
    g_string_append_printf(s, "  if(%s) discard;\n", condition);
    return true;
}

static void d3d11_ps_append_border_adjustment(GString *s, const PshState *state,
                                              unsigned int stage,
                                              const char *coordinate)
{
    if (state->tex_cubemap[stage] ||
        state->border_logical_size[stage][0] == 0.0f) {
        return;
    }
    if (state->dim_tex[stage] == 1) {
        g_string_append_printf(
            s, "  %s.x=(%s.x*%.9g+4)*%.9g;\n", coordinate, coordinate,
            state->border_logical_size[stage][0],
            state->border_inv_real_size[stage][0]);
        return;
    }
    g_string_append_printf(
        s,
        "  %s=(%s*float3(%.9g,%.9g,%.9g)+float3(4,4,4))*"
        "float3(%.9g,%.9g,%.9g);\n",
        coordinate, coordinate, state->border_logical_size[stage][0],
        state->border_logical_size[stage][1],
        state->border_logical_size[stage][2],
        state->border_inv_real_size[stage][0],
        state->border_inv_real_size[stage][1],
        state->border_inv_real_size[stage][2]);
}

static bool d3d11_ps_append_convolution_filter(GString *s,
                                               const PshState *state,
                                               unsigned int stage,
                                               Error **errp)
{
    if (state->conv_tex[stage] != CONVOLUTION_FILTER_GAUSSIAN &&
        state->conv_tex[stage] != CONVOLUTION_FILTER_QUINCUNX) {
        error_setg(errp, "D3D11: unsupported NV2A convolution kernel %u",
                   state->conv_tex[stage]);
        return false;
    }
    if (state->dim_tex[stage] != 2 || state->tex_cubemap[stage]) {
        error_setg(errp,
                   "D3D11: PROJECT2D convolution requires a non-cubemap 2D "
                   "texture");
        return false;
    }

    char border_coord[32];
    g_snprintf(border_coord, sizeof(border_coord), "pT%u.xyz", stage);
    d3d11_ps_append_border_adjustment(s, state, stage, border_coord);

    g_string_append_printf(
        s,
        "  uint convolutionWidth%u,convolutionHeight%u;\n"
        "  tex%u.GetDimensions(convolutionWidth%u,convolutionHeight%u);\n"
        "  float2 convolutionTexel%u=1.0/float2(convolutionWidth%u,"
        "convolutionHeight%u);\n"
        "  float2 convolutionBase%u=pT%u.xy*texCoordScale[%u].xy;\n"
        "  T%u=float4(0,0,0,0);\n",
        stage, stage, stage, stage, stage, stage, stage, stage, stage, stage,
        stage, stage);

    static const unsigned int weights[3][3] = {
        { 1, 2, 1 }, { 2, 4, 2 }, { 1, 2, 1 },
    };
    for (unsigned int y = 0; y < 3; y++) {
        for (unsigned int x = 0; x < 3; x++) {
            g_string_append_printf(
                s,
                "  T%u+=NV2A_SAMPLE2(tex%u,samp%u,%u,(convolutionBase%u+"
                "float2(%.1f,%.1f)*convolutionTexel%u)/pT%u.w)*(%u.0/16.0);\n",
                stage, stage, stage, stage, stage, (double)x - 1.0,
                (double)y - 1.0, stage, stage, weights[y][x]);
        }
    }
    return true;
}

static void d3d11_ps_append_window_clip(GString *s, const PshState *state)
{
    g_string_append(s, "  float2 windowCoord=input.position.xy-0.5;\n");
    if (state->window_clip_exclusive) {
        for (unsigned int i = 0; i < 8; i++) {
            g_string_append_printf(
                s,
                "  if(all(windowCoord>=float2(clipRegion[%u].xy)) && "
                "all(windowCoord<float2(clipRegion[%u].zw))) discard;\n",
                i, i);
        }
    } else {
        g_string_append(s, "  bool windowClipContained=false;\n");
        for (unsigned int i = 0; i < 8; i++) {
            g_string_append_printf(
                s,
                "  windowClipContained=windowClipContained || "
                "(all(windowCoord>=float2(clipRegion[%u].xy)) && "
                "all(windowCoord<float2(clipRegion[%u].zw)));\n",
                i, i);
        }
        g_string_append(s, "  if(!windowClipContained) discard;\n");
    }
}

static void d3d11_ps_append_custom_depth(GString *s, const PshState *state)
{
    g_string_append(
        s,
        "  float2 unscaledXY=input.position.xy/surfaceScale;\n"
        "  precise float bc0=nv2a_area(unscaledXY,input.depthPos1.xy,"
        "input.depthPos2.xy);\n"
        "  precise float bc1=nv2a_area(unscaledXY,input.depthPos2.xy,"
        "input.depthPos0.xy);\n"
        "  precise float bc2=nv2a_area(unscaledXY,input.depthPos0.xy,"
        "input.depthPos1.xy);\n");
    if (state->z_perspective) {
        g_string_append(
            s,
            "  bc0/=input.depthPos0.w; bc1/=input.depthPos1.w;\n"
            "  bc2/=input.depthPos2.w;\n"
            "  float invBcSum=1.0/(bc0+bc1+bc2);\n"
            "  if(isinf(invBcSum)) invBcSum=0.0;\n"
            "  bc1*=invBcSum; bc2*=invBcSum;\n"
            "  precise float zvalue=input.depthPos0.w+"
            "(bc1*(input.depthPos1.w-input.depthPos0.w)+"
            "bc2*(input.depthPos2.w-input.depthPos0.w));\n"
            "  if(zvalue>0.0){\n"
            "    float zslopeofs=depthFactor*input.triMzPerspective*"
            "zvalue*zvalue;\n"
            "    zvalue+=depthOffset; zvalue+=zslopeofs;\n"
            "  }else{ zvalue=asfloat(0x7f7fffff); }\n"
            "  if(isnan(zvalue)) zvalue=asfloat(0x7f7fffff);\n");
    } else {
        g_string_append(
            s,
            "  float invBcSum=1.0/(bc0+bc1+bc2);\n"
            "  if(isinf(invBcSum)) invBcSum=0.0;\n"
            "  bc1*=invBcSum; bc2*=invBcSum;\n"
            "  precise float zvalue=input.depthPos0.z+"
            "(bc1*(input.depthPos1.z-input.depthPos0.z)+"
            "bc2*(input.depthPos2.z-input.depthPos0.z));\n"
            "  zvalue+=depthOffset; zvalue+=depthFactor*input.triMzAffine;\n");
    }
    if (state->depth_clipping) {
        g_string_append(s,
                        "  if(zvalue<clipRange.z||clipRange.w<zvalue) discard;\n");
    } else {
        g_string_append(s, "  zvalue=clamp(zvalue,clipRange.z,clipRange.w);\n");
    }
    if (state->depth_format == DEPTH_FORMAT_D16) {
        g_string_append(s, "  float depthResult=floor(zvalue)/65535.0;\n");
    } else if (state->depth_format == DEPTH_FORMAT_D24) {
        g_string_append(
            s,
            "  float depthResult=asfloat(asuint(floor(zvalue)/16777216.0)+1u);\n");
    } else {
        g_string_append(s, "  float depthResult=zvalue/clipRange.y;\n");
    }
    g_string_append(s,
                    "  PSOut output; output.color=result; "
                    "output.depth=depthResult; return output;\n");
}

static bool d3d11_ps_append_shadow_stage(GString *s, const PshState *state,
                                         unsigned int stage, unsigned int mode,
                                         Error **errp)
{
    /* Match the GLSL path: filter the sampled depth, then compare once. This
     * is not compare-before-filter PCF; do not claim hardware PCF parity here.
     */
    bool project2d = mode == PS_TEXTUREMODES_PROJECT2D;
    bool supported_dimension =
        project2d ? (state->dim_tex[stage] == 2 || state->dim_tex[stage] == 3) :
                    (mode == PS_TEXTUREMODES_PROJECT3D &&
                     state->dim_tex[stage] == 2);
    if (!supported_dimension || state->tex_cubemap[stage] ||
        state->shadow_depth_func > SHADOW_DEPTH_FUNC_ALWAYS) {
        error_setg(errp,
                   "D3D11: unsupported shadow-map mode/dimension/function "
                   "at stage %u",
                   stage);
        return false;
    }

    if (state->shadow_depth_func == SHADOW_DEPTH_FUNC_NEVER ||
        state->shadow_depth_func == SHADOW_DEPTH_FUNC_ALWAYS) {
        unsigned int value = state->shadow_depth_func == SHADOW_DEPTH_FUNC_ALWAYS;
        g_string_append_printf(s, "  T%u=float4(%u,%u,%u,%u);\n", stage,
                               value, value, value, value);
        return true;
    }

    const char *comparison;
    switch (state->shadow_depth_func) {
    case SHADOW_DEPTH_FUNC_LESS: comparison = "<"; break;
    case SHADOW_DEPTH_FUNC_EQUAL: comparison = "=="; break;
    case SHADOW_DEPTH_FUNC_LEQUAL: comparison = "<="; break;
    case SHADOW_DEPTH_FUNC_GREATER: comparison = ">"; break;
    case SHADOW_DEPTH_FUNC_NOTEQUAL: comparison = "!="; break;
    case SHADOW_DEPTH_FUNC_GEQUAL: comparison = ">="; break;
    default:
        error_setg(errp, "D3D11: invalid shadow-map comparison at stage %u",
                   stage);
        return false;
    }

    if (project2d) {
        char sample[24], depth[24], result[24];
        snprintf(sample, sizeof(sample), "shadowSample%u", stage);
        snprintf(depth, sizeof(depth), "shadowDepth%u", stage);
        snprintf(result, sizeof(result), "shadowResult%u", stage);
        if (state->dim_tex[stage] == 3) {
            g_string_append_printf(
                s,
                "  float4 %s=NV2A_SAMPLE3(tex%u,samp%u,%u,float3(pT%u.xy/pT%u.w*"
                "texCoordScale[%u].xy,0));\n"
                "  float %s=%s.r+%s.g+%s.b;\n",
                sample, stage, stage, stage, stage, stage, stage, depth, sample,
                sample, sample);
        } else {
            g_string_append_printf(
                s,
                "  float4 %s=NV2A_SAMPLE2(tex%u,samp%u,%u,pT%u.xy/pT%u.w*"
                "texCoordScale[%u].xy);\n"
                "  float %s=%s.r+%s.g+%s.b;\n",
                sample, stage, stage, stage, stage, stage, stage, depth, sample,
                sample, sample);
        }
        g_string_append_printf(s, "  float %s=%s %s 0.0 ? 1.0 : 0.0;\n",
                               result, depth, comparison);
        g_string_append_printf(
            s, "  T%u=float4(%s,%s,%s,%s);\n", stage, result, result,
            result, result);
        return true;
    }

    char sample[24], max_depth[24], map_depth[24], reference[24], result[24];
    snprintf(sample, sizeof(sample), "shadowSample%u", stage);
    snprintf(max_depth, sizeof(max_depth), "shadowMaxDepth%u", stage);
    snprintf(map_depth, sizeof(map_depth), "shadowMapDepth%u", stage);
    snprintf(reference, sizeof(reference), "shadowReference%u", stage);
    snprintf(result, sizeof(result), "shadowResult%u", stage);
    const char *max_depth_value =
        state->tex_x8y24[stage] ? "16777215.0" :
        state->tex_depth_float[stage] ? "511.9375" : "65535.0";
    /* UNORM filtering is component-wise linear, so decoding after Sample
     * reconstructs the same weighted normalized depth (up to float rounding). */
    char depth_decode[160];
    if (state->tex_x8y24[stage]) {
        g_snprintf(depth_decode, sizeof(depth_decode),
                   "((%s.r*16711680.0+%s.g*65280.0+%s.b*255.0)"
                   "/16777215.0)",
                   sample, sample, sample);
    } else {
        g_snprintf(depth_decode, sizeof(depth_decode),
                   "((%s.r*65280.0+%s.g*255.0)/65535.0)", sample, sample);
    }
    g_string_append_printf(
        s,
        "  float4 %s=NV2A_SAMPLE2(tex%u,samp%u,%u,pT%u.xy/pT%u.w*"
        "texCoordScale[%u].xy);\n"
        "  float %s=%s;\n"
        "  float %s=%s*%s;\n"
        "  float %s=clamp(pT%u.z/pT%u.w,0.0,%s);\n"
        "  float %s=%s %s %s ? 1.0 : 0.0;\n"
        "  T%u=float4(%s,%s,%s,%s);\n",
        sample, stage, stage, stage, stage, stage, stage, max_depth, max_depth_value,
        map_depth, depth_decode, max_depth, reference, stage, stage, max_depth,
        result, map_depth, comparison, reference, stage, result, result, result,
        result);
    return true;
}

static bool d3d11_ps_append_texture_postprocess(GString *s,
                                                const PshState *state,
                                                unsigned int stage,
                                                bool samples_texture,
                                                Error **errp)
{
    if (!samples_texture) {
        return true;
    }
    if (state->alphakill[stage]) {
        g_string_append_printf(s, "  if(T%u.a==0) discard;\n", stage);
    }
    switch (state->colorkey_mode[stage]) {
    case COLOR_KEY_NONE:
        break;
    case COLOR_KEY_DISCARD:
        g_string_append_printf(
            s, "  if(nv_color_key_match(T%u,colorKey[%u],colorKeyMask[%u])) "
               "discard;\n",
            stage, stage, stage);
        break;
    case COLOR_KEY_KILL_ALPHA:
        g_string_append_printf(
            s, "  if(nv_color_key_match(T%u,colorKey[%u],colorKeyMask[%u])) "
               "T%u.a=0;\n",
            stage, stage, stage, stage);
        break;
    case COLOR_KEY_KILL_COLOR_AND_ALPHA:
        g_string_append_printf(
            s, "  if(nv_color_key_match(T%u,colorKey[%u],colorKeyMask[%u])) "
               "T%u=float4(0,0,0,0);\n",
            stage, stage, stage, stage);
        break;
    default:
        error_setg(errp, "D3D11: invalid color-key mode %d at stage %u",
                   state->colorkey_mode[stage], stage);
        return false;
    }
    return true;
}

static void d3d11_ps_append_clamp_ogl_helpers(GString *s,
                                             const PshState *state)
{
    bool cube_border = false;
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        cube_border |= state->tex_cubemap[i] &&
                       state->border_logical_size[i][0] > 0;
    }
    if (cube_border) {
        /* Keep the native cube face orientation, but address the logical
         * rectangle inside each physical face. Derivatives must be taken
         * after this inset so LOD follows logical, not padded dimensions.
         */
        g_string_append(s,
            "float3 nv2a_cube_border(float3 c,uint stage){\n"
            "  float2 logical=0,invSize=0;\n");
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (state->tex_cubemap[i] && state->border_logical_size[i][0] > 0) {
                g_string_append_printf(s,
                    "  if(stage==%u){logical=float2(%.9f,%.9f);"
                    "invSize=float2(%.9f,%.9f);}\n", i,
                    state->border_logical_size[i][0],
                    state->border_logical_size[i][1],
                    state->border_inv_real_size[i][0],
                    state->border_inv_real_size[i][1]);
            }
        }
        g_string_append(s,
            "  if(logical.x==0)return c;\n"
            "  float3 a=abs(c);float2 uv;float major;uint face;\n"
            "  if(a.x>=a.y&&a.x>=a.z){major=a.x;"
            "face=c.x>=0?0:1;uv=float2(c.x>=0?-c.z:c.z,-c.y);}\n"
            "  else if(a.y>=a.z){major=a.y;face=c.y>=0?2:3;"
            "uv=float2(c.x,c.y>=0?c.z:-c.z);}\n"
            "  else{major=a.z;face=c.z>=0?4:5;"
            "uv=float2(c.z>=0?c.x:-c.x,-c.y);}\n"
            "  if(major==0)return c;\n"
            "  uv=(((uv/major)*0.5+0.5)*logical+4.0)*invSize*2.0-1.0;\n"
            "  if(face==0)return float3(1,-uv.y,-uv.x)*major;\n"
            "  if(face==1)return float3(-1,-uv.y,uv.x)*major;\n"
            "  if(face==2)return float3(uv.x,1,uv.y)*major;\n"
            "  if(face==3)return float3(uv.x,-1,-uv.y)*major;\n"
            "  if(face==4)return float3(uv.x,-uv.y,1)*major;\n"
            "  return float3(-uv.x,-uv.y,-1)*major;}\n");
    }
    static const char axes[] = "xyz";
    for (unsigned int dimensions = 2; dimensions <= 3; dimensions++) {
        g_string_append_printf(
            s, "float%u nv2a_clamp_ogl%u(float%u coord,uint stage){\n",
            dimensions, dimensions, dimensions);
        for (unsigned int stage = 0; stage < NV2A_MAX_TEXTURES; stage++) {
            bool has_clamp = false;
            for (unsigned int axis = 0; axis < dimensions; axis++) {
                has_clamp |= state->clamp_ogl[stage][axis];
            }
            if (!has_clamp) {
                continue;
            }
            g_string_append_printf(s, "  if(stage==%u){", stage);
            for (unsigned int axis = 0; axis < dimensions; axis++) {
                if (state->clamp_ogl[stage][axis]) {
                    g_string_append_printf(
                        s, "coord.%c=clamp(coord.%c,0.0,1.0);", axes[axis],
                        axes[axis]);
                }
            }
            g_string_append(s, "}\n");
        }
        g_string_append(s, "  return coord;}\n");
    }
    g_string_append(
        s,
        "#define NV2A_SAMPLE2(TEX,SAMP,STAGE,COORD) "
        "TEX.SampleGrad(SAMP,nv2a_clamp_ogl2(COORD,STAGE),"
        "ddx(COORD),ddy(COORD))\n");
    if (cube_border) {
        g_string_append(s,
            "#define NV2A_SAMPLE3(TEX,SAMP,STAGE,COORD) "
            "TEX.SampleGrad(SAMP,nv2a_clamp_ogl3(nv2a_cube_border(COORD,STAGE),STAGE),"
            "ddx(nv2a_cube_border(COORD,STAGE)),"
            "ddy(nv2a_cube_border(COORD,STAGE)))\n");
    } else {
        g_string_append(s,
            "#define NV2A_SAMPLE3(TEX,SAMP,STAGE,COORD) "
            "TEX.SampleGrad(SAMP,nv2a_clamp_ogl3(COORD,STAGE),"
            "ddx(COORD),ddy(COORD))\n");
    }
}

static char *d3d11_translate_pixel_combiners_with_depth(
    const PshState *state, bool custom_depth, Error **errp)
{
    unsigned int stages = state->combiner_control & 0xff;
    if (stages > 8) {
        error_setg(errp, "D3D11: invalid NV2A combiner count %u", stages);
        return NULL;
    }
    GString *s =
        g_string_new(state->smooth_shading ?
                         "#define NV2A_COLOR_INTERPOLATION\n" :
                         "#define NV2A_COLOR_INTERPOLATION nointerpolation\n");
    g_string_append(s, d3d11_psh_preamble);
    d3d11_ps_append_clamp_ogl_helpers(s, state);
    if (custom_depth) {
        g_string_append(
            s,
            "float nv2a_area(float2 p,float2 a,float2 b){\n"
            "  precise float2 d0=a-p; precise float2 d1=b-p;\n"
            "  precise float cd=d0.y*d1.x;\n"
            "  precise float err=mad(-d0.y,d1.x,cd);\n"
            "  precise float value=mad(d0.x,d1.y,-cd)+err; return value;\n"
            "}\n"
            "struct PSOut{float4 color:SV_Target0;float depth:SV_Depth;};\n");
    }
    g_string_append(
        s,
        "float3 nv_dotmap(float4 c,uint mode){float3 x=c.rgb*255.0;"
        "if(mode==1)return (x-128.0)/127.0;"
        "if(mode==2)return float3(x.x>=128?(x.x-255.5)/127.5:(x.x+0.5)/127.5,"
        "x.y>=128?(x.y-255.5)/127.5:(x.y+0.5)/127.5,"
        "x.z>=128?(x.z-255.5)/127.5:(x.z+0.5)/127.5);"
        "if(mode==3)return float3(x.x>=128?x.x-256:x.x,"
        "x.y>=128?x.y-256:x.y,x.z>=128?x.z-256:x.z)/127.0;"
        "if(mode==4){uint4 b=(uint4)(c*255.0);"
        "return float3((b.a*256+b.r)/65535.0,(b.g*256+b.b)/65535.0,1);}"
        "/* modes 5-7 match the current GLSL fallback, not hardware HILO */"
        "return c.rgb;}\n");
    g_string_append(
        s,
        "float2 nv_remap_cube_to_2d(float3 c){float3 a=abs(c);float2 uv;"
        "if(a.x>a.y&&a.x>a.z){uv=c.x>0?float2(-c.z,c.y):float2(c.z,c.y);"
        "uv/=a.x;}else if(a.y>a.x&&a.y>a.z){"
        "uv=c.y>0?float2(c.x,-c.z):float2(c.x,c.z);uv/=a.y;}"
        "else{uv=c.z>0?float2(c.x,c.y):float2(-c.x,c.y);uv/=a.z;}"
        "return uv;}\n");
    g_string_append(
        s,
        "float3 nv_remap_2d_to_cube(float3 c){float2 st=c.xy/c.z;"
        "return normalize(float3(1,st.y,-st.x));}\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = (state->shader_stage_program >> (i * 5)) & 0x1f;
        bool cube_sampler = state->tex_cubemap[i] &&
                            ((mode == PS_TEXTUREMODES_PROJECT2D &&
                              state->dim_tex[i] == 2) ||
                             mode == PS_TEXTUREMODES_CUBEMAP ||
                             mode == PS_TEXTUREMODES_DOT_RFLCT_DIFF ||
                             mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC ||
                             mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST ||
                             mode == PS_TEXTUREMODES_DOT_STR_CUBE);
        if (cube_sampler) {
            g_string_append_printf(s, "TextureCube tex%u:register(t%u);\n", i,
                                   i);
        } else if (state->dim_tex[i] == 3) {
            g_string_append_printf(s, "Texture3D tex%u:register(t%u);\n", i, i);
        } else {
            g_string_append_printf(s, "Texture2D tex%u:register(t%u);\n", i, i);
        }
        g_string_append_printf(s, "SamplerState samp%u:register(s%u);\n", i, i);
    }
    g_string_append(s, custom_depth ? "PSOut main(PSIn input){\n" :
                                      "float4 main(PSIn input):SV_Target {\n");
    d3d11_ps_append_window_clip(s, state);
    g_string_append(
        s, "  float4 V0=input.frontFace?input.diffuse:input.backDiffuse;\n"
           "  float4 V1=input.frontFace?input.specular:input.backSpecular;\n"
           "  float4 pT0=input.tex0,pT1=input.tex1,pT2=input.tex2;\n");
    if (state->point_sprite) {
        g_string_append(s,
                        "  float4 pT3=float4(input.pointCoord,1.0,1.0);\n");
        if (state->rect_tex[3]) {
            /* Sprite coordinates cover the whole image; linear textures use
             * texel coordinates before the normal texture-coordinate scale.
             */
            g_string_append(s, "  pT3.xy/=texCoordScale[3].xy;\n");
        }
    } else {
        g_string_append(s, "  float4 pT3=input.tex3;\n");
    }
    g_string_append(
        s, "  float4 T0=pT0,T1=pT1,T2=pT2,T3=pT3;\n"
           "  float4 R0=0,R1=0;\n"
           "  float dots[4]={0,0,0,0};\n"
           "  float4 C0=K[0],C1=K[1];\n"
           "  float4 FOG=float4(fogColor.rgb,saturate(input.fog));\n"
           "  float4 V1R0_SUM=0,EF_PROD=0;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = (state->shader_stage_program >> (i * 5)) & 0x1f;
        if (mode == PS_TEXTUREMODES_NONE) {
            g_string_append_printf(s, "  T%u=float4(0,0,0,1);\n", i);
            continue;
        }
        if (mode == PS_TEXTUREMODES_PASSTHRU) {
            continue;
        }
        if (state->shadow_map[i]) {
            if (!d3d11_ps_append_shadow_stage(s, state, i, mode, errp)) {
                g_string_free(s, TRUE);
                return NULL;
            }
            if (!d3d11_ps_append_texture_postprocess(s, state, i, true,
                                                    errp)) {
                g_string_free(s, TRUE);
                return NULL;
            }
            continue;
        }
        bool samples_texture = mode != PS_TEXTUREMODES_CLIPPLANE &&
                               mode != PS_TEXTUREMODES_DOTPRODUCT &&
                               mode != PS_TEXTUREMODES_DOT_ZW &&
                               mode != PS_TEXTUREMODES_BRDF;
        unsigned int input =
            i < 2 ? 0 : (state->other_stage_input >> (i == 2 ? 16 : 20)) & 0xf;
        unsigned int mapping =
            i ? (state->other_stage_input >> ((i - 1) * 4)) & 0xf : 0;
        bool uses_dot = mode == PS_TEXTUREMODES_DOTPRODUCT ||
                        mode == PS_TEXTUREMODES_DOT_ST ||
                        mode == PS_TEXTUREMODES_DOT_ZW ||
                        mode == PS_TEXTUREMODES_DOT_STR_3D ||
                        mode == PS_TEXTUREMODES_DOT_STR_CUBE ||
                        mode == PS_TEXTUREMODES_DOT_RFLCT_DIFF ||
                        mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC ||
                        mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST;
        if (mode >= PS_TEXTUREMODES_BUMPENVMAP &&
            (!i || input >= i || (uses_dot && mapping > 7))) {
            error_setg(errp, "D3D11: invalid/unsupported dependent texture "
                             "input or dot mapping");
            g_string_free(s, TRUE);
            return NULL;
        }
        if (mode == PS_TEXTUREMODES_PROJECT2D &&
            state->conv_tex[i] != CONVOLUTION_FILTER_DISABLED) {
            if (!d3d11_ps_append_convolution_filter(s, state, i, errp)) {
                g_string_free(s, TRUE);
                return NULL;
            }
        } else if (mode == PS_TEXTUREMODES_CLIPPLANE) {
            for (unsigned int c = 0; c < 4; c++) {
                g_string_append_printf(
                    s, "  if(pT%u.%c%s0)discard;\n", i, "xyzw"[c],
                    state -> compare_mode[i][c] ? ">=" : "<");
            }
            g_string_append_printf(s, "  T%u=0;\n", i);
            continue;
        } else if (mode == PS_TEXTUREMODES_DPNDNT_AR ||
                   mode == PS_TEXTUREMODES_DPNDNT_GB) {
            if (state->dim_tex[i] != 2 || state->tex_cubemap[i]) {
                error_setg(errp,
                           "D3D11: dependent AR/GB requires a 2D texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s, "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,T%u.%s*texCoordScale[%u].xy);\n",
                i, i, i, i, input,
                mode == PS_TEXTUREMODES_DPNDNT_AR ? "ar" : "gb", i);
        } else if (mode == PS_TEXTUREMODES_BUMPENVMAP ||
                   mode == PS_TEXTUREMODES_BUMPENVMAP_LUM) {
            g_string_append_printf(s, "  float2 bump%u=", i);
            if (state->snorm_tex[input]) {
                g_string_append_printf(s, "T%u.bg;\n", input);
            } else {
                g_string_append_printf(s, "nv_dotmap(T%u,3).bg;\n", input);
            }
            g_string_append_printf(
                s,
                "  float2 delta%u=float2(dot(bumpMat[%u].xz,bump%u),"
                "dot(bumpMat[%u].yw,bump%u));\n",
                i, i, i, i, i);
            if (state->dim_tex[i] == 3 && !state->tex_cubemap[i]) {
                g_string_append_printf(s,
                                       "  "
                                       "T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,float3(pT%u.xy+"
                                       "delta%u,pT%u.z));\n",
                                       i, i, i, i, i, i, i);
            } else if (state->dim_tex[i] == 2 && !state->tex_cubemap[i]) {
                g_string_append_printf(s,
                                       "  "
                                       "T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,(pT%u.xy+delta%"
                                       "u)*texCoordScale[%u].xy);\n",
                                       i, i, i, i, i, i, i);
            } else {
                error_setg(
                    errp,
                    "D3D11: bump environment requires a 2D or 3D texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            if (mode == PS_TEXTUREMODES_BUMPENVMAP_LUM) {
                g_string_append_printf(s, "  float lum%u=T%u.r;\n", i, input);
                if (state->snorm_tex[input]) {
                    g_string_append_printf(
                        s, "  lum%u=lum%u>=0?lum%u/2:1+lum%u/2;\n", i, i, i, i);
                }
                g_string_append_printf(
                    s, "  T%u*=bumpParams[%u].x*lum%u+bumpParams[%u].y;\n", i,
                    i, i, i);
            }
        } else if (mode == PS_TEXTUREMODES_DOTPRODUCT ||
                   mode == PS_TEXTUREMODES_DOT_ZW ||
                   mode == PS_TEXTUREMODES_DOT_ST) {
            g_string_append_printf(
                s, "  dots[%u]=dot(pT%u.xyz,nv_dotmap(T%u,%u));\n", i, i, input,
                mapping);
            if (mode == PS_TEXTUREMODES_DOT_ST && i >= 2 &&
                state->dim_tex[i] == 2) {
                g_string_append_printf(s,
                                       "  "
                                       "T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,float2(dots[%u]"
                                       ",dots[%u])*texCoordScale[%u].xy);\n",
                                       i, i, i, i, i - 1, i, i);
            } else {
                g_string_append_printf(s, "  T%u=0;\n", i);
            }
        } else if (mode == PS_TEXTUREMODES_DOT_RFLCT_DIFF) {
            unsigned int next_input =
                (state->other_stage_input >> 20) & 0xf;
            unsigned int next_mapping =
                (state->other_stage_input >> 8) & 0xf;
            unsigned int previous_mode =
                (state->shader_stage_program >> 5) & 0x1f;
            if (i != 2 || state->dim_tex[i] != 2 || next_input >= 3 ||
                previous_mode != PS_TEXTUREMODES_DOTPRODUCT ||
                next_mapping > 7) {
                error_setg(errp,
                           "D3D11: DOT_RFLCT_DIFF requires stage 2, a 2D/cube "
                           "texture, and valid dot inputs");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s,
                "  dots[2]=dot(pT2.xyz,nv_dotmap(T%u,%u));\n"
                "  float dot_diffuse_z=dot(pT3.xyz,nv_dotmap(T%u,%u));\n"
                "  float3 diffuse_vec_2=float3(dots[1],dots[2],dot_diffuse_z);\n",
                input, mapping, next_input, next_mapping);
            d3d11_ps_append_border_adjustment(s, state, i, "diffuse_vec_2");
            if (state->tex_cubemap[i]) {
                g_string_append(s,
                                "  T2=NV2A_SAMPLE3(tex2,samp2,2,diffuse_vec_2);\n");
            } else {
                g_string_append(s,
                                "  T2=NV2A_SAMPLE2(tex2,samp2,2,"
                                "nv_remap_cube_to_2d(diffuse_vec_2));\n");
            }
        } else if (mode == PS_TEXTUREMODES_BRDF) {
            g_string_append_printf(
                s, "  T%u=float4(0,0,0,0); /* xemu GLSL fallback stub */\n", i);
        } else if (mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST) {
            unsigned int previous_mode_1 =
                (state->shader_stage_program >> 5) & 0x1f;
            unsigned int previous_mode_2 =
                (state->shader_stage_program >> 10) & 0x1f;
            if (i != 3 || state->dim_tex[i] != 2 ||
                previous_mode_1 != PS_TEXTUREMODES_DOTPRODUCT ||
                previous_mode_2 != PS_TEXTUREMODES_DOTPRODUCT) {
                error_setg(errp,
                           "D3D11: DOT_RFLCT_SPEC_CONST requires stage 3, "
                           "DOTPRODUCT stages 1/2, and a 2D/cube texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s,
                "  dots[3]=dot(pT3.xyz,nv_dotmap(T%u,%u));\n"
                "  float3 reflect_n_3=float3(dots[1],dots[2],dots[3]);\n",
                input, mapping);
            g_string_append(s, "  float3 reflect_e_3=eyeVector;\n");
            g_string_append(
                s, "  float3 reflect_vec_3=2*reflect_n_3*"
                   "dot(reflect_n_3,reflect_e_3)/dot(reflect_n_3,reflect_n_3)-"
                   "reflect_e_3;\n");
            d3d11_ps_append_border_adjustment(s, state, i, "reflect_vec_3");
            if (state->tex_cubemap[i]) {
                g_string_append(s,
                                "  T3=NV2A_SAMPLE3(tex3,samp3,3,reflect_vec_3);\n");
            } else {
                g_string_append(s,
                                "  T3=NV2A_SAMPLE2(tex3,samp3,3,"
                                "nv_remap_cube_to_2d(reflect_vec_3));\n");
            }
        } else if (mode == PS_TEXTUREMODES_DOT_RFLCT_SPEC) {
            unsigned int previous_mode_1 =
                (state->shader_stage_program >> 5) & 0x1f;
            unsigned int previous_mode_2 =
                (state->shader_stage_program >> 10) & 0x1f;
            if (i != 3 || state->dim_tex[i] != 2 ||
                previous_mode_1 != PS_TEXTUREMODES_DOTPRODUCT ||
                previous_mode_2 != PS_TEXTUREMODES_DOTPRODUCT) {
                error_setg(errp,
                           "D3D11: DOT_RFLCT_SPEC requires stage 3, "
                           "DOTPRODUCT stages 1/2, and a 2D/cube texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s,
                "  dots[3]=dot(pT3.xyz,nv_dotmap(T%u,%u));\n"
                "  float3 reflect_n_3=float3(dots[1],dots[2],dots[3]);\n",
                input, mapping);
            g_string_append(
                s, "  float3 reflect_e_3=float3(pT1.w,pT2.w,pT3.w);\n");
            g_string_append(
                s, "  float3 reflect_vec_3=2*reflect_n_3*"
                   "dot(reflect_n_3,reflect_e_3)/dot(reflect_n_3,reflect_n_3)-"
                   "reflect_e_3;\n");
            d3d11_ps_append_border_adjustment(s, state, i, "reflect_vec_3");
            if (state->tex_cubemap[i]) {
                g_string_append(s,
                                "  T3=NV2A_SAMPLE3(tex3,samp3,3,reflect_vec_3);\n");
            } else {
                g_string_append(s,
                                "  T3=NV2A_SAMPLE2(tex3,samp3,3,"
                                "nv_remap_cube_to_2d(reflect_vec_3));\n");
            }
        } else if (mode == PS_TEXTUREMODES_DOT_STR_3D ||
                   mode == PS_TEXTUREMODES_DOT_STR_CUBE) {
            unsigned int previous_mode_1 =
                (state->shader_stage_program >> 5) & 0x1f;
            unsigned int previous_mode_2 =
                (state->shader_stage_program >> 10) & 0x1f;
            if (i != 3 ||
                (mode == PS_TEXTUREMODES_DOT_STR_3D &&
                 state->tex_cubemap[i]) ||
                (mode == PS_TEXTUREMODES_DOT_STR_3D &&
                 state->dim_tex[i] != 2 && state->dim_tex[i] != 3) ||
                (mode == PS_TEXTUREMODES_DOT_STR_CUBE &&
                 state->dim_tex[i] != 2) ||
                previous_mode_1 != PS_TEXTUREMODES_DOTPRODUCT ||
                previous_mode_2 != PS_TEXTUREMODES_DOTPRODUCT) {
                error_setg(errp,
                           "D3D11: DOT_STR texture mode requires stage 3 and "
                           "a compatible 2D/3D or cube texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s, "  dots[%u]=dot(pT%u.xyz,nv_dotmap(T%u,%u));\n", i, i,
                input, mapping);
            g_string_append_printf(
                s, "  float3 dotSTR_%u=float3(dots[%u],dots[%u],dots[%u]);\n",
                i, i - 2, i - 1, i);
            d3d11_ps_append_border_adjustment(s, state, i, "dotSTR_3");
            if (mode == PS_TEXTUREMODES_DOT_STR_CUBE) {
                if (state->tex_cubemap[i]) {
                    g_string_append_printf(
                        s, "  T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,dotSTR_%u);\n",
                        i, i, i, i, i);
                } else {
                    g_string_append_printf(
                        s,
                        "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,"
                        "nv_remap_cube_to_2d(dotSTR_%u));\n",
                        i, i, i, i, i);
                }
            } else if (state->dim_tex[i] == 3) {
                g_string_append_printf(
                    s, "  T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,dotSTR_%u);\n",
                    i, i, i, i, i);
            } else {
                g_string_append_printf(
                    s,
                    "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,dotSTR_%u.xy*"
                    "texCoordScale[%u].xy);\n",
                    i, i, i, i, i, i);
            }
        } else if (mode != PS_TEXTUREMODES_PROJECT2D &&
                   mode != PS_TEXTUREMODES_PROJECT3D &&
                   mode != PS_TEXTUREMODES_CUBEMAP) {
            error_setg(errp, "D3D11: texture mode 0x%x at stage %u (program 0x%x) is not implemented",
                       mode, i, state->shader_stage_program);
            g_string_free(s, TRUE);
            return NULL;
        } else if (mode == PS_TEXTUREMODES_CUBEMAP &&
                   !state->tex_cubemap[i]) {
            if (state->dim_tex[i] != 2) {
                error_setg(errp, "D3D11: CUBEMAP requires a 2D texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            g_string_append_printf(
                s,
                "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,nv_remap_cube_to_2d(pT%u.xyz));\n",
                i, i, i, i, i);
        } else if (mode == PS_TEXTUREMODES_PROJECT2D &&
                   state->tex_cubemap[i]) {
            if (state->dim_tex[i] != 2) {
                error_setg(errp,
                           "D3D11: projected cubemap requires a 2D texture");
                g_string_free(s, TRUE);
                return NULL;
            }
            char border_coord[32];
            g_snprintf(border_coord, sizeof(border_coord), "pT%u.xyz", i);
            d3d11_ps_append_border_adjustment(s, state, i, border_coord);
            g_string_append_printf(
                s,
                "  T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,nv_remap_2d_to_cube("
                "float3(pT%u.xy,pT%u.w)));\n",
                i, i, i, i, i, i);
        } else if (mode == PS_TEXTUREMODES_CUBEMAP &&
                   state->tex_cubemap[i]) {
            g_string_append_printf(
                s, "  T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,pT%u.xyz);\n", i, i,
                i, i, i);
        } else if (mode == PS_TEXTUREMODES_PROJECT2D &&
                   state->dim_tex[i] == 3) {
            char border_coord[32];
            g_snprintf(border_coord, sizeof(border_coord), "pT%u.xyz", i);
            d3d11_ps_append_border_adjustment(s, state, i, border_coord);
            g_string_append_printf(
                s,
                "  T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,float3(pT%u.xy/pT%u.w*"
                "texCoordScale[%u].xy,0));\n",
                i, i, i, i, i, i, i);
        } else if (state->dim_tex[i] == 3) {
            if (mode == PS_TEXTUREMODES_PROJECT3D) {
                char border_coord[32];
                g_snprintf(border_coord, sizeof(border_coord), "pT%u.xyz", i);
                d3d11_ps_append_border_adjustment(s, state, i, border_coord);
            }
            g_string_append_printf(s,
                                   "  "
                                   "T%u=NV2A_SAMPLE3(tex%u,samp%u,%u,pT%u.xyz/"
                                   "pT%u.w*texCoordScale[%u].xyz);\n",
                                   i, i, i, i, i, i, i);
        } else if (state->dim_tex[i] == 1 &&
                   (mode == PS_TEXTUREMODES_PROJECT2D ||
                    mode == PS_TEXTUREMODES_PROJECT3D)) {
            char border_coord[24];
            g_snprintf(border_coord, sizeof(border_coord), "pT%u", i);
            d3d11_ps_append_border_adjustment(s, state, i, border_coord);
            g_string_append_printf(
                s, "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,float2(pT%u.x/pT%u.w*"
                   "texCoordScale[%u].x,0.5));\n",
                i, i, i, i, i, i, i);
        } else if (mode == PS_TEXTUREMODES_PROJECT2D ||
                   mode == PS_TEXTUREMODES_PROJECT3D) {
            char border_coord[32];
            g_snprintf(border_coord, sizeof(border_coord), "pT%u.xyz", i);
            d3d11_ps_append_border_adjustment(s, state, i, border_coord);
            g_string_append_printf(s,
                                   "  "
                                   "T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,pT%u.xy/"
                                   "pT%u.w*texCoordScale[%u].xy);\n",
                                   i, i, i, i, i, i, i);
        } else {
            g_string_append_printf(
                s, "  T%u=NV2A_SAMPLE2(tex%u,samp%u,%u,pT%u.xy);\n", i, i,
                i, i, i);
        }
        if (!d3d11_ps_append_texture_postprocess(s, state, i,
                                                samples_texture, errp)) {
            g_string_free(s, TRUE);
            return NULL;
        }
    }
    g_string_append(s, "  R0.a=T0.a;\n");
    for (unsigned int i = 0; i < stages; i++) {
        if (!d3d11_ps_append_stage(s, state, i, errp)) {
            g_string_free(s, TRUE);
            return NULL;
        }
    }
    if (!state->final_inputs_0 && !state->final_inputs_1) {
        g_string_append(s, "  float4 result=R0;\n");
    } else {
        uint32_t settings = state->final_inputs_1 & 0xff;
        uint32_t e = d3d11_ps_input_byte(state->final_inputs_1, 0);
        uint32_t f = d3d11_ps_input_byte(state->final_inputs_1, 1);
        uint32_t g = d3d11_ps_input_byte(state->final_inputs_1, 2);
        g_string_append(s,
                        "  C0=K[16]; C1=K[17];\n"
                        "  float3 v1sum=V1.rgb; float3 r0sum=R0.rgb;\n");
        if (settings & PS_FINALCOMBINERSETTING_COMPLEMENT_V1) {
            g_string_append(s, "  v1sum=1-v1sum;\n");
        }
        if (settings & PS_FINALCOMBINERSETTING_COMPLEMENT_R0) {
            g_string_append(s, "  r0sum=1-r0sum;\n");
        }
        g_string_append(s, "  V1R0_SUM=float4(v1sum+r0sum,0);\n");
        if (settings & PS_FINALCOMBINERSETTING_CLAMP_SUM) {
            g_string_append(s, "  V1R0_SUM.rgb=saturate(V1R0_SUM.rgb);\n");
        }
        g_string_append(s, "  float3 efE=");
        d3d11_ps_append_final_input(s, e, false, false);
        g_string_append(s, "; float3 efF=");
        d3d11_ps_append_final_input(s, f, false, false);
        g_string_append(s, "; EF_PROD=float4(efE*efF,0);\n");
        const char names[] = { 'A', 'B', 'C', 'D' };
        for (unsigned int i = 0; i < 4; i++) {
            g_string_append_printf(s, "  float3 fc%c=", names[i]);
            d3d11_ps_append_final_input(
                s, d3d11_ps_input_byte(state->final_inputs_0, i), false, true);
            g_string_append(s, ";\n");
        }
        g_string_append(s, "  float fcG=");
        d3d11_ps_append_final_input(s, g, true, true);
        g_string_append(
            s, ";\n  float4 result; "
               "result.rgb=saturate(lerp(fcC,fcB,fcA)+fcD); result.a=fcG;\n");
    }
    if (!d3d11_ps_append_alpha_test(s, state, errp)) {
        g_string_free(s, TRUE);
        return NULL;
    }
    if (custom_depth) {
        d3d11_ps_append_custom_depth(s, state);
    } else {
        g_string_append(s, "  return result;\n");
    }
    g_string_append(s, "}\n");
    return g_string_free(s, FALSE);
}

/* The GS keeps both face modes in one draw. Splitting front/back into draws
 * would reorder blending, stencil and occlusion for overlapping primitives.
 */
static void d3d11_source_replace(GString *source, const char *from,
                                  const char *to)
{
    char *match = strstr(source->str, from);
    g_assert(match);
    size_t offset = match - source->str;
    g_string_erase(source, offset, strlen(from));
    g_string_insert(source, offset, to);
}

static char *d3d11_raster_geometry_source(bool smooth, unsigned int provoking,
                                         unsigned int primitive,
                                         uint32_t raster, bool aa)
{
    char *base = d3d11_depth_geometry_shader_source(
        smooth, provoking, PRIM_TYPE_QUADS, POLY_MODE_FILL, true);
    GString *s = g_string_new_len(base, strstr(base, "[maxvertexcount(") - base);
    g_free(base);
    d3d11_source_replace(s, "  float2 pointCoord:TEXCOORD11;",
                     "  float2 pointCoord:TEXCOORD11;\n"
                     "  noperspective float4 coverage:TEXCOORD13;\n"
                     "  nointerpolation float rasterOffset:TEXCOORD14;");
    d3d11_source_replace(s, "o.pointCoord=float2(0.0,0.0);",
                     "o.pointCoord=float2(0.0,0.0); o.coverage=0.0; o.rasterOffset=0.0;");
    d3d11_source_replace(s, "GSOut o; o.position=vertex.position;",
                        "GSOut o=(GSOut)0; o.position=vertex.position;");
    bool quad = primitive == PRIM_TYPE_QUADS || primitive == PRIM_TYPE_QUAD_STRIP;
    bool polygon = primitive >= PRIM_TYPE_TRIANGLES;
    bool fan = primitive == PRIM_TYPE_POLYGON;
    unsigned int count = quad ? 4 : polygon ? 3 : 2;
    bool line_smooth = !aa && (raster & NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE);
    bool poly_smooth = !aa && (raster & NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE);
    unsigned int front = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    unsigned int back = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    g_string_append(s,
        "cbuffer Nv2aRasterDraw : register(b1) { uint nv2aFanTriangles; };\n"
        "float2 nv2a_screen(float4 p){return p.xy/p.w*nv2aLineParams.xy*0.5;}\n"
        "float nv2a_edge(float2 a,float2 b,float2 p){\n"
        " float2 e=b-a;return (e.x*(p.y-a.y)-e.y*(p.x-a.x))/max(length(e),1e-20);}\n"
        "GSOut nv2a_raster_vertex(GSIn v,GSIn flatV,float4 a,float4 b,float4 c,"
        "float mzP,float mzA,bool front){\n"
        " GSOut o=nv2a_make_quad_vertex(v,flatV,a,b,c,mzP,mzA);\n"
        " if(!front){o.diffuse=o.backDiffuse;o.specular=o.backSpecular;}\n"
        " o.backDiffuse=o.diffuse;o.backSpecular=o.specular;return o;}\n");
    g_string_append_printf(s,
        "[maxvertexcount(16)]\n"
        "void main(%s GSIn vertices[%u],uint primitiveId:SV_PrimitiveID,"
        "inout TriangleStream<GSOut> stream){\n",
        quad ? "lineadj" : polygon ? "triangle" : "line", count);
    g_string_append(s, " GSIn input[4];\n");
    for (unsigned int i = 0; i < 4; i++) {
        g_string_append_printf(s, " input[%u]=vertices[%u];\n", i, i < count ? i : 0);
    }
    if (primitive == PRIM_TYPE_QUAD_STRIP) {
        g_string_append(s, " if((primitiveId&1u)!=0u)return;\n");
    }
    g_string_append_printf(s,
        " uint n=%u; uint order[4]={0,1,%u,%u};\n"
        " float2 screen[4]={float2(0,0),float2(0,0),float2(0,0),float2(0,0)};\n"
        " [unroll]for(uint k=0;k<4;k++){\n"
        " if(abs(input[k].position.w)<1e-20)return;\n"
        " screen[k]=nv2a_screen(input[k].position);}\n"
        " float area=%s;\n"
        " bool front=%s;\n",
        count, primitive == PRIM_TYPE_QUAD_STRIP ? 3 : 2,
        primitive == PRIM_TYPE_QUAD_STRIP ? 2 : 3,
        polygon ? "nv2a_edge(screen[order[0]],screen[order[1]],screen[order[2]])" : "0.0",
        polygon ? (raster & NV_PGRAPH_SETUPRASTER_FRONTFACE ? "area>0.0" :
                                                                          "area<0.0") :
                  "true");
    if (polygon && (raster & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
        unsigned int cull = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
        g_string_append_printf(s, " if(%s)return;\n",
            cull == NV_PGRAPH_SETUPRASTER_CULLCTRL_FRONT ? "front" :
            cull == NV_PGRAPH_SETUPRASTER_CULLCTRL_BACK ? "!front" : "true");
    }
    g_string_append_printf(s,
        " uint mode=%s;uint p=%s;bool flip=front!=%s;\n",
        polygon ? "(front?NV2A_FRONT_MODE:NV2A_BACK_MODE)" : "NV2A_LINE_MODE",
        quad ? "3u" : provoking == 0 ? "0u" : !polygon ? "1u" :
        provoking == 1 ? "1u" : provoking == 2 ? "2u" : "(2u-(primitiveId&1u))",
        raster & NV_PGRAPH_SETUPRASTER_FRONTFACE ? "true" : "false");
    char *defines = g_strdup_printf(
        "#define NV2A_FRONT_MODE %u\n#define NV2A_BACK_MODE %u\n"
        "#define NV2A_LINE_MODE %u\n", front, back, POLY_MODE_LINE);
    g_string_prepend(s, defines);
    g_free(defines);
    g_string_append_printf(s,
        " bool offsetEnabled=%s&&(mode==%u?%s:mode==%u?%s:%s);\n",
        polygon ? "true" : "false", POLY_MODE_FILL,
        raster & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE ? "true" : "false",
        POLY_MODE_LINE,
        raster & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE ? "true" : "false",
        raster & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE ? "true" : "false");
    if (fan) {
        g_string_append_printf(s, " if(mode==%u)p=0u;\n", POLY_MODE_LINE);
    }
    g_string_append_printf(s,
        " if(mode==%u){\n"
        "  for(uint k=0;k<n;k++){uint i=order[k],j=order[(k+1)%%n];\n",
        POLY_MODE_LINE);
    if (!polygon) {
        g_string_append(s, "   if(k!=0)continue;\n");
    } else if (fan) {
        g_string_append(s,
            "   if((k==0&&primitiveId!=0)||(k==2&&primitiveId+1!=nv2aFanTriangles))continue;\n");
    }
    g_string_append_printf(s,
        "   float4 a=input[i].depthPos0,b=input[j].depthPos0;\n"
        "   float2 delta=b.xy-a.xy;float4 c=float4(a.xy+float2(-delta.y,delta.x),a.zw);\n"
        "   float mzP=nv2a_tri_mz_perspective(a,b,c),mzA=nv2a_tri_mz_affine(a,b,c);\n"
        "   float halfWidth=nv2aLineParams.z*0.5;\n"
        "   float extent=halfWidth+%s;float2 e=screen[j]-screen[i];\n"
        "   float2 normal=float2(-e.y,e.x)/max(length(e),1e-20);\n"
        "   for(uint q=0;q<4;q++){uint corner=flip?(q^1u):q;\n"
        "    float side=(corner&1u)?-1.0:1.0;uint v=corner<2?i:j;\n"
        "    GSOut o=nv2a_raster_vertex(input[v],input[p],a,b,c,mzP,mzA,front);\n"
        "    o.position.xy+=normal*side*extent*2.0/nv2aLineParams.xy*o.position.w;\n"
        "    o.rasterOffset=offsetEnabled;\n"
        "    o.coverage=float4(side*extent,halfWidth,0,%s);stream.Append(o);}\n"
        "   stream.RestartStrip();}return;}\n",
        line_smooth ? "0.5" : "0.0", line_smooth ? "1" : "0");
    g_string_append_printf(s,
        " if(mode==%u){for(uint k=0;k<n;k++){uint i=order[k];\n", POLY_MODE_POINT);
    if (fan) {
        g_string_append(s, "  if(primitiveId!=0&&k<2)continue;\n");
    }
    g_string_append(s,
        "  float4 a=input[i].depthPos0;\n"
        "  for(uint q=0;q<4;q++){uint corner=flip?(q^1u):q;\n"
        "   GSOut o=nv2a_raster_vertex(input[i],input[p],a,a,a,0,0,front);\n"
        "   float2 sign=float2((corner&1u)?1:-1,corner<2?-1:1);\n"
        "   o.position.xy+=sign*max(o.pointSize,1.0)/nv2aLineParams.xy*o.position.w;\n"
        "   o.rasterOffset=offsetEnabled;\n"
        "   stream.Append(o);}stream.RestartStrip();}return;}\n");
    g_string_append_printf(s,
        " [unroll]for(uint t=0;t<%u;t++){uint ids[3]={%s};\n"
        "  float4 a=input[ids[0]].depthPos0,b=input[ids[1]].depthPos0,c=input[ids[2]].depthPos0;\n"
        "  float mzP=nv2a_tri_mz_perspective(a,b,c),mzA=nv2a_tri_mz_affine(a,b,c);\n"
        "  [unroll]for(uint k=0;k<3;k++){uint i=ids[k];\n"
        "   GSOut o=nv2a_raster_vertex(input[i],input[p],a,b,c,mzP,mzA,front);\n",
        quad ? 2 : 1,
        primitive == PRIM_TYPE_QUAD_STRIP ? "2,t==0?0:1,t==0?1:3" :
                                          "order[0],order[t+1],order[t+2]");
    if (poly_smooth) {
        g_string_append(s,
            "   float dir=area>0?1.0:-1.0;float3 edges;\n"
            "   [unroll]for(uint e=0;e<3;e++)edges[e]=dir*nv2a_edge(screen[ids[e]],screen[ids[(e+1)%3]],screen[i]);\n");
        if (quad) {
            g_string_append(s, "   if(t==0)edges.z=1e10;else edges.x=1e10;\n");
        } else if (fan) {
            g_string_append(s,
                "   if(primitiveId!=0)edges.x=1e10;\n"
                "   if(primitiveId+1!=nv2aFanTriangles)edges.z=1e10;\n");
        }
        g_string_append(s, "   o.coverage=float4(edges,2);\n");
    }
    g_string_append(s, "   o.rasterOffset=offsetEnabled;\n"
                       "   stream.Append(o);}stream.RestartStrip();}\n}\n");
    return g_string_free(s, FALSE);
}

static char *d3d11_ps_raster_coverage(char *source, bool custom_depth)
{
    GString *s = g_string_new(source);
    g_free(source);
    char *input = strstr(s->str, "struct PSIn {");
    g_assert(input);
    char *end = strstr(input, "};");
    char *face = strstr(input, "  bool frontFace");
    size_t offset = ((face && face < end) ? face : end) - s->str;
    g_string_insert(s, offset,
                    "  noperspective float4 coverage:TEXCOORD13;\n"
                    "  nointerpolation float rasterOffset:TEXCOORD14;\n");
    if (custom_depth) {
        d3d11_source_replace(s, "zvalue+=depthOffset;",
                            "zvalue+=depthOffset*input.rasterOffset;");
        d3d11_source_replace(s, "depthFactor*",
                            "(depthFactor*input.rasterOffset)*");
    }
    d3d11_source_replace(s, " main(", " nv2a_coverage_main(");
    g_string_append(s, custom_depth ? "PSOut main(PSIn input){\n"
                                    " PSOut o=nv2a_coverage_main(input);\n" :
                                    "float4 main(PSIn input):SV_Target{\n"
                                    " float4 o=nv2a_coverage_main(input);\n");
    g_string_append(s,
        " float c=input.coverage.w>1.5?saturate(min(input.coverage.x,min(input.coverage.y,input.coverage.z))+0.5):\n"
        " input.coverage.w>0.5?saturate(input.coverage.y+0.5-abs(input.coverage.x)):1;\n"
        " if(c<=0)discard;\n");
    g_string_append(s, custom_depth ? " o.color.a*=c;return o;}\n" :
                                    " o.a*=c;return o;}\n");
    return g_string_free(s, FALSE);
}

static char *d3d11_translate_pixel_combiners(const PshState *state,
                                             Error **errp)
{
    return d3d11_translate_pixel_combiners_with_depth(state, false, errp);
}

static char *d3d11_ps_dual_source(char *source, bool custom_depth)
{
    /* Keep NV2A's constant color in OM's vector and supply constant alpha
     * independently from the second PS output. Only render target 0 is bound.
     */
    char *wrapped = g_strconcat(
        "#define main nv2a_pixel_main\n", source,
        "\n#undef main\n"
        "struct DualPSOut {float4 color:SV_Target0;float4 factor:SV_Target1;",
        custom_depth ? "float depth:SV_Depth;" : "",
        "};\nDualPSOut main(PSIn input){DualPSOut output;",
        custom_depth ?
            "PSOut original=nv2a_pixel_main(input);output.color=original.color;"
            "output.depth=original.depth;" :
            "output.color=nv2a_pixel_main(input);",
        "output.factor=blendColor.aaaa;return output;}\n", NULL);
    g_free(source);
    return wrapped;
}

static bool d3d11_primitive_supports_custom_depth(
    enum ShaderPrimitiveMode primitive)
{
    switch (primitive) {
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        return true;
    case PRIM_TYPE_POINTS:
        /* The vertex shader already repeats its position for point depth. */
        return true;
    case PRIM_TYPE_LINES:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_LINE_STRIP:
        return true;
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
        /* The adjacency GS preserves the GL split, flat provoking vertex and
         * custom depth. QUADS/QUAD_STRIP LINE and POINT emit their perimeter
         * edges or vertices; triangle POINT emits source vertices, triangle
         * list/strip/fan LINE emits source edges, and polygon LINE keeps its
         * wireframe fallback. */
        return true;
    default:
        return false;
    }
}

static bool d3d11_triangle_line_primitive(enum ShaderPrimitiveMode primitive)
{
    return primitive == PRIM_TYPE_TRIANGLES ||
           primitive == PRIM_TYPE_TRIANGLE_STRIP ||
           primitive == PRIM_TYPE_TRIANGLE_FAN;
}

bool pgraph_d3d11_needs_wide_lines(unsigned int primitive,
                                  unsigned int polygon_mode,
                                  unsigned int surface_scale_factor)
{
    if (surface_scale_factor <= 1) {
        return false;
    }
    switch (primitive) {
    case PRIM_TYPE_LINES:
    case PRIM_TYPE_LINE_LOOP:
    case PRIM_TYPE_LINE_STRIP:
        return true;
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return polygon_mode == POLY_MODE_LINE;
    default:
        return false;
    }
}

static unsigned int d3d11_depth_geometry_variant_for(
    enum ShaderPrimitiveMode primitive, bool smooth_shading,
    bool first_vertex, enum ShaderPolygonMode polygon_mode)
{
    if (primitive == PRIM_TYPE_POINTS) {
        return PGRAPHD3D11_POINT_GS_VARIANT;
    }
    if (primitive == PRIM_TYPE_POLYGON && polygon_mode == POLY_MODE_POINT) {
        return PGRAPHD3D11_POLYGON_POINT_GS_VARIANT + smooth_shading;
    }
    if (primitive == PRIM_TYPE_POLYGON && polygon_mode == POLY_MODE_LINE) {
        return 40 + (smooth_shading ? 1 : 0);
    }
    if (primitive == PRIM_TYPE_QUADS) {
        unsigned int base = polygon_mode == POLY_MODE_LINE ? 16 :
                            polygon_mode == POLY_MODE_POINT ? 20 : 8;
        return base + (smooth_shading ? 1 : 0);
    }
    if (primitive == PRIM_TYPE_QUAD_STRIP) {
        unsigned int base = polygon_mode == POLY_MODE_LINE ? 18 :
                            polygon_mode == POLY_MODE_POINT ? 22 : 10;
        return base + (smooth_shading ? 1 : 0);
    }
    if (primitive == PRIM_TYPE_LINES ||
        primitive == PRIM_TYPE_LINE_LOOP ||
        primitive == PRIM_TYPE_LINE_STRIP) {
        return 12 + (smooth_shading ? 2 : 0) + (first_vertex ? 0 : 1);
    }
    unsigned int provoking_mode = 2;
    if (first_vertex) {
        provoking_mode = 0;
    } else if (primitive == PRIM_TYPE_TRIANGLE_FAN) {
        provoking_mode = 1;
    } else if (primitive == PRIM_TYPE_TRIANGLE_STRIP) {
        provoking_mode = 3;
    }
    unsigned int base = polygon_mode == POLY_MODE_POINT ? 24 :
                        polygon_mode == POLY_MODE_LINE &&
                                d3d11_triangle_line_primitive(primitive) ? 32 :
                                                                          0;
    return base + (smooth_shading ? 4 : 0) + provoking_mode;
}

static bool d3d11_custom_depth_needs_geometry_shader(
    enum ShaderPrimitiveMode primitive)
{
    return primitive != PRIM_TYPE_POINTS;
}

static bool d3d11_triangle_primitive(enum ShaderPrimitiveMode primitive)
{
    switch (primitive) {
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        return true;
    default:
        return false;
    }
}
bool pgraph_d3d11_needs_raster_geometry(PGRAPHState *pg)
{
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    bool polygon = pg->primitive_mode >= PRIM_TYPE_TRIANGLES;
    bool aa = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
                       NV_PGRAPH_ANTIALIASING_ENABLE);
    return (polygon && GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE) !=
                           GET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE)) ||
           (!aa && pg->primitive_mode != PRIM_TYPE_POINTS &&
            ((raster & NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) ||
             (polygon && (raster & NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE))));
}

static bool d3d11_polygon_mode_supported(unsigned int primitive,
                                         unsigned int polygon_mode)
{
    return primitive >= PRIM_TYPE_POINTS && primitive <= PRIM_TYPE_POLYGON &&
           (polygon_mode == POLY_MODE_FILL || polygon_mode == POLY_MODE_LINE ||
            polygon_mode == POLY_MODE_POINT);
}

static enum ShaderPolygonMode d3d11_polygon_mode(PGRAPHState *pg)
{
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    enum ShaderPolygonMode front = GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    return front;
}

static PGRAPHD3D11PixelShader *d3d11_compile_pixel_shader(PGRAPHD3D11State *r,
                                                          const char *source,
                                                          Error **errp)
{
    ID3DBlob *bytecode = NULL, *errors = NULL;
    const char *profile = d3d11_shader_profile(r, false);
    if (!profile) {
        error_setg(errp, "D3D11: unsupported shader feature level 0x%x",
                   r->feature_level);
        return NULL;
    }
    HRESULT hr = D3DCompile2(
        source, strlen(source), "nv2a_pixel.hlsl", NULL, NULL, "main", profile,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        0, NULL, 0, &bytecode, &errors);
    if (FAILED(hr)) {
        const char *message = errors ? ID3D10Blob_GetBufferPointer(errors) :
                                       "no compiler diagnostics";
        error_setg(errp,
                   "D3D11: NV2A pixel shader compilation failed "
                   "(HRESULT 0x%08lx): %s",
                   (unsigned long)hr, message);
        if (errors) {
            ID3D10Blob_Release(errors);
        }
        return NULL;
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    PGRAPHD3D11PixelShader *shader = g_new0(PGRAPHD3D11PixelShader, 1);
    shader->bytecode = bytecode;
    hr = ID3D11Device_CreatePixelShader(
        r->device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader->shader);
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: CreatePixelShader failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        d3d11_pixel_shader_destroy(shader);
        return NULL;
    }
    return shader;
}

static const char *d3d11_geometry_shader_profile(const PGRAPHD3D11State *r)
{
    if (r->feature_level >= D3D_FEATURE_LEVEL_11_0) {
        return "gs_5_0";
    }
    if (r->feature_level >= D3D_FEATURE_LEVEL_10_1) {
        return "gs_4_1";
    }
    if (r->feature_level >= D3D_FEATURE_LEVEL_10_0) {
        return "gs_4_0";
    }
    return NULL;
}

static unsigned int d3d11_depth_geometry_variant(PGRAPHState *pg,
                                                 const PshState *state)
{
    uint32_t control3 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3);
    bool first_vertex =
        GET_MASK(control3, NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
        NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
    enum ShaderPolygonMode polygon_mode = d3d11_polygon_mode(pg);
    return d3d11_depth_geometry_variant_for(
        pg->primitive_mode, state->smooth_shading, first_vertex, polygon_mode);
}

static ID3D11GeometryShader *d3d11_get_depth_geometry_shader(
    PGRAPHState *pg, const PshState *state, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    unsigned int base_variant = d3d11_depth_geometry_variant(pg, state);
    bool wide_lines = pgraph_d3d11_needs_wide_lines(
        pg->primitive_mode, d3d11_polygon_mode(pg), pg->surface_scale_factor);
    unsigned int variant = base_variant +
                           (wide_lines ?
                                PGRAPHD3D11_DEPTH_GS_VARIANTS_PER_WIDTH : 0);
    ID3D11GeometryShader *shader =
        r->shaders->depth_geometry_shaders[variant];
    if (shader) {
        return shader;
    }
    if (r->shaders->depth_geometry_shader_failed[variant]) {
        error_setg(errp, "D3D11: cached depth geometry shader failure");
        return NULL;
    }
    const char *profile = d3d11_geometry_shader_profile(r);
    if (!profile) {
        error_setg(errp, "D3D11: geometry shaders require feature level 10.0+");
        return NULL;
    }
    enum ShaderPolygonMode polygon_mode = d3d11_polygon_mode(pg);
    unsigned int provoking_mode = base_variant >= 40 ? 0 :
                                  base_variant < 8 ? base_variant % 4 :
                                  base_variant < 12 ? 2 :
                                  base_variant < 24 ?
                                      (base_variant - 12) % 2 :
                                  base_variant < 32 ?
                                      (base_variant - 24) % 4 :
                                      (base_variant - 32) % 4;
    char *source = d3d11_depth_geometry_shader_source(
        state->smooth_shading, provoking_mode, pg->primitive_mode,
        polygon_mode, wide_lines);
    ID3DBlob *bytecode = NULL, *errors = NULL;
    HRESULT hr = D3DCompile2(
        source, strlen(source), "nv2a_depth_geometry.hlsl", NULL, NULL, "main",
        profile, D3DCOMPILE_ENABLE_STRICTNESS |
                     D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, 0, NULL, 0, &bytecode, &errors);
    g_free(source);
    if (FAILED(hr)) {
        const char *message = errors ? ID3D10Blob_GetBufferPointer(errors) :
                                       "no compiler diagnostics";
        error_setg(errp,
                   "D3D11: NV2A depth geometry shader compilation failed "
                   "(HRESULT 0x%08lx): %s",
                   (unsigned long)hr, message);
        if (errors) {
            ID3D10Blob_Release(errors);
        }
        if (bytecode) {
            ID3D10Blob_Release(bytecode);
        }
        r->shaders->depth_geometry_shader_failed[variant] = true;
        return NULL;
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    hr = ID3D11Device_CreateGeometryShader(
        r->device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader);
    ID3D10Blob_Release(bytecode);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: CreateGeometryShader failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        r->shaders->depth_geometry_shader_failed[variant] = true;
        return NULL;
    }
    r->shaders->depth_geometry_shaders[variant] = shader;
    return shader;
}

static PGRAPHD3D11PixelShader *
d3d11_create_cached_pixel_shader(PGRAPHD3D11State *r, GBytes *key)
{
    ID3DBlob *bytecode =
        d3d11_shader_cache_load(r->shaders, key, D3D11_SHADER_STAGE_PIXEL);
    if (!bytecode) {
        return NULL;
    }
    PGRAPHD3D11PixelShader *shader = g_new0(PGRAPHD3D11PixelShader, 1);
    shader->bytecode = bytecode;
    HRESULT hr = ID3D11Device_CreatePixelShader(
        r->device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader->shader);
    if (FAILED(hr)) {
        char *path =
            d3d11_shader_cache_path(r->shaders, key, D3D11_SHADER_STAGE_PIXEL);
        qemu_unlink(path);
        g_free(path);
        d3d11_pixel_shader_destroy(shader);
        return NULL;
    }
    return shader;
}

static void d3d11_trim_pixel_cache(PGRAPHD3D11ShaderState *shaders)
{
    if (g_hash_table_size(shaders->pixel_cache) <
        D3D11_SHADER_MEMORY_CACHE_LIMIT) {
        return;
    }
    GHashTableIter iter;
    gpointer key, value, oldest_key = NULL;
    uint64_t oldest_use = UINT64_MAX;
    g_hash_table_iter_init(&iter, shaders->pixel_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PGRAPHD3D11PixelShader *shader = value;
        if (shader != shaders->pixel && shader->last_used < oldest_use) {
            oldest_key = key;
            oldest_use = shader->last_used;
        }
    }
    if (oldest_key) {
        g_hash_table_remove(shaders->pixel_cache, oldest_key);
    }
}

typedef struct PGRAPHD3D11PixelConstants {
    float constants[18][4];
    float fog_color[4];
    int32_t alpha_ref;
    float eye_vector[3];
    float bump_mat[4][4];
    float bump_params[4][4];
    float clip_range[4];
    uint32_t color_key[4];
    uint32_t color_key_mask[4];
    int32_t clip_region[8][4];
    float tex_scale[4];
    float tex_coord_scale[4][4];
    float surface_scale[2];
    float depth_offset;
    float depth_factor;
    float blend_color[4];
} PGRAPHD3D11PixelConstants;

QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D11PixelConstants) !=
                  D3D11_PIXEL_CONSTANT_BUFFER_SIZE);

static void d3d11_texture_coordinate_scale(PGRAPHState *pg, unsigned int stage,
                                           float scale[4])
{
    for (unsigned int i = 0; i < 4; i++) {
        scale[i] = 1;
    }
    // Disabled stages may have uninitialized dimensionality and mip counts.
    if (!pgraph_is_texture_enabled(pg, stage)) {
        return;
    }
    TextureShape shape = pgraph_get_texture_shape(pg, stage);
    if (kelvin_color_format_info_map[shape.color_format].linear) {
        scale[0] = 1.0f / MAX(1, shape.width);
        scale[1] = 1.0f / MAX(1, shape.height);
    }
}

static void d3d11_ps_set_snorm_textures(PGRAPHState *pg, PshState *state)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(state->snorm_tex); i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            continue;
        }
        TextureShape shape = pgraph_get_texture_shape(pg, i);
        state->snorm_tex[i] |=
            shape.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5;
        if (shape.cubemap && shape.border && shape.levels > 1) {
            state->border_logical_size[i][0] = shape.width;
            state->border_logical_size[i][1] = shape.height;
            state->border_inv_real_size[i][0] =
                1.0f / MAX(16u, shape.width * 2);
            state->border_inv_real_size[i][1] =
                1.0f / MAX(16u, shape.height * 2);
        }
    }
}

static bool d3d11_update_pixel_constants(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PshUniformLocs locs;
    for (unsigned int i = 0; i < ARRAY_SIZE(locs); i++) {
        locs[i] = -1;
    }
    locs[PshUniform_consts] = 0;
    locs[PshUniform_fogColor] = 0;
    locs[PshUniform_alphaRef] = 0;
    locs[PshUniform_bumpMat] = 0;
    locs[PshUniform_bumpOffset] = 0;
    locs[PshUniform_bumpScale] = 0;
    locs[PshUniform_clipRange] = 0;
    locs[PshUniform_clipRegion] = 0;
    locs[PshUniform_colorKey] = 0;
    locs[PshUniform_colorKeyMask] = 0;
    locs[PshUniform_depthFactor] = 0;
    locs[PshUniform_depthOffset] = 0;
    locs[PshUniform_surfaceScale] = 0;
    locs[PshUniform_texScale] = 0;
    PshUniformValues values = { 0 };
    pgraph_glsl_set_psh_uniform_values(pg, locs, &values);
    PGRAPHD3D11PixelConstants constants = { 0 };
    memcpy(constants.constants, values.consts, sizeof(constants.constants));
    memcpy(constants.fog_color, values.fogColor[0],
           sizeof(constants.fog_color));
    constants.alpha_ref = values.alphaRef[0];
    memcpy(constants.bump_mat, values.bumpMat, sizeof(constants.bump_mat));
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        constants.bump_params[i][0] = values.bumpScale[i];
        constants.bump_params[i][1] = values.bumpOffset[i];
        constants.tex_scale[i] = values.texScale[i];
        d3d11_texture_coordinate_scale(pg, i, constants.tex_coord_scale[i]);
    }
    memcpy(constants.clip_range, values.clipRange[0],
           sizeof(constants.clip_range));
    memcpy(constants.color_key, values.colorKey, sizeof(constants.color_key));
    memcpy(constants.color_key_mask, values.colorKeyMask,
           sizeof(constants.color_key_mask));
    memcpy(constants.clip_region, values.clipRegion,
           sizeof(constants.clip_region));
    memcpy(constants.surface_scale, values.surfaceScale[0],
           sizeof(constants.surface_scale));
    constants.depth_offset = values.depthOffset[0];
    if (pgraph_d3d11_needs_raster_geometry(pg)) {
        uint32_t bias = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        uint32_t factor = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
        memcpy(&constants.depth_offset, &bias, sizeof(bias));
        memcpy(&values.depthFactor[0], &factor, sizeof(factor));
    }
    constants.depth_factor = values.depthFactor[0];
    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                    constants.blend_color);
    /* SET_EYE_VECTOR stores three float bit patterns in consecutive PGRAPH registers. */
    for (unsigned int i = 0; i < 3; i++) {
        uint32_t eye_vector_bits = pgraph_reg_r(pg, NV_PGRAPH_EYEVEC0 + i * 4);
        memcpy(&constants.eye_vector[i], &eye_vector_bits,
               sizeof(eye_vector_bits));
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(
        r->context, (ID3D11Resource *)r->shaders->pixel_constants, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: pixel constant-buffer update failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ID3D11DeviceContext_Unmap(r->context,
                              (ID3D11Resource *)r->shaders->pixel_constants, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(r->context, 0, 1,
                                             &r->shaders->pixel_constants);
    return true;
}

static ID3D11GeometryShader *d3d11_get_raster_geometry_shader(
    PGRAPHState *pg, const PshState *state, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    bool aa = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
                       NV_PGRAPH_ANTIALIASING_ENABLE);
    bool first = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                          NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX) ==
                 NV_PGRAPH_CONTROL_3_PROVOKING_VERTEX_FIRST;
    unsigned int provoking = first ? 0 :
        pg->primitive_mode == PRIM_TYPE_TRIANGLE_STRIP ? 3 :
        pg->primitive_mode == PRIM_TYPE_TRIANGLE_FAN ? 1 :
        pg->primitive_mode < PRIM_TYPE_TRIANGLES ? 1 : 2;
    uint32_t words[] = { raster, pg->primitive_mode, state->smooth_shading,
                         provoking | ((unsigned int)aa << 8) };
    GBytes *key = g_bytes_new(words, sizeof(words));
    ID3D11GeometryShader *shader =
        g_hash_table_lookup(r->shaders->raster_geometry_cache, key);
    if (shader) {
        g_bytes_unref(key);
        return shader;
    }
    if (g_hash_table_contains(r->shaders->raster_geometry_cache, key)) {
        error_setg(errp, "D3D11: cached raster geometry shader failure");
        g_bytes_unref(key);
        return NULL;
    }
    char *source = d3d11_raster_geometry_source(
        state->smooth_shading, provoking, pg->primitive_mode, raster, aa);
    ID3DBlob *bytecode = NULL, *errors = NULL;
    HRESULT hr = D3DCompile2(source, strlen(source), "nv2a_raster.hlsl", NULL,
        NULL, "main", d3d11_geometry_shader_profile(r),
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, 0, NULL, 0, &bytecode, &errors);
    g_free(source);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateGeometryShader(
            r->device, ID3D10Blob_GetBufferPointer(bytecode),
            ID3D10Blob_GetBufferSize(bytecode), NULL, &shader);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: raster geometry shader failed (0x%08lx): %s",
                   (unsigned long)hr,
                   errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "");
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    if (bytecode) {
        ID3D10Blob_Release(bytecode);
    }
    if (g_hash_table_size(r->shaders->raster_geometry_cache) >= 64) {
        g_hash_table_remove_all(r->shaders->raster_geometry_cache);
    }
    g_hash_table_insert(r->shaders->raster_geometry_cache, key, shader);
    return shader;
}

bool pgraph_d3d11_bind_pixel_shader(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (!pgraph_d3d11_validate_texture_registers(pg, errp)) {
        return false;
    }
    uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    unsigned int front_mode = GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    unsigned int back_mode = GET_MASK(
        raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE);
    if (!d3d11_polygon_mode_supported(pg->primitive_mode, front_mode) ||
        !d3d11_polygon_mode_supported(pg->primitive_mode, back_mode)) {
        error_setg(errp,
                   "D3D11: invalid primitive or polygon mode");
        return false;
    }
    PshState state = { 0 };
    pgraph_glsl_set_psh_state(pg, &state);
    d3d11_ps_set_snorm_textures(pg, &state);
    state.point_sprite = state.point_sprite &&
                         pg->primitive_mode == PRIM_TYPE_POINTS;
    bool custom_depth_format =
        state.surface_zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ||
        state.surface_zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
    bool custom_depth =
        custom_depth_format &&
        d3d11_primitive_supports_custom_depth(pg->primitive_mode);
    bool dual_source = pgraph_d3d11_dual_source_blend(
        pgraph_reg_r(pg, NV_PGRAPH_BLEND));
    r->shaders->quad_adjacency_geometry = false;
    r->shaders->polygon_line_geometry = false;
    bool raster_geometry = pgraph_d3d11_needs_raster_geometry(pg);
    r->shaders->raster_geometry = raster_geometry;
    GByteArray *key_data = g_byte_array_sized_new(sizeof(state) + 1);
    g_byte_array_append(key_data, (const uint8_t *)&state, sizeof(state));
    uint8_t custom_depth_key = custom_depth;
    g_byte_array_append(key_data, &custom_depth_key, sizeof(custom_depth_key));
    uint8_t dual_source_key = dual_source;
    g_byte_array_append(key_data, &dual_source_key, sizeof(dual_source_key));
    uint8_t raster_key = raster_geometry;
    g_byte_array_append(key_data, &raster_key, sizeof(raster_key));
    GBytes *key = g_byte_array_free_to_bytes(key_data);
    PGRAPHD3D11PixelShader *shader =
        g_hash_table_lookup(r->shaders->pixel_cache, key);
    if (!shader) {
        if (g_hash_table_contains(r->shaders->failed_pixel_cache, key)) {
            error_setg(errp, "D3D11: cached NV2A pixel shader failure");
            g_bytes_unref(key);
            return false;
        }
        shader = d3d11_create_cached_pixel_shader(r, key);
    }
    if (!shader) {
        char *source = custom_depth ?
                           d3d11_translate_pixel_combiners_with_depth(
                               &state, true, errp) :
                           d3d11_translate_pixel_combiners(&state, errp);
        if (!source) {
            if (g_hash_table_size(r->shaders->failed_pixel_cache) >=
                D3D11_SHADER_FAILURE_CACHE_LIMIT) {
                g_hash_table_remove_all(r->shaders->failed_pixel_cache);
            }
            g_hash_table_add(r->shaders->failed_pixel_cache, g_bytes_ref(key));
            g_bytes_unref(key);
            return false;
        }
        if (raster_geometry) {
            source = d3d11_ps_raster_coverage(source, custom_depth);
        }
        if (dual_source) {
            source = d3d11_ps_dual_source(source, custom_depth);
        }
        shader = d3d11_compile_pixel_shader(r, source, errp);
        g_free(source);
        if (!shader) {
            if (g_hash_table_size(r->shaders->failed_pixel_cache) >=
                D3D11_SHADER_FAILURE_CACHE_LIMIT) {
                g_hash_table_remove_all(r->shaders->failed_pixel_cache);
            }
            g_hash_table_add(r->shaders->failed_pixel_cache, g_bytes_ref(key));
            g_bytes_unref(key);
            return false;
        }
        d3d11_shader_cache_store(r->shaders, key, D3D11_SHADER_STAGE_PIXEL,
                                 shader->bytecode);
        d3d11_trim_pixel_cache(r->shaders);
        g_hash_table_insert(r->shaders->pixel_cache, key, shader);
    } else {
        if (!g_hash_table_lookup(r->shaders->pixel_cache, key)) {
            d3d11_trim_pixel_cache(r->shaders);
            g_hash_table_insert(r->shaders->pixel_cache, key, shader);
        } else {
            g_bytes_unref(key);
        }
    }
    shader->last_used = ++r->shaders->use_serial;
    r->shaders->pixel = shader;
    bool custom_depth_uses_geometry_shader =
        custom_depth &&
        d3d11_custom_depth_needs_geometry_shader(pg->primitive_mode);
    bool quad_nonfill_mode =
        d3d11_polygon_mode(pg) != POLY_MODE_FILL &&
        (pg->primitive_mode == PRIM_TYPE_QUADS ||
         pg->primitive_mode == PRIM_TYPE_QUAD_STRIP);
    bool triangle_point_mode =
        d3d11_polygon_mode(pg) == POLY_MODE_POINT &&
        d3d11_triangle_primitive(pg->primitive_mode);
    bool triangle_line_mode =
        d3d11_polygon_mode(pg) == POLY_MODE_LINE &&
        d3d11_triangle_line_primitive(pg->primitive_mode);
    bool polygon_line_mode =
        pg->primitive_mode == PRIM_TYPE_POLYGON &&
        d3d11_polygon_mode(pg) == POLY_MODE_LINE;
    bool wide_line_mode = pgraph_d3d11_needs_wide_lines(
        pg->primitive_mode, d3d11_polygon_mode(pg), pg->surface_scale_factor);
    bool geometry_shader_needed =
        pg->primitive_mode == PRIM_TYPE_POINTS ||
        custom_depth_uses_geometry_shader || quad_nonfill_mode ||
        triangle_point_mode || triangle_line_mode || polygon_line_mode ||
        wide_line_mode;
    ID3D11GeometryShader *depth_geometry_shader =
        raster_geometry ? d3d11_get_raster_geometry_shader(pg, &state, errp) :
        geometry_shader_needed ?
            d3d11_get_depth_geometry_shader(pg, &state, errp) : NULL;
    if ((geometry_shader_needed || raster_geometry) && !depth_geometry_shader) {
        return false;
    }
    r->shaders->quad_adjacency_geometry =
        (geometry_shader_needed || raster_geometry) &&
        (pg->primitive_mode == PRIM_TYPE_QUADS ||
         pg->primitive_mode == PRIM_TYPE_QUAD_STRIP);
    r->shaders->polygon_line_geometry = polygon_line_mode && !raster_geometry;
    if (!d3d11_update_pixel_constants(pg, errp)) {
        return false;
    }
    ID3D11DeviceContext_PSSetShader(r->context, shader->shader, NULL, 0);
    ID3D11DeviceContext_GSSetShader(r->context, depth_geometry_shader, NULL, 0);
    return true;
}
