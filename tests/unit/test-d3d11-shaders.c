/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the actual translator extracted by test-d3d11-shaders.py.
 * This does not start the emulator or a UWP application. */
#include "test-d3d11-pixel-generated.h"
#include <d3d11shader.h>
#include <math.h>

static void test_pixel_shader_profile(ID3D11Device *device, const char *source,
                                      const char *name, const char *profile)
{
    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), name, NULL, NULL, "main",
                            profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob,
                            &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "%s (%s): %s\n", name, profile,
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    ID3D11PixelShader *shader = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
    ID3D11PixelShader_Release(shader);
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
}

static void test_pixel_shader_4_x(ID3D11Device *device_10_0,
                                 ID3D11Device *device_10_1,
                                 const char *source, const char *name)
{
    test_pixel_shader_profile(device_10_0, source, name, "ps_4_0");
    test_pixel_shader_profile(device_10_1, source, name, "ps_4_1");
}

static void test_dual_source_blending(ID3D11Device *device)
{
    PshState state = { 0 };
    state.smooth_shading = true;
    state.final_inputs_0 = PS_REGISTER_V0;
    state.final_inputs_1 = (PS_REGISTER_V0 | PS_CHANNEL_ALPHA) << 8;
    for (unsigned int depth = 0; depth < 2; depth++) {
        state.surface_zeta_format = NV097_SET_SURFACE_FORMAT_ZETA_Z16;
        char *source = d3d11_translate_pixel_combiners_with_depth(
            &state, depth != 0, NULL);
        source = d3d11_ps_dual_source(source, depth != 0);
        test_pixel_shader_profile(device, source, "dual-source", "ps_4_0");
        g_free(source);
    }

    char *source = d3d11_ps_dual_source(
        d3d11_translate_pixel_combiners(&state, NULL), false);
    ID3DBlob *blob = NULL;
    assert(SUCCEEDED(D3DCompile(source, strlen(source), "dual-source-readback",
                               NULL, NULL, "main", "ps_4_0", 0, 0,
                               &blob, NULL)));
    g_free(source);
    ID3D11PixelShader *ps = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &ps)));
    ID3D10Blob_Release(blob);

    const char *vs_source =
        "struct O {float4 p:SV_Position;float4 d:COLOR0;};"
        "O main(uint id:SV_VertexID){O o;"
        "o.p=float4(id==2?3:-1,id==1?3:-1,0,1);"
        "o.d=float4(0.7,0.4,0.15,0.65);return o;}";
    assert(SUCCEEDED(D3DCompile(vs_source, strlen(vs_source), "dual-source-vs",
                               NULL, NULL, "main", "vs_4_0", 0, 0,
                               &blob, NULL)));
    ID3D11VertexShader *vs = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &vs)));
    ID3D10Blob_Release(blob);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11DeviceContext_ClearState(context);

    D3D11_TEXTURE2D_DESC td = {
        .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    ID3D11Texture2D *target = NULL, *staging = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &target)));
    ID3D11RenderTargetView *rtv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)target, NULL, &rtv)));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &staging)));
    PGRAPHD3D11PixelConstants constants = { 0 };
    constants.clip_region[0][2] = constants.clip_region[0][3] = 1;
    const float blend_color[4] = { 0.2f, 0.45f, 0.8f, 0.37f };
    memcpy(constants.blend_color, blend_color, sizeof(blend_color));
    D3D11_BUFFER_DESC bd = {
        .ByteWidth = sizeof(constants), .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    D3D11_SUBRESOURCE_DATA data = { .pSysMem = &constants };
    ID3D11Buffer *cb = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, &data, &cb)));
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &cb);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
    D3D11_VIEWPORT viewport = { .Width = 1, .Height = 1, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    const float src[4] = { 0.7f, 0.4f, 0.15f, 0.65f };
    const float dst[4] = { 0.1f, 0.5f, 0.6f, 0.25f };
    for (unsigned int a = 12; a < 16; a++) {
        for (unsigned int b = 12; b < 16; b++) {
            for (unsigned int op = 0; op < 5; op++) {
                D3D11_BLEND_OP operation = d3d11_blend_op(op);
                uint32_t blend = NV_PGRAPH_BLEND_EN;
                SET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR, a);
                SET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR, b);
                SET_MASK(blend, NV_PGRAPH_BLEND_EQN, op);
                bool dual = pgraph_d3d11_dual_source_blend(blend);
                assert(dual == d3d11_blend_factors_conflict(a, b, operation));
                assert(!pgraph_d3d11_dual_source_blend(
                    blend & ~NV_PGRAPH_BLEND_EN));
                assert(!pgraph_d3d11_dual_source_blend(
                    blend | NV_PGRAPH_BLEND_LOGICOP_ENABLE));
                if (!dual) {
                    continue;
                }
                D3D11_RENDER_TARGET_BLEND_DESC rt = {
                    .BlendEnable = TRUE,
                    .SrcBlend = d3d11_dual_source_factor(a),
                    .DestBlend = d3d11_dual_source_factor(b),
                    .BlendOp = operation, .BlendOpAlpha = operation,
                    .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
                };
                rt.SrcBlendAlpha = d3d11_alpha_blend_factor(rt.SrcBlend);
                rt.DestBlendAlpha = d3d11_alpha_blend_factor(rt.DestBlend);
                D3D11_BLEND_DESC desc = { 0 };
                desc.RenderTarget[0] = rt;
                ID3D11BlendState *bs = NULL;
                assert(SUCCEEDED(ID3D11Device_CreateBlendState(device, &desc, &bs)));
                float factor[4];
                memcpy(factor, blend_color, sizeof(factor));
                d3d11_adjust_blend_factor_constant_alpha(factor, a, b);
                assert(memcmp(factor, blend_color, sizeof(factor)) == 0);
                ID3D11DeviceContext_OMSetBlendState(context, bs, factor, UINT_MAX);
                ID3D11DeviceContext_ClearRenderTargetView(context, rtv, dst);
                ID3D11DeviceContext_Draw(context, 3, 0);
                ID3D11DeviceContext_CopyResource(context,
                    (ID3D11Resource *)staging, (ID3D11Resource *)target);
                D3D11_MAPPED_SUBRESOURCE mapped;
                assert(SUCCEEDED(ID3D11DeviceContext_Map(context,
                    (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
                const float *actual = mapped.pData;
                for (unsigned int c = 0; c < 4; c++) {
                    float sf = a < 14 ? blend_color[c] : blend_color[3];
                    float df = b < 14 ? blend_color[c] : blend_color[3];
                    if (a & 1) { sf = 1 - sf; }
                    if (b & 1) { df = 1 - df; }
                    float expected = operation == D3D11_BLEND_OP_ADD ?
                        src[c] * sf + dst[c] * df :
                        operation == D3D11_BLEND_OP_SUBTRACT ?
                            src[c] * sf - dst[c] * df :
                            dst[c] * df - src[c] * sf;
                    assert(fabsf(actual[c] - expected) < 0.00001f);
                }
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
                ID3D11BlendState_Release(bs);
            }
        }
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11Buffer_Release(cb);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(staging);
    ID3D11PixelShader_Release(ps);
    ID3D11VertexShader_Release(vs);
    ID3D11DeviceContext_Release(context);
    puts("PASS: mixed constant color/alpha, inversions, add/subtract/reverse, WARP RGBA readback");
}

static void test_bordered_cube_sampling(ID3D11Device *device)
{
    PshState state = { 0 };
    state.smooth_shading = true;
    state.shader_stage_program = PS_TEXTUREMODES_CUBEMAP;
    state.tex_cubemap[0] = true;
    state.dim_tex[0] = 2;
    state.border_logical_size[0][0] = state.border_logical_size[0][1] = 16;
    state.border_inv_real_size[0][0] = state.border_inv_real_size[0][1] =
        1.0f / 32;
    state.final_inputs_0 = PS_REGISTER_T0;
    state.final_inputs_1 = (PS_REGISTER_T0 | PS_CHANNEL_ALPHA) << 8;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "nv2a_cube_border"));
    ID3DBlob *blob = NULL;
    assert(
        SUCCEEDED(D3DCompile(source, strlen(source), "bordered-cube-ps", NULL,
                             NULL, "main", "ps_4_0", 0, 0, &blob, NULL)));
    g_free(source);
    ID3D11PixelShader *ps = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &ps)));
    ID3D10Blob_Release(blob);
    const char *vs_source =
        "cbuffer Params:register(b0){float4 direction;float4 dx;float4 dy;}"
        "struct O{float4 p:SV_Position;float4 d:COLOR0;float4 s:COLOR1;"
        "float4 bd:TEXCOORD4;float4 bs:TEXCOORD5;float f:FOG;float "
        "ps:TEXCOORD12;"
        "float4 t:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;"
        "float4 t3:TEXCOORD3;nointerpolation float4 z0:TEXCOORD6;"
        "nointerpolation float4 z1:TEXCOORD7;nointerpolation float4 "
        "z2:TEXCOORD8;"
        "nointerpolation float mzp:TEXCOORD9;nointerpolation float "
        "mza:TEXCOORD10;"
        "float2 pointCoord:TEXCOORD11;};"
        "O main(uint id:SV_VertexID){O o=(O)0;"
        "o.p=float4(id==2?3:-1,id==1?3:-1,0,1);"
        "o.t=direction+o.p.x*0.5*dx-o.p.y*0.5*dy;return o;}";
    assert(
        SUCCEEDED(D3DCompile(vs_source, strlen(vs_source), "bordered-cube-vs",
                             NULL, NULL, "main", "vs_4_0", 0, 0, &blob, NULL)));
    ID3D11VertexShader *vs = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &vs)));
    ID3D10Blob_Release(blob);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11DeviceContext_ClearState(context);

    enum { levels = 5 };
    D3D11_SUBRESOURCE_DATA initial[6 * levels];
    float *pixels[6 * levels];
    for (unsigned int face = 0; face < 6; face++) {
        for (unsigned int level = 0, size = 32; level < levels;
             level++, size >>= 1) {
            unsigned int index = face * levels + level;
            pixels[index] = g_new(float, size *size * 4);
            for (unsigned int y = 0; y < size; y++) {
                for (unsigned int x = 0; x < size; x++) {
                    float *p = pixels[index] + (y * size + x) * 4;
                    p[0] = (float)x / size;
                    p[1] = (float)y / size;
                    p[2] = (float)face / 8;
                    p[3] = (float)level / 8;
                }
            }
            initial[index] = (D3D11_SUBRESOURCE_DATA){
                .pSysMem = pixels[index],
                .SysMemPitch = size * 4 * sizeof(float),
            };
        }
    }
    D3D11_TEXTURE2D_DESC td = {
        .Width = 32,
        .Height = 32,
        .MipLevels = levels,
        .ArraySize = 6,
        .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE,
    };
    ID3D11Texture2D *cube = NULL;
    assert(
        SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, initial, &cube)));
    for (unsigned int i = 0; i < ARRAY_SIZE(pixels); i++) {
        g_free(pixels[i]);
    }
    ID3D11ShaderResourceView *srv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateShaderResourceView(
        device, (ID3D11Resource *)cube, NULL, &srv)));
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &srv);
    td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &target)));
    ID3D11RenderTargetView *rtv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)target, NULL, &rtv)));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(
        SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &staging)));
    PGRAPHD3D11PixelConstants constants = { 0 };
    constants.clip_region[0][2] = constants.clip_region[0][3] = 1;
    D3D11_BUFFER_DESC bd = {
        .ByteWidth = sizeof(constants),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    D3D11_SUBRESOURCE_DATA data = { .pSysMem = &constants };
    ID3D11Buffer *cb = NULL, *params = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, &data, &cb)));
    bd.ByteWidth = 12 * sizeof(float);
    bd.Usage = D3D11_USAGE_DEFAULT;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, NULL, &params)));
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &cb);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &params);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport = { .Width = 1, .Height = 1, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    const float coords[] = { 0.25f, 0.5f, 0.875f };
    for (unsigned int filter = 0; filter < 2; filter++) {
        for (unsigned int level = 0; level <= levels + 2; level++) {
            bool implicit_lod = level == levels;
            bool fractional_lod = level == levels + 1;
            bool shifted_view = level == levels + 2;
            ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &srv);
            if (shifted_view) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd = {
                    .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                    .ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
                    .TextureCube = { .MostDetailedMip = 1,
                                     .MipLevels = levels - 1 },
                };
                ID3D11ShaderResourceView *shifted = NULL;
                assert(SUCCEEDED(ID3D11Device_CreateShaderResourceView(
                    device, (ID3D11Resource *)cube, &vd, &shifted)));
                ID3D11DeviceContext_PSSetShaderResources(context, 0, 1,
                                                         &shifted);
                ID3D11ShaderResourceView_Release(shifted);
            }
            float lod = shifted_view ? 0 : fractional_lod ? 1.75f : level;
            D3D11_SAMPLER_DESC sd = {
                .Filter = filter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR :
                                   D3D11_FILTER_MIN_MAG_MIP_POINT,
                .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
                .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
                .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
                .MinLOD = implicit_lod ? 0 : lod,
                .MaxLOD = implicit_lod ? levels - 1 : lod,
            };
            ID3D11SamplerState *sampler = NULL;
            assert(SUCCEEDED(
                ID3D11Device_CreateSamplerState(device, &sd, &sampler)));
            ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &sampler);
            for (unsigned int face = 0; face < 6; face++) {
                for (unsigned int c = 0; c < ARRAY_SIZE(coords); c++) {
                    if (implicit_lod && c != 1) {
                        continue;
                    }
                    float u = coords[c], v = coords[ARRAY_SIZE(coords) - 1 - c];
                    float x = u * 2 - 1, y = v * 2 - 1;
                    float directions[6][3] = {
                        { 1, -y, -x }, { -1, -y, x }, { x, 1, y },
                        { x, -1, -y }, { x, -y, 1 },  { -x, -y, -1 },
                    };
                    float values[12] = { 0 };
                    memcpy(values, directions[face], 3 * sizeof(float));
                    values[3] = 1;
                    if (implicit_lod) {
                        const float dx[6][3] = {
                            { 0, 0, -0.5f }, { 0, 0, 0.5f }, { 0.5f, 0, 0 },
                            { 0.5f, 0, 0 },  { 0.5f, 0, 0 }, { -0.5f, 0, 0 },
                        };
                        const float dy[6][3] = {
                            { 0, -0.5f, 0 }, { 0, -0.5f, 0 }, { 0, 0, 0.5f },
                            { 0, 0, -0.5f }, { 0, -0.5f, 0 }, { 0, -0.5f, 0 },
                        };
                        memcpy(values + 4, dx[face], 3 * sizeof(float));
                        memcpy(values + 8, dy[face], 3 * sizeof(float));
                    }
                    ID3D11DeviceContext_UpdateSubresource(
                        context, (ID3D11Resource *)params, 0, NULL, values, 0,
                        0);
                    ID3D11DeviceContext_Draw(context, 3, 0);
                    ID3D11DeviceContext_CopyResource(context,
                                                     (ID3D11Resource *)staging,
                                                     (ID3D11Resource *)target);
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    assert(SUCCEEDED(ID3D11DeviceContext_Map(
                        context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ,
                        0, &mapped)));
                    unsigned int sampled_level =
                        shifted_view   ? 1 :
                        implicit_lod   ? 2 :
                        fractional_lod ? (filter ? 1 : 2) :
                                         level;
                    unsigned int size = 32 >> sampled_level;
                    float expected[4] = {
                        ((u * 16 + 4) / 32) * size,
                        ((v * 16 + 4) / 32) * size,
                        (float)face / 8,
                        (float)sampled_level / 8,
                    };
                    for (unsigned int k = 0; k < 2; k++) {
                        expected[k] =
                            filter ? expected[k] - 0.5f : floorf(expected[k]);
                        expected[k] = CLAMP(expected[k], 0, size - 1) / size;
                    }
                    if (fractional_lod && filter) {
                        expected[0] = ((u * 16 + 4) / 32) -
                                      0.5f * (0.25f / 16 + 0.75f / 8);
                        expected[1] = ((v * 16 + 4) / 32) -
                                      0.5f * (0.25f / 16 + 0.75f / 8);
                        expected[3] = 1.75f / 8;
                    }
                    const float *actual = mapped.pData;
                    for (unsigned int k = 0; k < 4; k++) {
                        if (fabsf(actual[k] - expected[k]) >= 0.0001f) {
                            fprintf(stderr,
                                    "cube filter=%u mip=%u face=%u coord=%u "
                                    "channel=%u actual=%g expected=%g\n",
                                    filter, level, face, c, k, actual[k],
                                    expected[k]);
                        }
                        assert(fabsf(actual[k] - expected[k]) < 0.0001f);
                    }
                    ID3D11DeviceContext_Unmap(context,
                                              (ID3D11Resource *)staging, 0);
                }
            }
            ID3D11SamplerState_Release(sampler);
        }
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11Buffer_Release(cb);
    ID3D11Buffer_Release(params);
    ID3D11Texture2D_Release(cube);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(staging);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11VertexShader_Release(vs);
    ID3D11PixelShader_Release(ps);
    ID3D11DeviceContext_Release(context);
    puts("PASS: bordered cube six-face orientation, point/linear/trilinear, "
         "all mips, SRV min mip and derivative LOD, WARP pixels");
}

static void test_depth_geometry_profile(ID3D11Device *device,
                                        const char *source,
                                        const char *profile)
{
    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), "nv2a-depth-geometry", NULL,
                            NULL, "main", profile,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "depth GS (%s): %s\n", profile,
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    ID3D11GeometryShader *shader = NULL;
    hr = ID3D11Device_CreateGeometryShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &shader);
    assert(SUCCEEDED(hr));
    ID3D11GeometryShader_Release(shader);
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
}

static void test_presentation_shader_profile(ID3D11Device *device,
                                             const char *entry,
                                             const char *profile)
{
    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(d3d11_presentation_shader,
                            strlen(d3d11_presentation_shader),
                            "d3d11-presentation-test", NULL, NULL, entry,
                            profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob,
                            &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "presentation %s (%s): %s\n", entry, profile,
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    if (strcmp(entry, "vs_main") == 0) {
        ID3D11VertexShader *shader = NULL;
        hr = ID3D11Device_CreateVertexShader(
            device, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), NULL, &shader);
        assert(SUCCEEDED(hr));
        ID3D11VertexShader_Release(shader);
    } else {
        ID3D11PixelShader *shader = NULL;
        hr = ID3D11Device_CreatePixelShader(
            device, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), NULL, &shader);
        assert(SUCCEEDED(hr));
        ID3D11ShaderReflection *reflection = NULL;
        hr = D3DReflect(ID3D10Blob_GetBufferPointer(blob),
                        ID3D10Blob_GetBufferSize(blob),
                        &IID_ID3D11ShaderReflection, (void **)&reflection);
        assert(SUCCEEDED(hr));
        ID3D11ShaderReflectionConstantBuffer *buffer =
            reflection->lpVtbl->GetConstantBufferByName(
                reflection, "PresentationConstants");
        assert(buffer);
        D3D11_SHADER_BUFFER_DESC desc;
        hr = buffer->lpVtbl->GetDesc(buffer, &desc);
        assert(SUCCEEDED(hr));
        assert(desc.Size == 96);
        reflection->lpVtbl->Release(reflection);
        ID3D11PixelShader_Release(shader);
    }
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
}

static void test_presentation_shaders(ID3D11Device *device_10_0,
                                      ID3D11Device *device_10_1)
{
    test_presentation_shader_profile(device_10_0, "vs_main", "vs_4_0");
    test_presentation_shader_profile(device_10_0, "ps_main", "ps_4_0");
    test_presentation_shader_profile(device_10_1, "vs_main", "vs_4_1");
    test_presentation_shader_profile(device_10_1, "ps_main", "ps_4_1");
}

static void test_presentation_scanout_row_mapping(ID3D11Device *device)
{
    ID3DBlob *vertex_blob = NULL, *pixel_blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(
        d3d11_presentation_shader, strlen(d3d11_presentation_shader),
        "d3d11-presentation-row-map", NULL, NULL, "vs_main", "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex_blob, &errors);
    assert(SUCCEEDED(hr));
    if (errors) {
        ID3D10Blob_Release(errors);
        errors = NULL;
    }
    hr = D3DCompile(
        d3d11_presentation_shader, strlen(d3d11_presentation_shader),
        "d3d11-presentation-row-map", NULL, NULL, "ps_main", "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel_blob, &errors);
    assert(SUCCEEDED(hr));
    if (errors) {
        ID3D10Blob_Release(errors);
    }

    ID3D11VertexShader *vertex_shader = NULL;
    ID3D11PixelShader *pixel_shader = NULL;
    hr = ID3D11Device_CreateVertexShader(
        device, ID3D10Blob_GetBufferPointer(vertex_blob),
        ID3D10Blob_GetBufferSize(vertex_blob), NULL, &vertex_shader);
    assert(SUCCEEDED(hr));
    hr = ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(pixel_blob),
        ID3D10Blob_GetBufferSize(pixel_blob), NULL, &pixel_shader);
    assert(SUCCEEDED(hr));
    ID3D10Blob_Release(vertex_blob);
    ID3D10Blob_Release(pixel_blob);

    const uint8_t source_pixels[] = {
        255, 0,   0,   255, 0,   255, 0,   255,
        0,   0,   255, 255, 255, 255, 255, 255,
    };
    D3D11_TEXTURE2D_DESC source_desc = {
        .Width = 1,
        .Height = 4,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    D3D11_SUBRESOURCE_DATA source_data = {
        .pSysMem = source_pixels,
        .SysMemPitch = 4,
        .SysMemSlicePitch = sizeof(source_pixels),
    };
    ID3D11Texture2D *source_texture = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &source_desc, &source_data,
                                      &source_texture);
    assert(SUCCEEDED(hr));
    ID3D11ShaderResourceView *source_view = NULL;
    hr = ID3D11Device_CreateShaderResourceView(
        device, (ID3D11Resource *)source_texture, NULL, &source_view);
    assert(SUCCEEDED(hr));

    D3D11_TEXTURE2D_DESC output_desc = source_desc;
    output_desc.Height = 4;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    output_desc.CPUAccessFlags = 0;
    ID3D11Texture2D *output_texture = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &output_desc, NULL,
                                      &output_texture);
    assert(SUCCEEDED(hr));
    ID3D11RenderTargetView *output_view = NULL;
    hr = ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)output_texture, NULL, &output_view);
    assert(SUCCEEDED(hr));

    float constants[24] = { 0 };
    constants[2] = 1.0f;
    constants[3] = 2.0f;
    constants[4] = 1.0f;
    constants[5] = 2.0f;
    constants[6] = 0.5f;
    constants[7] = 1.0f;
    D3D11_BUFFER_DESC constants_desc = {
        .ByteWidth = sizeof(constants),
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    D3D11_SUBRESOURCE_DATA constants_data = { .pSysMem = constants };
    ID3D11Buffer *constant_buffer = NULL;
    hr = ID3D11Device_CreateBuffer(device, &constants_desc, &constants_data,
                                   &constant_buffer);
    assert(SUCCEEDED(hr));
    D3D11_SAMPLER_DESC sampler_desc = {
        .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    ID3D11SamplerState *sampler = NULL;
    hr = ID3D11Device_CreateSamplerState(device, &sampler_desc, &sampler);
    assert(SUCCEEDED(hr));

    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    D3D11_VIEWPORT viewport = { .Width = 1, .Height = 2, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &output_view, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &source_view);
    ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &constant_buffer);
    ID3D11DeviceContext_Draw(context, 3, 0);

    D3D11_TEXTURE2D_DESC staging_desc = output_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)output_texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t expected_top[] = { 255, 0, 0, 255 };
    const uint8_t expected_bottom[] = { 0, 255, 0, 255 };
    assert(memcmp(mapped.pData, expected_top, sizeof(expected_top)) == 0);
    assert(memcmp((uint8_t *)mapped.pData + mapped.RowPitch, expected_bottom,
                  sizeof(expected_bottom)) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);

    /* A doubled interlace display height should consume the full source field. */
    constants[5] = 4.0f;
    constants[6] = 1.0f;
    ID3D11DeviceContext_UpdateSubresource(
        context, (ID3D11Resource *)constant_buffer, 0, NULL, constants, 0, 0);
    viewport.Height = 4;
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_Draw(context, 3, 0);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)output_texture);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t expected_rows[][4] = {
        { 255, 0, 0, 255 }, { 0, 255, 0, 255 },
        { 0, 0, 255, 255 }, { 255, 255, 255, 255 },
    };
    for (unsigned int row = 0; row < ARRAY_SIZE(expected_rows); row++) {
        assert(memcmp((uint8_t *)mapped.pData + row * mapped.RowPitch,
                      expected_rows[row], sizeof(expected_rows[row])) == 0);
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11DeviceContext_ClearState(context);
    ID3D11Texture2D_Release(staging);
    ID3D11SamplerState_Release(sampler);
    ID3D11Buffer_Release(constant_buffer);
    ID3D11RenderTargetView_Release(output_view);
    ID3D11Texture2D_Release(output_texture);
    ID3D11ShaderResourceView_Release(source_view);
    ID3D11Texture2D_Release(source_texture);
    ID3D11PixelShader_Release(pixel_shader);
    ID3D11VertexShader_Release(vertex_shader);
    ID3D11DeviceContext_Release(context);
    puts("PASS: WARP scanout readback honors pitch ratio and full-height mapping");
}

static void test_polygon_point_readback(ID3D11Device *device)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11DeviceContext_ClearState(context);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 32, .Height = 32, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    ID3D11Texture2D *target = NULL, *staging = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target)));
    ID3D11RenderTargetView *rtv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)target, NULL, &rtv)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    float constants[D3D11_VERTEX_CONSTANT_BUFFER_SIZE / sizeof(float)] = { 0 };
    constants[ARRAY_SIZE(constants) - 4] = 32;
    constants[ARRAY_SIZE(constants) - 3] = 32;
    D3D11_BUFFER_DESC bd = {
        .ByteWidth = sizeof(constants), .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    D3D11_SUBRESOURCE_DATA data = { .pSysMem = constants };
    ID3D11Buffer *cb = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, &data, &cb)));
    ID3D11DeviceContext_GSSetConstantBuffers(context, 0, 1, &cb);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    D3D11_VIEWPORT viewport = { .Width = 32, .Height = 32, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    D3D11_RASTERIZER_DESC raster_desc = {
        .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE,
    };
    ID3D11RasterizerState *raster = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &raster_desc, &raster)));
    ID3D11DeviceContext_RSSetState(context, raster);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const unsigned int coordinates[5][2] = {
        { 4, 4 }, { 24, 4 }, { 28, 16 }, { 16, 28 }, { 4, 24 },
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        assert(d3d11_depth_geometry_variant_for(
            PRIM_TYPE_POLYGON, smooth, false, POLY_MODE_POINT) ==
                PGRAPHD3D11_POLYGON_POINT_GS_VARIANT + smooth);
        char *source = d3d11_depth_geometry_shader_source(
            smooth, 0, PRIM_TYPE_POLYGON, POLY_MODE_POINT, false);
        assert(strstr(source, "if(primitiveId!=0 && i<2) continue;"));
        ID3DBlob *blob = NULL;
        assert(SUCCEEDED(D3DCompile(source, strlen(source), "polygon-point-gs",
            NULL, NULL, "main", "gs_4_0", 0, 0, &blob, NULL)));
        ID3D11GeometryShader *gs = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateGeometryShader(device,
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &gs)));
        ID3D10Blob_Release(blob);
        char *input = g_strndup(source, strstr(source, "struct GSOut") - source);
        char *vs_source = g_strconcat(input,
            "GSIn main(uint id:SV_VertexID){GSIn o=(GSIn)0;"
            "uint indices[9]={0,1,2,0,2,3,0,3,4};uint v=indices[id];"
            "float2 p[5]={float2(4.5,4.5),float2(24.5,4.5),float2(28.5,16.5),"
            "float2(16.5,28.5),float2(4.5,24.5)};"
            "o.position=float4(p[v].x/16-1,1-p[v].y/16,0,1);"
            "o.depthPos0=float4(p[v],0,1);o.pointSize=1;"
            "o.diffuse=float4((v+1)*0.1,0,0,1);return o;}", NULL);
        assert(SUCCEEDED(D3DCompile(vs_source, strlen(vs_source), "polygon-point-vs",
            NULL, NULL, "main", "vs_4_0", 0, 0, &blob, NULL)));
        ID3D11VertexShader *vs = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &vs)));
        ID3D10Blob_Release(blob);
        const char *output_start = strstr(source, "struct GSOut");
        char *output = g_strndup(output_start,
            strstr(output_start, "};") + 2 - output_start);
        char *ps_source = g_strconcat(output,
            "float4 main(GSOut input):SV_Target0{return input.diffuse;}", NULL);
        assert(SUCCEEDED(D3DCompile(ps_source, strlen(ps_source), "polygon-point-ps",
            NULL, NULL, "main", "ps_4_0", 0, 0, &blob, NULL)));
        ID3D11PixelShader *ps = NULL;
        assert(SUCCEEDED(ID3D11Device_CreatePixelShader(device,
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &ps)));
        ID3D10Blob_Release(blob);
        ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
        ID3D11DeviceContext_GSSetShader(context, gs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
        const float black[4] = { 0 };
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, black);
        D3D11_QUERY_DESC query_desc = { .Query = D3D11_QUERY_PIPELINE_STATISTICS };
        ID3D11Query *query = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateQuery(device, &query_desc, &query)));
        ID3D11DeviceContext_Begin(context, (ID3D11Asynchronous *)query);
        ID3D11DeviceContext_Draw(context, 9, 0);
        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
        ID3D11DeviceContext_CopyResource(context,
            (ID3D11Resource *)staging, (ID3D11Resource *)target);
        D3D11_MAPPED_SUBRESOURCE mapped;
        assert(SUCCEEDED(ID3D11DeviceContext_Map(context,
            (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
        for (unsigned int v = 0; v < 5; v++) {
            const float *pixel = (const float *)((const uint8_t *)mapped.pData +
                mapped.RowPitch * coordinates[v][1]) + 4 * coordinates[v][0];
            float expected = smooth ? (v + 1) * 0.1f : 0.1f;
            assert(fabsf(pixel[0] - expected) < 0.00001f);
            assert(pixel[3] == 1.0f);
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
        assert(ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query,
            &stats, sizeof(stats), 0) == S_OK);
        assert(stats.GSPrimitives == 10); /* five points, two triangles each */
        ID3D11Query_Release(query);
        ID3D11GeometryShader_Release(gs);
        ID3D11VertexShader_Release(vs);
        ID3D11PixelShader_Release(ps);
        g_free(source);
        g_free(input);
        g_free(output);
        g_free(vs_source);
        g_free(ps_source);
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11RasterizerState_Release(raster);
    ID3D11Buffer_Release(cb);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(staging);
    ID3D11DeviceContext_Release(context);
    puts("PASS: polygon POINT flat/smooth pixel readback, five unique points, WARP pipeline statistics");
}

static void test_custom_depth_shaders(ID3D11Device *device_11_0,
                                      ID3D11Device *device_10_0,
                                      ID3D11Device *device_10_1)
{
    bool supports_shader_model_5 =
        ID3D11Device_GetFeatureLevel(device_11_0) >= D3D_FEATURE_LEVEL_11_0;
    static const enum ShaderPrimitiveMode triangle_modes[] = {
        PRIM_TYPE_TRIANGLES,
        PRIM_TYPE_TRIANGLE_STRIP,
        PRIM_TYPE_TRIANGLE_FAN,
        PRIM_TYPE_POLYGON,
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(triangle_modes); i++) {
        assert(d3d11_primitive_supports_custom_depth(triangle_modes[i]));
    }
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_QUADS));
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_QUAD_STRIP));
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_FILL) == 8);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, true, false, POLY_MODE_FILL) == 9);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, false, false, POLY_MODE_FILL) == 10);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, true, true, POLY_MODE_FILL) == 11);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_LINE) == 16);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, true, false, POLY_MODE_LINE) == 17);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, false, false, POLY_MODE_LINE) == 18);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, true, true, POLY_MODE_LINE) == 19);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, false, true, POLY_MODE_POINT) == 20);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUADS, true, false, POLY_MODE_POINT) == 21);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, false, false, POLY_MODE_POINT) == 22);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_QUAD_STRIP, true, true, POLY_MODE_POINT) == 23);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, false, true, POLY_MODE_POINT) == 24);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_FAN, false, false, POLY_MODE_POINT) == 25);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, true, false, POLY_MODE_POINT) == 30);
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
    assert(!d3d11_triangle_line_primitive(PRIM_TYPE_POLYGON));
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLES, false, true, POLY_MODE_FILL) == 0);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_FAN, false, false, POLY_MODE_FILL) == 1);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_TRIANGLE_STRIP, true, false, POLY_MODE_FILL) == 7);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_LINES, false, true, POLY_MODE_FILL) == 12);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_LINES, false, false, POLY_MODE_FILL) == 13);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_LINE_STRIP, true, true, POLY_MODE_FILL) == 14);
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_LINE_LOOP, true, false, POLY_MODE_FILL) == 15);
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_POINTS));
    assert(!d3d11_custom_depth_needs_geometry_shader(PRIM_TYPE_POINTS));
    assert(d3d11_depth_geometry_variant_for(
               PRIM_TYPE_POINTS, false, true, POLY_MODE_FILL) ==
           PGRAPHD3D11_POINT_GS_VARIANT);
    assert(d3d11_custom_depth_needs_geometry_shader(PRIM_TYPE_LINES));
    assert(d3d11_custom_depth_needs_geometry_shader(PRIM_TYPE_TRIANGLES));
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_LINES));
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_LINE_STRIP));
    assert(d3d11_primitive_supports_custom_depth(PRIM_TYPE_LINE_LOOP));
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINES, POLY_MODE_FILL, 1));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_LINES, POLY_MODE_FILL, 2));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_TRIANGLES,
                                         POLY_MODE_LINE, 2));
    assert(pgraph_d3d11_needs_wide_lines(PRIM_TYPE_QUADS,
                                         POLY_MODE_LINE, 4));
    assert(!pgraph_d3d11_needs_wide_lines(PRIM_TYPE_POINTS,
                                          POLY_MODE_LINE, 4));

    char *point_source = d3d11_depth_geometry_shader_source(
        true, 2, PRIM_TYPE_POINTS, POLY_MODE_FILL, false);
    assert(point_source && strstr(point_source, "point GSIn input[1]"));
    assert(strstr(point_source, "TriangleStream<GSOut> stream"));
    assert(strstr(point_source, "pointCoord:TEXCOORD11"));
    test_depth_geometry_profile(device_10_0, point_source, "gs_4_0");
    test_depth_geometry_profile(device_10_1, point_source, "gs_4_1");
    if (supports_shader_model_5) {
        test_depth_geometry_profile(device_11_0, point_source, "gs_5_0");
    }
    g_free(point_source);

    PshState point_sprite_state = { .point_sprite = true };
    point_sprite_state.shader_stage_program =
        PS_TEXTUREMODES_PROJECT2D << (3 * 5);
    point_sprite_state.dim_tex[3] = 2;
    char *point_sprite_pixel =
        d3d11_translate_pixel_combiners(&point_sprite_state, NULL);
    assert(point_sprite_pixel &&
           strstr(point_sprite_pixel,
                  "pT3=float4(input.pointCoord,1.0,1.0)"));
    assert(strstr(point_sprite_pixel,
                  "NV2A_SAMPLE2(tex3,samp3,3,pT3.xy/pT3.w*texCoordScale[3].xy)"));
    test_pixel_shader_4_x(device_10_0, device_10_1, point_sprite_pixel,
                          "point-sprite-texcoord");
    if (supports_shader_model_5) {
        test_pixel_shader_profile(device_11_0, point_sprite_pixel,
                                  "point-sprite-texcoord", "ps_5_0");
    }
    g_free(point_sprite_pixel);

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            char *source = d3d11_depth_geometry_shader_source(
                smooth != 0, provoking, PRIM_TYPE_TRIANGLES, POLY_MODE_FILL,
                false);
            assert(source);
            test_depth_geometry_profile(device_10_0, source, "gs_4_0");
            test_depth_geometry_profile(device_10_1, source, "gs_4_1");
            if (supports_shader_model_5) {
                test_depth_geometry_profile(device_11_0, source, "gs_5_0");
            }
            g_free(source);
        }
    }

    static const enum ShaderPrimitiveMode quad_modes[] = {
        PRIM_TYPE_QUADS,
        PRIM_TYPE_QUAD_STRIP,
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int i = 0; i < ARRAY_SIZE(quad_modes); i++) {
            char *source = d3d11_depth_geometry_shader_source(
                smooth != 0, 2, quad_modes[i], POLY_MODE_FILL, false);
            assert(source && strstr(source, "lineadj GSIn input[4]"));
            if (!smooth) {
                assert(strstr(source, "input[3]"));
                assert(strstr(source, "flatVertex.diffuse"));
            }
            if (quad_modes[i] == PRIM_TYPE_QUADS) {
                assert(strstr(source, "p11=input[2].depthPos0"));
                assert(strstr(source, "input[1],input[3]"));
                assert(strstr(source, "input[2],input[3]"));
                assert(!strstr(source, "primitiveId&1u"));
            } else {
                assert(strstr(source, "p00=input[2].depthPos0"));
                assert(strstr(source, "p01=input[0].depthPos0"));
                assert(strstr(source, "p02=input[1].depthPos0"));
                assert(strstr(source, "input[0],input[3]"));
                assert(strstr(source, "input[2],input[3]"));
                assert(strstr(source, "primitiveId&1u"));
            }
            test_depth_geometry_profile(device_10_0, source, "gs_4_0");
            test_depth_geometry_profile(device_10_1, source, "gs_4_1");
            if (supports_shader_model_5) {
                test_depth_geometry_profile(device_11_0, source, "gs_5_0");
            }
            g_free(source);
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int i = 0; i < ARRAY_SIZE(quad_modes); i++) {
            char *source = d3d11_depth_geometry_shader_source(
                smooth != 0, 2, quad_modes[i], POLY_MODE_LINE, false);
            assert(source && strstr(source, "LineStream<GSOut> stream"));
            assert(strstr(source, "[maxvertexcount(8)]"));
            assert(strstr(source, "nv2a_make_quad_vertex"));
            if (quad_modes[i] == PRIM_TYPE_QUADS) {
                assert(strstr(source, "input[0],input[3]"));
                assert(strstr(source, "input[1],input[3]"));
                assert(strstr(source, "input[2],input[3]"));
                assert(strstr(source, "input[3],input[3]"));
                assert(!strstr(source, "TriangleStream<GSOut> stream"));
            } else {
                assert(strstr(source, "primitiveId&1u"));
                assert(strstr(source, "input[0],input[3]"));
                assert(strstr(source, "input[1],input[3]"));
                assert(strstr(source, "input[3],input[3]"));
                assert(strstr(source, "input[2],input[3]"));
            }
            test_depth_geometry_profile(device_10_0, source, "gs_4_0");
            test_depth_geometry_profile(device_10_1, source, "gs_4_1");
            if (supports_shader_model_5) {
                test_depth_geometry_profile(device_11_0, source, "gs_5_0");
            }
            g_free(source);
            char *wide_source = d3d11_depth_geometry_shader_source(
                smooth != 0, 2, quad_modes[i], POLY_MODE_LINE, true);
            assert(wide_source &&
                   strstr(wide_source, "TriangleStream<GSOut> stream"));
            assert(strstr(wide_source, "[maxvertexcount(16)]"));
            test_depth_geometry_profile(device_10_0, wide_source, "gs_4_0");
            test_depth_geometry_profile(device_10_1, wide_source, "gs_4_1");
            if (supports_shader_model_5) {
                test_depth_geometry_profile(device_11_0, wide_source, "gs_5_0");
            }
            g_free(wide_source);
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int i = 0; i < ARRAY_SIZE(quad_modes); i++) {
            char *source = d3d11_depth_geometry_shader_source(
                smooth != 0, 2, quad_modes[i], POLY_MODE_POINT, false);
            assert(source && strstr(source, "TriangleStream<GSOut> stream"));
            assert(strstr(source, "[maxvertexcount(16)]"));
            assert(strstr(source, "nv2a_make_point_vertex("));
            assert(!strstr(source, "LineStream<GSOut> stream"));
            assert(strstr(source, "input[0],input[3]"));
            assert(strstr(source, "input[1],input[3]"));
            assert(strstr(source, "input[2],input[3]"));
            assert(strstr(source, "input[3],input[3]"));
            assert(strstr(source,
                          "input[0].depthPos0,input[0].depthPos0,input[0].depthPos0"));
            if (!smooth) {
                assert(strstr(source, "flatVertex.diffuse"));
            }
            if (quad_modes[i] == PRIM_TYPE_QUAD_STRIP) {
                assert(strstr(source, "primitiveId&1u"));
            }
            test_depth_geometry_profile(device_10_0, source, "gs_4_0");
            test_depth_geometry_profile(device_10_1, source, "gs_4_1");
            if (supports_shader_model_5) {
                test_depth_geometry_profile(device_11_0, source, "gs_5_0");
            }
            g_free(source);
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            for (unsigned int i = 0; i < ARRAY_SIZE(triangle_modes); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangle_modes[i],
                    POLY_MODE_POINT, false);
                assert(source && strstr(source, "TriangleStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(12)]"));
                assert(strstr(source, "stream.RestartStrip()"));
                assert(strstr(source, "stream.Append(nv2a_make_point_vertex(v,"));
                assert(strstr(source,
                              "v.depthPos1=input[i].depthPos0; v.depthPos2="
                              "input[i].depthPos0;"));
                if (!smooth) {
                    assert(strstr(source, "v.diffuse=flatDiffuse"));
                    assert(strstr(source, "flatDiffuse=p=="));
                } else {
                    assert(strstr(source, "v.diffuse=input[i].diffuse"));
                }
                if (provoking == 0) {
                    assert(strstr(source, "uint p=0u"));
                } else if (provoking == 1) {
                    assert(strstr(source, "uint p=1u"));
                } else if (provoking == 2) {
                    assert(strstr(source, "uint p=2u"));
                } else {
                    assert(strstr(source,
                                  "uint p=(2u-(primitiveId&1u))"));
                }
                test_depth_geometry_profile(device_10_0, source, "gs_4_0");
                test_depth_geometry_profile(device_10_1, source, "gs_4_1");
                if (supports_shader_model_5) {
                    test_depth_geometry_profile(device_11_0, source, "gs_5_0");
                }
                g_free(source);
            }
        }
    }

    static const enum ShaderPrimitiveMode triangle_line_modes[] = {
        PRIM_TYPE_TRIANGLES,
        PRIM_TYPE_TRIANGLE_STRIP,
        PRIM_TYPE_TRIANGLE_FAN,
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            for (unsigned int i = 0; i < ARRAY_SIZE(triangle_line_modes); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangle_line_modes[i],
                    POLY_MODE_LINE, false);
                assert(source && strstr(source, "LineStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(6)]"));
                assert(!strstr(source, "TriangleStream<GSOut> stream"));
                assert(strstr(source, "for(uint i=0;i<3;i++)"));
                assert(strstr(source, "stream.RestartStrip()"));
                assert(strstr(source,
                              "float4 lp2=float4(lp0.xy+float2(-delta.y,delta.x),lp0.zw)"));
                if (!smooth) {
                    assert(strstr(source, "o.diffuse=flatVertex.diffuse"));
                } else {
                    assert(strstr(source, "o.diffuse=vertex.diffuse"));
                }
                if (provoking == 0) {
                    assert(strstr(source, "uint p=0u"));
                } else if (provoking == 1) {
                    assert(strstr(source, "uint p=1u"));
                } else if (provoking == 2) {
                    assert(strstr(source, "uint p=2u"));
                } else {
                    assert(strstr(source,
                                  "uint p=(2u-(primitiveId&1u))"));
                }
                test_depth_geometry_profile(device_10_0, source, "gs_4_0");
                test_depth_geometry_profile(device_10_1, source, "gs_4_1");
                if (supports_shader_model_5) {
                    test_depth_geometry_profile(device_11_0, source, "gs_5_0");
                }
                g_free(source);
            }
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 4; provoking++) {
            for (unsigned int i = 0; i < ARRAY_SIZE(triangle_line_modes); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, triangle_line_modes[i],
                    POLY_MODE_LINE, true);
                assert(source &&
                       strstr(source, "TriangleStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(12)]"));
                test_depth_geometry_profile(device_10_0, source, "gs_4_0");
                test_depth_geometry_profile(device_10_1, source, "gs_4_1");
                if (supports_shader_model_5) {
                    test_depth_geometry_profile(device_11_0, source, "gs_5_0");
                }
                g_free(source);
            }
        }
    }

    static const enum ShaderPrimitiveMode line_modes[] = {
        PRIM_TYPE_LINES,
        PRIM_TYPE_LINE_STRIP,
        PRIM_TYPE_LINE_LOOP,
    };
    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 2; provoking++) {
            for (unsigned int i = 0; i < ARRAY_SIZE(line_modes); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, line_modes[i], POLY_MODE_FILL,
                    false);
                assert(source && strstr(source, "line GSIn input[2]"));
                assert(strstr(source, "LineStream<GSOut> stream"));
                assert(strstr(source,
                              "float4 p2=float4(p0.xy+float2(-delta.y,delta.x),p0.zw)"));
                assert(strstr(source, "nv2a_tri_mz_perspective(p0,p1,p2)"));
                assert(strstr(source, "nv2a_tri_mz_affine(p0,p1,p2)"));
                if (!smooth) {
                    assert(strstr(source, "flatVertex.diffuse"));
                }
                if (provoking == 0) {
                    assert(strstr(source, "input[0],input[0]"));
                    assert(strstr(source, "input[1],input[0]"));
                } else {
                    assert(strstr(source, "input[0],input[1]"));
                    assert(strstr(source, "input[1],input[1]"));
                }
                test_depth_geometry_profile(device_10_0, source, "gs_4_0");
                test_depth_geometry_profile(device_10_1, source, "gs_4_1");
                if (supports_shader_model_5) {
                    test_depth_geometry_profile(device_11_0, source, "gs_5_0");
                }
                g_free(source);
            }
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        for (unsigned int provoking = 0; provoking < 2; provoking++) {
            for (unsigned int i = 0; i < ARRAY_SIZE(line_modes); i++) {
                char *source = d3d11_depth_geometry_shader_source(
                    smooth != 0, provoking, line_modes[i], POLY_MODE_FILL,
                    true);
                assert(source &&
                       strstr(source, "TriangleStream<GSOut> stream"));
                assert(strstr(source, "[maxvertexcount(4)]"));
                test_depth_geometry_profile(device_10_0, source, "gs_4_0");
                test_depth_geometry_profile(device_10_1, source, "gs_4_1");
                if (supports_shader_model_5) {
                    test_depth_geometry_profile(device_11_0, source, "gs_5_0");
                }
                g_free(source);
            }
        }
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        char *source = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_POLYGON, POLY_MODE_LINE, false);
        assert(source && strstr(source, "line GSIn input[2]"));
        assert(strstr(source, "LineStream<GSOut> stream"));
        assert(strstr(source, "[maxvertexcount(2)]"));
        assert(strstr(source, "p0,p1,p2,0.0,0.0"));
        assert(strstr(source, "input[0],input[0]"));
        assert(strstr(source, "input[1],input[0]"));
        test_depth_geometry_profile(device_10_0, source, "gs_4_0");
        test_depth_geometry_profile(device_10_1, source, "gs_4_1");
        if (supports_shader_model_5) {
            test_depth_geometry_profile(device_11_0, source, "gs_5_0");
        }
        g_free(source);
    }

    for (unsigned int smooth = 0; smooth < 2; smooth++) {
        char *source = d3d11_depth_geometry_shader_source(
            smooth != 0, 2, PRIM_TYPE_POLYGON, POLY_MODE_LINE, true);
        assert(source &&
               strstr(source, "TriangleStream<GSOut> stream"));
        assert(strstr(source, "[maxvertexcount(4)]"));
        test_depth_geometry_profile(device_10_0, source, "gs_4_0");
        test_depth_geometry_profile(device_10_1, source, "gs_4_1");
        if (supports_shader_model_5) {
            test_depth_geometry_profile(device_11_0, source, "gs_5_0");
        }
        g_free(source);
    }

    for (unsigned int depth_format = DEPTH_FORMAT_D24;
         depth_format <= DEPTH_FORMAT_F16; depth_format++) {
        for (unsigned int mode = 0; mode < 4; mode++) {
            PshState state = { 0 };
            state.smooth_shading = true;
            state.z_perspective = mode & 1;
            state.depth_clipping = mode & 2;
            state.depth_format = depth_format;
            char *source = d3d11_translate_pixel_combiners_with_depth(
                &state, true, NULL);
            assert(source);
            assert(strstr(source, "SV_Depth"));
            assert(strstr(source, "nv2a_area"));
            test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                  "nv2a-custom-depth");
            if (supports_shader_model_5) {
                test_pixel_shader_profile(device_11_0, source,
                                          "nv2a-custom-depth", "ps_5_0");
            }
            g_free(source);
        }
    }
    if (supports_shader_model_5) {
        puts("PASS: custom-depth gs_5_0/ps_5_0 on WARP with FL >= 11.0");
    } else {
        puts("SKIP: custom-depth gs_5_0/ps_5_0; WARP is below FL 11.0");
    }
}

static void test_1d_projected_texture_shaders(ID3D11Device *device_10_0,
                                              ID3D11Device *device_10_1)
{
    const unsigned int modes[] = {
        PS_TEXTUREMODES_PROJECT2D,
        PS_TEXTUREMODES_PROJECT3D,
    };
    for (unsigned int border = 0; border < 2; border++) {
        for (unsigned int i = 0; i < ARRAY_SIZE(modes); i++) {
            PshState state = { 0 };
            state.shader_stage_program = modes[i];
            state.dim_tex[0] = 1;
            if (border) {
                state.border_logical_size[0][0] = 8;
                state.border_inv_real_size[0][0] = 0.0625f;
            }
            char *source = d3d11_translate_pixel_combiners(&state, NULL);
            assert(source);
            assert(strstr(source,
                          "float2(pT0.x/pT0.w*texCoordScale[0].x,0.5)"));
            if (border) {
                assert(strstr(source, "pT0.x=(pT0.x*8+4)*0.0625"));
            }
            test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                  "nv2a-1d-projected-texture");
            g_free(source);
        }
    }
}

static void test_pixel_output_at(
    ID3D11Device *device, ID3D11PixelShader *pixel, const uint32_t *texels,
    unsigned int tex_width, unsigned int tex_height, D3D11_FILTER filter,
    float tex_coord_scale, const char *tex_coord,
    D3D11_TEXTURE_ADDRESS_MODE address_u,
    D3D11_TEXTURE_ADDRESS_MODE address_v, float border_alpha,
    uint8_t expected_alpha, const char *test_name)
{
    /* The WARP target is offscreen and all resources are local to this test. */
    char vertex_source[1024];
    const char *vertex_source_prefix =
        "struct O {float4 p:SV_Position;float4 d:COLOR0;float4 s:COLOR1;"
        "float4 bd:TEXCOORD4;float4 bs:TEXCOORD5;float f:FOG;float ps:TEXCOORD12;"
        "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;"
        "float4 t3:TEXCOORD3;nointerpolation float4 z0:TEXCOORD6;"
        "nointerpolation float4 z1:TEXCOORD7;nointerpolation float4 z2:TEXCOORD8;"
        "nointerpolation float mzp:TEXCOORD9;nointerpolation float mza:TEXCOORD10;"
        "float2 pointCoord:TEXCOORD11;"
        "};O main(uint id:SV_VertexID){O o=(O)0;"
        "o.p=float4(id==2?3:-1,id==1?3:-1,0,1);"
        "o.t0=float4(";
    int vertex_source_length = snprintf(
        vertex_source, sizeof(vertex_source), "%s%s%s", vertex_source_prefix,
        tex_coord, ");o.pointCoord=o.t0.xy;return o;}");
    assert(vertex_source_length > 0 &&
           (size_t)vertex_source_length < sizeof(vertex_source));
    ID3DBlob *blob = NULL;
    HRESULT hr = D3DCompile(vertex_source, strlen(vertex_source), "test-vs",
                            NULL, NULL, "main", "vs_4_0", 0, 0, &blob, NULL);
    assert(SUCCEEDED(hr));
    ID3D11VertexShader *vertex = NULL;
    hr = ID3D11Device_CreateVertexShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &vertex);
    assert(SUCCEEDED(hr));
    ID3D10Blob_Release(blob);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11DeviceContext_ClearState(context);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = tex_width,
        .Height = tex_height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    D3D11_SUBRESOURCE_DATA data = {
        .pSysMem = texels,
        .SysMemPitch = tex_width * sizeof(*texels),
    };
    ID3D11Texture2D *texture = NULL, *target = NULL, *staging = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &desc, &data, &texture);
    assert(SUCCEEDED(hr));
    ID3D11ShaderResourceView *srv = NULL;
    hr = ID3D11Device_CreateShaderResourceView(
        device, (ID3D11Resource *)texture, NULL, &srv);
    assert(SUCCEEDED(hr));
    desc.Width = desc.Height = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &target);
    assert(SUCCEEDED(hr));
    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target,
                                             NULL, &rtv);
    assert(SUCCEEDED(hr));
    PGRAPHD3D11PixelConstants constants = { 0 };
    /* Inclusive clipping must contain the pixel exercised by this fixture. */
    constants.clip_region[0][2] = 1;
    constants.clip_region[0][3] = 1;
    for (unsigned int i = 0; i < 4; i++) {
        constants.tex_coord_scale[i][0] = constants.tex_coord_scale[i][1] =
            tex_coord_scale;
        constants.tex_coord_scale[i][2] = constants.tex_coord_scale[i][3] = 1;
    }
    D3D11_BUFFER_DESC buffer_desc = {
        .ByteWidth = sizeof(constants),
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    data.pSysMem = &constants;
    ID3D11Buffer *buffer = NULL;
    hr = ID3D11Device_CreateBuffer(device, &buffer_desc, &data, &buffer);
    assert(SUCCEEDED(hr));
    D3D11_SAMPLER_DESC sampler_desc = {
        .Filter = filter,
        .AddressU = address_u,
        .AddressV = address_v,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .BorderColor = { 0, 0, 0, border_alpha },
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    ID3D11SamplerState *sampler = NULL;
    hr = ID3D11Device_CreateSamplerState(device, &sampler_desc, &sampler);
    assert(SUCCEEDED(hr));
    D3D11_VIEWPORT viewport = { .Width = 1, .Height = 1, .MaxDepth = 1 };
    D3D11_RASTERIZER_DESC raster_desc = {
        .FillMode = D3D11_FILL_SOLID,
        .CullMode = D3D11_CULL_NONE,
        .DepthClipEnable = TRUE,
    };
    ID3D11RasterizerState *raster = NULL;
    hr = ID3D11Device_CreateRasterizerState(device, &raster_desc, &raster);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_RSSetState(context, raster);
    ID3D11RasterizerState_Release(raster);
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
    for (unsigned int slot = 0; slot < 4; slot++) {
        ID3D11DeviceContext_PSSetShaderResources(context, slot, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(context, slot, 1, &sampler);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &buffer);
    ID3D11DeviceContext_Draw(context, 3, 0);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t *actual = mapped.pData;
    int alpha_delta = (int)actual[3] - (int)expected_alpha;
    if (alpha_delta < -1 || alpha_delta > 1) {
        fprintf(stderr, "%s: texture alpha actual=%u expected=%u coord=%s\n",
                test_name, actual[3], expected_alpha, tex_coord);
    }
    assert(actual[0] == 0 && actual[1] == 0 && actual[2] == 0);
    assert(alpha_delta >= -1 && alpha_delta <= 1);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11DeviceContext_ClearState(context);
    ID3D11Texture2D_Release(staging);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11Texture2D_Release(texture);
    ID3D11SamplerState_Release(sampler);
    ID3D11Buffer_Release(buffer);
    ID3D11VertexShader_Release(vertex);
    ID3D11DeviceContext_Release(context);
    printf("PASS: offscreen WARP pixel readback (%s)\n", test_name);
}

static void test_pixel_output(ID3D11Device *device, ID3D11PixelShader *pixel,
                              const uint32_t *texels, unsigned int tex_width,
                              unsigned int tex_height, D3D11_FILTER filter,
                              float tex_coord_scale, float tex_coord_w,
                              uint8_t expected_alpha,
                              const char *test_name)
{
    assert(tex_coord_w == 1.0f || tex_coord_w == 2.0f);
    const char *tex_coord = tex_coord_w == 2.0f ?
        "1.0,1.0,0,2.0" : "0.5,0.5,0,1.0";
    test_pixel_output_at(device, pixel, texels, tex_width, tex_height, filter,
                         tex_coord_scale, tex_coord,
                         D3D11_TEXTURE_ADDRESS_CLAMP,
                         D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, expected_alpha,
                         test_name);
}

static void test_rectangle_point_sprite(ID3D11Device *device)
{
    PshState state = { 0 };
    state.smooth_shading = true;
    state.point_sprite = true;
    state.rect_tex[3] = true;
    state.dim_tex[3] = 2;
    state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D << 15;
    state.final_inputs_1 = (PS_REGISTER_T3 | PS_CHANNEL_ALPHA) << 8;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "pT3.xy/=texCoordScale[3].xy"));
    ID3DBlob *blob = NULL;
    assert(SUCCEEDED(D3DCompile(source, strlen(source), "rect-point-sprite",
                               NULL, NULL, "main", "ps_4_0", 0, 0,
                               &blob, NULL)));
    ID3D11PixelShader *pixel = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &pixel)));
    const uint32_t texels[] = { 0x40000000, 0x80000000,
                               0xc0000000, 0xff000000 };
    test_pixel_output_at(device, pixel, texels, 4, 1,
        D3D11_FILTER_MIN_MAG_MIP_POINT, 0.25f, "0.625,0.5,0,1",
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
        0, 0xc0, "rectangle point sprite covers the whole texture");
    ID3D11PixelShader_Release(pixel);
    ID3D10Blob_Release(blob);
    g_free(source);
}

static void test_convolution_pixel_output(ID3D11Device *device)
{
    PshState state = { 0 };
    state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    state.dim_tex[0] = 2;
    state.conv_tex[0] = CONVOLUTION_FILTER_GAUSSIAN;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source);

    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), "convolution-pixel-output",
                            NULL, NULL, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "convolution readback PS: %s\n",
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    ID3D11PixelShader *pixel = NULL;
    hr = ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &pixel);
    assert(SUCCEEDED(hr));
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    g_free(source);

    const uint32_t center_impulse[9] = {
        0, 0, 0,
        0, 0xff000000, 0,
        0, 0, 0,
    };
    const uint32_t edge_impulse[9] = {
        0, 0xff000000, 0,
        0, 0, 0,
        0, 0, 0,
    };
    const uint32_t corner_impulse[9] = {
        0xff000000, 0, 0,
        0, 0, 0,
        0, 0, 0,
    };
    test_pixel_output(device, pixel, center_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 1.0f, 64,
                      "Gaussian 3x3 center impulse w=1 -> alpha 64");
    test_pixel_output(device, pixel, edge_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 1.0f, 32,
                      "Gaussian 3x3 edge impulse w=1 -> alpha 32");
    test_pixel_output(device, pixel, corner_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 1.0f, 16,
                      "Gaussian 3x3 corner impulse w=1 -> alpha 16");
    /* textureProj also divides each tap offset by q; at w=2 the linear
     * samples overlap half-texels, so these are distinct expected values. */
    test_pixel_output(device, pixel, center_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 2.0f, 143,
                      "Gaussian 3x3 center impulse w=2 -> alpha 143");
    test_pixel_output(device, pixel, edge_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 2.0f, 24,
                      "Gaussian 3x3 edge impulse w=2 -> alpha 24");
    test_pixel_output(device, pixel, corner_impulse, 3, 3,
                      D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f, 2.0f, 4,
                      "Gaussian 3x3 corner impulse w=2 -> alpha 4");
    ID3D11PixelShader_Release(pixel);
}

static void test_clamp_ogl_pixel_output(ID3D11Device *device)
{
    PshState state = { 0 };
    state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    state.dim_tex[0] = 2;
    state.clamp_ogl[0][0] = true;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "coord.x=clamp(coord.x,0.0,1.0)"));

    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), "clamp-ogl-pixel-output",
                            NULL, NULL, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "CLAMP_OGL readback PS: %s\n",
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    ID3D11PixelShader *pixel = NULL;
    hr = ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &pixel);
    assert(SUCCEEDED(hr));
    ID3D10Blob_Release(blob);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    g_free(source);

    const uint32_t texels[4] = {
        0x80000000, 0x40000000,
        0x80000000, 0x40000000,
    };
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f,
        "-0.25,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 128,
        "CLAMP_OGL linear u<0 matches edge texel");
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f,
        "0.0,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 128,
        "CLAMP_OGL linear u=0 matches edge texel");
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f,
        "1.0,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 64,
        "CLAMP_OGL linear u=1 matches edge texel");
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1.0f,
        "1.25,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 64,
        "CLAMP_OGL linear u>1 matches edge texel");
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_POINT, 1.0f,
        "-0.25,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 128,
        "CLAMP_OGL nearest u<0 clamps to edge texel");
    test_pixel_output_at(
        device, pixel, texels, 2, 2, D3D11_FILTER_MIN_MAG_MIP_POINT, 1.0f,
        "1.25,0.5,0,1.0", D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 64,
        "CLAMP_OGL nearest u>1 clamps to edge texel");
    ID3D11PixelShader_Release(pixel);
}

static bool expect_error;
static char expected_error_message[512];

void error_setg_internal(Error **errp, const char *src, int line,
                         const char *func, const char *fmt, ...)
{
    (void)errp;
    (void)src;
    (void)line;
    (void)func;
    va_list args;
    va_start(args, fmt);
    if (expect_error) {
        vsnprintf(expected_error_message, sizeof(expected_error_message), fmt,
                  args);
        va_end(args);
        return;
    }
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

static void test_final_combiner_inputs(ID3D11Device *device_10_0,
                                       ID3D11Device *device_10_1)
{
    static const uint32_t mappings[] = {
        PS_INPUTMAPPING_UNSIGNED_IDENTITY,
        PS_INPUTMAPPING_UNSIGNED_INVERT,
        PS_INPUTMAPPING_EXPAND_NORMAL,
        PS_INPUTMAPPING_EXPAND_NEGATE,
        PS_INPUTMAPPING_HALFBIAS_NORMAL,
        PS_INPUTMAPPING_HALFBIAS_NEGATE,
        PS_INPUTMAPPING_SIGNED_IDENTITY,
        PS_INPUTMAPPING_SIGNED_NEGATE,
    };
    static const char *const formulas[] = {
        "max(float3(0,0,0),T0.rgb)",
        "(float3(1,1,1)-saturate(T0.rgb))",
        "(2*max(float3(0,0,0),T0.rgb)-float3(1,1,1))",
        "(float3(1,1,1)-2*max(float3(0,0,0),T0.rgb))",
        "(max(float3(0,0,0),T0.rgb)-0.5)",
        "(0.5-max(float3(0,0,0),T0.rgb))",
        "T0.rgb",
        "(-T0.rgb)",
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(mappings); i++) {
        PshState state = { 0 };
        state.final_inputs_0 =
            (PS_REGISTER_T0 | mappings[i]) << 24;
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        char *expected = g_strdup_printf("float3 fcA=%s;", formulas[i]);
        assert(source && strstr(source, expected));
        test_pixel_shader_4_x(device_10_0, device_10_1, source,
                              "final-combiner-mapping");
        g_free(expected);
        g_free(source);
    }

    PshState state = { 0 };
    state.final_inputs_1 =
        (PS_REGISTER_T0 | PS_CHANNEL_ALPHA) << 8;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "float fcG=max(0.0,T0.a);"));
    assert(strstr(source, "V1R0_SUM=float4(v1sum+r0sum,0)"));
    assert(strstr(source, "EF_PROD=float4(efE*efF,0)"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "final-combiner-alpha-channel");
    g_free(source);

    state.final_inputs_1 = PS_REGISTER_T0 << 8;
    source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "float fcG=max(0.0,T0.b);"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "final-combiner-blue-channel");
    g_free(source);
}

static void test_combiner_mux_truncation(ID3D11Device *device_10_0,
                                         ID3D11Device *device_10_1)
{
    PshState state = { 0 };
    state.combiner_control = 1;
    state.rgb_outputs[0] = PS_COMBINEROUTPUT_AB_CD_MUX << 12;
    state.alpha_outputs[0] = PS_COMBINEROUTPUT_AB_CD_MUX << 12;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(
                         source,
                         "bool mux_0=(((uint)(saturate(R0.a)*255)&1)!=0);"));
    assert(!strstr(source, "saturate(R0.a)*255+0.5"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "combiner-mux-truncation");
    g_free(source);
}

static void test_alpha_test_quantization(ID3D11Device *device_10_0,
                                         ID3D11Device *device_10_1)
{
    static const char *const conditions[] = {
        "resultAlpha>=alphaRef",
        "resultAlpha!=alphaRef",
        "resultAlpha>alphaRef",
        "resultAlpha<=alphaRef",
        "resultAlpha==alphaRef",
        "resultAlpha<alphaRef",
    };
    for (unsigned int i = ALPHA_FUNC_LESS; i <= ALPHA_FUNC_GEQUAL; i++) {
        PshState state = { .alpha_test = true, .alpha_func = i };
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        assert(source && strstr(source,
                                "int resultAlpha=(int)round(result.a*255.0);"));
        assert(strstr(source, conditions[i - ALPHA_FUNC_LESS]));
        test_pixel_shader_4_x(device_10_0, device_10_1, source,
                              "alpha-test-quantization");
        g_free(source);
    }
}

static void test_color_key_modes(ID3D11Device *device_10_0,
                                 ID3D11Device *device_10_1)
{
    static const char *const actions[] = {
        "if(nv_color_key_match(T0,colorKey[0],colorKeyMask[0])) T0.a=0;",
        "if(nv_color_key_match(T0,colorKey[0],colorKeyMask[0])) "
        "T0=float4(0,0,0,0);",
        "if(nv_color_key_match(T0,colorKey[0],colorKeyMask[0])) discard;",
    };
    for (unsigned int mode = COLOR_KEY_KILL_ALPHA;
         mode <= COLOR_KEY_DISCARD; mode++) {
        PshState state = { 0 };
        state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
        state.dim_tex[0] = 2;
        state.colorkey_mode[0] = mode;
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        assert(source && strstr(source, actions[mode - COLOR_KEY_KILL_ALPHA]));
        test_pixel_shader_4_x(device_10_0, device_10_1, source,
                              "color-key-mode");
        g_free(source);
    }
}

static void test_projected_and_cubemap_modes(ID3D11Device *device_10_0,
                                             ID3D11Device *device_10_1)
{
    PshState state = { 0 };
    state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    state.dim_tex[0] = 2;
    state.tex_cubemap[0] = true;
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source,
                            "nv_remap_2d_to_cube(float3(pT0.xy,pT0.w))"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "projected-cubemap");
    g_free(source);

    state.tex_cubemap[0] = false;
    state.shader_stage_program = PS_TEXTUREMODES_CUBEMAP;
    source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "nv_remap_cube_to_2d(pT0.xyz)"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "cubemap-remapped-to-2d");
    g_free(source);

    state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    state.dim_tex[0] = 3;
    state.border_logical_size[0][0] = 2.0f;
    state.border_logical_size[0][1] = 2.0f;
    state.border_logical_size[0][2] = 2.0f;
    state.border_inv_real_size[0][0] = 0.5f;
    state.border_inv_real_size[0][1] = 0.5f;
    state.border_inv_real_size[0][2] = 0.5f;
    source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(
                         source,
                         "NV2A_SAMPLE3(tex0,samp0,0,float3(pT0.xy/pT0.w*"
                         "texCoordScale[0].xy,0))"));
    assert(strstr(source, "pT0.xyz=(pT0.xyz*float3(2,2,2)"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "projected-2d-on-3d-with-border");
    g_free(source);
}

static void test_convolution_filter_shaders(ID3D11Device *device_10_0,
                                            ID3D11Device *device_10_1)
{
    const enum ConvolutionFilter kernels[] = {
        CONVOLUTION_FILTER_QUINCUNX,
        CONVOLUTION_FILTER_GAUSSIAN,
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(kernels); i++) {
        PshState state = { 0 };
        state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
        state.dim_tex[0] = 2;
        state.conv_tex[0] = kernels[i];
        if (i) {
            state.border_logical_size[0][0] = 8;
            state.border_logical_size[0][1] = 4;
            state.border_inv_real_size[0][0] = 1.0f / 16.0f;
            state.border_inv_real_size[0][1] = 1.0f / 8.0f;
        }
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        assert(source);
        assert(strstr(source, "tex0.GetDimensions(convolutionWidth0,"));
        assert(strstr(source, "convolutionBase0=pT0.xy*texCoordScale[0].xy"));
        assert(strstr(source,
                      "(convolutionBase0+float2(-1.0,-1.0)*"
                      "convolutionTexel0)/pT0.w"));
        assert(strstr(source, "(4.0/16.0)"));
        unsigned int samples = 0;
        const char *cursor = source;
        while ((cursor = strstr(cursor, "T0+=NV2A_SAMPLE2(tex0,samp0,0,"))) {
            samples++;
            cursor++;
        }
        assert(samples == 9);
        if (i) {
            assert(strstr(source, "pT0.xyz=(pT0.xyz*float3(8,4,0)"));
        }
        test_pixel_shader_4_x(device_10_0, device_10_1, source,
                              "nv2a-project2d-convolution");
        g_free(source);
    }

    PshState invalid = { 0 };
    invalid.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    invalid.dim_tex[0] = 3;
    invalid.conv_tex[0] = CONVOLUTION_FILTER_GAUSSIAN;
    expect_error = true;
    char *source = d3d11_translate_pixel_combiners(&invalid, NULL);
    expect_error = false;
    assert(!source);
    assert(strstr(expected_error_message, "2D texture"));

    invalid.dim_tex[0] = 2;
    invalid.tex_cubemap[0] = true;
    expect_error = true;
    source = d3d11_translate_pixel_combiners(&invalid, NULL);
    expect_error = false;
    assert(!source);
    assert(strstr(expected_error_message, "2D texture"));
}

static void test_window_clip_modes(ID3D11Device *device_10_0,
                                   ID3D11Device *device_10_1)
{
    PshState state = { 0 };
    char *source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source, "windowClipContained=false"));
    assert(strstr(source, "if(!windowClipContained) discard;"));
    assert(strstr(source, "clipRegion[7].zw"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "inclusive-window-clip");
    g_free(source);

    state.window_clip_exclusive = true;
    source = d3d11_translate_pixel_combiners(&state, NULL);
    assert(source && strstr(source,
                            "if(all(windowCoord>=float2(clipRegion[0].xy))"));
    assert(strstr(source, "clipRegion[7].zw"));
    test_pixel_shader_4_x(device_10_0, device_10_1, source,
                          "exclusive-window-clip");
    g_free(source);
}

static void test_shadow_map_translation(ID3D11Device *device_10_0,
                                       ID3D11Device *device_10_1)
{
    static const char *operators[] = {
        NULL, "<", "==", "<=", ">", "!=", ">=", NULL,
    };
    for (int depth_func = SHADOW_DEPTH_FUNC_NEVER;
         depth_func <= SHADOW_DEPTH_FUNC_ALWAYS; depth_func++) {
        for (unsigned int mode_index = 0; mode_index < 2; mode_index++) {
            PshState state = { 0 };
            unsigned int mode = mode_index == 0 ? PS_TEXTUREMODES_PROJECT2D :
                                                  PS_TEXTUREMODES_PROJECT3D;
            state.shader_stage_program = mode;
            state.dim_tex[0] = 2;
            state.shadow_map[0] = true;
            state.shadow_depth_func = (enum PshShadowDepthFunc)depth_func;
            state.alphakill[0] = true;
            state.colorkey_mode[0] = COLOR_KEY_KILL_ALPHA;
            char *source = d3d11_translate_pixel_combiners(&state, NULL);
            assert(source);
            assert(strstr(source, "if(T0.a==0) discard"));
            assert(strstr(source, "nv_color_key_match(T0,colorKey[0],"));
            if (depth_func == SHADOW_DEPTH_FUNC_NEVER) {
                assert(strstr(source, "T0=float4(0,0,0,0)"));
            } else if (depth_func == SHADOW_DEPTH_FUNC_ALWAYS) {
                assert(strstr(source, "T0=float4(1,1,1,1)"));
            } else if (mode_index == 0) {
                char expected[64];
                snprintf(expected, sizeof(expected), "shadowDepth0 %s 0.0",
                         operators[depth_func]);
                assert(strstr(source,
                              "shadowDepth0=shadowSample0.r+shadowSample0.g+"
                              "shadowSample0.b"));
                assert(strstr(source, expected));
            } else {
                char expected[96];
                snprintf(expected, sizeof(expected),
                         "shadowMapDepth0 %s shadowReference0",
                         operators[depth_func]);
                assert(strstr(source, "shadowMaxDepth0"));
                assert(strstr(source, "shadowMaxDepth0=65535.0"));
                assert(strstr(source, "shadowSample0.r*65280.0"));
                assert(strstr(source, expected));
            }
            test_pixel_shader_4_x(
                device_10_0, device_10_1, source,
                mode_index == 0 ? "shadow-project2d" : "shadow-project3d");
            g_free(source);
        }
    }

    for (unsigned int format_kind = 1; format_kind <= 2; format_kind++) {
        PshState state = { 0 };
        state.shader_stage_program = PS_TEXTUREMODES_PROJECT3D;
        state.dim_tex[0] = 2;
        state.shadow_map[0] = true;
        state.shadow_depth_func = SHADOW_DEPTH_FUNC_LEQUAL;
        state.tex_x8y24[0] = format_kind == 1;
        state.tex_depth_float[0] = format_kind == 2;
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        assert(source);
        assert(strstr(source, format_kind == 1 ?
                                  "shadowMaxDepth0=16777215.0" :
                                  "shadowMaxDepth0=511.9375"));
        assert(strstr(source, format_kind == 1 ?
                                  "shadowSample0.r*16711680.0" :
                                  "shadowSample0.r*65280.0"));
        test_pixel_shader_4_x(device_10_0, device_10_1, source,
                              "shadow-depth-format-scale");
        g_free(source);
    }

    PshState volume_state = { 0 };
    volume_state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D;
    volume_state.dim_tex[0] = 3;
    volume_state.shadow_map[0] = true;
    volume_state.shadow_depth_func = SHADOW_DEPTH_FUNC_LEQUAL;
    char *volume_source =
        d3d11_translate_pixel_combiners(&volume_state, NULL);
    assert(volume_source);
    assert(strstr(volume_source, "Texture3D tex0:register(t0);"));
    assert(strstr(volume_source,
                  "NV2A_SAMPLE3(tex0,samp0,0,float3(pT0.xy/pT0.w*"
                  "texCoordScale[0].xy,0))"));
    test_pixel_shader_4_x(device_10_0, device_10_1, volume_source,
                          "shadow-project2d-volume-z0");
    g_free(volume_source);

    volume_state.tex_cubemap[0] = true;
    Error *volume_error = NULL;
    expect_error = true;
    volume_source =
        d3d11_translate_pixel_combiners(&volume_state, &volume_error);
    expect_error = false;
    assert(volume_source == NULL);
    assert(strstr(expected_error_message, "unsupported shadow-map mode"));

    PshState state = { 0 };
    state.shader_stage_program = PS_TEXTUREMODES_BUMPENVMAP;
    state.dim_tex[0] = 2;
    state.shadow_map[0] = true;
    Error *error = NULL;
    expect_error = true;
    char *source = d3d11_translate_pixel_combiners(&state, &error);
    expect_error = false;
    assert(source == NULL);
    assert(strstr(expected_error_message, "unsupported shadow-map mode"));
}

static void test_d3dcompile2_runtime_api(ID3D11Device *device)
{
    static const char source[] =
        "float4 main() : SV_TARGET { return float4(0.25, 0.5, 0.75, 1.0); }";
    ID3DBlob *bytecode = NULL, *errors = NULL;
    HRESULT hr = D3DCompile2(
        source, sizeof(source) - 1, "d3dcompile2-runtime-test.hlsl", NULL,
        NULL, "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, 0, NULL, 0,
        &bytecode, &errors);
    if (FAILED(hr) && errors) {
        fprintf(stderr, "D3DCompile2 runtime test: %s\n",
                (char *)ID3D10Blob_GetBufferPointer(errors));
    }
    assert(SUCCEEDED(hr));
    ID3D11PixelShader *shader = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(bytecode),
        ID3D10Blob_GetBufferSize(bytecode), NULL, &shader)));
    ID3D11PixelShader_Release(shader);
    ID3D10Blob_Release(bytecode);
    if (errors) {
        ID3D10Blob_Release(errors);
    }
}

static void test_raster_readback(ID3D11Device *device)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    ID3D11DeviceContext_ClearState(context);
    D3D11_TEXTURE2D_DESC td = {
        .Width = 32, .Height = 32, .MipLevels = 1, .ArraySize = 1,
        .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET,
    };
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &target)));
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)target, NULL, &rtv)));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &staging)));
    float constants[D3D11_VERTEX_CONSTANT_BUFFER_SIZE / sizeof(float)] = { 0 };
    constants[ARRAY_SIZE(constants) - 4] = 32;
    constants[ARRAY_SIZE(constants) - 3] = 32;
    constants[ARRAY_SIZE(constants) - 2] = 1;
    D3D11_BUFFER_DESC bd = {
        .ByteWidth = sizeof(constants), .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
    };
    D3D11_SUBRESOURCE_DATA data = { .pSysMem = constants };
    ID3D11Buffer *cb = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, &data, &cb)));
    ID3D11DeviceContext_GSSetConstantBuffers(context, 0, 1, &cb);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = { .Width = 32, .Height = 32, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const unsigned int modes[] = { POLY_MODE_FILL, POLY_MODE_LINE, POLY_MODE_POINT };
    for (unsigned int cw = 0; cw < 2; cw++) {
        D3D11_RASTERIZER_DESC rd = {
            .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE,
            .FrontCounterClockwise = cw,
        };
        ID3D11RasterizerState *rs = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &rd, &rs)));
        ID3D11DeviceContext_RSSetState(context, rs);
        for (unsigned int f = 0; f < 3; f++) {
            for (unsigned int b = 0; b < 3; b++) {
                uint32_t raster = NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE |
                                  NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE |
                                  NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
                                  NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE;
                if (cw) {
                    raster |= NV_PGRAPH_SETUPRASTER_FRONTFACE;
                }
                SET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE, modes[f]);
                SET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE, modes[b]);
                char *gs_source = d3d11_raster_geometry_source(
                    true, 2, PRIM_TYPE_TRIANGLES, raster, false);
                const char *input_start = strstr(gs_source, "struct GSIn");
                char *input = g_strndup(input_start, strstr(input_start, "};") + 2 - input_start);
                char *vs_source = g_strconcat(input,
                    "GSIn main(uint id:SV_VertexID){GSIn o=(GSIn)0;"
                    "float2 p[6]={float2(2.25,4.25),float2(14.25,4.25),float2(2.25,28.25),"
                    "float2(18.25,4.25),float2(18.25,28.25),float2(30.25,4.25)};"
                    "o.position=float4(p[id].x/16-1,1-p[id].y/16,0,1);"
                    "o.depthPos0=float4(p[id],0,1);o.pointSize=1;"
                    "o.diffuse=float4(1,0,0,1);o.backDiffuse=float4(0,1,0,1);return o;}", NULL);
                const char *output_start = strstr(gs_source, "struct GSOut");
                char *output = g_strndup(output_start, strstr(output_start, "};") + 2 - output_start);
                GString *ps = g_string_new(output);
                d3d11_source_replace(ps, "GSOut", "PSIn");
                d3d11_source_replace(ps,
                    "  noperspective float4 coverage:TEXCOORD13;", "");
                d3d11_source_replace(ps,
                    "  nointerpolation float rasterOffset:TEXCOORD14;", "");
                d3d11_source_replace(ps, "};",
                    "  bool frontFace : SV_IsFrontFace;\n};");
                g_string_append(ps, "\nfloat4 main(PSIn input):SV_Target{"
                    "float a=input.frontFace==(input.diffuse.r>0.5)?input.diffuse.a:0;"
                    "return float4(input.diffuse.rg,input.rasterOffset,a);}\n");
                char *ps_source = d3d11_ps_raster_coverage(g_string_free(ps, FALSE), false);
                ID3DBlob *blob = NULL;
                ID3D11VertexShader *vs = NULL;
                ID3D11GeometryShader *gs = NULL;
                ID3D11PixelShader *pixel = NULL;
                assert(SUCCEEDED(D3DCompile(vs_source, strlen(vs_source), "raster-vs", NULL, NULL,
                    "main", "vs_4_0", 0, 0, &blob, NULL)));
                assert(SUCCEEDED(ID3D11Device_CreateVertexShader(device,
                    ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &vs)));
                ID3D10Blob_Release(blob);
                assert(SUCCEEDED(D3DCompile(gs_source, strlen(gs_source), "raster-gs", NULL, NULL,
                    "main", "gs_4_0", 0, 0, &blob, NULL)));
                assert(SUCCEEDED(ID3D11Device_CreateGeometryShader(device,
                    ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &gs)));
                ID3D10Blob_Release(blob);
                assert(SUCCEEDED(D3DCompile(ps_source, strlen(ps_source), "raster-ps", NULL, NULL,
                    "main", "ps_4_0", 0, 0, &blob, NULL)));
                assert(SUCCEEDED(ID3D11Device_CreatePixelShader(device,
                    ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &pixel)));
                ID3D10Blob_Release(blob);
                ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
                ID3D11DeviceContext_GSSetShader(context, gs, NULL, 0);
                ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
                const float clear[4] = { 0 };
                ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);
                D3D11_QUERY_DESC qd = { .Query = D3D11_QUERY_PIPELINE_STATISTICS };
                ID3D11Query *query = NULL;
                assert(SUCCEEDED(ID3D11Device_CreateQuery(device, &qd, &query)));
                ID3D11DeviceContext_Begin(context, (ID3D11Asynchronous *)query);
                ID3D11DeviceContext_Draw(context, 6, 0);
                ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
                ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                    (ID3D11Resource *)target);
                D3D11_MAPPED_SUBRESOURCE mapped;
                assert(SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
                    0, D3D11_MAP_READ, 0, &mapped)));
                for (unsigned int side = 0; side < 2; side++) {
                    bool front = side == cw;
                    unsigned int mode = front ? modes[f] : modes[b];
                    const float *center = (const float *)((const uint8_t *)mapped.pData +
                        12 * mapped.RowPitch) + 4 * (side ? 22 : 6);
                    if (center[3] != (mode == POLY_MODE_FILL ? 1.0f : 0.0f)) {
                        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
                        assert(ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query,
                            &stats, sizeof(stats), 0) == S_OK);
                        fprintf(stderr, "GS=%llu raster=%llu PS=%llu\n",
                            (unsigned long long)stats.GSPrimitives,
                            (unsigned long long)stats.CPrimitives,
                            (unsigned long long)stats.PSInvocations);
                        fprintf(stderr, "raster pixel cw=%u f=%u b=%u side=%u mode=%u rgba=%g,%g,%g,%g\n",
                            cw, modes[f], modes[b], side, mode,
                            center[0], center[1], center[2], center[3]);
                    }
                    assert(center[3] == (mode == POLY_MODE_FILL ? 1.0f : 0.0f));
                    if (mode == POLY_MODE_FILL) {
                        assert(center[front ? 0 : 1] == 1.0f);
                    }
                    unsigned int fractional = 0;
                    unsigned int visible = 0;
                    for (unsigned int y = 0; y < 32; y++) {
                        for (unsigned int x = side ? 17 : 0; x < (side ? 32 : 16); x++) {
                            const float *v = (const float *)((const uint8_t *)mapped.pData +
                                y * mapped.RowPitch) + 4 * x;
                            fractional += v[3] > 0 && v[3] < 0.99f;
                            if (v[3] > 0) {
                                visible++;
                                assert(v[front ? 0 : 1] == 1.0f);
                                assert(v[front ? 1 : 0] == 0.0f);
                                assert(v[2] == (mode != POLY_MODE_LINE ? 1.0f : 0.0f));
                            }
                        }
                    }
                    if (mode != POLY_MODE_POINT) {
                        assert(fractional > 0);
                    }
                    assert(visible > 0);
                }
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
                ID3D11Query_Release(query);
                ID3D11VertexShader_Release(vs);
                ID3D11GeometryShader_Release(gs);
                ID3D11PixelShader_Release(pixel);
                g_free(input); g_free(output);
                g_free(vs_source); g_free(gs_source); g_free(ps_source);
            }
        }
        ID3D11RasterizerState_Release(rs);
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11Buffer_Release(cb);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11Texture2D_Release(staging);
    ID3D11DeviceContext_Release(context);
    puts("PASS: all nine front/back mode pairs, winding/colors and fractional edge coverage, WARP pixels");
}

static void test_raster_geometry(ID3D11Device *device_10_0,
                                 ID3D11Device *device_10_1)
{
    const unsigned int modes[] = { POLY_MODE_FILL, POLY_MODE_LINE, POLY_MODE_POINT };
    const unsigned int primitives[] = {
        PRIM_TYPE_LINES, PRIM_TYPE_LINE_STRIP, PRIM_TYPE_LINE_LOOP,
        PRIM_TYPE_TRIANGLES, PRIM_TYPE_TRIANGLE_STRIP, PRIM_TYPE_TRIANGLE_FAN,
        PRIM_TYPE_QUADS, PRIM_TYPE_QUAD_STRIP, PRIM_TYPE_POLYGON,
    };
    for (unsigned int p = 0; p < ARRAY_SIZE(primitives); p++) {
        for (unsigned int f = 0; f < ARRAY_SIZE(modes); f++) {
            for (unsigned int b = 0; b < ARRAY_SIZE(modes); b++) {
                uint32_t raster = NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE |
                                  NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE;
                SET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE, modes[f]);
                SET_MASK(raster, NV_PGRAPH_SETUPRASTER_BACKFACEMODE, modes[b]);
                for (unsigned int level = 0; level < 2; level++) {
                    char *source = d3d11_raster_geometry_source(
                        (p + f + b) % 2, (p + f + b) % 4,
                        primitives[p], raster, false);
                    ID3DBlob *blob = NULL, *errors = NULL;
                    HRESULT hr = D3DCompile2(source, strlen(source), "raster-test",
                        NULL, NULL, "main", level ? "gs_4_1" : "gs_4_0",
                        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                        0, 0, NULL, 0, &blob, &errors);
                    if (FAILED(hr)) {
                        fprintf(stderr, "raster p=%u f=%u b=%u: %s\n", primitives[p],
                            modes[f], modes[b], errors ?
                            (char *)ID3D10Blob_GetBufferPointer(errors) : "no diagnostics");
                    }
                    assert(SUCCEEDED(hr));
                    ID3D11GeometryShader *shader = NULL;
                    assert(SUCCEEDED(ID3D11Device_CreateGeometryShader(
                        level ? device_10_1 : device_10_0,
                        ID3D10Blob_GetBufferPointer(blob),
                        ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
                    ID3D11GeometryShader_Release(shader);
                    ID3D10Blob_Release(blob);
                    if (errors) {
                        ID3D10Blob_Release(errors);
                    }
                    g_free(source);
                }
            }
        }
    }
    for (unsigned int depth = 0; depth < 2; depth++) {
        PshState state = { 0 };
        char *source = d3d11_translate_pixel_combiners_with_depth(&state, depth, NULL);
        source = d3d11_ps_raster_coverage(source, depth);
        test_pixel_shader_4_x(device_10_0, device_10_1, source, "raster-coverage");
        source = d3d11_ps_dual_source(source, depth);
        test_pixel_shader_4_x(device_10_0, device_10_1, source, "raster-coverage-dual");
        g_free(source);
    }
    puts("PASS: 162 independent face/smoothing GS variants and coverage/dual-source PS on WARP");
    test_raster_readback(device_10_0);
}

int main(void)
{
    ID3D11Device *device = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &device, NULL, NULL);
    assert(SUCCEEDED(hr));
    const D3D_FEATURE_LEVEL requested_level_10_0 = D3D_FEATURE_LEVEL_10_0;
    D3D_FEATURE_LEVEL obtained_level = D3D_FEATURE_LEVEL_9_1;
    ID3D11Device *device_10_0 = NULL;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                           &requested_level_10_0, 1, D3D11_SDK_VERSION,
                           &device_10_0, &obtained_level, NULL);
    assert(SUCCEEDED(hr) && obtained_level == D3D_FEATURE_LEVEL_10_0);
    test_d3dcompile2_runtime_api(device_10_0);
    const D3D_FEATURE_LEVEL requested_level = D3D_FEATURE_LEVEL_10_1;
    ID3D11Device *device_10_1 = NULL;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                           &requested_level, 1, D3D11_SDK_VERSION,
                           &device_10_1, &obtained_level, NULL);
    assert(SUCCEEDED(hr) && obtained_level == D3D_FEATURE_LEVEL_10_1);
    if (g_getenv("XEMU_RASTER_READBACK_ONLY")) {
        test_raster_readback(device_10_0);
        ID3D11Device_Release(device_10_0);
        ID3D11Device_Release(device_10_1);
        ID3D11Device_Release(device);
        return 0;
    }
    test_final_combiner_inputs(device_10_0, device_10_1);
    test_dual_source_blending(device_10_0);
    test_dual_source_blending(device_10_1);
    test_dual_source_blending(device);
    test_rectangle_point_sprite(device_10_0);
    test_rectangle_point_sprite(device_10_1);
    test_bordered_cube_sampling(device_10_0);
    test_bordered_cube_sampling(device_10_1);
    test_polygon_point_readback(device_10_0);
    test_polygon_point_readback(device_10_1);
    test_combiner_mux_truncation(device_10_0, device_10_1);
    test_alpha_test_quantization(device_10_0, device_10_1);
    test_color_key_modes(device_10_0, device_10_1);
    test_projected_and_cubemap_modes(device_10_0, device_10_1);
    test_convolution_filter_shaders(device_10_0, device_10_1);
    test_window_clip_modes(device_10_0, device_10_1);
    test_shadow_map_translation(device_10_0, device_10_1);
    for (unsigned int dimension = 2; dimension <= 3; dimension++) {
        for (unsigned int stages = 0; stages <= 8; stages++) {
            PshState state = { 0 };
            state.smooth_shading = true;
            state.combiner_control = stages;
            state.shader_stage_program = dimension == 3 ?
                                             PS_TEXTUREMODES_PROJECT3D :
                                             PS_TEXTUREMODES_PROJECT2D;
            for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                state.dim_tex[i] = dimension;
            }
            char *source = d3d11_translate_pixel_combiners(&state, NULL);
            assert(source);
            test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                  "generated-pixel");
            /* Regression: initialize the combiner alpha after texture reads. */
            assert(strstr(source, "R0.a=T0.a") >
                   strstr(source, "T0=NV2A_SAMPLE"));
            ID3DBlob *blob = NULL;
            ID3DBlob *errors = NULL;
            hr = D3DCompile(source, strlen(source), "test-pixel", NULL, NULL,
                            "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                            &blob, &errors);
            if (FAILED(hr)) {
                fprintf(stderr, "HLSL dimension=%u stages=%u: %s\n", dimension,
                        stages,
                        errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                                 "");
            }
            assert(SUCCEEDED(hr));
            ID3D11PixelShader *shader = NULL;
            hr = ID3D11Device_CreatePixelShader(
                device, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &shader);
            assert(SUCCEEDED(hr));
            if (dimension == 2 && stages == 0) {
                const uint32_t texels[] = { 0x40000000, 0x80000000,
                                            0xc0000000, 0xff000000 };
                test_pixel_output(device, shader, texels, 2, 2,
                                  D3D11_FILTER_MIN_MAG_MIP_POINT, 0.5f, 1.0f,
                                  0x40, "base sample/R0 alpha");
                test_convolution_pixel_output(device);
                test_clamp_ogl_pixel_output(device);
            }
            ID3D11ShaderReflection *reflection = NULL;
            hr = D3DReflect(ID3D10Blob_GetBufferPointer(blob),
                            ID3D10Blob_GetBufferSize(blob),
                            &IID_ID3D11ShaderReflection, (void **)&reflection);
            assert(SUCCEEDED(hr));
            ID3D11ShaderReflectionConstantBuffer *buffer =
                reflection->lpVtbl->GetConstantBufferByName(
                    reflection, "Nv2aPixelConstants");
            D3D11_SHADER_BUFFER_DESC desc;
            hr = buffer->lpVtbl->GetDesc(buffer, &desc);
            assert(SUCCEEDED(hr));
            assert(desc.Size == sizeof(PGRAPHD3D11PixelConstants));
            reflection->lpVtbl->Release(reflection);
            ID3D11PixelShader_Release(shader);
            ID3D10Blob_Release(blob);
            if (errors) {
                ID3D10Blob_Release(errors);
            }
            g_free(source);
        }
    }
    const unsigned int modes[] = {
        PS_TEXTUREMODES_CLIPPLANE,      PS_TEXTUREMODES_BUMPENVMAP,
        PS_TEXTUREMODES_BUMPENVMAP_LUM, PS_TEXTUREMODES_DOTPRODUCT,
        PS_TEXTUREMODES_DOT_ST,         PS_TEXTUREMODES_DOT_ZW,
        PS_TEXTUREMODES_DPNDNT_AR,      PS_TEXTUREMODES_DPNDNT_GB,
        PS_TEXTUREMODES_BRDF,
    };
    for (unsigned int m = 0; m < ARRAY_SIZE(modes); m++) {
        for (unsigned int mapping = 0; mapping <= 7; mapping++) {
            PshState state = { 0 };
            state.smooth_shading = true;
            state.shader_stage_program = PS_TEXTUREMODES_PROJECT2D |
                                         (PS_TEXTUREMODES_DOTPRODUCT << 5) |
                                         (modes[m] << 10);
            state.other_stage_input = mapping | (mapping << 4);
            for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                state.dim_tex[i] = 2;
            }
            char *source = d3d11_translate_pixel_combiners(&state, NULL);
            assert(source);
            test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                  "dependent-texture");
            ID3DBlob *blob = NULL, *errors = NULL;
            hr = D3DCompile(source, strlen(source), "dependent-texture", NULL,
                            NULL, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
            if (FAILED(hr) && errors) {
                fprintf(stderr, "%s\n",
                        (char *)ID3D10Blob_GetBufferPointer(errors));
            }
            assert(SUCCEEDED(hr));
            ID3D11PixelShader *shader = NULL;
            assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
                device, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
            ID3D11PixelShader_Release(shader);
            ID3D10Blob_Release(blob);
            if (errors) {
                ID3D10Blob_Release(errors);
            }
            g_free(source);
        }
    }
    const unsigned int str_modes[] = {
        PS_TEXTUREMODES_DOT_STR_3D,
        PS_TEXTUREMODES_DOT_STR_CUBE,
    };
    for (unsigned int m = 0; m < ARRAY_SIZE(str_modes); m++) {
        for (unsigned int variant = 0; variant < 3; variant++) {
            for (unsigned int mapping = 0; mapping <= 7; mapping++) {
                PshState state = { 0 };
                state.smooth_shading = true;
                state.shader_stage_program =
                    PS_TEXTUREMODES_PROJECT2D |
                    (PS_TEXTUREMODES_DOTPRODUCT << 5) |
                    (PS_TEXTUREMODES_DOTPRODUCT << 10) |
                    (str_modes[m] << 15);
                state.other_stage_input = mapping << 8;
                for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                    state.dim_tex[i] = 2;
                }
                if (str_modes[m] == PS_TEXTUREMODES_DOT_STR_3D && variant) {
                    if (variant == 1) {
                        state.dim_tex[3] = 3;
                    }
                } else if (str_modes[m] == PS_TEXTUREMODES_DOT_STR_CUBE &&
                           variant == 1) {
                    state.tex_cubemap[3] = true;
                }
                if (variant == 2) {
                    state.border_logical_size[3][0] = 8.0f;
                    state.border_logical_size[3][1] = 4.0f;
                    state.border_logical_size[3][2] = 1.0f;
                    state.border_inv_real_size[3][0] = 0.5f;
                    state.border_inv_real_size[3][1] = 0.25f;
                    state.border_inv_real_size[3][2] = 1.0f;
                }
                char *source = d3d11_translate_pixel_combiners(&state, NULL);
                assert(source);
                char expected_dot[96];
                snprintf(expected_dot, sizeof(expected_dot),
                         "dots[3]=dot(pT3.xyz,nv_dotmap(T0,%u));", mapping);
                assert(strstr(source, expected_dot));
                assert(strstr(source,
                              "float3 dotSTR_3=float3(dots[1],dots[2],dots[3])"));
                if (variant == 2) {
                    assert(strstr(source, "dotSTR_3=(dotSTR_3*float3(8,4,1)"));
                }
                if (str_modes[m] == PS_TEXTUREMODES_DOT_STR_CUBE && variant == 1) {
                    assert(strstr(source, "TextureCube tex3:register(t3)"));
                } else if (str_modes[m] == PS_TEXTUREMODES_DOT_STR_3D &&
                           variant == 1) {
                    assert(strstr(source, "Texture3D tex3:register(t3)"));
                } else {
                    assert(strstr(source, "Texture2D tex3:register(t3)"));
                }
                test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                      "dot-str");
                ID3DBlob *blob = NULL, *errors = NULL;
                hr = D3DCompile(source, strlen(source), "dot-str", NULL, NULL,
                                "main", "ps_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
                if (FAILED(hr) && errors) {
                    fprintf(stderr, "%s\n",
                            (char *)ID3D10Blob_GetBufferPointer(errors));
                }
                assert(SUCCEEDED(hr));
                ID3D11PixelShader *shader = NULL;
                assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
                    device, ID3D10Blob_GetBufferPointer(blob),
                    ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
                ID3D11PixelShader_Release(shader);
                ID3D10Blob_Release(blob);
                if (errors) {
                    ID3D10Blob_Release(errors);
                }
                g_free(source);
            }
        }
    }
    const unsigned int reflection_modes[] = {
        PS_TEXTUREMODES_DOT_RFLCT_DIFF,
        PS_TEXTUREMODES_DOT_RFLCT_SPEC,
        PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST,
    };
    for (unsigned int m = 0; m < ARRAY_SIZE(reflection_modes); m++) {
        for (unsigned int cube = 0; cube < 2; cube++) {
            for (unsigned int mapping = 0; mapping <= 7; mapping++) {
                PshState state = { 0 };
                state.smooth_shading = true;
                state.shader_stage_program =
                    PS_TEXTUREMODES_PROJECT2D |
                    (PS_TEXTUREMODES_DOTPRODUCT << 5) |
                    (m == 0 ? 0 : PS_TEXTUREMODES_DOTPRODUCT << 10) |
                    (reflection_modes[m] << (m == 0 ? 10 : 15));
                state.other_stage_input = (mapping << 4) | (mapping << 8);
                for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                    state.dim_tex[i] = 2;
                }
                unsigned int reflection_stage = m == 0 ? 2 : 3;
                state.tex_cubemap[reflection_stage] = cube != 0;
                char *source = d3d11_translate_pixel_combiners(&state, NULL);
                assert(source);
                if (m == 0) {
                    assert(strstr(source,
                                  "float3 diffuse_vec_2=float3(dots[1],dots[2],"));
                } else if (reflection_modes[m] ==
                           PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST) {
                    assert(strstr(source,
                                  "float3 reflect_e_3=eyeVector;"));
                    assert(strstr(source,
                                  "dot(reflect_n_3,reflect_e_3)"));
                    assert(strstr(source, cube ?
                                          "T3=NV2A_SAMPLE3(tex3,samp3,3,reflect_vec_3)" :
                                          "nv_remap_cube_to_2d(reflect_vec_3)"));
                } else {
                    assert(strstr(source,
                                  "float3 reflect_vec_3=2*reflect_n_3*"));
                    assert(strstr(source, "dot(reflect_n_3,reflect_e_3)"));
                }
                test_pixel_shader_4_x(device_10_0, device_10_1, source,
                                      "dot-reflection");
                ID3DBlob *blob = NULL, *errors = NULL;
                hr = D3DCompile(source, strlen(source), "dot-reflection", NULL,
                                NULL, "main", "ps_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
                if (FAILED(hr) && errors) {
                    fprintf(stderr, "%s\n",
                            (char *)ID3D10Blob_GetBufferPointer(errors));
                }
                assert(SUCCEEDED(hr));
                ID3D11PixelShader *shader = NULL;
                assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
                    device, ID3D10Blob_GetBufferPointer(blob),
                    ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
                if (reflection_modes[m] ==
                    PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST) {
                    ID3D11ShaderReflection *reflection = NULL;
                    assert(SUCCEEDED(D3DReflect(
                        ID3D10Blob_GetBufferPointer(blob),
                        ID3D10Blob_GetBufferSize(blob),
                        &IID_ID3D11ShaderReflection, (void **)&reflection)));
                    ID3D11ShaderReflectionConstantBuffer *buffer =
                        reflection->lpVtbl->GetConstantBufferByName(
                            reflection, "Nv2aPixelConstants");
                    assert(buffer);
                    ID3D11ShaderReflectionVariable *variable =
                        buffer->lpVtbl->GetVariableByName(buffer, "eyeVector");
                    assert(variable);
                    D3D11_SHADER_VARIABLE_DESC variable_desc;
                    assert(SUCCEEDED(variable->lpVtbl->GetDesc(
                        variable, &variable_desc)));
                    assert(variable_desc.StartOffset ==
                           offsetof(PGRAPHD3D11PixelConstants, eye_vector));
                    assert(variable_desc.Size ==
                           sizeof(((PGRAPHD3D11PixelConstants *)0)->eye_vector));
                    reflection->lpVtbl->Release(reflection);
                }
                ID3D11PixelShader_Release(shader);
                ID3D10Blob_Release(blob);
                if (errors) {
                    ID3D10Blob_Release(errors);
                }
                g_free(source);
            }
        }
    }
    PshState invalid_eye_reflection = { 0 };
    invalid_eye_reflection.shader_stage_program =
        PS_TEXTUREMODES_PROJECT2D |
        (PS_TEXTUREMODES_DOTPRODUCT << 5) |
        (PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST << 10);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        invalid_eye_reflection.dim_tex[i] = 2;
    }
    Error *error = NULL;
    expect_error = true;
    char *invalid_source =
        d3d11_translate_pixel_combiners(&invalid_eye_reflection, &error);
    expect_error = false;
    assert(!invalid_source);
    assert(strstr(expected_error_message, "DOT_RFLCT_SPEC_CONST requires stage 3"));
    for (unsigned int stage = 2; stage <= 3; stage++) {
        PshState state = { 0 };
        state.smooth_shading = true;
        state.shader_stage_program =
            PS_TEXTUREMODES_PROJECT2D |
            (PS_TEXTUREMODES_PROJECT2D << 5) |
            (PS_TEXTUREMODES_BRDF << (stage * 5));
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            state.dim_tex[i] = 2;
        }
        char *source = d3d11_translate_pixel_combiners(&state, NULL);
        assert(source);
        char expected_brdf[64];
        snprintf(expected_brdf, sizeof(expected_brdf),
                 "T%u=float4(0,0,0,0); /* xemu GLSL fallback stub */", stage);
        assert(strstr(source, expected_brdf));
        test_pixel_shader_4_x(device_10_0, device_10_1, source, "brdf-stub");
        g_free(source);
    }
    test_raster_geometry(device_10_0, device_10_1);
    test_custom_depth_shaders(device, device_10_0, device_10_1);
    test_1d_projected_texture_shaders(device_10_0, device_10_1);
    test_presentation_shaders(device_10_0, device_10_1);
    test_presentation_scanout_row_mapping(device_10_0);
    ID3D11Device_Release(device_10_0);
    ID3D11Device_Release(device_10_1);
    ID3D11Device_Release(device);
    puts("PASS: dependent/clip/BRDF shaders, dot mappings 0-7, WARP creation");
    puts("PASS: 48 DOT_STR_3D/CUBE variants, WARP creation");
    puts("PASS: 48 diffuse/specular/constant-eye reflection variants, WARP creation");
    puts("PASS: BRDF stages 2/3 match the zero-output GLSL stub");
    puts("PASS: depth GS/PS variants and primitive selection on WARP FL 10.0/10.1");
    puts("PASS: D3DCompile2 in-memory shader compile/create on WARP FL 10.0");
    puts("PASS: projected 1D-texture variants compile/create on WARP FL 10.0/10.1");
    puts("PASS: presentation VS/PS compile/create, 96-byte cbuffer ABI "
         "on WARP FL 10.0/10.1");
    puts("PASS: presentation scanout vertical pitch scaling, WARP pixel readback");
    puts("PASS: generated ps_4_0/ps_4_1 shaders compile and create on WARP FL 10.0/10.1");
    puts("PASS: 18 generated pixel shaders, WARP creation, cbuffer ABI");
    return 0;
}
