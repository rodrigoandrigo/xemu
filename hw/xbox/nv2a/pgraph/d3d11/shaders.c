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

#define D3D11_SHADER_CACHE_VERSION 4
#define D3D11_SHADER_CACHE_MAGIC 0x31485358U /* XSH1 */
#define D3D11_SHADER_CACHE_MAX_BYTECODE (16 * MiB)
#define D3D11_SHADER_MEMORY_CACHE_LIMIT 2048
#define D3D11_SHADER_FAILURE_CACHE_LIMIT 256
#define D3D11_PIXEL_CONSTANT_BUFFER_SIZE 656

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

static const char d3d11_vsh_preamble[] =
    "#define NV2A_COLOR_INTERPOLATION\n"
    "cbuffer Nv2aVertexConstants : register(b0) {\n"
    "  float4 C[192];\n"
    "  float4 clipRange;\n"
    "  float2 surfaceSize;\n"
    "  float2 _constantPadding;\n"
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
    "  float fog : FOG; float pointSize : PSIZE;\n"
    "  float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1;\n"
    "  float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3;\n"
    "};\n"
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
    "  return x >= 0 ? clamp(x, asfloat(0x1f800000), asfloat(0x5f800000))\n"
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
    "}\n";

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

bool pgraph_d3d11_shaders_init(PGRAPHD3D11State *r, Error **errp)
{
    r->shaders = g_new0(PGRAPHD3D11ShaderState, 1);
    r->shaders->vertex_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref,
        d3d11_vertex_shader_destroy);
    r->shaders->pixel_cache = g_hash_table_new_full(
        g_bytes_hash, g_bytes_equal, (GDestroyNotify)g_bytes_unref,
        d3d11_pixel_shader_destroy);
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
        .ByteWidth =
            NV2A_VERTEXSHADER_CONSTANTS * 4 * sizeof(float) + 8 * sizeof(float),
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
    g_hash_table_unref(r->shaders->vertex_cache);
    g_hash_table_unref(r->shaders->pixel_cache);
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
    r->shaders->vertex = NULL;
    r->shaders->pixel = NULL;
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

static char *d3d11_translate_vertex_program(const ProgrammableVshState *program,
                                            Error **errp)
{
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
    g_string_append(
        s, "  oPos.xy = round(oPos.xy * 16.0) / 16.0;\n"
           "  oPos.w = nv_clamp_away(oPos.w);\n"
           "  oPos.xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize;\n"
           "  oPos.z = oPos.z / clipRange.y; oPos.xyz *= oPos.w;\n"
           "  VSOut output; output.position=oPos; output.diffuse=oD0;\n"
           "  output.specular=oD1; output.backDiffuse=oB0;\n"
           "  output.backSpecular=oB1; output.fog=oFog.x;\n"
           "  output.pointSize=oPts.x; output.tex0=oT0;\n"
           "  output.tex1=oT1; output.tex2=oT2; output.tex3=oT3;\n"
           "  return output;\n}\n");
    return g_string_free(s, FALSE);
}

static PGRAPHD3D11VertexShader *d3d11_compile_vertex_shader(PGRAPHD3D11State *r,
                                                            const char *source,
                                                            Error **errp)
{
    ID3DBlob *bytecode = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(
        source, strlen(source), "nv2a_vertex.hlsl", NULL, NULL, "main",
        "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &bytecode, &errors);
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
    float padding[2];
} PGRAPHD3D11VertexConstants;

static bool d3d11_update_vertex_constants(PGRAPHState *pg,
                                          const VshState *state, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    VshUniformLocs locs;
    for (unsigned int i = 0; i < ARRAY_SIZE(locs); i++) {
        locs[i] = -1;
    }
    locs[VshUniform_c] = 0;
    locs[VshUniform_clipRange] = 0;
    locs[VshUniform_surfaceSize] = 0;
    VshUniformValues values = { 0 };
    pgraph_glsl_set_vsh_uniform_values(pg, state, locs, &values);
    PGRAPHD3D11VertexConstants constants = { 0 };
    memcpy(constants.c, values.c, sizeof(constants.c));
    memcpy(constants.clip_range, values.clipRange[0],
           sizeof(constants.clip_range));
    memcpy(constants.surface_size, values.surfaceSize[0],
           sizeof(constants.surface_size));

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
    return true;
}

bool pgraph_d3d11_bind_vertex_shader(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    VshState state = { 0 };
    pgraph_glsl_set_vsh_state(pg, &state);
    if (state.is_fixed_function) {
        error_setg(errp, "D3D11: fixed-function NV2A vertex translation is not "
                         "implemented");
        return false;
    }
    const ProgrammableVshState *program = &state.programmable;
    GByteArray *key_data = g_byte_array_sized_new(
        sizeof(program->program_length) +
        program->program_length * sizeof(program->program_data[0]));
    g_byte_array_append(key_data, (const uint8_t *)&program->program_length,
                        sizeof(program->program_length));
    g_byte_array_append(key_data, (const uint8_t *)program->program_data,
                        program->program_length *
                            sizeof(program->program_data[0]));
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
        char *source = d3d11_translate_vertex_program(program, errp);
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
    "  float4 K[18]; float4 fogColor; int alphaRef; float3 _psPad;\n"
    "  float4 bumpMat[4]; float4 bumpParams[4]; float4 clipRange;\n"
    "  uint4 colorKey; uint4 colorKeyMask; int4 clipRegion[8];\n"
    "  float4 texScale; float2 surfaceScale; float depthOffset;\n"
    "  float depthFactor;\n"
    "};\n"
    "struct PSIn {\n"
    "  float4 position : SV_Position;\n"
    "  float4 diffuse : COLOR0; float4 specular : COLOR1;\n"
    "  float4 backDiffuse : TEXCOORD4; float4 backSpecular : TEXCOORD5;\n"
    "  float fog : FOG; float pointSize : PSIZE;\n"
    "  float4 tex0 : TEXCOORD0; float4 tex1 : TEXCOORD1;\n"
    "  float4 tex2 : TEXCOORD2; float4 tex3 : TEXCOORD3;\n"
    "  bool frontFace : SV_IsFrontFace;\n"
    "};\n"
    "float nv_mul1(float a,float b) { return (a==0||b==0)?0:a*b; }\n"
    "float3 nv_mul3(float3 a,float3 b) {\n"
    "  float3 r=a*b; if(a.x==0||b.x==0)r.x=0;\n"
    "  if(a.y==0||b.y==0)r.y=0; if(a.z==0||b.z==0)r.z=0; return r;\n"
    "}\n";

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

static void d3d11_ps_append_input(GString *s, uint32_t input, bool alpha,
                                  bool final_abcd)
{
    uint32_t index = input & D3D11_PS_REGISTER_MASK;
    uint32_t mapping = input & D3D11_PS_INPUT_MAPPING_MASK;
    bool alpha_channel = input & PS_CHANNEL_ALPHA;
    if (final_abcd && mapping >= PS_INPUTMAPPING_EXPAND_NORMAL) {
        mapping &= PS_INPUTMAPPING_UNSIGNED_INVERT;
    }
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
        d3d11_ps_append_input(s, d3d11_ps_input_byte(rgb_input, i), false,
                              false);
        g_string_append(s, ";\n");
    }
    const char *alpha_prefixes[] = { "aA", "aB", "aC", "aD" };
    for (unsigned int i = 0; i < 4; i++) {
        g_string_append_printf(s, "  float %s_%u=", alpha_prefixes[i], stage);
        d3d11_ps_append_input(s, d3d11_ps_input_byte(alpha_input, i), true,
                              false);
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
                  "  bool mux_%u=(((uint)(saturate(R0.a)*255+0.5)&1)!=0);\n",
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

static void d3d11_ps_append_final_input(GString *s, uint32_t input, bool abcd)
{
    uint32_t index = input & D3D11_PS_REGISTER_MASK;
    uint32_t mapping = input & D3D11_PS_INPUT_MAPPING_MASK;
    bool alpha = input & PS_CHANNEL_ALPHA;
    if (mapping >= PS_INPUTMAPPING_EXPAND_NORMAL) {
        mapping &= PS_INPUTMAPPING_UNSIGNED_INVERT;
    }
    const char *name = d3d11_ps_register_name(index);
    GString *value = g_string_new(NULL);
    if (!name || index == PS_REGISTER_ZERO) {
        g_string_append(value, "float4(0,0,0,0)");
    } else if (index == PS_REGISTER_FOG) {
        g_string_append(value, "float4(0,0,0,FOG.a)");
    } else if (!abcd && (index == PS_REGISTER_V1R0_SUM ||
                         index == PS_REGISTER_EF_PROD)) {
        g_string_append(value, "float4(0,0,0,0)");
    } else if (alpha) {
        g_string_append_printf(value, "%s.aaaa", name);
    } else {
        g_string_append(value, name);
    }
    if (mapping == PS_INPUTMAPPING_UNSIGNED_INVERT) {
        g_string_append_printf(s, "(1-saturate(%s))", value->str);
    } else {
        g_string_append_printf(s, "max(float4(0,0,0,0),%s)", value->str);
    }
    g_string_free(value, TRUE);
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
        condition = "result.a>=alphaRef/255.0";
        break;
    case ALPHA_FUNC_EQUAL:
        condition = "result.a!=alphaRef/255.0";
        break;
    case ALPHA_FUNC_LEQUAL:
        condition = "result.a>alphaRef/255.0";
        break;
    case ALPHA_FUNC_GREATER:
        condition = "result.a<=alphaRef/255.0";
        break;
    case ALPHA_FUNC_NOTEQUAL:
        condition = "result.a==alphaRef/255.0";
        break;
    case ALPHA_FUNC_GEQUAL:
        condition = "result.a<alphaRef/255.0";
        break;
    default:
        error_setg(errp, "D3D11: invalid NV2A alpha function %u",
                   state->alpha_func);
        return false;
    }
    g_string_append_printf(s, "  if(%s) discard;\n", condition);
    return true;
}

static char *d3d11_translate_pixel_combiners(const PshState *state,
                                             Error **errp)
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
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_cubemap[i]) {
            g_string_append_printf(s, "TextureCube tex%u:register(t%u);\n", i,
                                   i);
        } else if (state->dim_tex[i] == 3) {
            g_string_append_printf(s, "Texture3D tex%u:register(t%u);\n", i, i);
        } else {
            g_string_append_printf(s, "Texture2D tex%u:register(t%u);\n", i, i);
        }
        g_string_append_printf(s, "SamplerState samp%u:register(s%u);\n", i, i);
    }
    g_string_append(s, "float4 main(PSIn input):SV_Target {\n");
    g_string_append(
        s, "  float4 V0=input.frontFace?input.diffuse:input.backDiffuse;\n"
           "  float4 V1=input.frontFace?input.specular:input.backSpecular;\n"
           "  float4 "
           "pT0=input.tex0,pT1=input.tex1,pT2=input.tex2,pT3=input.tex3;\n"
           "  float4 T0=pT0,T1=pT1,T2=pT2,T3=pT3;\n"
           "  float4 R0=float4(0,0,0,T0.a),R1=0;\n"
           "  float4 C0=K[0],C1=K[1];\n"
           "  float4 FOG=float4(fogColor.rgb,saturate(input.fog));\n"
           "  float4 V1R0_SUM=0,EF_PROD=0;\n");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        unsigned int mode = (state->shader_stage_program >> (i * 5)) & 0x1f;
        if (mode == PS_TEXTUREMODES_NONE || mode == PS_TEXTUREMODES_PASSTHRU) {
            continue;
        }
        if (state->tex_cubemap[i]) {
            g_string_append_printf(s, "  T%u=tex%u.Sample(samp%u,pT%u.xyz);\n",
                                   i, i, i, i);
        } else if (state->dim_tex[i] == 3) {
            g_string_append_printf(
                s, "  T%u=tex%u.Sample(samp%u,pT%u.xyz/pT%u.w);\n", i, i, i, i,
                i);
        } else if (mode == PS_TEXTUREMODES_PROJECT2D ||
                   mode == PS_TEXTUREMODES_PROJECT3D) {
            g_string_append_printf(
                s, "  T%u=tex%u.Sample(samp%u,pT%u.xy/pT%u.w);\n", i, i, i, i,
                i);
        } else {
            g_string_append_printf(s, "  T%u=tex%u.Sample(samp%u,pT%u.xy);\n",
                                   i, i, i, i);
        }
        if (state->alphakill[i]) {
            g_string_append_printf(s, "  if(T%u.a==0) discard;\n", i);
        }
        if (state->snorm_tex[i]) {
            g_string_append_printf(s, "  T%u=T%u*2.0-1.0;\n", i, i);
        }
    }
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
        g_string_append(s, "  C0=K[16]; C1=K[17];\n  EF_PROD.rgb=");
        d3d11_ps_append_final_input(s, e, false);
        g_string_append(s, ".rgb*");
        d3d11_ps_append_final_input(s, f, false);
        g_string_append(s, ".rgb; EF_PROD.a=1;\n  float3 v1sum=V1.rgb; float3 "
                           "r0sum=R0.rgb;\n");
        if (settings & PS_FINALCOMBINERSETTING_COMPLEMENT_V1) {
            g_string_append(s, "  v1sum=1-v1sum;\n");
        }
        if (settings & PS_FINALCOMBINERSETTING_COMPLEMENT_R0) {
            g_string_append(s, "  r0sum=1-r0sum;\n");
        }
        g_string_append(s, "  V1R0_SUM=float4(v1sum+r0sum,1);\n");
        if (settings & PS_FINALCOMBINERSETTING_CLAMP_SUM) {
            g_string_append(s, "  V1R0_SUM.rgb=saturate(V1R0_SUM.rgb);\n");
        }
        const char names[] = { 'A', 'B', 'C', 'D' };
        for (unsigned int i = 0; i < 4; i++) {
            g_string_append_printf(s, "  float3 fc%c=", names[i]);
            d3d11_ps_append_final_input(
                s, d3d11_ps_input_byte(state->final_inputs_0, i), true);
            g_string_append(s, ".rgb;\n");
        }
        g_string_append(s, "  float fcG=");
        d3d11_ps_append_final_input(s, g, false);
        g_string_append(
            s, ".a;\n  float4 result; "
               "result.rgb=saturate(lerp(fcC,fcB,fcA)+fcD); result.a=fcG;\n");
    }
    if (!d3d11_ps_append_alpha_test(s, state, errp)) {
        g_string_free(s, TRUE);
        return NULL;
    }
    g_string_append(s, "  return result;\n}\n");
    return g_string_free(s, FALSE);
}

static PGRAPHD3D11PixelShader *d3d11_compile_pixel_shader(PGRAPHD3D11State *r,
                                                          const char *source,
                                                          Error **errp)
{
    ID3DBlob *bytecode = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(
        source, strlen(source), "nv2a_pixel.hlsl", NULL, NULL, "main", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &errors);
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
    float padding[3];
    float bump_mat[4][4];
    float bump_params[4][4];
    float clip_range[4];
    uint32_t color_key[4];
    uint32_t color_key_mask[4];
    int32_t clip_region[8][4];
    float tex_scale[4];
    float surface_scale[2];
    float depth_offset;
    float depth_factor;
} PGRAPHD3D11PixelConstants;

QEMU_BUILD_BUG_ON(sizeof(PGRAPHD3D11PixelConstants) !=
                  D3D11_PIXEL_CONSTANT_BUFFER_SIZE);

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
    constants.depth_factor = values.depthFactor[0];
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

bool pgraph_d3d11_bind_pixel_shader(PGRAPHState *pg, Error **errp)
{
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PshState state = { 0 };
    pgraph_glsl_set_psh_state(pg, &state);
    GBytes *key = g_bytes_new(&state, sizeof(state));
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
        char *source = d3d11_translate_pixel_combiners(&state, errp);
        if (!source) {
            if (g_hash_table_size(r->shaders->failed_pixel_cache) >=
                D3D11_SHADER_FAILURE_CACHE_LIMIT) {
                g_hash_table_remove_all(r->shaders->failed_pixel_cache);
            }
            g_hash_table_add(r->shaders->failed_pixel_cache, g_bytes_ref(key));
            g_bytes_unref(key);
            return false;
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
    if (!d3d11_update_pixel_constants(pg, errp)) {
        return false;
    }
    ID3D11DeviceContext_PSSetShader(r->context, shader->shader, NULL, 0);
    return true;
}
