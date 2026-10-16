/* SPDX-License-Identifier: GPL-2.0-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vertex-output-generated.h"

static void test_generated_fog_hlsl(ID3D11Device *device_10_0,
                                   ID3D11Device *device_10_1,
                                   ID3D11Device *device_11)
{
    static const char *profiles[] = { "vs_4_0", "vs_4_1", "vs_5_0" };
    ID3D11Device *devices[] = { device_10_0, device_10_1, device_11 };
    for (unsigned int i = 0; i < sizeof(fog_factors) / sizeof(fog_factors[0]);
         i++) {
        char source[4096];
        int length = snprintf(
            source, sizeof(source),
            "cbuffer FogConstants:register(b0){float2 fogParam;float4 C[192];};"
            "struct VSIn{float4 v5:ATTRIBUTE0;};"
            "struct VSOut{float4 position:SV_Position;float fog:TEXCOORD0;};"
            "VSOut main(VSIn input){"
            "float4 tPosition=float4(1,2,3,1);"
            "float4 specular=float4(0,0,0,0.5);"
            "float4 oFog=float4(0.25,0,0,0);"
            "%s"
            "VSOut output;output.position=float4(0,0,0,1);"
            "output.fog=fogFactor;return output;}", fog_factors[i]);
        assert(length > 0 && (size_t)length < sizeof(source));
        for (unsigned int profile = 0;
             profile < sizeof(profiles) / sizeof(profiles[0]); profile++) {
            ID3DBlob *blob = NULL;
            ID3DBlob *errors = NULL;
            HRESULT result = D3DCompile2(
                source, (SIZE_T)length, "generated-fog", NULL, NULL,
                "main", profiles[profile], 0, 0, 0, NULL, 0, &blob, &errors);
            if (FAILED(result) && errors) {
                fwrite(ID3D10Blob_GetBufferPointer(errors), 1,
                       ID3D10Blob_GetBufferSize(errors), stderr);
            }
            assert(SUCCEEDED(result));
            if (errors) {
                ID3D10Blob_Release(errors);
            }
            ID3D11VertexShader *shader = NULL;
            assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
                devices[profile], ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), NULL, &shader)));
            ID3D11VertexShader_Release(shader);
            ID3D10Blob_Release(blob);
        }
    }
}

int main(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &device,
                                       NULL, &context)));
    const D3D_FEATURE_LEVEL requested_level_10_0 = D3D_FEATURE_LEVEL_10_0;
    D3D_FEATURE_LEVEL obtained_level = D3D_FEATURE_LEVEL_9_1;
    ID3D11Device *device_10_0 = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &requested_level_10_0, 1,
        D3D11_SDK_VERSION, &device_10_0, &obtained_level, NULL)));
    assert(obtained_level == D3D_FEATURE_LEVEL_10_0);
    const D3D_FEATURE_LEVEL requested_level = D3D_FEATURE_LEVEL_10_1;
    ID3D11Device *device_10_1 = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &requested_level, 1,
        D3D11_SDK_VERSION, &device_10_1, &obtained_level, NULL)));
    assert(obtained_level == D3D_FEATURE_LEVEL_10_1);
    test_generated_fog_hlsl(device_10_0, device_10_1, device);
    const char *pixel_source =
        "float4 main():SV_Target{return float4(1,1,1,1);}";
    ID3DBlob *blob = NULL;
    assert(
        SUCCEEDED(D3DCompile(pixel_source, strlen(pixel_source), "pixel", NULL,
                             NULL, "main", "ps_5_0", 0, 0, &blob, NULL)));
    ID3D11PixelShader *pixel = NULL;
    assert(SUCCEEDED(ID3D11Device_CreatePixelShader(
        device, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), NULL, &pixel)));
    ID3D10Blob_Release(blob);
    D3D11_TEXTURE2D_DESC desc = { 0 };
    desc.Width = desc.Height = 32;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    assert(
        SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target)));
    assert(SUCCEEDED(ID3D11Device_CreateRenderTargetView(
        device, (ID3D11Resource *)target, NULL, &rtv)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(
        SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)));
    D3D11_RASTERIZER_DESC raster_desc = { 0 };
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_BACK;
    assert(!XEMU_FRONT_COUNTER_CLOCKWISE(0));
    raster_desc.FrontCounterClockwise = XEMU_FRONT_COUNTER_CLOCKWISE(1);
    raster_desc.DepthClipEnable = TRUE;
    ID3D11RasterizerState *raster = NULL;
    assert(SUCCEEDED(
        ID3D11Device_CreateRasterizerState(device, &raster_desc, &raster)));
    for (unsigned int i = 0; i < sizeof(epilogues) / sizeof(epilogues[0]);
         i++) {
        char vertex_source[4096];
        snprintf(
            vertex_source, sizeof(vertex_source),
            "float nv_clamp_away(float x){return x;}"
            "float4 main(uint id:SV_VertexID):SV_Position{"
            "float2 surfaceSize=float2(32,32);float4 clipRange=float4(0,1,0,1);"
            "float4 oPos=float4(id==2?%u:0,id==1?%u:0,%f,2);"
            "%s return oPos;}",
            i ? 32 : 16, i ? 32 : 16, i ? 1.5 : 0.5, epilogues[i]);
        assert(SUCCEEDED(D3DCompile(vertex_source, strlen(vertex_source),
                                    "vertex-fl10-0", NULL, NULL, "main",
                                    "vs_4_0", 0, 0, &blob, NULL)));
        ID3D11VertexShader *vertex_4_0 = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
            device_10_0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), NULL, &vertex_4_0)));
        ID3D11VertexShader_Release(vertex_4_0);
        ID3D10Blob_Release(blob);
        assert(SUCCEEDED(D3DCompile(vertex_source, strlen(vertex_source),
                                    "vertex-fl10", NULL, NULL, "main",
                                    "vs_4_1", 0, 0, &blob, NULL)));
        ID3D11VertexShader *vertex_4_1 = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
            device_10_1, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), NULL, &vertex_4_1)));
        ID3D11VertexShader_Release(vertex_4_1);
        ID3D10Blob_Release(blob);
        assert(SUCCEEDED(D3DCompile(vertex_source, strlen(vertex_source),
                                    "vertex", NULL, NULL, "main", "vs_5_0", 0,
                                    0, &blob, NULL)));
        ID3D11VertexShader *vertex = NULL;
        assert(SUCCEEDED(ID3D11Device_CreateVertexShader(
            device, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), NULL, &vertex)));
        ID3D10Blob_Release(blob);
        const float black[4] = { 0, 0, 0, 1 };
        D3D11_VIEWPORT viewport = { 0, 0, 32, 32, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, black);
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetState(context, raster);
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(context, vertex, NULL, 0);
        ID3D11DeviceContext_PSSetShader(context, pixel, NULL, 0);
        ID3D11DeviceContext_Draw(context, 3, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                         (ID3D11Resource *)target);
        D3D11_MAPPED_SUBRESOURCE mapped;
        assert(SUCCEEDED(ID3D11DeviceContext_Map(context,
                                                 (ID3D11Resource *)staging, 0,
                                                 D3D11_MAP_READ, 0, &mapped)));
        unsigned char *data = mapped.pData;
        unsigned char expected = i == 2 ? 0 : 255;
        assert(data[3 * mapped.RowPitch + 3 * 4] == expected);
        assert(data[2 * mapped.RowPitch + 10 * 4] == expected);
        assert(data[2 * mapped.RowPitch + 20 * 4] == 0);
        assert(data[28 * mapped.RowPitch + 3 * 4] == 0);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        ID3D11VertexShader_Release(vertex);
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11RasterizerState_Release(raster);
    ID3D11Texture2D_Release(staging);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);
    ID3D11PixelShader_Release(pixel);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device_10_0);
    ID3D11Device_Release(device_10_1);
    ID3D11Device_Release(device);
    puts("PASS: vs_4_0/vs_4_1 output, top-left pixels, culling, W=2; "
         "old projection clips to black");
    puts("PASS: generated vertex epilogues compile and create as vs_4_1 on WARP FL 10.1");
    puts("PASS: generated fog helpers compile/create via D3DCompile2 at FL 10.0/10.1/11.0");
    return 0;
}
