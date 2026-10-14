#!/usr/bin/env python3
"""Test the actual presentation state guard with native D3D11 WARP.

Run in MSYS2 UCRT64: python tests/unit/test-d3d11-presentation.py
No emulator or installed UWP application is started.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "hw/xbox/nv2a/pgraph/d3d11/presentation.c").read_text()
guard = source[source.index("bool pgraph_d3d11_present_color("):]
fixture = r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define NV2A_MAX_TEXTURES 4
typedef void Error;
typedef struct { ID3D11DeviceContext *context; } PGRAPHD3D11State;
typedef struct { struct { PGRAPHD3D11State *d3d11_renderer_state; } pgraph; } NV2AState;
static bool outcome;
static bool d3d11_present_color(NV2AState *d, Error **errp)
{
    (void)errp;
    ID3D11DeviceContext_ClearState(d->pgraph.d3d11_renderer_state->context);
    return outcome;
}
'''
main = r'''
int main(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)));
    PGRAPHD3D11State r = {context};
    NV2AState d = {{&r}};
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture)));
    assert(SUCCEEDED(ID3D11Device_CreateShaderResourceView(device,
        (ID3D11Resource *)texture, NULL, &srv)));
    ID3D11ShaderResourceView *views[4] = {srv, srv, srv, srv};
    D3D11_VIEWPORT viewport = {2, 3, 17, 19, 0, 1};
    for (int success = 0; success < 2; success++) {
        outcome = success;
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(context,
            D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        ID3D11DeviceContext_PSSetShaderResources(context, 0, 4, views);
        assert(pgraph_d3d11_present_color(&d, NULL) == outcome);
        UINT count = 1;
        D3D11_VIEWPORT restored;
        ID3D11DeviceContext_RSGetViewports(context, &count, &restored);
        assert(count == 1 && restored.Width == 17 && restored.TopLeftY == 3);
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11DeviceContext_IAGetPrimitiveTopology(context, &topology);
        assert(topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        ID3D11ShaderResourceView *actual[4] = {0};
        ID3D11DeviceContext_PSGetShaderResources(context, 0, 4, actual);
        for (int i = 0; i < 4; i++) {
            assert(actual[i] == srv);
            ID3D11ShaderResourceView_Release(actual[i]);
        }
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11Texture2D_Release(texture);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: native WARP presentation state restoration on success and failure");
}
'''
profile = (root / "hw/xbox/nv2a/pgraph/profile.c").read_text()
profile = profile[profile.index("void nv2a_profile_increment("):
                  profile.index("void nv2a_profile_get_video_metrics(")]
profile_fixture = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define QEMU_CLOCK_REALTIME 0
#define NV2A_PROF_NUM_FRAMES 4
#define MAX(a,b) ((a)>(b)?(a):(b))
#define qatomic_set(p,v) (*(p)=(v))
static int64_t now;
static int64_t qemu_clock_get_us(int clock) {(void)clock; return now;}
struct Frame { int mspf; };
static struct {
    int64_t last_flip_time;
    unsigned int increment_fps, current_mspf, frame_ptr, frame_count;
    struct Frame frame_working, frame_history[4];
} g_nv2a_stats;
'''
profile_main = r'''
int main(void) {
    now = 11472088000LL;
    nv2a_profile_flip_stall();
    assert(g_nv2a_stats.current_mspf == 0);
    nv2a_profile_increment();
    now += 16000;
    nv2a_profile_flip_stall();
    assert(g_nv2a_stats.current_mspf == 16);
    for (int i=0; i<16; i++) { now += 16667; nv2a_profile_increment(); }
    assert(g_nv2a_stats.increment_fps >= 59 && g_nv2a_stats.increment_fps <= 65);
    puts("PASS: profiling ignores system uptime and measures subsequent frames");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-present-") as directory:
    tmp = pathlib.Path(directory)
    for name, content, libs in (
        ("presentation", fixture + guard + main, ["-ld3d11"]),
        ("profile", profile_fixture + profile + profile_main, []),
    ):
        cfile = tmp / (name + ".c")
        cfile.write_text(content)
        exe = tmp / (name + ".exe")
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(cfile),
                        "-o", str(exe), *libs], check=True)
        subprocess.run([str(exe)], check=True)
