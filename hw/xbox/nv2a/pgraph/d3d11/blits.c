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

static void d3d11_blit_copy(uint8_t *destination, size_t destination_pitch,
                            const uint8_t *source, size_t source_pitch,
                            size_t row_bytes, size_t height)
{
    size_t copy_size;
    if (__builtin_mul_overflow(row_bytes, height, &copy_size)) {
        return;
    }

    /* Snapshot the source so overlapping two-dimensional copies are safe. */
    uint8_t *copy = g_malloc(copy_size);
    for (size_t y = 0; y < height; y++) {
        memcpy(copy + y * row_bytes, source + y * source_pitch, row_bytes);
    }
    for (size_t y = 0; y < height; y++) {
        memcpy(destination + y * destination_pitch, copy + y * row_bytes,
               row_bytes);
    }
    g_free(copy);
}

static void d3d11_blit_blend_and(uint8_t *destination, size_t destination_pitch,
                                 const uint8_t *source, size_t source_pitch,
                                 size_t width, size_t height, uint32_t beta)
{
    const uint32_t max_beta = 0x7f80;
    const uint32_t source_beta = MIN(beta >> 16, max_beta);
    const uint32_t destination_beta = max_beta - source_beta;
    size_t copy_size;
    if (__builtin_mul_overflow(width, sizeof(uint32_t), &copy_size) ||
        __builtin_mul_overflow(copy_size, height, &copy_size)) {
        return;
    }

    uint8_t *copy = g_malloc(copy_size);
    for (size_t y = 0; y < height; y++) {
        memcpy(copy + y * width * 4, source + y * source_pitch, width * 4);
    }
    for (size_t y = 0; y < height; y++) {
        const uint8_t *src = copy + y * width * 4;
        uint8_t *dst = destination + y * destination_pitch;
        for (size_t x = 0; x < width; x++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                uint32_t blended = src[x * 4 + channel] * source_beta +
                                   dst[x * 4 + channel] * destination_beta;
                dst[x * 4 + channel] = blended / max_beta;
            }
        }
    }
    g_free(copy);
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
    switch (pg->image_blit.operation) {
    case NV09F_SET_OPERATION_SRCCOPY:
        d3d11_blit_copy(destination, destination_pitch, source, source_pitch,
                        width * bytes_per_pixel, height);
        return true;
    case NV09F_SET_OPERATION_BLEND_AND:
        if (bytes_per_pixel != 4) {
            return false;
        }
        d3d11_blit_blend_and(destination, destination_pitch, source,
                             source_pitch, width, height, pg->beta.beta);
        return true;
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
           !__builtin_add_overflow(end, row_bytes, &end) && end <= dma_length;
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

    hwaddr source_dma_length;
    uint8_t *source_base =
        nv_dma_map(d, surfaces->dma_image_source, &source_dma_length);
    hwaddr destination_dma_length;
    uint8_t *destination_base =
        nv_dma_map(d, surfaces->dma_image_dest, &destination_dma_length);
    if (!source_base || !destination_base ||
        surfaces->source_offset >= source_dma_length ||
        surfaces->dest_offset >= destination_dma_length) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: image blit DMA mapping is invalid");
        return;
    }

    size_t row_pixels =
        MIN((size_t)blit->width,
            MIN(surfaces->source_pitch, surfaces->dest_pitch) / bpp);
    hwaddr source_offset = surfaces->source_offset +
                           (uint64_t)blit->in_y * surfaces->source_pitch +
                           (uint64_t)blit->in_x * bpp;
    hwaddr destination_offset = surfaces->dest_offset +
                                (uint64_t)blit->out_y * surfaces->dest_pitch +
                                (uint64_t)blit->out_x * bpp;
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
    if (full_rows &&
        !d3d11_blit_rows(pg, destination, surfaces->dest_pitch, source,
                         surfaces->source_pitch, row_pixels, full_rows, bpp)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: unsupported image blit operation");
        return;
    }
    if (remainder &&
        !d3d11_blit_rows(pg, destination + full_rows * surfaces->dest_pitch, 0,
                         source + full_rows * surfaces->source_pitch, 0,
                         remainder / bpp, 1, bpp)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: unsupported image blit operation");
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
