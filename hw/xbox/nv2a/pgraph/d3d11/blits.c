/*
 * GeForce NV2A PGRAPH Direct3D 11 image blits
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "qemu/qemu-host.h"
#include "blits.h"
#include "renderer.h"
#include "surfaces.h"

static unsigned int d3d11_blit_bytes_per_pixel(unsigned int format)
{
    switch (format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        return 1;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        return 2;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        return 4;
    default:
        return 0;
    }
}

static bool d3d11_blit_row_pixels(size_t requested_pixels,
                                  unsigned int source_x,
                                  unsigned int source_pitch,
                                  unsigned int destination_x,
                                  unsigned int destination_pitch,
                                  unsigned int bytes_per_pixel,
                                  size_t *row_pixels)
{
    size_t source_x_bytes;
    size_t destination_x_bytes;
    if (!bytes_per_pixel || !row_pixels ||
        __builtin_mul_overflow((size_t)source_x,
                               (size_t)bytes_per_pixel, &source_x_bytes) ||
        __builtin_mul_overflow((size_t)destination_x,
                               (size_t)bytes_per_pixel,
                               &destination_x_bytes) ||
        source_x_bytes >= source_pitch ||
        destination_x_bytes >= destination_pitch) {
        return false;
    }

    size_t source_pixels = (source_pitch - source_x_bytes) / bytes_per_pixel;
    size_t destination_pixels =
        (destination_pitch - destination_x_bytes) / bytes_per_pixel;
    *row_pixels = MIN(requested_pixels,
                      MIN(source_pixels, destination_pixels));
    return *row_pixels != 0;
}

static bool d3d11_blit_offset(hwaddr base_offset, unsigned int x,
                              unsigned int y, unsigned int pitch,
                              unsigned int bytes_per_pixel, hwaddr *offset)
{
    uint64_t row_offset;
    uint64_t column_offset;
    uint64_t base_plus_row;
    uint64_t result;
    if (!offset || !bytes_per_pixel ||
        __builtin_mul_overflow((uint64_t)y, (uint64_t)pitch, &row_offset) ||
        __builtin_mul_overflow((uint64_t)x, (uint64_t)bytes_per_pixel,
                               &column_offset) ||
        __builtin_add_overflow((uint64_t)base_offset, row_offset,
                               &base_plus_row) ||
        __builtin_add_overflow(base_plus_row, column_offset, &result)) {
        return false;
    }
    *offset = result;
    return true;
}

static bool d3d11_blit_row_interval(const uint8_t *base, size_t pitch,
                                    size_t row_bytes, size_t height,
                                    size_t trailing_bytes, size_t index,
                                    uintptr_t *start, uintptr_t *end)
{
    size_t byte_offset;
    size_t byte_length;
    uintptr_t address_offset;
    uintptr_t address_length;
    uintptr_t address_start = (uintptr_t)base;
    if (!start || !end) {
        return false;
    }

    if (index < height) {
        if (__builtin_mul_overflow(index, pitch, &byte_offset)) {
            return false;
        }
        byte_length = row_bytes;
    } else if (index == height && trailing_bytes) {
        if (__builtin_mul_overflow(height, pitch, &byte_offset)) {
            return false;
        }
        byte_length = trailing_bytes;
    } else {
        return false;
    }
    address_offset = (uintptr_t)byte_offset;
    address_length = (uintptr_t)byte_length;
    if ((size_t)address_offset != byte_offset ||
        (size_t)address_length != byte_length ||
        __builtin_add_overflow(address_start, address_offset, start) ||
        __builtin_add_overflow(*start, address_length, end)) {
        return false;
    }

    return true;
}

static bool d3d11_blit_regions_overlap(const uint8_t *source,
                                      const uint8_t *destination,
                                      size_t source_pitch,
                                      size_t destination_pitch,
                                      size_t row_bytes, size_t height,
                                      size_t trailing_bytes, bool *overlap)
{
    size_t interval_count = height;
    size_t source_index = 0;
    size_t destination_index = 0;
    if (!overlap || (trailing_bytes && height == SIZE_MAX)) {
        return false;
    }
    if (trailing_bytes) {
        interval_count++;
    }
    *overlap = false;

    while (source_index < interval_count &&
           destination_index < interval_count) {
        uintptr_t source_start;
        uintptr_t source_end;
        uintptr_t destination_start;
        uintptr_t destination_end;
        if (!d3d11_blit_row_interval(source, source_pitch, row_bytes, height,
                                     trailing_bytes, source_index,
                                     &source_start, &source_end) ||
            !d3d11_blit_row_interval(destination, destination_pitch,
                                     row_bytes, height, trailing_bytes,
                                     destination_index, &destination_start,
                                     &destination_end)) {
            return false;
        }
        if (source_start == source_end) {
            source_index++;
        } else if (destination_start == destination_end) {
            destination_index++;
        } else if (source_start < destination_end &&
                   destination_start < source_end) {
            *overlap = true;
            return true;
        } else if (source_end <= destination_start) {
            source_index++;
        } else {
            destination_index++;
        }
    }
    return true;
}

static bool d3d11_blit_copy_clipped(uint8_t *destination,
                                   size_t destination_pitch,
                                   const uint8_t *source, size_t source_pitch,
                                   size_t row_bytes, size_t height,
                                   size_t trailing_bytes)
{
    size_t full_size;
    size_t copy_size;
    size_t source_tail_offset;
    size_t destination_tail_offset;
    if (trailing_bytes > row_bytes ||
        __builtin_mul_overflow(row_bytes, height, &full_size) ||
        __builtin_add_overflow(full_size, trailing_bytes, &copy_size) ||
        __builtin_mul_overflow(source_pitch, height, &source_tail_offset) ||
        __builtin_mul_overflow(destination_pitch, height,
                               &destination_tail_offset)) {
        return false;
    }
    if (!copy_size) {
        return true;
    }

    bool overlaps;
    if (!d3d11_blit_regions_overlap(source, destination, source_pitch,
                                    destination_pitch, row_bytes, height,
                                    trailing_bytes, &overlaps)) {
        return false;
    }
    if (!overlaps) {
        for (size_t y = 0; y < height; y++) {
            memcpy(destination + y * destination_pitch,
                   source + y * source_pitch, row_bytes);
        }
        if (trailing_bytes) {
            memcpy(destination + destination_tail_offset,
                   source + source_tail_offset, trailing_bytes);
        }
        return true;
    }

    /* Snapshot only when accessed source and destination bytes overlap. */
    uint8_t *copy = g_try_malloc(copy_size);
    if (!copy) {
        return false;
    }
    for (size_t y = 0; y < height; y++) {
        memcpy(copy + y * row_bytes, source + y * source_pitch, row_bytes);
    }
    if (trailing_bytes) {
        memcpy(copy + full_size, source + source_tail_offset, trailing_bytes);
    }
    for (size_t y = 0; y < height; y++) {
        memcpy(destination + y * destination_pitch, copy + y * row_bytes,
               row_bytes);
    }
    if (trailing_bytes) {
        memcpy(destination + destination_tail_offset, copy + full_size,
               trailing_bytes);
    }
    g_free(copy);
    return true;
}

static bool d3d11_blit_copy(uint8_t *destination, size_t destination_pitch,
                            const uint8_t *source, size_t source_pitch,
                            size_t row_bytes, size_t height)
{
    return d3d11_blit_copy_clipped(destination, destination_pitch, source,
                                   source_pitch, row_bytes, height, 0);
}

static bool d3d11_blit_blend_and_clipped(uint8_t *destination,
                                         size_t destination_pitch,
                                         const uint8_t *source,
                                         size_t source_pitch, size_t width,
                                         size_t height,
                                         size_t trailing_pixels,
                                         uint32_t beta)
{
    const uint32_t max_beta = 0x7f80;
    const uint32_t source_beta = MIN(beta >> 16, max_beta);
    const uint32_t destination_beta = max_beta - source_beta;
    size_t row_bytes;
    size_t trailing_bytes;
    size_t source_tail_offset;
    size_t destination_tail_offset;
    size_t full_size;
    size_t copy_size;
    if (__builtin_mul_overflow(width, sizeof(uint32_t), &row_bytes) ||
        __builtin_mul_overflow(trailing_pixels, sizeof(uint32_t),
                               &trailing_bytes) ||
        __builtin_mul_overflow(row_bytes, height, &full_size) ||
        __builtin_add_overflow(full_size, trailing_bytes, &copy_size) ||
        __builtin_mul_overflow(source_pitch, height, &source_tail_offset) ||
        __builtin_mul_overflow(destination_pitch, height,
                               &destination_tail_offset)) {
        return false;
    }
    if (!copy_size) {
        return true;
    }

    bool overlaps;
    if (!d3d11_blit_regions_overlap(source, destination, source_pitch,
                                    destination_pitch, row_bytes, height,
                                    trailing_bytes, &overlaps)) {
        return false;
    }

    uint8_t *copy = NULL;
    if (overlaps) {
        copy = g_try_malloc(copy_size);
        if (!copy) {
            return false;
        }
        for (size_t y = 0; y < height; y++) {
            memcpy(copy + y * row_bytes, source + y * source_pitch,
                   row_bytes);
        }
        if (trailing_bytes) {
            memcpy(copy + full_size, source + source_tail_offset,
                   trailing_bytes);
        }
    }
    for (size_t y = 0; y < height; y++) {
        const uint8_t *src = overlaps ? copy + y * row_bytes :
                                        source + y * source_pitch;
        uint8_t *dst = destination + y * destination_pitch;
        for (size_t x = 0; x < width; x++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                uint32_t blended = src[x * 4 + channel] * source_beta +
                                   dst[x * 4 + channel] * destination_beta;
                dst[x * 4 + channel] = blended / max_beta;
            }
        }
    }
    if (trailing_bytes) {
        const uint8_t *src = overlaps ? copy + full_size :
                                        source + source_tail_offset;
        uint8_t *dst = destination + destination_tail_offset;
        for (size_t x = 0; x < trailing_pixels; x++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                uint32_t blended = src[x * 4 + channel] * source_beta +
                                   dst[x * 4 + channel] * destination_beta;
                dst[x * 4 + channel] = blended / max_beta;
            }
        }
    }
    g_free(copy);
    return true;
}

static bool d3d11_blit_blend_and(uint8_t *destination,
                                 size_t destination_pitch,
                                 const uint8_t *source, size_t source_pitch,
                                 size_t width, size_t height, uint32_t beta)
{
    return d3d11_blit_blend_and_clipped(destination, destination_pitch, source,
                                        source_pitch, width, height, 0, beta);
}

static void d3d11_blit_patch_alpha(uint8_t *destination, size_t pitch,
                                   size_t width, size_t height, uint8_t alpha)
{
    for (size_t y = 0; y < height; y++) {
        uint8_t *row = destination + y * pitch;
        for (size_t x = 0; x < width; x++) {
            row[x * 4 + 3] = alpha;
        }
    }
}

static bool d3d11_blit_rows(PGRAPHState *pg, uint8_t *destination,
                            size_t destination_pitch, const uint8_t *source,
                            size_t source_pitch, size_t width, size_t height,
                            unsigned int bytes_per_pixel)
{
    if (!bytes_per_pixel) {
        return false;
    }
    switch (pg->image_blit.operation) {
    case NV09F_SET_OPERATION_SRCCOPY: {
        size_t row_bytes;
        if (__builtin_mul_overflow(width, (size_t)bytes_per_pixel,
                                   &row_bytes)) {
            return false;
        }
        return d3d11_blit_copy(destination, destination_pitch, source,
                               source_pitch, row_bytes, height);
    }
    case NV09F_SET_OPERATION_BLEND_AND:
        if (bytes_per_pixel != 4) {
            return false;
        }
        return d3d11_blit_blend_and(destination, destination_pitch, source,
                                    source_pitch, width, height, pg->beta.beta);
    default:
        return false;
    }
}

static bool d3d11_blit_range_valid(hwaddr offset, size_t pitch, size_t width,
                                   size_t height, unsigned int bpp,
                                   hwaddr dma_length)
{
    uint64_t row_bytes;
    uint64_t last_row;
    uint64_t end;
    return !__builtin_mul_overflow((uint64_t)width, bpp, &row_bytes) &&
           !__builtin_mul_overflow((uint64_t)(height - 1), pitch, &last_row) &&
           !__builtin_add_overflow((uint64_t)offset, last_row, &end) &&
           !__builtin_add_overflow(end, row_bytes, &end) &&
           end <= dma_length;
}

void pgraph_d3d11_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (!r || !r->resources_ready || r->device_lost || r->app_suspended ||
        r->memory_suspended) {
        return;
    }
    ContextSurfaces2DState *surfaces = &pg->context_surfaces_2d;
    ImageBlitState *blit = &pg->image_blit;
    Error *error = NULL;

    if (surfaces->object_instance != blit->context_surfaces) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit has mismatched 2D context");
        return;
    }
    unsigned int bpp = d3d11_blit_bytes_per_pixel(surfaces->color_format);
    if (!bpp || !blit->width || !blit->height || !surfaces->source_pitch ||
        !surfaces->dest_pitch) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: unsupported or invalid image blit state");
        return;
    }
    if (blit->operation == NV09F_SET_OPERATION_BLEND_AND && bpp != 4) {
        qemu_host_emit_log(
            QEMU_HOST_LOG_ERROR,
            "D3D11: BLEND_AND image blit requires a 32-bit surface");
        return;
    }
    if (blit->operation != NV09F_SET_OPERATION_SRCCOPY &&
        blit->operation != NV09F_SET_OPERATION_BLEND_AND) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: unsupported image blit operation");
        return;
    }

    if (!pgraph_d3d11_surface_flush(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR, error_get_pretty(error));
        error_free(error);
        return;
    }

    hwaddr source_dma_length = 0;
    uint8_t *source_base = NULL;
    hwaddr destination_dma_length = 0;
    uint8_t *destination_base = NULL;
    /* The checked mapper preserves nv_dma_map's inclusive DMA limit. */
    if (!nv_dma_map_checked(d, surfaces->dma_image_source, &source_base,
                            &source_dma_length) ||
        !nv_dma_map_checked(d, surfaces->dma_image_dest, &destination_base,
                            &destination_dma_length) ||
        !source_base || !destination_base ||
        surfaces->source_offset > source_dma_length ||
        surfaces->dest_offset > destination_dma_length) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit DMA mapping is invalid");
        return;
    }

    size_t row_pixels;
    if (!d3d11_blit_row_pixels(blit->width, blit->in_x,
                               surfaces->source_pitch, blit->out_x,
                               surfaces->dest_pitch, bpp, &row_pixels)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit coordinates exceed row pitches");
        return;
    }
    hwaddr source_offset;
    hwaddr destination_offset;
    if (!d3d11_blit_offset(surfaces->source_offset, blit->in_x, blit->in_y,
                           surfaces->source_pitch, bpp, &source_offset) ||
        !d3d11_blit_offset(surfaces->dest_offset, blit->out_x, blit->out_y,
                           surfaces->dest_pitch, bpp, &destination_offset)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit DMA offset overflow");
        return;
    }
    if (!row_pixels ||
        !d3d11_blit_range_valid(source_offset, surfaces->source_pitch,
                                row_pixels, blit->height, bpp,
                                source_dma_length) ||
        !d3d11_blit_range_valid(destination_offset, surfaces->dest_pitch,
                                row_pixels, blit->height, bpp,
                                destination_dma_length)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit exceeds its DMA object");
        return;
    }

    hwaddr destination_address =
        destination_base - d->vram_ptr + destination_offset;
    size_t requested_size =
        (size_t)(blit->height - 1) * surfaces->dest_pitch + row_pixels * bpp;
    size_t clipped_size =
        nv_clip_gpu_tile_blit(d, destination_address, requested_size);
    size_t full_rows =
        MIN((size_t)blit->height, clipped_size / surfaces->dest_pitch);
    size_t remainder = clipped_size - full_rows * surfaces->dest_pitch;
    remainder = MIN(remainder, row_pixels * bpp);

    uint8_t *source = source_base + source_offset;
    uint8_t *destination = destination_base + destination_offset;
    bool blit_succeeded;
    if (blit->operation == NV09F_SET_OPERATION_SRCCOPY) {
        /* Preserve raw clipped tail bytes, matching the GL SRCCOPY path. */
        blit_succeeded = d3d11_blit_copy_clipped(
            destination, surfaces->dest_pitch, source, surfaces->source_pitch,
            row_pixels * bpp, full_rows, remainder);
    } else {
        if (remainder) {
            blit_succeeded = d3d11_blit_blend_and_clipped(
                destination, surfaces->dest_pitch, source,
                surfaces->source_pitch, row_pixels, full_rows, remainder / bpp,
                pg->beta.beta);
        } else {
            blit_succeeded =
                !full_rows ||
                d3d11_blit_rows(pg, destination, surfaces->dest_pitch, source,
                                surfaces->source_pitch, row_pixels, full_rows,
                                bpp);
        }
    }
    if (!blit_succeeded) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit execution failed");
        return;
    }

    bool patch_alpha =
        surfaces->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ||
        surfaces->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8;
    if (patch_alpha) {
        uint8_t alpha =
            surfaces->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8 ?
                0xff :
                0;
        d3d11_blit_patch_alpha(destination, surfaces->dest_pitch, row_pixels,
                               full_rows, alpha);
        if (remainder) {
            d3d11_blit_patch_alpha(destination +
                                       full_rows * surfaces->dest_pitch,
                                   0, remainder / bpp, 1, alpha);
        }
    }

    if (clipped_size) {
        memory_region_set_client_dirty(d->vram, destination_address,
                                       clipped_size, DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, destination_address,
                                       clipped_size, DIRTY_MEMORY_NV2A_TEX);
        pgraph_d3d11_surface_invalidate_range(d, destination_address,
                                              clipped_size);
        pg->draw_time++;
    }
}
