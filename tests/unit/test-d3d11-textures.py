#!/usr/bin/env python3
"""Compile and run the D3D11 CPU texture converter without starting xemu.

Run in an MSYS2 UCRT64 shell: python tests/unit/test-d3d11-textures.py
Tests actual functions extracted from textures.c, including native D3D11 WARP
resource creation and GPU readback. No UWP application is installed or started.
Compressed and special conversions are outside this isolated test's scope.
"""

import pathlib
import shlex
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
PGRAPH = ROOT / "hw/xbox/nv2a/pgraph"

PREAMBLE = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d11.h>
#define NV2A_MAX_TEXTURES 4
typedef uint64_t hwaddr;
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#define ROUND_UP(n, d) (((n) + (d) - 1) / (d) * (d))
"""

SUPPORT = r"""
static const BasicColorFormatInfo kelvin_color_format_info_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = {4, true, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] = {4, false, false},
};
static uint8_t *pgraph_convert_texture_data(TextureShape s, const uint8_t *data,
    const uint8_t *palette, unsigned int w, unsigned int h, unsigned int d,
    unsigned int row, unsigned int slice, size_t *size)
{
    (void)s; (void)data; (void)palette; (void)w; (void)h; (void)d;
    (void)row; (void)slice; (void)size;
    return NULL;
}
uint8_t *s3tc_decompress_3d(enum S3TC_DECOMPRESS_FORMAT f,
    const uint8_t *data, unsigned int w, unsigned int h, unsigned int d)
{
    (void)f; (void)data; (void)w; (void)h; (void)d;
    assert(!"Compressed textures are outside this test");
    return NULL;
}
"""

DEVICE_SUPPORT = r"""
typedef struct PGRAPHD3D11State {
    ID3D11Device *device;
    PGRAPHD3D11TextureState *textures;
} PGRAPHD3D11State;
typedef struct PGRAPHState {
    PGRAPHD3D11State *d3d11_renderer_state;
} PGRAPHState;
typedef struct NV2AState {
    PGRAPHState pgraph;
    uint8_t *vram_ptr;
} NV2AState;
typedef int Error;
static void error_setg(Error **errp, const char *format, ...)
{
    (void)errp;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}
/* Minimal guest-memory fixture for uncompressed mip chains. */
static size_t pgraph_get_texture_length(PGRAPHState *pg, TextureShape *s)
{
    (void)pg;
    unsigned int w = s->width;
    unsigned int h = s->dimensionality >= 2 ? s->height : 1;
    unsigned int d = s->dimensionality == 3 ? s->depth : 1;
    size_t length = 0;
    for (unsigned int level = 0; level < s->levels; level++) {
        length += (size_t)MAX(1, w) * MAX(1, h) * MAX(1, d) * 4;
        w /= 2; h /= 2; d /= 2;
    }
    return length;
}
"""

DEVICE_TEST = r"""
static void test_volume_upload(const uint8_t *swizzled,
                               const uint8_t *expected)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL,
                                  0, D3D11_SDK_VERSION, &device, &level,
                                  &context);
    assert(SUCCEEDED(hr));
    PGRAPHD3D11TextureState textures = { 0 };
    PGRAPHD3D11State renderer = { .device = device, .textures = &textures };
    uint8_t guest[36];
    memcpy(guest, swizzled, 32);
    /* Mip 1 is 1x1x1, not a second 2-slice volume. */
    const uint32_t mip_pixel = 0xffaabbcc;
    memcpy(guest + 32, &mip_pixel, 4);
    NV2AState nv2a = {
        .pgraph = { .d3d11_renderer_state = &renderer }, .vram_ptr = guest,
    };
    TextureShape shape = {
        .dimensionality = 3, .width = 2, .height = 2, .depth = 2, .levels = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8,
    };
    assert(d3d11_create_texture(&nv2a, 0, shape, 0, NULL, NULL));
    ID3D11Texture3D *volume = (ID3D11Texture3D *)textures.bindings[0].resource;
    D3D11_TEXTURE3D_DESC desc;
    ID3D11Texture3D_GetDesc(volume, &desc);
    assert(desc.Depth == 2 && desc.MipLevels == 2);
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    ID3D11ShaderResourceView_GetDesc(textures.bindings[0].view, &view);
    assert(view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D);
    assert(view.Texture3D.MipLevels == 2);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture3D *staging = NULL;
    hr = ID3D11Device_CreateTexture3D(device, &desc, NULL, &staging);
    assert(SUCCEEDED(hr));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                    (ID3D11Resource *)volume);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    for (unsigned int z = 0; z < 2; z++) {
        for (unsigned int y = 0; y < 2; y++) {
            const uint8_t *row = (const uint8_t *)mapped.pData +
                                 z * mapped.DepthPitch + y * mapped.RowPitch;
            assert(memcmp(row, expected + (z * 2 + y) * 8, 8) == 0);
        }
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 1,
                                 D3D11_MAP_READ, 0, &mapped);
    assert(SUCCEEDED(hr));
    const uint8_t mip_expected[] = {0xaa, 0xbb, 0xcc, 0xff};
    assert(memcmp(mapped.pData, mip_expected, 4) == 0);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 1);
    ID3D11Texture3D_Release(staging);
    d3d11_release_texture(&textures.bindings[0]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    g_print("PASS: D3D11 WARP volume, SRV, both mip levels GPU readback\n");
}
"""

TEST = r"""
int main(void)
{
    /* Padding bytes must never turn into visible pixels. Two slices also
     * exercise slice_pitch independently of the tight destination layout. */
    const uint32_t source[] = {
        0xff112233, 0x80445566, 0xdeadbeef,
        0x40778899, 0x20aabbcc, 0xdeadbeef,
        0xff010203, 0xff040506, 0xdeadbeef,
        0xff070809, 0xff0a0b0c, 0xdeadbeef,
    };
    const uint8_t expected[] = {
        0x11,0x22,0x33,0xff, 0x44,0x55,0x66,0x80,
        0x77,0x88,0x99,0x40, 0xaa,0xbb,0xcc,0x20,
        1,2,3,0xff, 4,5,6,0xff, 7,8,9,0xff, 10,11,12,0xff,
    };
    TextureShape shape = {
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .pitch = 12,
    };
    unsigned int consumed = 0;
    uint8_t *rgba = d3d11_convert_level(shape, (const uint8_t *)source, NULL,
                                       2, 2, 2, &consumed);
    assert(consumed == sizeof(source));
    assert(memcmp(rgba, expected, sizeof(expected)) == 0);
    g_free(rgba);

    /* Round-trip a real swizzled volume through the shared swizzle helper. */
    uint32_t linear[8] = {
        source[0], source[1], source[3], source[4],
        source[6], source[7], source[9], source[10],
    };
    uint8_t swizzled[sizeof(linear)];
    swizzle_box((const uint8_t *)linear, 2, 2, 2, swizzled, 8, 16, 4);
    shape.color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    rgba = d3d11_convert_level(shape, swizzled, NULL, 2, 2, 2, &consumed);
    assert(consumed == sizeof(linear));
    assert(memcmp(rgba, expected, sizeof(expected)) == 0);
    g_free(rgba);
    g_print("PASS: padded rows, slice pitch, channel order, swizzled volume\n");
    test_volume_upload(swizzled, expected);
    return 0;
}
"""


def main():
    source = (PGRAPH / "d3d11/textures.c").read_text(encoding="utf-8")
    converter = source[source.index("static uint8_t expand_bits"):
                       source.index("static D3D11_TEXTURE_ADDRESS_MODE")]
    header = (PGRAPH / "texture.h").read_text(encoding="utf-8")
    types = header[header.index("typedef struct TextureShape"):
                   header.index("extern const BasicColorFormatInfo")]
    bindings = (PGRAPH / "d3d11/textures.h").read_text(encoding="utf-8")
    bindings = bindings[bindings.index("typedef struct PGRAPHD3D11TextureBinding"):
                        bindings.index("bool pgraph_d3d11_textures_init")]
    release = source[source.index("static void d3d11_release_texture"):
                     source.index("bool pgraph_d3d11_textures_init")]
    create = source[source.index("static bool d3d11_create_texture"):
                    source.index("bool pgraph_d3d11_bind_textures")]
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-textures-") as tmp:
        tmp = pathlib.Path(tmp)
        test = tmp / "test.c"
        test.write_text(PREAMBLE + types + bindings + SUPPORT + DEVICE_SUPPORT +
                        release + converter + create + DEVICE_TEST + TEST,
                        encoding="utf-8")
        exe = tmp / "test.exe"
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare",
                        f"-I{ROOT}", str(test), str(PGRAPH / "swizzle.c"),
                        "-o", str(exe), *flags, "-ld3d11"], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
