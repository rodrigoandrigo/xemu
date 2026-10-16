#!/usr/bin/env python3
"""Compile and execute the production D3D11 texture-bind path with mocked edges.

This portable regression extracts the production texture-register preflight,
texture-bind path, range helpers, and checked DMA mapper. Mocks provide RAMIN/
VRAM storage, surface flush, texture creation, samplers, and final D3D11 context
calls. It checks that unsupported formats, invalid combiner counts, zero mips,
zero dimensionality and invalid linear dimensionality fail before shared shape parsing; malformed DMA objects and
out-of-VRAM bases are rejected without assertions; DMA lengths and texture/
palette spans are bounded; and aliased dirty targets are flushed before reads.
It does not emulate D3D11/WARP or validate pixels.
"""

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
PGRAPH = ROOT / "hw/xbox/nv2a/pgraph"

HARNESS = r"""
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define ctz32(value) __builtin_ctz(value)
#include "hw/xbox/nv2a/nv2a_regs.h"
#define NV2A_MAX_TEXTURES 4
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef uint64_t hwaddr;
typedef int Error;
typedef struct MemoryRegion {
    bool is_ramin;
} MemoryRegion;
typedef struct DMAObject {
    unsigned int dma_class;
    unsigned int dma_target;
    hwaddr address;
    hwaddr limit;
} DMAObject;
typedef struct ID3D11ShaderResourceView { int marker; } ID3D11ShaderResourceView;
typedef struct ID3D11SamplerState { int marker; } ID3D11SamplerState;
typedef struct ID3D11DeviceContext { int marker; } ID3D11DeviceContext;

__TEXTURE_SHAPE__

static const BasicColorFormatInfo kelvin_color_format_info_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = {1, false, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = {4, true, false},
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] = {2, false, false},
};

typedef struct Surface {
    bool draw_dirty;
} Surface;
typedef struct PGRAPHD3D11RenderTarget {
    void *texture;
    hwaddr vram_address;
    size_t storage_length;
} PGRAPHD3D11RenderTarget;
typedef struct PGRAPHD3D11RenderTargets {
    PGRAPHD3D11RenderTarget color;
    PGRAPHD3D11RenderTarget depth_stencil;
} PGRAPHD3D11RenderTargets;
typedef struct PGRAPHD3D11TextureBinding {
    ID3D11ShaderResourceView *view;
    bool surface_copy;
    uint64_t data_hash;
    hwaddr address;
    size_t length;
    TextureShape shape;
} PGRAPHD3D11TextureBinding;
typedef struct PGRAPHD3D11TextureState {
    PGRAPHD3D11TextureBinding bindings[NV2A_MAX_TEXTURES];
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES];
    bool transfer_disabled;
} PGRAPHD3D11TextureState;
typedef struct PGRAPHD3D11State {
    PGRAPHD3D11TextureState *textures;
    PGRAPHD3D11RenderTargets render_targets;
    ID3D11DeviceContext *context;
} PGRAPHD3D11State;
typedef struct PGRAPHState {
    PGRAPHD3D11State *d3d11_renderer_state;
    Surface surface_color;
    Surface surface_zeta;
    hwaddr dma_a;
    hwaddr dma_b;
    bool texture_enabled[NV2A_MAX_TEXTURES];
    bool texture_dirty[NV2A_MAX_TEXTURES];
} PGRAPHState;
typedef struct NV2AState {
    PGRAPHState pgraph;
    uint8_t *vram_ptr;
    MemoryRegion *vram;
    MemoryRegion ramin;
    uint8_t *ramin_ptr;
} NV2AState;

static uint8_t guest_vram[0x5000];
static uint8_t guest_ramin[0x80];
static MemoryRegion vram_region;
static size_t configured_vram_size;
static size_t configured_ramin_size;
static uint32_t configured_dma_a_frame;
static uint32_t configured_dma_b_frame;
static hwaddr configured_dma_a_limit;
static hwaddr configured_dma_b_limit;
static uint32_t configured_texture_filter;
static uint32_t configured_combiner_control;
static TextureShape configured_shape;
static hwaddr configured_texture_address;
static size_t configured_texture_length;
static unsigned int texture_length_calls;
static unsigned int texture_shape_calls;
static hwaddr configured_palette_address;
static unsigned int configured_palette_length_index;
static bool configured_texture_dma_b;
static bool configured_palette_dma_b;
static unsigned int flush_calls;
static unsigned int hash_calls;
static unsigned int create_calls;
static unsigned int dma_map_calls;
static bool fail_flush;
static bool allow_expected_error;
static bool saw_expected_error;
static bool expect_flush_before_read;
static bool expect_palette_refresh;
static bool expect_null_palette;
static bool saw_refreshed_palette_hash;
static ID3D11DeviceContext mock_context;
static ID3D11ShaderResourceView mock_view;

static void test_trace_dma_map(hwaddr object, unsigned int dma_class,
                               unsigned int dma_target, hwaddr address,
                               hwaddr limit)
{
    (void)object;
    (void)dma_class;
    (void)dma_target;
    (void)address;
    (void)limit;
    dma_map_calls++;
}
#define trace_nv2a_dma_map(...) test_trace_dma_map(__VA_ARGS__)

static uint32_t test_ldl_le_p(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
#define ldl_le_p(pointer) test_ldl_le_p((const uint8_t *)(pointer))

static void test_stl_le_p(uint8_t *bytes, uint32_t value)
{
    bytes[0] = value;
    bytes[1] = value >> 8;
    bytes[2] = value >> 16;
    bytes[3] = value >> 24;
}

static void store_dma_object(hwaddr object, uint32_t frame, uint32_t limit)
{
    assert(object + 3 * sizeof(uint32_t) <= sizeof(guest_ramin));
    test_stl_le_p(guest_ramin + object, NV_DMA_IN_MEMORY_CLASS);
    test_stl_le_p(guest_ramin + object + 4, limit);
    test_stl_le_p(guest_ramin + object + 8, frame);
}

static uint64_t memory_region_size(const MemoryRegion *region)
{
    return region->is_ramin ? configured_ramin_size : configured_vram_size;
}

static bool pgraph_is_texture_enabled(PGRAPHState *pg, unsigned int stage)
{
    return pg->texture_enabled[stage];
}

static TextureShape pgraph_get_texture_shape(PGRAPHState *pg, int stage)
{
    (void)pg;
    assert(stage == 0);
    texture_shape_calls++;
    return configured_shape;
}

static uint32_t pgraph_reg_r(PGRAPHState *pg, unsigned int reg)
{
    (void)pg;
    if (reg == NV_PGRAPH_TEXFMT0) {
        uint32_t format = configured_texture_dma_b ?
                              NV_PGRAPH_TEXFMT0_CONTEXT_DMA : 0;
        format |= (configured_shape.color_format <<
                   ctz32(NV_PGRAPH_TEXFMT0_COLOR)) &
                  NV_PGRAPH_TEXFMT0_COLOR;
        format |= (configured_shape.dimensionality <<
                   ctz32(NV_PGRAPH_TEXFMT0_DIMENSIONALITY)) &
                  NV_PGRAPH_TEXFMT0_DIMENSIONALITY;
        if (configured_shape.cubemap) {
            format |= NV_PGRAPH_TEXFMT0_CUBEMAPENABLE;
        }
        format |= (configured_shape.levels <<
                   ctz32(NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS)) &
                  NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS;
        return format;
    }
    if (reg == NV_PGRAPH_TEXFILTER0) {
        return configured_texture_filter;
    }
    if (reg == NV_PGRAPH_COMBINECTL) {
        return configured_combiner_control;
    }
    if (reg == NV_PGRAPH_TEXOFFSET0) {
        return configured_texture_address;
    }
    if (reg == NV_PGRAPH_TEXPALETTE0) {
        return (configured_palette_dma_b ?
                    NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA : 0) |
               ((configured_palette_length_index <<
                 ctz32(NV_PGRAPH_TEXPALETTE0_LENGTH)) &
                NV_PGRAPH_TEXPALETTE0_LENGTH) |
               (configured_palette_address & NV_PGRAPH_TEXPALETTE0_OFFSET);
    }
    return 0;
}

static size_t pgraph_get_texture_length(PGRAPHState *pg, TextureShape *shape)
{
    (void)pg;
    (void)shape;
    texture_length_calls++;
    return configured_texture_length;
}

static uint64_t fast_hash(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    hash_calls++;
    if (expect_flush_before_read) {
        assert(flush_calls == 1);
    }
    if (data == guest_vram + configured_palette_address) {
        if (expect_palette_refresh) {
            assert(bytes[0x80] == 0x5a);
            saw_refreshed_palette_hash = true;
        }
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool pgraph_d3d11_surface_flush(NV2AState *d, Error **errp)
{
    (void)errp;
    flush_calls++;
    if (fail_flush) {
        return false;
    }
    PGRAPHD3D11RenderTarget *target =
        &d->pgraph.d3d11_renderer_state->render_targets.color;
    memset(guest_vram + target->vram_address, 0x5a, target->storage_length);
    d->pgraph.surface_color.draw_dirty = false;
    return true;
}

static bool d3d11_create_texture(NV2AState *d, unsigned int stage,
                                 TextureShape shape, hwaddr address,
                                 const uint8_t *palette, Error **errp)
{
    (void)errp;
    assert(stage == 0);
    create_calls++;
    if (expect_palette_refresh) {
        assert(palette == guest_vram + configured_palette_address);
        assert(palette[0x80] == 0x5a);
    }
    if (expect_null_palette) {
        assert(palette == NULL);
    }
    PGRAPHD3D11TextureBinding *binding =
        &d->pgraph.d3d11_renderer_state->textures->bindings[stage];
    binding->view = &mock_view;
    binding->address = address;
    binding->length = configured_texture_length;
    binding->shape = shape;
    return true;
}

static bool d3d11_update_sampler(PGRAPHD3D11State *r, PGRAPHState *pg,
                                 unsigned int stage, Error **errp)
{
    (void)r;
    (void)pg;
    (void)errp;
    assert(stage == 0);
    return true;
}

static void error_setg(Error **errp, const char *format, ...)
{
    (void)errp;
    (void)format;
    if (allow_expected_error) {
        saw_expected_error = true;
        return;
    }
    assert(!"unexpected error in texture-bind fixture");
}
#define QEMU_HOST_LOG_WARNING 2
static const char *error_get_pretty(Error *error) { (void)error; return "mock"; }
static void error_free(Error *error) { (void)error; }
static void qemu_host_emit_log(int level, const char *text) { (void)level; (void)text; }

static void ID3D11DeviceContext_PSSetShaderResources(
    ID3D11DeviceContext *context, unsigned int start, unsigned int count,
    ID3D11ShaderResourceView *const *views)
{
    (void)context;
    (void)start;
    (void)count;
    (void)views;
}

static void ID3D11DeviceContext_PSSetSamplers(
    ID3D11DeviceContext *context, unsigned int start, unsigned int count,
    ID3D11SamplerState *const *samplers)
{
    (void)context;
    (void)start;
    (void)count;
    (void)samplers;
}

__SHARED_DMA_HELPERS__
__RANGE_HELPERS__
__DMA_HELPERS__
static bool d3d11_surface_texture_compatible(
    const PGRAPHD3D11RenderTarget *target, const Surface *surface,
    TextureShape shape, hwaddr address, size_t length)
{
    (void)target; (void)surface; (void)shape; (void)address; (void)length;
    return false; /* Real compatibility and GPU copies run in the WARP fixture. */
}
static bool d3d11_snapshot_surface(PGRAPHD3D11State *r,
    PGRAPHD3D11TextureBinding *binding, const PGRAPHD3D11RenderTarget *target,
    TextureShape shape, Error **errp)
{
    (void)r; (void)binding; (void)target; (void)shape; (void)errp;
    assert(!"unexpected GPU copy in fallback-only mock fixture");
    return false;
}
static bool d3d11_transfer_chain(NV2AState *d, unsigned int stage,
    TextureShape shape, hwaddr address, size_t length, bool *handled, Error **errp)
{
    (void)d; (void)stage; (void)shape; (void)address; (void)length; (void)errp;
    *handled = false;
    return true;
}
__BIND_FUNCTION__

static NV2AState fixture_init(PGRAPHD3D11State *renderer,
                              PGRAPHD3D11TextureState *textures)
{
    memset(guest_vram, 0x11, sizeof(guest_vram));
    memset(guest_ramin, 0, sizeof(guest_ramin));
    memset(renderer, 0, sizeof(*renderer));
    memset(textures, 0, sizeof(*textures));
    flush_calls = hash_calls = create_calls = dma_map_calls = 0;
    texture_length_calls = 0;
    texture_shape_calls = 0;
    fail_flush = false;
    allow_expected_error = false;
    saw_expected_error = false;
    expect_flush_before_read = false;
    expect_palette_refresh = false;
    expect_null_palette = false;
    saw_refreshed_palette_hash = false;
    configured_vram_size = sizeof(guest_vram);
    configured_ramin_size = sizeof(guest_ramin);
    configured_dma_a_frame = 0;
    configured_dma_b_frame = 0x1000;
    configured_dma_a_limit = sizeof(guest_vram) - 1;
    configured_dma_b_limit = sizeof(guest_vram) - 0x1000 - 1;
    store_dma_object(0, configured_dma_a_frame, configured_dma_a_limit);
    store_dma_object(0x10, configured_dma_b_frame, configured_dma_b_limit);
    configured_texture_address = 0x100;
    configured_texture_length = 0x20;
    configured_texture_filter = 0;
    configured_combiner_control = 0;
    configured_palette_address = 0x300;
    configured_palette_length_index = NV_PGRAPH_TEXPALETTE0_LENGTH_64;
    configured_texture_dma_b = false;
    configured_palette_dma_b = false;
    configured_shape = (TextureShape) {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
    };
    renderer->textures = textures;
    renderer->context = &mock_context;
    renderer->render_targets.color.texture = (void *)1;
    renderer->render_targets.color.vram_address = 0x380;
    renderer->render_targets.color.storage_length = 0x20;
    NV2AState d = {
        .vram_ptr = guest_vram,
        .vram = &vram_region,
        .ramin = { .is_ramin = true },
        .ramin_ptr = guest_ramin,
    };
    d.pgraph.d3d11_renderer_state = renderer;
    d.pgraph.dma_a = 0;
    d.pgraph.dma_b = 0x10;
    d.pgraph.texture_enabled[0] = true;
    d.pgraph.texture_dirty[0] = true;
    d.pgraph.surface_color.draw_dirty = true;
    return d;
}

static void test_palette_alias_flushes_before_hash_and_upload(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    /* Texels [0x100,0x120) do not alias target [0x380,0x3a0),
     * but the indexed palette [0x300,0x400) does. */
    expect_flush_before_read = true;
    expect_palette_refresh = true;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 1);
    assert(hash_calls == 2);
    assert(create_calls == 1);
    assert(saw_refreshed_palette_hash);
    assert(!d.pgraph.surface_color.draw_dirty);
    assert(!d.pgraph.texture_dirty[0]);
    puts("PASS: indexed palette alias flush precedes VRAM hash and texture upload");
}

static void test_texel_alias_flushes_before_hash(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    renderer.render_targets.color.vram_address = configured_texture_address;
    expect_flush_before_read = true;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 1);
    assert(hash_calls == 2);
    puts("PASS: texture texel alias flush precedes VRAM hash");
}

static void test_nonindexed_texture_ignores_invalid_palette_state(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_shape.color_format = 0x29;
    configured_palette_address = 0x4f80;
    expect_null_palette = true;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 0);
    assert(hash_calls == 1);
    assert(create_calls == 1);
    assert(dma_map_calls == 1);
    puts("PASS: non-indexed texture ignores invalid palette state");
}

static void test_indexed_invalid_palette_fails_before_vram_reads(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_palette_address = 0x4f80;
    configured_dma_a_limit = sizeof(guest_vram) + 0x100;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 2);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: indexed palette outside VRAM aborts before hashing or creation");
}

static void test_texture_beyond_dma_limit_fails_before_vram_reads(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_dma_a_limit = configured_texture_address +
                              configured_texture_length - 2;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 1);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: texture crossing its DMA limit is rejected before VRAM reads");
}

static void test_texture_ending_at_inclusive_dma_limit_is_allowed(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_shape.color_format = 0x29;
    configured_dma_a_limit = configured_texture_address +
                             configured_texture_length - 1;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    d.pgraph.surface_color.draw_dirty = false;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(hash_calls == 1);
    assert(create_calls == 1);
    puts("PASS: texture whose final byte equals the inclusive DMA limit binds");
}

static void test_palette_beyond_dma_limit_fails_before_vram_reads(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_dma_a_limit = configured_palette_address + 0x80;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 2);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: palette crossing its DMA limit is rejected before VRAM reads");
}

static void test_dma_b_texture_address_uses_mapping_base(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_texture_dma_b = true;
    configured_texture_address = 0x10;
    d.pgraph.surface_color.draw_dirty = false;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(dma_map_calls == 2);
    assert(textures.bindings[0].address == 0x1010);
    puts("PASS: texture DMA-B offset is relative to its mapped VRAM base");
}

static void test_checked_mapping_caps_length_to_vram(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_vram_size = 0x4010;
    configured_dma_a_frame = 0x4000;
    configured_dma_a_limit = UINT32_MAX;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);

    uint8_t *mapping = NULL;
    hwaddr length = 0;
    assert(nv_dma_map_checked(&d, d.pgraph.dma_a, &mapping, &length));
    assert(mapping == guest_vram + 0x4000);
    assert(length == 0x10);

    configured_dma_a_limit = 0x7;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    assert(nv_dma_map_checked(&d, d.pgraph.dma_a, &mapping, &length));
    assert(length == 0x8);

    configured_dma_a_limit = 0;
    store_dma_object(d.pgraph.dma_a, configured_dma_a_frame,
                     configured_dma_a_limit);
    assert(nv_dma_map_checked(&d, d.pgraph.dma_a, &mapping, &length));
    assert(length == 1);
    puts("PASS: checked DMA length converts inclusive limits and caps to VRAM");
}

static void test_checked_mapping_rejects_non_memory_class(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    test_stl_le_p(guest_ramin + d.pgraph.dma_a, NV_DMA_TO_MEMORY_CLASS);

    uint8_t *mapping = (uint8_t *)1;
    hwaddr length = 7;
    assert(!nv_dma_map_checked(&d, d.pgraph.dma_a, &mapping, &length));
    assert(mapping == NULL && length == 0);
    assert(dma_map_calls == 1);

    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound && saw_expected_error);
    assert(dma_map_calls == 2);
    assert(hash_calls == 0 && create_calls == 0);
    puts("PASS: non-memory DMA class is rejected before texture reads");
}

static void test_invalid_dma_object_fails_without_assert(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    d.pgraph.dma_a = configured_ramin_size - sizeof(uint32_t);
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 0);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: truncated DMA object is rejected before load or VRAM reads");
}

static void test_invalid_dma_base_fails_before_vram_reads(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_dma_b_frame = configured_vram_size;
    store_dma_object(d.pgraph.dma_b, configured_dma_b_frame,
                     configured_dma_b_limit);
    configured_palette_dma_b = true;
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 2);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: DMA base outside VRAM is rejected before texture or palette reads");
}

static void test_clean_or_missing_target_does_not_flush(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    d.pgraph.surface_color.draw_dirty = false;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 0);

    d = fixture_init(&renderer, &textures);
    renderer.render_targets.color.texture = NULL;
    assert(pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 0);
    puts("PASS: clean/missing surface does not trigger a resolve");
}

static void test_failed_flush_prevents_vram_reads(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    fail_flush = true;
    assert(!pgraph_d3d11_bind_textures(&d, NULL));
    assert(flush_calls == 1);
    assert(hash_calls == 0);
    assert(create_calls == 0);
    puts("PASS: failed surface flush aborts before VRAM hashing or texture creation");
}

static void test_bordered_cubemap_mip_chain(void)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_shape.cubemap = true;
    configured_shape.border = true;
    configured_shape.levels = 2;
    configured_texture_length = 6 * ((16 * 16 + 8 * 8 + 127) & ~127);
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    assert(bound);
    assert(dma_map_calls > 0);
    assert(hash_calls > 0);
    assert(create_calls == 1);
    d = fixture_init(&renderer, &textures);
    configured_shape.cubemap = true;
    configured_shape.border = true;
    configured_shape.levels = 2;
    configured_texture_length = sizeof(guest_vram);
    allow_expected_error = true;
    assert(!pgraph_d3d11_bind_textures(&d, NULL));
    allow_expected_error = false;
    assert(saw_expected_error && hash_calls == 0 && create_calls == 0);
    puts("PASS: bordered cubemap mip chain binds; truncated DMA rejected before hashing");
}

static void assert_invalid_shape_fails_before_reads(TextureShape shape)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_shape = shape;
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(dma_map_calls == 0);
    assert(texture_length_calls == 0);
    assert(hash_calls == 0);
    assert(create_calls == 0);
}

static void test_invalid_texture_shapes_fail_before_dma_or_length(void)
{
    TextureShape shape = {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
        .pitch = 16,
        .cubemap = true,
    };
    assert_invalid_shape_fails_before_reads(shape);

    shape = (TextureShape) {
        .dimensionality = 3,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
        .depth = 4,
        .cubemap = true,
    };
    assert_invalid_shape_fails_before_reads(shape);

    shape = (TextureShape) {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 2,
        .cubemap = true,
    };
    assert_invalid_shape_fails_before_reads(shape);

    shape = (TextureShape) {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
        .pitch = 4,
    };
    assert_invalid_shape_fails_before_reads(shape);
    puts("PASS: invalid cubemap/linear shapes and short pitch fail before DMA, size, or VRAM reads");
}

static void assert_invalid_registers_fail_before_shape_parse_with_filter(
    TextureShape shape, uint32_t texture_filter)
{
    PGRAPHD3D11State renderer;
    PGRAPHD3D11TextureState textures;
    NV2AState d = fixture_init(&renderer, &textures);
    configured_shape = shape;
    configured_texture_filter = texture_filter;
    allow_expected_error = true;
    bool bound = pgraph_d3d11_bind_textures(&d, NULL);
    allow_expected_error = false;
    assert(!bound);
    assert(saw_expected_error);
    assert(texture_shape_calls == 0);
    assert(dma_map_calls == 0);
    assert(texture_length_calls == 0);
    assert(hash_calls == 0);
    assert(create_calls == 0);
}

static void assert_invalid_registers_fail_before_shape_parse(TextureShape shape)
{
    assert_invalid_registers_fail_before_shape_parse_with_filter(shape, 0);
}

static void test_invalid_texture_registers_fail_before_shape_parse(void)
{
    TextureShape shape = {
        .dimensionality = 2,
        .color_format = 0x3d,
        .levels = 1,
        .width = 4,
        .height = 4,
    };
    assert_invalid_registers_fail_before_shape_parse(shape);

    shape.color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8;
    shape.levels = 0;
    assert_invalid_registers_fail_before_shape_parse(shape);

    shape.levels = 1;
    shape.dimensionality = 0;
    assert_invalid_registers_fail_before_shape_parse(shape);

    shape = (TextureShape) {
        .dimensionality = 1,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
        .pitch = 16,
    };
    assert_invalid_registers_fail_before_shape_parse(shape);
    puts("PASS: unsupported format, zero mip count/dimensionality, and invalid linear dimensionality fail before shape parsing");
}

static void test_combiner_stage_count_preflight(void)
{
    for (unsigned int stage_count = 0; stage_count < 256; stage_count++) {
        PGRAPHD3D11State renderer;
        PGRAPHD3D11TextureState textures;
        NV2AState d = fixture_init(&renderer, &textures);
        configured_combiner_control = stage_count;
        if (stage_count <= 8) {
            assert(pgraph_d3d11_bind_textures(&d, NULL));
            continue;
        }
        allow_expected_error = true;
        bool bound = pgraph_d3d11_bind_textures(&d, NULL);
        allow_expected_error = false;
        assert(!bound);
        assert(saw_expected_error);
        assert(texture_shape_calls == 0);
        assert(dma_map_calls == 0);
        assert(texture_length_calls == 0);
        assert(hash_calls == 0);
        assert(create_calls == 0);
    }
    puts("PASS: all 256 combiner counts; 0-8 bind, 9-255 fail before texture parsing or VRAM access");
}

static uint32_t test_convolution_filter(unsigned int kernel)
{
    return (NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0 <<
            ctz32(NV_PGRAPH_TEXFILTER0_MIN)) |
           (kernel << ctz32(NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL));
}

static void test_convolution_kernel_preflight(void)
{
    TextureShape shape = {
        .dimensionality = 2,
        .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
        .levels = 1,
        .width = 4,
        .height = 4,
    };
    for (unsigned int kernel = 0; kernel < 8; kernel++) {
        if (kernel == NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_QUINCUNX ||
            kernel == NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_GAUSSIAN_3) {
            PGRAPHD3D11State renderer;
            PGRAPHD3D11TextureState textures;
            NV2AState d = fixture_init(&renderer, &textures);
            configured_texture_filter = test_convolution_filter(kernel);
            assert(pgraph_d3d11_validate_texture_registers(&d.pgraph, NULL));
            assert(texture_shape_calls == 0);
        } else {
            assert_invalid_registers_fail_before_shape_parse_with_filter(
                shape, test_convolution_filter(kernel));
        }
    }
    puts("PASS: valid convolution kernels pass; invalid kernels fail before shared shape parsing");
}
static void test_convolution_requires_non_cubemap_2d_before_dma(void)
{
    const unsigned int kernels[] = {
        NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_QUINCUNX,
        NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_GAUSSIAN_3,
    };
    TextureShape incompatible_shapes[] = {
        {
            .dimensionality = 3,
            .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
            .levels = 1,
            .width = 4,
            .height = 4,
            .depth = 4,
        },
        {
            .cubemap = true,
            .dimensionality = 2,
            .color_format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8,
            .levels = 1,
            .width = 4,
            .height = 4,
        },
    };
    for (unsigned int kernel = 0; kernel < ARRAY_SIZE(kernels); kernel++) {
        uint32_t filter = test_convolution_filter(kernels[kernel]);
        for (unsigned int shape = 0;
             shape < ARRAY_SIZE(incompatible_shapes); shape++) {
            assert_invalid_registers_fail_before_shape_parse_with_filter(
                incompatible_shapes[shape], filter);
        }
    }
    puts("PASS: convolution on 3D/cubemap textures fails before shape parsing, DMA, or VRAM reads");
}

int main(void)
{
    test_palette_alias_flushes_before_hash_and_upload();
    test_texel_alias_flushes_before_hash();
    test_nonindexed_texture_ignores_invalid_palette_state();
    test_indexed_invalid_palette_fails_before_vram_reads();
    test_texture_beyond_dma_limit_fails_before_vram_reads();
    test_texture_ending_at_inclusive_dma_limit_is_allowed();
    test_palette_beyond_dma_limit_fails_before_vram_reads();
    test_dma_b_texture_address_uses_mapping_base();
    test_checked_mapping_caps_length_to_vram();
    test_checked_mapping_rejects_non_memory_class();
    test_invalid_dma_object_fails_without_assert();
    test_invalid_dma_base_fails_before_vram_reads();
    test_clean_or_missing_target_does_not_flush();
    test_failed_flush_prevents_vram_reads();
    test_bordered_cubemap_mip_chain();
    test_invalid_texture_shapes_fail_before_dma_or_length();
    test_invalid_texture_registers_fail_before_shape_parse();
    test_combiner_stage_count_preflight();
    test_convolution_kernel_preflight();
    test_convolution_requires_non_cubemap_2d_before_dma();
    return 0;
}
"""


def extract(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    source = (PGRAPH / "d3d11/textures.c").read_text(encoding="utf-8")
    dma_source = (ROOT / "hw/xbox/nv2a/nv2a.c").read_text(encoding="utf-8")
    shader_source = (PGRAPH / "d3d11/shaders.c").read_text(encoding="utf-8")
    header = (PGRAPH / "texture.h").read_text(encoding="utf-8")
    pixel_bind = shader_source[
        shader_source.index("bool pgraph_d3d11_bind_pixel_shader("):
    ]
    assert pixel_bind.index("pgraph_d3d11_validate_texture_registers(") < \
        pixel_bind.index("pgraph_glsl_set_psh_state(")
    assert pixel_bind.index("pgraph_d3d11_validate_texture_registers(") < \
        pixel_bind.index("d3d11_ps_set_snorm_textures(")
    shape = extract(header, "typedef struct TextureShape {", "} TextureShape;")
    shape += "} TextureShape;"
    shape += "\n" + extract(
        header, "typedef struct BasicColorFormatInfo {", "} BasicColorFormatInfo;"
    ) + "} BasicColorFormatInfo;"
    shared_dma_helpers = extract(
        dma_source, "DMAObject nv_dma_load(", "\nbool nv_dma_map_checked("
    ) + "\n" + extract(
        dma_source, "bool nv_dma_map_checked(", "\nvoid *nv_dma_map("
    )
    range_helper = extract(
        source, "static bool d3d11_vram_ranges_overlap(", "\nstatic ")
    overlap_helper = extract(
        source, "static bool d3d11_texture_data_overlaps_target(", "\nstatic ")
    cubemap_border_mips_helper = extract(
        source, "static bool d3d11_cubemap_border_mips_supported(", "\nstatic "
    )
    texture_shape_helpers = "\n".join((
        extract(source, "static bool d3d11_is_depth_texture_format(",
                "\nstatic unsigned int d3d11_texture_pixel_bytes("),
        extract(source, "static bool d3d11_texture_format_supported(",
                "\nbool pgraph_d3d11_validate_texture_registers("),
        extract(source, "bool pgraph_d3d11_validate_texture_registers(",
                "\nstatic float d3d11_half_to_float("),
        *(extract(source, start, "\nstatic ")
          for start in (
              "static bool d3d11_border_resource_dimensions(",
              "static unsigned int d3d11_effective_mip_levels(",
              "static unsigned int d3d11_effective_min_mipmap_level(",
              "static bool d3d11_cubemap_shape_valid(",
              "static bool d3d11_texture_shape_valid(",
          )),
    ))
    dma_helpers = "\n".join(
        extract(source, start, "\nstatic ")
        for start in (
            "static bool d3d11_dma_range(",
            "static bool d3d11_texture_dma_range(",
            "static bool d3d11_palette_dma_range(",
        )
    )
    binding = source[source.index("bool pgraph_d3d11_bind_textures("):]
    assert binding.index("pgraph_d3d11_validate_texture_registers(") < \
        binding.index("pgraph_get_texture_shape(")
    harness = (HARNESS.replace("__TEXTURE_SHAPE__", shape)
               .replace("__SHARED_DMA_HELPERS__", shared_dma_helpers)
               .replace("__RANGE_HELPERS__", range_helper + "\n" + overlap_helper +
                        "\n" + cubemap_border_mips_helper + "\n" +
                        texture_shape_helpers)
               .replace("__DMA_HELPERS__", dma_helpers)
               .replace("__BIND_FUNCTION__", binding))

    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-bind-alias-") as tmp:
        tmp = pathlib.Path(tmp)
        c_file = tmp / "test.c"
        executable = tmp / "test.exe"
        c_file.write_text(harness, encoding="utf-8")
        subprocess.run([
            "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            f"-I{ROOT}",
            str(c_file), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
