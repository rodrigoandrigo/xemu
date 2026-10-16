#!/usr/bin/env python3
"""Test the actual presentation state guard with native D3D11 WARP.

Run in MSYS2 UCRT64: python tests/unit/test-d3d11-presentation.py
No emulator or installed UWP application is started.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / "hw/xbox/nv2a/pgraph/d3d11/presentation.c").read_text()


def extract_c_function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


host_source = (root / "system/host.c").read_text()
host_clear = extract_c_function(
    host_source, "static void host_clear_d3d12_present_target(void)")
host_set = extract_c_function(
    host_source, "int qemu_host_set_d3d12_present_target(")
host_get = extract_c_function(
    host_source, "int qemu_host_get_d3d12_present_target(")
host_cleanup = extract_c_function(host_source, "int qemu_host_cleanup(void)")
assert host_cleanup.index("xemu_shutdown_embedded_display();") < \
       host_cleanup.index("host_clear_d3d12_present_target();")
uwp_host_source = (root / "UWP-Port/XemuHost.cpp").read_text()
swapchain_callback = extract_c_function(
    uwp_host_source, "long __cdecl XemuHost::AttachMesaSwapChain(")
host_destructor = extract_c_function(
    uwp_host_source, "XemuHost::~XemuHost()")
assert "[host, attach]" not in swapchain_callback
assert "[attach]" in swapchain_callback
assert "state->active.load()" in swapchain_callback
assert "m_setMesaSwapChainAttach(nullptr, nullptr)" in host_destructor
assert "m_swapChainAttachState->active.store(false)" in host_destructor
assert "m_setD3D12PresentTarget(0, 0, nullptr, nullptr)" in host_destructor

host_target_fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { int unused; } GMutex;
static void g_mutex_lock(GMutex *mutex) { (void)mutex; }
static void g_mutex_unlock(GMutex *mutex) { (void)mutex; }
typedef long (*QemuHostSwapChainAttachCallback)(void *, void *);
typedef struct {
    uint32_t width;
    uint32_t height;
    QemuHostSwapChainAttachCallback attach;
    void *opaque;
} QemuHostD3D12PresentTarget;
static GMutex host_state_lock;
static bool host_cleaned;
static QemuHostD3D12PresentTarget host_d3d12_present_target;
''' + host_clear + host_set + host_get + r'''
static long dummy_attach(void *opaque, void *swapchain)
{
    (void)opaque;
    (void)swapchain;
    return 0;
}
int main(void)
{
    QemuHostD3D12PresentTarget target = {0};
    int opaque = 1;
    assert(qemu_host_get_d3d12_present_target(&target) == -ENODEV);
    assert(qemu_host_set_d3d12_present_target(1280, 720,
                                               dummy_attach, &opaque) == 0);
    assert(qemu_host_get_d3d12_present_target(&target) == 0);
    assert(target.width == 1280 && target.height == 720);
    assert(target.attach == dummy_attach && target.opaque == &opaque);
    assert(qemu_host_set_d3d12_present_target(0, 0, NULL, NULL) == 0);
    assert(qemu_host_get_d3d12_present_target(&target) == -ENODEV);
    assert(qemu_host_set_d3d12_present_target(1280, 0,
                                               dummy_attach, &opaque) == -EINVAL);
    host_cleaned = true;
    assert(qemu_host_set_d3d12_present_target(1280, 720,
                                               dummy_attach, &opaque) == -EBUSY);
    puts("PASS: presentation target registration and unregister");
}
'''

scanout_scale = source[
    source.index("static float d3d11_scanout_texcoord_scale_y("):
    source.index("static bool d3d11_present_color(")]
guard = source[source.index("bool pgraph_d3d11_present_color("):]
fixture = r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
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
    assert(d3d11_scanout_texcoord_scale_y(480, 960, 2560, 2560) == 0.5f);
    assert(d3d11_scanout_texcoord_scale_y(480, 960, 5120, 2560) == 0.25f);
    assert(d3d11_scanout_texcoord_scale_y(480, 960, 2560, 0) == 0.5f);
    assert(d3d11_scanout_texcoord_scale_y(480, 0, 2560, 2560) == 1.0f);
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
    D3D11_BUFFER_DESC buffer_desc = {0};
    buffer_desc.ByteWidth = 16;
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *constant_buffer = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateBuffer(device, &buffer_desc, NULL,
                                               &constant_buffer)));
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
        ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1,
                                                 &constant_buffer);
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
        ID3D11Buffer *actual_constant_buffer = NULL;
        ID3D11DeviceContext_PSGetConstantBuffers(context, 0, 1,
                                                 &actual_constant_buffer);
        assert(actual_constant_buffer == constant_buffer);
        ID3D11Buffer_Release(actual_constant_buffer);
    }
    ID3D11DeviceContext_ClearState(context);
    ID3D11ShaderResourceView_Release(srv);
    ID3D11Texture2D_Release(texture);
    ID3D11Buffer_Release(constant_buffer);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: native WARP presentation state and PS b0 restoration on success and failure");
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
        ("presentation", fixture + scanout_scale + guard + main, ["-ld3d11"]),
        ("profile", profile_fixture + profile + profile_main, []),
        ("host-target", host_target_fixture, []),
    ):
        if "--host-only" in sys.argv and name != "host-target":
            continue
        cfile = tmp / (name + ".c")
        cfile.write_text(content)
        exe = tmp / (name + ".exe")
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(cfile),
                        "-o", str(exe), *libs], check=True)
        subprocess.run([str(exe)], check=True)
