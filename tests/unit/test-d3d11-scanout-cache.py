#!/usr/bin/env python3
"""Exercise actual backbuffer retention and PCRTC lookup on native WARP."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'hw/xbox/nv2a/pgraph/d3d11/presentation.c').read_text()
header = (root / 'hw/xbox/nv2a/pgraph/d3d11/presentation.h').read_text()
types = header[header.index('typedef struct PGRAPHD3D11CachedScanout'):
               header.index('bool pgraph_d3d11_presentation_init')]
retain = source[source.index('static bool d3d11_retain_scanout'):
                source.index('bool pgraph_d3d11_retain_color_scanout')]
lookup = source[source.index('static PGRAPHD3D11CachedScanout *d3d11_find'):
                source.index('static bool d3d11_prepare_raw_scanout')]
fixture = r'''
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MiB (1024*1024)
#define DIRTY_MEMORY_NV2A 1
typedef void Error;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;
typedef struct NV2AState NV2AState;
'''
state = r'''
typedef struct {
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *srv;
    uint32_t width, height;
    uint32_t pitch;
    uint64_t vram_address;
    size_t storage_length;
} PGRAPHD3D11RenderTarget;
struct PGRAPHD3D11State {
    PGRAPHD3D11PresentationState *presentation;
    unsigned int invalidated_scanouts;
};
struct NV2AState {
    struct { PGRAPHD3D11State *d3d11_renderer_state; } pgraph;
    struct { uint64_t start; } pcrtc;
    void *vram;
};
static uint64_t dirty_address;
static bool memory_region_test_and_clear_dirty(void *vram, uint64_t address,
                                               size_t length, int client)
{
    (void)vram; (void)length; (void)client;
    if (address == dirty_address) { dirty_address = 0; return true; }
    return false;
}
'''
main = r'''
int main(void) {
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    assert(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,0,NULL,0,
        D3D11_SDK_VERSION,&device,NULL,&context)));
    PGRAPHD3D11PresentationState presentation = {0};
    PGRAPHD3D11State renderer = {&presentation,0};
    NV2AState d = {{&renderer},{0},NULL};
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = desc.Height = 2;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (unsigned int i = 1; i <= 10; i++) {
        uint32_t pixels[4] = {i,i,i,i};
        D3D11_SUBRESOURCE_DATA data = {pixels,8,0};
        PGRAPHD3D11RenderTarget target = {0};
        target.width = target.height = 2;
        target.pitch = 8;
        target.vram_address = i * 4096;
        target.storage_length = 128;
        assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&desc,&data,&target.texture)));
        assert(SUCCEEDED(ID3D11Device_CreateShaderResourceView(device,
            (ID3D11Resource *)target.texture,NULL,&target.srv)));
        /* PCRTC has not selected this buffer yet. */
        assert(d3d11_retain_scanout(&renderer,&target,NULL));
        ID3D11ShaderResourceView_Release(target.srv);
        ID3D11Texture2D_Release(target.texture);
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    assert(SUCCEEDED(ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging)));
    for (unsigned int i = 3; i <= 10; i++) {
        d.pcrtc.start = i * 4096 + 64;
        PGRAPHD3D11CachedScanout *entry = d3d11_find_retained_scanout(&d);
        assert(entry && entry->address == i*4096 && entry->pitch == 8);
        ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,
                                         (ID3D11Resource *)entry->texture);
        D3D11_MAPPED_SUBRESOURCE mapped;
        assert(SUCCEEDED(ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,
                                                0,D3D11_MAP_READ,0,&mapped)));
        assert(*(uint32_t *)mapped.pData == i);
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
    }
    d.pcrtc.start = 4096;
    assert(!d3d11_find_retained_scanout(&d)); /* LRU eviction */
    /* A surface can start on the second scanout row. Its pixels are on the
     * GPU, while the raw guest framebuffer still contains zeros. */
    d.pcrtc.start = 3*4096-2560;
    presentation.scanout_line_offset = 2560;
    assert(d3d11_find_retained_scanout(&d)->address == 3*4096);
    presentation.scanout_line_offset = 0;
    assert(!d3d11_find_retained_scanout(&d));
    d.pcrtc.start = 3*4096;
    dirty_address = d.pcrtc.start;
    assert(!d3d11_find_retained_scanout(&d));
    assert(!d3d11_find_retained_scanout(&d)); /* no stale reuse next frame */
    d.pcrtc.start = 10*4096;
    dirty_address = d.pcrtc.start;
    assert(!d3d11_find_retained_scanout(&d)); /* a hole before an older entry */
    d.pcrtc.start = 5*4096;
    PGRAPHD3D11CachedScanout *entry = d3d11_find_retained_scanout(&d);
    assert(entry);
    PGRAPHD3D11RenderTarget replacement = {
        entry->texture, entry->srv, entry->width, entry->height, entry->pitch,
        entry->address, entry->length
    };
    ID3D11Texture2D_AddRef(replacement.texture);
    ID3D11ShaderResourceView_AddRef(replacement.srv);
    assert(d3d11_retain_scanout(&renderer,&replacement,NULL));
    ID3D11ShaderResourceView_Release(replacement.srv);
    ID3D11Texture2D_Release(replacement.texture);
    unsigned int duplicates = 0;
    for (unsigned int i=0;i<8;i++) {
        duplicates += presentation.retained[i].srv &&
                      presentation.retained[i].address == d.pcrtc.start;
    }
    assert(duplicates == 1);
    dirty_address = d.pcrtc.start;
    assert(!d3d11_find_retained_scanout(&d));
    assert(!d3d11_find_retained_scanout(&d));
    for (unsigned int i=0;i<8;i++) {
        if (presentation.retained[i].srv) {
            ID3D11ShaderResourceView_Release(presentation.retained[i].srv);
            ID3D11Texture2D_Release(presentation.retained[i].texture);
        }
    }
    ID3D11Texture2D_Release(staging);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("PASS: future backbuffers, eight retained surfaces, PCRTC+64, GPU pixels, eviction, CPU invalidation");
}
'''
if sys.platform != "win32":
    print("SKIP: D3D11 WARP scanout-cache fixture requires Windows/MSYS2")
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix='xemu-scanout-') as directory:
    tmp = pathlib.Path(directory)
    cfile = tmp / 'test.c'
    cfile.write_text(fixture + types + state + retain + lookup + main)
    exe = tmp / 'test.exe'
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', str(cfile),
                    '-o', str(exe), '-ld3d11'], check=True)
    subprocess.run([str(exe)], check=True)
