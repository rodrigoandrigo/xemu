#!/usr/bin/env python3
"""Run the production D3D11 image-blit path with real DMA/tile helpers.

Only D3D/runtime edges are mocked. The harness extracts and compiles the real
blit implementation, checked DMA mapper, and GPU-tile clipping function, then
checks clipped output, dirty-range notifications, inclusive DMA limits, and
failure atomicity, including rejection of a non-IN_MEMORY DMA class. It is a
source-level C regression, not a WARP or pixel-parity test.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BLITS = ROOT / "hw/xbox/nv2a/pgraph/d3d11/blits.c"
NV2A = ROOT / "hw/xbox/nv2a/nv2a.c"


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main():
    blit_source = BLITS.read_text(encoding="utf-8")
    nv2a_source = NV2A.read_text(encoding="utf-8")
    helpers = [
        extract_function(blit_source, signature)
        for signature in (
            "static unsigned int d3d11_blit_bytes_per_pixel(",
            "static bool d3d11_blit_row_pixels(",
            "static bool d3d11_blit_offset(",
            "static bool d3d11_blit_row_interval(",
            "static bool d3d11_blit_regions_overlap(",
            "static bool d3d11_blit_copy_clipped(",
            "static bool d3d11_blit_copy(",
            "static bool d3d11_blit_blend_and_clipped(",
            "static bool d3d11_blit_blend_and(",
            "static void d3d11_blit_patch_alpha(",
            "static bool d3d11_blit_rows(",
            "static bool d3d11_blit_range_valid(",
        )
    ]
    dma_functions = [
        extract_function(nv2a_source, "DMAObject nv_dma_load("),
        extract_function(nv2a_source, "bool nv_dma_load_checked("),
        extract_function(nv2a_source, "bool nv_dma_map_checked("),
        extract_function(nv2a_source, "hwaddr nv_clip_gpu_tile_blit("),
    ]
    image_blit = extract_function(
        blit_source, "void pgraph_d3d11_image_blit(")

    fixture = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ctz32(value) __builtin_ctz(value)
#include "hw/xbox/nv2a/nv2a_regs.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ldl_le_p(pointer) test_ldl_le_p((const uint8_t *)(pointer))
static unsigned int allocation_calls;
static bool fail_next_allocation;
static void *test_g_try_malloc(size_t size)
{
    allocation_calls++;
    if (fail_next_allocation) {
        fail_next_allocation = false;
        return NULL;
    }
    return malloc(size);
}
#define g_try_malloc test_g_try_malloc
#define g_free free
#define QEMU_HOST_LOG_ERROR 1
#define DIRTY_MEMORY_VGA 0
#define DIRTY_MEMORY_NV2A_TEX 4
typedef uint64_t hwaddr;
typedef struct Error { const char *message; } Error;
typedef struct MemoryRegion { uint64_t size; } MemoryRegion;
typedef struct DMAObject {
    unsigned int dma_class;
    unsigned int dma_target;
    hwaddr address;
    hwaddr limit;
} DMAObject;
typedef struct ContextSurfaces2DState {
    hwaddr object_instance;
    hwaddr dma_image_source;
    hwaddr dma_image_dest;
    unsigned int color_format;
    unsigned int source_pitch;
    unsigned int dest_pitch;
    hwaddr source_offset;
    hwaddr dest_offset;
} ContextSurfaces2DState;
typedef struct ImageBlitState {
    hwaddr context_surfaces;
    unsigned int operation;
    unsigned int in_x;
    unsigned int in_y;
    unsigned int out_x;
    unsigned int out_y;
    unsigned int width;
    unsigned int height;
} ImageBlitState;
typedef struct BetaState { uint32_t beta; } BetaState;
typedef struct PGRAPHD3D11State {
    bool resources_ready;
    bool device_lost;
    bool app_suspended;
    bool memory_suspended;
} PGRAPHD3D11State;
typedef struct PGRAPHState {
    PGRAPHD3D11State *d3d11_renderer_state;
    ContextSurfaces2DState context_surfaces_2d;
    ImageBlitState image_blit;
    BetaState beta;
    uint64_t draw_time;
} PGRAPHState;
typedef struct NV2AState {
    PGRAPHState pgraph;
    uint8_t *vram_ptr;
    MemoryRegion *vram;
    MemoryRegion ramin;
    uint8_t *ramin_ptr;
    struct { uint32_t regs[0x1000]; } pfb;
} NV2AState;
typedef struct DirtyRecord {
    hwaddr address;
    hwaddr length;
    unsigned int client;
} DirtyRecord;
static uint8_t guest_vram[0x8000];
static uint8_t guest_ramin[0x80];
static MemoryRegion guest_vram_region = { sizeof(guest_vram) };
static unsigned int flush_calls;
static unsigned int dma_map_traces;
static unsigned int dirty_calls;
static unsigned int invalidate_calls;
static unsigned int log_calls;
static DirtyRecord dirty_records[4];
static DirtyRecord invalidation_record;
static char last_log[256];
static uint32_t test_ldl_le_p(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void test_stl_le_p(uint8_t *bytes, uint32_t value)
{
    bytes[0] = value;
    bytes[1] = value >> 8;
    bytes[2] = value >> 16;
    bytes[3] = value >> 24;
}
static uint64_t memory_region_size(const MemoryRegion *region)
{
    return region->size;
}
static void trace_nv2a_dma_map(hwaddr object, unsigned int dma_class,
                               unsigned int dma_target, hwaddr address,
                               hwaddr limit)
{
    (void)object;
    (void)dma_class;
    (void)dma_target;
    (void)address;
    (void)limit;
    dma_map_traces++;
}
static void qemu_host_emit_log(int level, const char *format, ...)
{
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    log_calls++;
}
static const char *error_get_pretty(Error *error)
{
    return error ? error->message : "mock error";
}
static void error_free(Error *error)
{
    free(error);
}
static bool pgraph_d3d11_surface_flush(NV2AState *d, Error **errp)
{
    (void)d;
    (void)errp;
    flush_calls++;
    return true;
}
static void memory_region_set_client_dirty(MemoryRegion *region,
                                           hwaddr address, hwaddr length,
                                           unsigned int client)
{
    (void)region;
    assert(dirty_calls < sizeof(dirty_records) / sizeof(dirty_records[0]));
    dirty_records[dirty_calls++] = (DirtyRecord) { address, length, client };
}
static void pgraph_d3d11_surface_invalidate_range(NV2AState *d,
                                                   hwaddr address,
                                                   hwaddr length)
{
    (void)d;
    invalidate_calls++;
    invalidation_record = (DirtyRecord) { address, length, 0 };
}
static void store_dma_object_with_class(hwaddr object, unsigned int dma_class,
                                        uint32_t base, uint32_t limit)
{
    test_stl_le_p(guest_ramin + object, dma_class);
    test_stl_le_p(guest_ramin + object + 4, limit);
    test_stl_le_p(guest_ramin + object + 8, base);
}
static void store_dma_object(hwaddr object, uint32_t base, uint32_t limit)
{
    store_dma_object_with_class(object, NV_DMA_IN_MEMORY_CLASS, base, limit);
}
static void fixture_init(NV2AState *d, PGRAPHD3D11State *renderer)
{
    memset(guest_vram, 0xcc, sizeof(guest_vram));
    memset(guest_ramin, 0, sizeof(guest_ramin));
    memset(d, 0, sizeof(*d));
    memset(renderer, 0, sizeof(*renderer));
    flush_calls = dma_map_traces = dirty_calls = invalidate_calls = log_calls = 0;
    allocation_calls = 0;
    fail_next_allocation = false;
    memset(dirty_records, 0, sizeof(dirty_records));
    memset(&invalidation_record, 0, sizeof(invalidation_record));
    last_log[0] = '\0';
    d->vram_ptr = guest_vram;
    d->vram = &guest_vram_region;
    d->ramin = (MemoryRegion) { sizeof(guest_ramin) };
    d->ramin_ptr = guest_ramin;
    d->pgraph.d3d11_renderer_state = renderer;
    renderer->resources_ready = true;
    d->pgraph.context_surfaces_2d = (ContextSurfaces2DState) {
        .object_instance = 0x55,
        .dma_image_source = 0,
        .dma_image_dest = 0x10,
        .color_format = NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,
        .source_pitch = 16,
        .dest_pitch = 16,
    };
    d->pgraph.image_blit = (ImageBlitState) {
        .context_surfaces = 0x55,
        .operation = NV09F_SET_OPERATION_SRCCOPY,
        .in_x = 1,
        .out_x = 1,
        .width = 3,
        .height = 2,
    };
    store_dma_object(0, 0x1000, 31);
    store_dma_object(0x10, 0x2000, 31);
    for (unsigned int i = 0; i < 64; i++) {
        guest_vram[0x1000 + i] = (uint8_t)(i + 1);
    }
}
static void set_partial_tile_limit(NV2AState *d)
{
    const hwaddr destination = 0x2000 + 4;
    d->pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(0)] =
        NV_PFB_TILE_FLAGS_VALID;
    d->pfb.regs[NV_PFB_TILE_LIMIT(0)] = destination + 21;
}
"""
    fixture += "\n\n".join(helpers)
    fixture += "\n\n".join(dma_functions)
    fixture += "\n\n" + image_blit
    fixture += r"""
static uint8_t expected_blend(uint8_t source, uint8_t destination,
                              uint32_t source_beta, uint32_t destination_beta)
{
    return (source * source_beta + destination * destination_beta) / 0x7f80;
}
static void test_clipped_copy_at_inclusive_dma_end(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    set_partial_tile_limit(&d);
    uint8_t before[32];
    memcpy(before, guest_vram + 0x2000, sizeof(before));
    pgraph_d3d11_image_blit(&d);
    assert(flush_calls == 1 && dma_map_traces == 2);
    assert(log_calls == 0 && dirty_calls == 2 && invalidate_calls == 1);
    assert(d.pgraph.draw_time == 1);
    for (unsigned int i = 0; i < 12; i++) {
        assert(guest_vram[0x2004 + i] == guest_vram[0x1004 + i]);
    }
    for (unsigned int i = 0; i < 6; i++) {
        assert(guest_vram[0x2014 + i] == guest_vram[0x1014 + i]);
    }
    for (unsigned int i = 12; i < 16; i++) {
        assert(guest_vram[0x2004 + i] == before[4 + i]);
    }
    for (unsigned int i = 6; i < sizeof(before) - 20; i++) {
        assert(guest_vram[0x2014 + i] == before[20 + i]);
    }
    assert(dirty_records[0].address == 0x2004 &&
           dirty_records[0].length == 22 &&
           dirty_records[0].client == DIRTY_MEMORY_VGA);
    assert(dirty_records[1].address == 0x2004 &&
           dirty_records[1].length == 22 &&
           dirty_records[1].client == DIRTY_MEMORY_NV2A_TEX);
    assert(invalidation_record.address == 0x2004 &&
           invalidation_record.length == 22);
}
static void test_blend_partial_tile_tail(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    set_partial_tile_limit(&d);
    d.pgraph.image_blit.operation = NV09F_SET_OPERATION_BLEND_AND;
    d.pgraph.beta.beta = 0x3f80U << 16;
    for (unsigned int i = 0; i < 64; i++) {
        guest_vram[0x1000 + i] = (uint8_t)(i + 5);
        guest_vram[0x2000 + i] = (uint8_t)(i + 100);
    }
    uint8_t before[32];
    memcpy(before, guest_vram + 0x2000, sizeof(before));
    pgraph_d3d11_image_blit(&d);
    const uint32_t source_beta = 0x3f80;
    const uint32_t destination_beta = 0x7f80 - source_beta;
    for (unsigned int row = 0; row < 2; row++) {
        unsigned int pixels = row == 0 ? 3 : 1;
        for (unsigned int pixel = 0; pixel < pixels; pixel++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                unsigned int byte = 4 + row * 16 + pixel * 4 + channel;
                assert(guest_vram[0x2000 + byte] == expected_blend(
                    guest_vram[0x1000 + byte], before[byte],
                    source_beta, destination_beta));
            }
            unsigned int alpha = 4 + row * 16 + pixel * 4 + 3;
            assert(guest_vram[0x2000 + alpha] == before[alpha]);
        }
    }
    assert(guest_vram[0x2018] == before[24] &&
           guest_vram[0x2019] == before[25]);
    assert(log_calls == 0 && dirty_calls == 2 && invalidate_calls == 1);
    assert(dirty_records[0].length == 22 && d.pgraph.draw_time == 1);
}
static void test_overlapping_dma_copy_snapshots_original_source(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object(0, 0x1000, 63);
    store_dma_object(0x10, 0x1000, 63);
    d.pgraph.image_blit.out_x = 2;

    uint8_t before[64];
    uint8_t expected[64];
    memcpy(before, guest_vram + 0x1000, sizeof(before));
    memcpy(expected, before, sizeof(expected));
    for (unsigned int row = 0; row < 2; row++) {
        memcpy(expected + 8 + row * 16, before + 4 + row * 16, 8);
    }

    pgraph_d3d11_image_blit(&d);
    assert(flush_calls == 1 && dma_map_traces == 2);
    assert(log_calls == 0 && dirty_calls == 2 && invalidate_calls == 1);
    assert(allocation_calls == 1);
    assert(d.pgraph.draw_time == 1);
    assert(memcmp(guest_vram + 0x1000, expected, sizeof(expected)) == 0);
    assert(dirty_records[0].address == 0x1008 &&
           dirty_records[0].length == 24 &&
           dirty_records[0].client == DIRTY_MEMORY_VGA);
    assert(dirty_records[1].address == 0x1008 &&
           dirty_records[1].length == 24 &&
           dirty_records[1].client == DIRTY_MEMORY_NV2A_TEX);
    assert(invalidation_record.address == 0x1008 &&
           invalidation_record.length == 24);
}
static void test_overlapping_dma_snapshot_allocation_failure_is_atomic(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object(0, 0x1000, 63);
    store_dma_object(0x10, 0x1000, 63);
    d.pgraph.image_blit.out_x = 2;

    uint8_t before[64];
    memcpy(before, guest_vram + 0x1000, sizeof(before));
    fail_next_allocation = true;
    pgraph_d3d11_image_blit(&d);

    assert(flush_calls == 1 && dma_map_traces == 2);
    assert(allocation_calls == 1 && !fail_next_allocation);
    assert(log_calls == 1 && dirty_calls == 0 && invalidate_calls == 0);
    assert(d.pgraph.draw_time == 0);
    assert(memcmp(guest_vram + 0x1000, before, sizeof(before)) == 0);
}
static void test_blend_vertical_overlap_keeps_clipped_source_snapshot(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object(0, 0x1000, 95);
    store_dma_object(0x10, 0x1000, 95);
    d.pgraph.image_blit.operation = NV09F_SET_OPERATION_BLEND_AND;
    d.pgraph.image_blit.out_y = 1;
    d.pgraph.image_blit.height = 3;
    d.pgraph.beta.beta = 0x3f80U << 16;
    for (unsigned int i = 0; i < 96; i++) {
        guest_vram[0x1000 + i] = (uint8_t)(i + 7);
    }

    uint8_t before[96];
    uint8_t expected[96];
    memcpy(before, guest_vram + 0x1000, sizeof(before));
    memcpy(expected, before, sizeof(expected));
    const uint32_t source_beta = 0x3f80;
    const uint32_t destination_beta = 0x7f80 - source_beta;
    for (unsigned int row = 0; row < 3; row++) {
        unsigned int source_row = 4 + row * 16;
        unsigned int destination_row = 20 + row * 16;
        for (unsigned int pixel = 0; pixel < 3; pixel++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                unsigned int source_byte = source_row + pixel * 4 + channel;
                unsigned int destination_byte =
                    destination_row + pixel * 4 + channel;
                expected[destination_byte] = expected_blend(
                    before[source_byte], before[destination_byte],
                    source_beta, destination_beta);
            }
        }
    }

    pgraph_d3d11_image_blit(&d);
    assert(flush_calls == 1 && dma_map_traces == 2);
    assert(log_calls == 0 && dirty_calls == 2 && invalidate_calls == 1);
    assert(allocation_calls == 1 && d.pgraph.draw_time == 1);
    assert(memcmp(guest_vram + 0x1000, expected, sizeof(expected)) == 0);
    assert(dirty_records[0].address == 0x1014 &&
           dirty_records[0].length == 44 &&
           dirty_records[0].client == DIRTY_MEMORY_VGA);
    assert(dirty_records[1].address == 0x1014 &&
           dirty_records[1].length == 44 &&
           dirty_records[1].client == DIRTY_MEMORY_NV2A_TEX);
    assert(invalidation_record.address == 0x1014 &&
           invalidation_record.length == 44);
}
static uint32_t blit_fuzz_state = 0x6d2b79f5;
static uint32_t blit_fuzz_random(void)
{
    blit_fuzz_state ^= blit_fuzz_state << 13;
    blit_fuzz_state ^= blit_fuzz_state >> 17;
    blit_fuzz_state ^= blit_fuzz_state << 5;
    return blit_fuzz_state;
}
static void test_varied_integrated_blits_match_snapshot_model(void)
{
    const uint32_t beta_values[] = { 0, 0x1f80, 0x3f00, 0x3f80, 0x7f80 };
    const uint32_t destination_bases[] = {
        0x0000, 0x1000, 0x2000, 0x3000,
    };
    const unsigned int formats[] = {
        NV062_SET_COLOR_FORMAT_LE_Y8,
        NV062_SET_COLOR_FORMAT_LE_R5G6B5,
        NV062_SET_COLOR_FORMAT_LE_A8R8G8B8,
        NV062_SET_COLOR_FORMAT_LE_X8R8G8B8,
        NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8,
        NV062_SET_COLOR_FORMAT_LE_Y32,
    };
    const unsigned int format_count = sizeof(formats) / sizeof(formats[0]);
    uint32_t seen_formats = 0;
    uint32_t seen_srccopy_formats = 0;
    uint32_t seen_blend_formats = 0;
    uint32_t seen_alpha_patch_formats = 0;
    unsigned int clipped_cases = 0;
    for (unsigned int test = 0; test < 512; test++) {
        NV2AState d;
        PGRAPHD3D11State renderer;
        fixture_init(&d, &renderer);
        for (size_t i = 0; i < sizeof(guest_vram); i++) {
            guest_vram[i] = (uint8_t)blit_fuzz_random();
        }

        unsigned int color_format = formats[test % format_count];
        unsigned int bpp = d3d11_blit_bytes_per_pixel(color_format);
        unsigned int source_pitch = bpp + blit_fuzz_random() % 25;
        unsigned int destination_pitch = bpp + blit_fuzz_random() % 25;
        unsigned int in_x = blit_fuzz_random() % (source_pitch / bpp);
        unsigned int out_x = blit_fuzz_random() % (destination_pitch / bpp);
        unsigned int in_y = blit_fuzz_random() % 3;
        unsigned int out_y = blit_fuzz_random() % 3;
        unsigned int max_width = MIN(source_pitch / bpp - in_x,
                                     destination_pitch / bpp - out_x);
        unsigned int width = 1 + blit_fuzz_random() % max_width;
        unsigned int height = 1 + blit_fuzz_random() % 4;
        uint32_t source_base = 0x1000;
        uint32_t destination_base =
            destination_bases[blit_fuzz_random() %
                              (sizeof(destination_bases) /
                               sizeof(destination_bases[0]))];
        uint32_t source_dma_offset = blit_fuzz_random() % 24;
        uint32_t destination_dma_offset = blit_fuzz_random() % 24;
        unsigned int operation = bpp == 4 &&
                                 ((test / format_count) & 1U) ?
            NV09F_SET_OPERATION_BLEND_AND : NV09F_SET_OPERATION_SRCCOPY;
        bool patch_alpha = color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ||
                           color_format ==
                               NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8;
        uint32_t beta = beta_values[blit_fuzz_random() %
                                    (sizeof(beta_values) /
                                     sizeof(beta_values[0]))];
        seen_formats |= 1U << color_format;
        if (operation == NV09F_SET_OPERATION_SRCCOPY) {
            seen_srccopy_formats |= 1U << color_format;
        } else {
            seen_blend_formats |= 1U << color_format;
        }
        if (patch_alpha) {
            seen_alpha_patch_formats |= 1U << color_format;
        }

        store_dma_object(0, source_base, 255);
        store_dma_object(0x10, destination_base, 255);
        d.pgraph.context_surfaces_2d.source_pitch = source_pitch;
        d.pgraph.context_surfaces_2d.dest_pitch = destination_pitch;
        d.pgraph.context_surfaces_2d.source_offset = source_dma_offset;
        d.pgraph.context_surfaces_2d.dest_offset = destination_dma_offset;
        d.pgraph.context_surfaces_2d.color_format = color_format;
        d.pgraph.image_blit = (ImageBlitState) {
            .context_surfaces = 0x55,
            .operation = operation,
            .in_x = in_x,
            .in_y = in_y,
            .out_x = out_x,
            .out_y = out_y,
            .width = width,
            .height = height,
        };
        d.pgraph.beta.beta = beta << 16;

        size_t row_bytes = (size_t)width * bpp;
        size_t requested_size = (size_t)(height - 1) * destination_pitch +
                                row_bytes;
        uint8_t *source_start = guest_vram + source_base + source_dma_offset +
                                in_y * source_pitch + in_x * bpp;
        uint8_t *destination_start =
            guest_vram + destination_base + destination_dma_offset +
            out_y * destination_pitch + out_x * bpp;
        size_t clipped_size = requested_size;
        if (requested_size > 3 && (blit_fuzz_random() & 1)) {
            clipped_size = 2 + blit_fuzz_random() % (requested_size - 2);
            clipped_cases++;
            d.pfb.regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(0)] =
                NV_PFB_TILE_FLAGS_VALID;
            d.pfb.regs[NV_PFB_TILE_LIMIT(0)] =
                (uint32_t)(destination_start - guest_vram + clipped_size - 1);
        }

        uint8_t before[sizeof(guest_vram)];
        uint8_t expected[sizeof(guest_vram)];
        memcpy(before, guest_vram, sizeof(before));
        memcpy(expected, before, sizeof(expected));
        size_t full_rows = MIN((size_t)height, clipped_size / destination_pitch);
        size_t remainder = clipped_size - full_rows * destination_pitch;
        remainder = MIN(remainder, row_bytes);
        size_t trailing_pixels = remainder / bpp;
        uint32_t source_beta = MIN(beta, 0x7f80);
        uint32_t destination_beta = 0x7f80 - source_beta;
        size_t source_offset = source_start - guest_vram;
        size_t destination_offset = destination_start - guest_vram;
        for (size_t row = 0; row < full_rows; row++) {
            if (operation == NV09F_SET_OPERATION_SRCCOPY) {
                memcpy(expected + destination_offset + row * destination_pitch,
                       before + source_offset + row * source_pitch, row_bytes);
            } else {
                for (size_t pixel = 0; pixel < width; pixel++) {
                    for (unsigned int channel = 0; channel < 3; channel++) {
                        size_t src = source_offset + row * source_pitch +
                                     pixel * 4 + channel;
                        size_t dst = destination_offset + row * destination_pitch +
                                     pixel * 4 + channel;
                        expected[dst] = expected_blend(
                            before[src], before[dst], source_beta,
                            destination_beta);
                    }
                }
            }
        }
        if (operation == NV09F_SET_OPERATION_SRCCOPY && remainder) {
            memcpy(expected + destination_offset + full_rows * destination_pitch,
                   before + source_offset + full_rows * source_pitch, remainder);
        } else if (operation == NV09F_SET_OPERATION_BLEND_AND) {
            for (size_t pixel = 0; pixel < trailing_pixels; pixel++) {
                for (unsigned int channel = 0; channel < 3; channel++) {
                    size_t src = source_offset + full_rows * source_pitch +
                                 pixel * 4 + channel;
                    size_t dst = destination_offset +
                                 full_rows * destination_pitch + pixel * 4 +
                                 channel;
                    expected[dst] = expected_blend(
                        before[src], before[dst], source_beta,
                        destination_beta);
                }
            }
        }
        if (patch_alpha) {
            uint8_t alpha =
                color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ? 0xff : 0;
            for (size_t row = 0; row < full_rows; row++) {
                for (size_t pixel = 0; pixel < width; pixel++) {
                    expected[destination_offset + row * destination_pitch +
                             pixel * 4 + 3] = alpha;
                }
            }
            for (size_t pixel = 0; pixel < trailing_pixels; pixel++) {
                expected[destination_offset + full_rows * destination_pitch +
                         pixel * 4 + 3] = alpha;
            }
        }

        pgraph_d3d11_image_blit(&d);
        assert(log_calls == 0 && dirty_calls == 2 && invalidate_calls == 1);
        assert(d.pgraph.draw_time == 1);
        assert(dirty_records[0].address ==
                   (hwaddr)(destination_start - guest_vram) &&
               dirty_records[0].length == clipped_size);
        assert(invalidation_record.address ==
                   (hwaddr)(destination_start - guest_vram) &&
               invalidation_record.length == clipped_size);
        assert(memcmp(guest_vram, expected, sizeof(expected)) == 0);
    }
    const uint32_t expected_formats =
        (1U << NV062_SET_COLOR_FORMAT_LE_Y8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_R5G6B5) |
        (1U << NV062_SET_COLOR_FORMAT_LE_A8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_Y32);
    const uint32_t expected_blend_formats =
        (1U << NV062_SET_COLOR_FORMAT_LE_A8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_Y32);
    const uint32_t expected_alpha_patch_formats =
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8) |
        (1U << NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8);
    assert(seen_formats == expected_formats);
    assert(seen_srccopy_formats == expected_formats);
    assert(seen_blend_formats == expected_blend_formats);
    assert(seen_alpha_patch_formats == expected_alpha_patch_formats);
    assert(clipped_cases > 0);
}
static void assert_rejected_without_write(NV2AState *d)
{
    uint8_t before[sizeof(guest_vram)];
    memcpy(before, guest_vram, sizeof(before));
    pgraph_d3d11_image_blit(d);
    assert(log_calls == 1 && dirty_calls == 0 && invalidate_calls == 0);
    assert(d->pgraph.draw_time == 0);
    assert(memcmp(guest_vram, before, sizeof(before)) == 0);
}
static void test_non_memory_dma_class_fails_without_write(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object_with_class(0, NV_DMA_TO_MEMORY_CLASS, 0x1000, 31);
    assert_rejected_without_write(&d);
    assert(flush_calls == 1 && dma_map_traces == 1);
    assert(strcmp(last_log, "D3D11: image blit DMA mapping is invalid") == 0);
}
static void test_dma_range_failure_precedes_destination_writes(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object(0x10, 0x2000, 30);
    assert_rejected_without_write(&d);
    assert(flush_calls == 1 && dma_map_traces == 2);
}
static void test_truncated_dma_descriptor_fails_without_assert(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    d.pgraph.context_surfaces_2d.dma_image_dest = sizeof(guest_ramin) - 4;
    assert_rejected_without_write(&d);
    assert(flush_calls == 1 && dma_map_traces == 1);
}
static void test_dma_base_outside_vram_fails_without_write(void)
{
    NV2AState d;
    PGRAPHD3D11State renderer;
    fixture_init(&d, &renderer);
    store_dma_object(0x10, sizeof(guest_vram), 31);
    assert_rejected_without_write(&d);
    assert(flush_calls == 1 && dma_map_traces == 2);
}
int main(void)
{
    test_clipped_copy_at_inclusive_dma_end();
    test_blend_partial_tile_tail();
    test_overlapping_dma_copy_snapshots_original_source();
    test_overlapping_dma_snapshot_allocation_failure_is_atomic();
    test_blend_vertical_overlap_keeps_clipped_source_snapshot();
    test_varied_integrated_blits_match_snapshot_model();
    test_dma_range_failure_precedes_destination_writes();
    test_truncated_dma_descriptor_fails_without_assert();
    test_dma_base_outside_vram_fails_without_write();
    test_non_memory_dma_class_fails_without_write();
    puts("PASS: integrated D3D11 image blit clips a partial tail at an inclusive DMA boundary");
    puts("PASS: integrated BLEND_AND processes only complete clipped pixels");
    puts("PASS: integrated overlapping DMA copy snapshots the original source");
    puts("PASS: integrated snapshot allocation failure leaves overlapping VRAM unchanged");
    puts("PASS: clipped BLEND_AND snapshots source rows before any overlapping writes");
    puts("PASS: 512 varied multi-format integrated blits match the snapshot reference model");
    puts("PASS: invalid DMA ranges/descriptors/bases do not write or dirty VRAM");
    puts("PASS: non-memory DMA class is rejected before any VRAM mutation");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-blit-integrated-") as directory:
        c_file = pathlib.Path(directory) / "blit-integrated.c"
        executable = pathlib.Path(directory) / "blit-integrated"
        c_file.write_text(fixture, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), str(c_file), "-o", str(executable)],
                       check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
