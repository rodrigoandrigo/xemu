/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the actual translator extracted by test-d3d11-shaders.py.
 * This does not start the emulator or a UWP application. */
#include "test-d3d11-pixel-generated.h"
#include <d3d11shader.h>

static void test_pixel_output(ID3D11Device *device, ID3D11PixelShader *pixel)
{
    /* Texel coordinates (0.5,0.5) must select the first texel, whose alpha is
     * 0x40. R0 alpha must come from that sample, not the incoming coordinate W.
     * All resources belong to this offscreen test, not to an emulator session.
     */
    const char *vertex_source =
        "struct O {float4 p:SV_Position;float4 d:COLOR0;float4 s:COLOR1;"
        "float4 bd:TEXCOORD4;float4 bs:TEXCOORD5;float f:FOG;float ps:PSIZE;"
        "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;"
        "float4 t3:TEXCOORD3;};O main(uint id:SV_VertexID){O o=(O)0;"
        "o.p=float4(id==2?3:-1,id==1?3:-1,0,1);"
        "o.t0=float4(0.5,0.5,0,1);return o;}";
    ID3DBlob *blob = NULL;
    HRESULT hr = D3DCompile(vertex_source, strlen(vertex_source), "test-vs",
                            NULL, NULL, "main", "vs_5_0", 0, 0, &blob, NULL);
    assert(SUCCEEDED(hr));
    ID3D11VertexShader *vertex = NULL;
    hr = ID3D11Device_CreateVertexShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &vertex);
    assert(SUCCEEDED(hr));
    ID3D10Blob_Release(blob);
    ID3D11DeviceContext *context = NULL;
    ID3D11Device_GetImmediateContext(device, &context);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = 2,
        .Height = 2,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    const uint32_t texels[] = { 0x40000000, 0x80000000, 0xc0000000,
                                0xff000000 };
    D3D11_SUBRESOURCE_DATA data = { .pSysMem = texels, .SysMemPitch = 8 };
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
    for (unsigned int i = 0; i < 4; i++) {
        constants.tex_coord_scale[i][0] = constants.tex_coord_scale[i][1] =
            0.5f;
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
    D3D11_VIEWPORT viewport = { .Width = 1, .Height = 1, .MaxDepth = 1 };
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &sampler);
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
    const uint8_t expected[] = { 0, 0, 0, 0x40 };
    assert(memcmp(mapped.pData, expected, sizeof(expected)) == 0);
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
    puts("PASS: offscreen WARP pixel validates texel coordinates and R0 alpha");
}

void error_setg_internal(Error **errp, const char *src, int line,
                         const char *func, const char *fmt, ...)
{
    (void)errp;
    (void)src;
    (void)line;
    (void)func;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

int main(void)
{
    ID3D11Device *device = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &device, NULL, NULL);
    assert(SUCCEEDED(hr));
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
            /* Regression: initialize the combiner alpha after texture reads. */
            assert(strstr(source, "R0.a=T0.a") >
                   strstr(source, "T0=tex0.Sample"));
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
                test_pixel_output(device, shader);
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
    };
    for (unsigned int m = 0; m < ARRAY_SIZE(modes); m++) {
        for (unsigned int mapping = 0; mapping <= 4; mapping++) {
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
    ID3D11Device_Release(device);
    puts("PASS: 40 dependent/clip texture shaders, dot mappings 0-4, WARP "
         "creation");
    puts("PASS: 18 generated pixel shaders, WARP creation, cbuffer ABI");
    return 0;
}
