#!/usr/bin/env python3
"""Exercise production D3D11 image-blit helpers without a D3D runtime."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "hw/xbox/nv2a/pgraph/d3d11/blits.c"
REGS = ROOT / "hw/xbox/nv2a/nv2a_regs.h"


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
    source = SOURCE.read_text(encoding="utf-8")
    registers = REGS.read_text(encoding="utf-8")
    constants = {}
    for name in ("NV09F_SET_OPERATION_BLEND_AND",
                 "NV09F_SET_OPERATION_SRCCOPY"):
        match = re.search(
            rf"^\s*#\s*define\s+{name}\s+([0-9]+)",
            registers, re.MULTILINE)
        assert match, f"missing image-blit operation: {name}"
        constants[name] = int(match.group(1))

    helpers = [
        extract_function(source, "static bool d3d11_blit_row_pixels("),
        extract_function(source, "static bool d3d11_blit_offset("),
        extract_function(source, "static bool d3d11_blit_row_interval("),
        extract_function(source, "static bool d3d11_blit_regions_overlap("),
        extract_function(source, "static bool d3d11_blit_copy_clipped("),
        extract_function(source, "static bool d3d11_blit_copy("),
        extract_function(source, "static bool d3d11_blit_blend_and_clipped("),
        extract_function(source, "static bool d3d11_blit_blend_and("),
        extract_function(source, "static void d3d11_blit_patch_alpha("),
        extract_function(source, "static bool d3d11_blit_rows("),
        extract_function(source, "static bool d3d11_blit_range_valid("),
    ]
    blit = extract_function(source, "void pgraph_d3d11_image_blit(")
    flush_call = "pgraph_d3d11_surface_flush(d, &error)"
    assert flush_call in blit
    assert blit.index(flush_call) < blit.index(
        "d3d11_blit_blend_and_clipped(")
    row_bounds_call = "d3d11_blit_row_pixels(blit->width, blit->in_x"
    assert row_bounds_call in blit
    assert blit.index(row_bounds_call) < blit.index("hwaddr source_offset")
    assert blit.count("d3d11_blit_offset(surfaces->") == 2
    assert blit.index("d3d11_blit_offset(surfaces->source_offset") < blit.index(
        "d3d11_blit_range_valid(")
    assert blit.index("d3d11_blit_offset(surfaces->dest_offset") < blit.index(
        "d3d11_blit_range_valid(")
    assert blit.index("d3d11_blit_blend_and_clipped(") < blit.index(
        "memory_region_set_client_dirty")
    assert blit.index("d3d11_blit_copy_clipped(") < blit.index(
        "memory_region_set_client_dirty")
    assert blit.index("!blit->width || !blit->height") < blit.index(
        "d3d11_blit_range_valid(")

    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t hwaddr;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static bool fail_next_allocation;
static size_t allocation_calls;
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
#define NV09F_SET_OPERATION_BLEND_AND __BLEND_AND__
#define NV09F_SET_OPERATION_SRCCOPY __SRCCOPY__
typedef struct ImageBlitState {
    unsigned int operation;
} ImageBlitState;
typedef struct BetaState {
    uint32_t beta;
} BetaState;
typedef struct PGRAPHState {
    ImageBlitState image_blit;
    BetaState beta;
} PGRAPHState;
""".replace("__BLEND_AND__", str(constants["NV09F_SET_OPERATION_BLEND_AND"])) \
       .replace("__SRCCOPY__", str(constants["NV09F_SET_OPERATION_SRCCOPY"]))
    fixture += "\n\n".join(helpers)
    fixture += r"""
static uint8_t expected_blend(uint8_t source, uint8_t destination,
                              uint32_t source_beta, uint32_t destination_beta)
{
    const uint32_t max_beta = 0x7f80;
    return (source * source_beta + destination * destination_beta) / max_beta;
}
int main(void)
{
    PGRAPHState pg = { .image_blit = { .operation = NV09F_SET_OPERATION_SRCCOPY } };
    size_t calls_before;
    size_t row_pixels;
    assert(d3d11_blit_row_pixels(4, 2, 12, 1, 12, 4, &row_pixels));
    assert(row_pixels == 1);
    assert(d3d11_blit_row_pixels(4, 1, 12, 2, 16, 4, &row_pixels));
    assert(row_pixels == 2);
    assert(!d3d11_blit_row_pixels(1, 3, 12, 0, 16, 4, &row_pixels));
    assert(!d3d11_blit_row_pixels(1, 0, 12, 4, 16, 4, &row_pixels));
    assert(!d3d11_blit_row_pixels(1, 2, 11, 0, 16, 4, &row_pixels));

    bool ranges_overlap;
    uint8_t range_probe[32];
    assert(d3d11_blit_regions_overlap(range_probe, range_probe + 3, 8, 8, 4,
                                      2, 0, &ranges_overlap));
    assert(ranges_overlap);
    assert(d3d11_blit_regions_overlap(range_probe, range_probe + 4, 8, 8, 4,
                                      2, 0, &ranges_overlap));
    assert(!ranges_overlap);
    assert(d3d11_blit_regions_overlap(range_probe, range_probe + 4, 8, 8, 4,
                                      1, 2, &ranges_overlap));
    assert(!ranges_overlap);
    assert(d3d11_blit_regions_overlap(range_probe, range_probe + 7, 8, 8, 4,
                                      1, 2, &ranges_overlap));
    assert(ranges_overlap);
    assert(!d3d11_blit_regions_overlap(range_probe, range_probe + 16,
                                       SIZE_MAX, 8, 4, 3, 0,
                                       &ranges_overlap));
    for (size_t source_base = 0; source_base < 4; source_base++) {
        for (size_t destination_base = 0; destination_base < 4;
             destination_base++) {
            for (size_t source_pitch = 0; source_pitch <= 4; source_pitch++) {
                for (size_t destination_pitch = 0;
                     destination_pitch <= 4; destination_pitch++) {
                    for (size_t row_bytes = 0; row_bytes <= 3; row_bytes++) {
                        for (size_t height = 0; height <= 3; height++) {
                            for (size_t trailing_bytes = 0;
                                 trailing_bytes <= row_bytes;
                                 trailing_bytes++) {
                                bool expected_overlap = false;
                                size_t source_intervals = height +
                                    (trailing_bytes != 0);
                                size_t destination_intervals = source_intervals;
                                for (size_t source_row = 0;
                                     source_row < source_intervals;
                                     source_row++) {
                                    size_t source_start = source_base +
                                        (source_row < height ?
                                            source_row * source_pitch :
                                            height * source_pitch);
                                    size_t source_length = source_row < height ?
                                        row_bytes : trailing_bytes;
                                    for (size_t destination_row = 0;
                                         destination_row <
                                             destination_intervals;
                                         destination_row++) {
                                        size_t destination_start =
                                            destination_base +
                                            (destination_row < height ?
                                                destination_row *
                                                    destination_pitch :
                                                height * destination_pitch);
                                        size_t destination_length =
                                            destination_row < height ?
                                                row_bytes : trailing_bytes;
                                        if (source_start < destination_start +
                                                destination_length &&
                                            destination_start < source_start +
                                                source_length) {
                                            expected_overlap = true;
                                        }
                                    }
                                }
                                assert(d3d11_blit_regions_overlap(
                                    range_probe + source_base,
                                    range_probe + destination_base,
                                    source_pitch, destination_pitch, row_bytes,
                                    height, trailing_bytes, &ranges_overlap));
                                assert(ranges_overlap == expected_overlap);
                            }
                        }
                    }
                }
            }
        }
    }

    hwaddr offset = 0;
    assert(d3d11_blit_offset(5, 2, 3, 16, 4, &offset));
    assert(offset == 61);
    assert(d3d11_blit_offset(UINT64_MAX - 8, 2, 0, 16, 4, &offset));
    assert(offset == UINT64_MAX);
    assert(!d3d11_blit_offset(UINT64_MAX - 7, 2, 0, 16, 4, &offset));
    assert(offset == UINT64_MAX);
    assert(!d3d11_blit_offset(UINT64_MAX - 3, 0, 1, 4, 1, &offset));
    assert(offset == UINT64_MAX);
    assert(!d3d11_blit_offset(0, 0, 0, 0, 0, &offset));

    uint8_t source[16] = {
        1, 2, 3, 4, 5, 6, 0xee, 0xee,
        7, 8, 9, 10, 11, 12, 0xee, 0xee,
    };
    uint8_t destination[20];
    memset(destination, 0xcc, sizeof(destination));
    assert(d3d11_blit_rows(&pg, destination, 10, source, 8, 3, 2, 2));
    assert(memcmp(destination, source, 6) == 0);
    assert(memcmp(destination + 10, source + 8, 6) == 0);
    assert(destination[6] == 0xcc && destination[9] == 0xcc &&
           destination[16] == 0xcc && destination[19] == 0xcc);

    uint8_t interleaved_copy[16] = {
        0, 1, 2, 3, 0xa0, 0xa1, 0xa2, 0xa3,
        8, 9, 10, 11, 0xb0, 0xb1, 0xb2, 0xb3,
    };
    calls_before = allocation_calls;
    fail_next_allocation = true;
    assert(d3d11_blit_rows(&pg, interleaved_copy + 4, 8, interleaved_copy, 8,
                           4, 2, 1));
    assert(allocation_calls == calls_before && fail_next_allocation);
    assert(memcmp(interleaved_copy,
                  (uint8_t[]){ 0, 1, 2, 3, 0, 1, 2, 3,
                               8, 9, 10, 11, 8, 9, 10, 11 },
                  sizeof(interleaved_copy)) == 0);
    fail_next_allocation = false;

    uint8_t interleaved_clipped[16] = {
        0, 1, 2, 3, 0xa0, 0xa1, 0xa2, 0xa3,
        8, 9, 10, 11, 0xb0, 0xb1, 0xb2, 0xb3,
    };
    calls_before = allocation_calls;
    fail_next_allocation = true;
    assert(d3d11_blit_copy_clipped(interleaved_clipped + 4, 8,
                                   interleaved_clipped, 8, 4, 1, 2));
    assert(allocation_calls == calls_before && fail_next_allocation);
    assert(memcmp(interleaved_clipped,
                  (uint8_t[]){ 0, 1, 2, 3, 0, 1, 2, 3,
                               8, 9, 10, 11, 8, 9, 0xb2, 0xb3 },
                  sizeof(interleaved_clipped)) == 0);
    fail_next_allocation = false;

    uint8_t overlap[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    assert(d3d11_blit_rows(&pg, overlap + 2, 4, overlap, 4, 2, 2, 1));
    const uint8_t overlap_expected[8] = { 0, 1, 0, 1, 4, 5, 4, 5 };
    assert(memcmp(overlap, overlap_expected, sizeof(overlap)) == 0);

    uint8_t clipped_overlap[12] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    };
    assert(d3d11_blit_copy_clipped(clipped_overlap + 2, 4, clipped_overlap,
                                   4, 4, 1, 2));
    const uint8_t clipped_overlap_expected[12] = {
        0, 1, 0, 1, 2, 3, 4, 5, 8, 9, 10, 11,
    };
    assert(memcmp(clipped_overlap, clipped_overlap_expected,
                  sizeof(clipped_overlap)) == 0);
    assert(!d3d11_blit_copy_clipped(destination, 10, source, 8, 1, 1, 2));
    assert(!d3d11_blit_copy_clipped(destination, 10, source, 8, SIZE_MAX, 2,
                                    0));

    uint8_t allocation_failure_dest[8] = {
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    };
    calls_before = allocation_calls;
    fail_next_allocation = true;
    assert(d3d11_blit_rows(&pg, allocation_failure_dest, 8, source, 8,
                           2, 1, 4));
    assert(allocation_calls == calls_before && fail_next_allocation);
    assert(memcmp(allocation_failure_dest, source,
                  sizeof(allocation_failure_dest)) == 0);
    fail_next_allocation = false;

    uint8_t overlapping_allocation_failure[8] = {
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    };
    const uint8_t unchanged_dest[8] = {
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    };
    fail_next_allocation = true;
    assert(!d3d11_blit_rows(&pg, overlapping_allocation_failure + 2, 8,
                            overlapping_allocation_failure, 8, 1, 1, 4));
    assert(memcmp(overlapping_allocation_failure, unchanged_dest,
                  sizeof(unchanged_dest)) == 0);
    assert(!fail_next_allocation);

    pg.image_blit.operation = NV09F_SET_OPERATION_BLEND_AND;
    pg.beta.beta = 0x3f80U << 16;
    uint8_t blend_source[8] = { 255, 20, 0, 0x11, 20, 255, 40, 0x22 };
    uint8_t blend_dest[8] = { 0, 100, 255, 0xa1, 100, 0, 200, 0xb2 };
    const uint32_t source_beta = 0x3f80;
    const uint32_t destination_beta = 0x7f80 - source_beta;
    assert(d3d11_blit_rows(&pg, blend_dest, 8, blend_source, 8, 2, 1, 4));
    for (unsigned int pixel = 0; pixel < 2; pixel++) {
        for (unsigned int channel = 0; channel < 3; channel++) {
            assert(blend_dest[pixel * 4 + channel] == expected_blend(
                blend_source[pixel * 4 + channel],
                (uint8_t[]){ 0, 100, 255, 100, 0, 200 }[
                    pixel * 3 + channel], source_beta, destination_beta));
        }
    }
    assert(blend_dest[3] == 0xa1 && blend_dest[7] == 0xb2);

    uint8_t blend_nonoverlap_dest[8] = {
        0, 100, 255, 0xa1, 100, 0, 200, 0xb2,
    };
    calls_before = allocation_calls;
    fail_next_allocation = true;
    assert(d3d11_blit_rows(&pg, blend_nonoverlap_dest, 8, blend_source, 8,
                           2, 1, 4));
    assert(allocation_calls == calls_before && fail_next_allocation);
    for (unsigned int pixel = 0; pixel < 2; pixel++) {
        for (unsigned int channel = 0; channel < 3; channel++) {
            assert(blend_nonoverlap_dest[pixel * 4 + channel] ==
                   expected_blend(
                       blend_source[pixel * 4 + channel],
                       (uint8_t[]){ 0, 100, 255, 100, 0, 200 }[
                           pixel * 3 + channel],
                       source_beta, destination_beta));
        }
    }
    assert(blend_nonoverlap_dest[3] == 0xa1 &&
           blend_nonoverlap_dest[7] == 0xb2);
    fail_next_allocation = false;

    uint8_t interleaved_blend[16] = {
        255, 20, 0, 0x11, 0, 100, 255, 0xa1,
        20, 255, 40, 0x22, 100, 0, 200, 0xb2,
    };
    calls_before = allocation_calls;
    fail_next_allocation = true;
    assert(d3d11_blit_rows(&pg, interleaved_blend + 4, 8,
                           interleaved_blend, 8, 1, 2, 4));
    assert(allocation_calls == calls_before && fail_next_allocation);
    for (unsigned int channel = 0; channel < 3; channel++) {
        assert(interleaved_blend[4 + channel] == expected_blend(
            interleaved_blend[channel], (uint8_t[]){ 0, 100, 255 }[channel],
            source_beta, destination_beta));
        assert(interleaved_blend[12 + channel] == expected_blend(
            interleaved_blend[8 + channel], (uint8_t[]){ 100, 0, 200 }[channel],
            source_beta, destination_beta));
    }
    assert(interleaved_blend[7] == 0xa1 && interleaved_blend[15] == 0xb2);
    fail_next_allocation = false;

    uint8_t blend_overlap[12] = {
        20, 40, 60, 0x10, 80, 100, 120, 0x20,
        140, 160, 180, 0x30,
    };
    const uint8_t blend_overlap_before[12] = {
        20, 40, 60, 0x10, 80, 100, 120, 0x20,
        140, 160, 180, 0x30,
    };
    assert(d3d11_blit_rows(&pg, blend_overlap + 4, 8, blend_overlap, 8,
                           2, 1, 4));
    for (unsigned int pixel = 0; pixel < 2; pixel++) {
        for (unsigned int channel = 0; channel < 3; channel++) {
            assert(blend_overlap[4 + pixel * 4 + channel] == expected_blend(
                blend_overlap_before[pixel * 4 + channel],
                blend_overlap_before[4 + pixel * 4 + channel],
                source_beta, destination_beta));
        }
    }
    assert(blend_overlap[7] == 0x20 && blend_overlap[11] == 0x30);

    uint8_t clipped_blend_overlap[64];
    uint8_t clipped_blend_before[64];
    uint8_t clipped_blend_expected[64];
    for (unsigned int i = 0; i < sizeof(clipped_blend_overlap); i++) {
        clipped_blend_overlap[i] = (uint8_t)(i + 7);
    }
    memcpy(clipped_blend_before, clipped_blend_overlap,
           sizeof(clipped_blend_before));
    memcpy(clipped_blend_expected, clipped_blend_before,
           sizeof(clipped_blend_expected));
    for (unsigned int row = 0; row < 3; row++) {
        for (unsigned int pixel = 0; pixel < 3; pixel++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                unsigned int source_byte = 4 + row * 16 + pixel * 4 + channel;
                unsigned int destination_byte =
                    20 + row * 16 + pixel * 4 + channel;
                clipped_blend_expected[destination_byte] = expected_blend(
                    clipped_blend_before[source_byte],
                    clipped_blend_before[destination_byte], source_beta,
                    destination_beta);
            }
        }
    }
    calls_before = allocation_calls;
    assert(d3d11_blit_blend_and_clipped(clipped_blend_overlap + 20, 16,
                                        clipped_blend_overlap + 4, 16, 3, 2,
                                        3, pg.beta.beta));
    assert(allocation_calls == calls_before + 1);
    assert(memcmp(clipped_blend_overlap, clipped_blend_expected,
                  sizeof(clipped_blend_expected)) == 0);

    uint8_t clipped_blend_allocation_failure[64];
    uint8_t clipped_blend_failure_before[64];
    for (unsigned int i = 0; i < sizeof(clipped_blend_allocation_failure); i++) {
        clipped_blend_allocation_failure[i] = (uint8_t)(i + 11);
    }
    memcpy(clipped_blend_failure_before, clipped_blend_allocation_failure,
           sizeof(clipped_blend_failure_before));
    fail_next_allocation = true;
    assert(!d3d11_blit_blend_and_clipped(
        clipped_blend_allocation_failure + 20, 16,
        clipped_blend_allocation_failure + 4, 16, 3, 2, 3, pg.beta.beta));
    assert(memcmp(clipped_blend_allocation_failure,
                  clipped_blend_failure_before,
                  sizeof(clipped_blend_failure_before)) == 0);
    assert(!fail_next_allocation);

    uint8_t blend_allocation_failure_source[8] = {
        0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    };
    const uint8_t unchanged_blend_dest[8] = {
        0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c, 0x5c,
    };
    fail_next_allocation = true;
    assert(!d3d11_blit_rows(&pg, blend_allocation_failure_source + 2, 8,
                            blend_allocation_failure_source, 8, 1, 1, 4));
    assert(memcmp(blend_allocation_failure_source, unchanged_blend_dest,
                  sizeof(unchanged_blend_dest)) == 0);
    assert(!fail_next_allocation);

    uint8_t alpha_surface[12];
    memset(alpha_surface, 0x5a, sizeof(alpha_surface));
    d3d11_blit_patch_alpha(alpha_surface, 8, 1, 2, 0xff);
    assert(alpha_surface[3] == 0xff && alpha_surface[11] == 0xff);
    assert(alpha_surface[4] == 0x5a && alpha_surface[7] == 0x5a);

    assert(d3d11_blit_range_valid(0x10, 8, 4, 2, 2, 0x20));
    assert(!d3d11_blit_range_valid(0x10, 8, 4, 2, 2, 0x1f));
    assert(d3d11_blit_range_valid(0x20, 4, 4, 1, 1, 0x24));
    assert(!d3d11_blit_range_valid(0x20, 4, 4, 1, 1, 0x23));
    assert(!d3d11_blit_range_valid(UINT64_MAX - 2, 4, 1, 2, 1,
                                   UINT64_MAX));

    pg.image_blit.operation = 0xff;
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8, 1, 1, 4));
    pg.image_blit.operation = NV09F_SET_OPERATION_SRCCOPY;
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8,
                            SIZE_MAX / sizeof(uint32_t) + 1, 1, 4));
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8, 1, 1, 0));
    pg.image_blit.operation = NV09F_SET_OPERATION_BLEND_AND;
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8, 1, 1, 2));
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8, SIZE_MAX, 2, 4));
    assert(!d3d11_blit_rows(&pg, destination, 10, source, 8,
                            SIZE_MAX / sizeof(uint32_t), 2, 4));
    puts("PASS: pitched copy preserves padding and overlapping 2D source data");
    puts("PASS: pitched interval overlap matches byte-wise reference grid");
    puts("PASS: clipped SRCCOPY preserves partial bytes with overlapping source");
    puts("PASS: disjoint SRCCOPY avoids temporary allocations");
    puts("PASS: interleaved SRCCOPY rows and clipped tail avoid padding-only snapshots");
    puts("PASS: overlapping SRCCOPY reports snapshot allocation failure atomically");
    puts("PASS: image-blit row bounds account for source/destination X offsets");
    puts("PASS: SRCCOPY rejects row-byte overflow and zero pixel size");
    puts("PASS: image-blit DMA offsets reject overflow before range validation");
    puts("PASS: BLEND_AND weights RGB and preserves destination alpha");
    puts("PASS: disjoint BLEND_AND avoids temporary allocations");
    puts("PASS: interleaved BLEND_AND rows avoid padding-only snapshots");
    puts("PASS: overlapping BLEND_AND snapshots source and fails atomically on allocation failure");
    puts("PASS: clipped BLEND_AND shares one snapshot across rows and tail");
    puts("PASS: BLEND_AND propagates temporary-size overflow");
    puts("PASS: alpha patch respects row pitch");
    puts("PASS: DMA range checks use byte lengths and reject overflow");
}
"""

    with tempfile.TemporaryDirectory(prefix="xemu-d3d11-blits-") as directory:
        c_file = pathlib.Path(directory) / "blits.c"
        executable = pathlib.Path(directory) / "blits"
        c_file.write_text(fixture, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

    print("PASS: image blit flushes pending surfaces before VRAM writes")
    print("PASS: invalid/zero-size blits are rejected before range arithmetic")


if __name__ == "__main__":
    main()
