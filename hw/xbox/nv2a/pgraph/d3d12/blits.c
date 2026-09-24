/*
 * Geforce NV2A PGRAPH Direct3D 12 blits
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "blits.h"
#include "surfaces.h"

static unsigned int d3d12_blit_bytes_per_pixel(unsigned int format)
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

static bool d3d12_perform_blit(int operation, uint8_t *source, uint8_t *dest,
                               size_t width, size_t height,
                               size_t bytes_per_pixel, size_t source_pitch,
                               size_t dest_pitch, const BetaState *beta)
{
    size_t row_bytes = width * bytes_per_pixel;
    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        uint8_t *source_end = source + (height - 1) * source_pitch + row_bytes;
        if (dest > source && dest < source_end) {
            source += (height - 1) * source_pitch;
            dest += (height - 1) * dest_pitch;
            for (size_t y = 0; y < height; y++) {
                memmove(dest, source, row_bytes);
                if (y + 1 < height) {
                    source -= source_pitch;
                    dest -= dest_pitch;
                }
            }
        } else {
            for (size_t y = 0; y < height; y++) {
                memmove(dest, source, row_bytes);
                source += source_pitch;
                dest += dest_pitch;
            }
        }
        return true;
    }
    if (operation != NV09F_SET_OPERATION_BLEND_AND || bytes_per_pixel != 4) {
        return false;
    }

    uint8_t *source_copy = NULL;
    uint8_t *source_end = source + (height - 1) * source_pitch + row_bytes;
    uint8_t *dest_end = dest + (height - 1) * dest_pitch + row_bytes;
    if (source < dest_end && dest < source_end) {
        source_copy = g_malloc(row_bytes * height);
        for (size_t y = 0; y < height; y++) {
            memcpy(source_copy + y * row_bytes, source + y * source_pitch,
                   row_bytes);
        }
        source = source_copy;
        source_pitch = row_bytes;
    }

    const uint32_t max_beta = 0x7f80;
    uint32_t beta_value = MIN(beta->beta >> 16, max_beta);
    uint32_t inverse = max_beta - beta_value;
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            for (unsigned int channel = 0; channel < 3; channel++) {
                uint32_t src = source[x * 4 + channel] * beta_value;
                uint32_t dst = dest[x * 4 + channel] * inverse;
                dest[x * 4 + channel] = (src + dst) / max_beta;
            }
        }
        source += source_pitch;
        dest += dest_pitch;
    }
    g_free(source_copy);
    return true;
}

static void d3d12_patch_alpha(uint8_t *dest, size_t width, size_t height,
                              size_t pitch, uint8_t alpha)
{
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            dest[x * 4 + 3] = alpha;
        }
        dest += pitch;
    }
}

void pgraph_d3d12_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context = &pg->context_surfaces_2d;
    ImageBlitState *blit = &pg->image_blit;
    Error *error = NULL;
    if (!pgraph_d3d12_surface_flush(d, &error)) {
        qemu_host_emit_log(
            QEMU_HOST_LOG_ERROR,
            error ? error_get_pretty(error) :
                    "D3D12: failed to synchronize surfaces for blit");
        error_free(error);
        return;
    }
    unsigned int bpp = d3d12_blit_bytes_per_pixel(context->color_format);
    if (!bpp || context->object_instance != blit->context_surfaces) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D12: invalid NV2A 2D blit state");
        return;
    }
    if (!blit->width || !blit->height || !context->source_pitch ||
        !context->dest_pitch) {
        return;
    }

    hwaddr source_length, dest_length;
    uint8_t *source = nv_dma_map(d, context->dma_image_source, &source_length);
    uint8_t *dest = nv_dma_map(d, context->dma_image_dest, &dest_length);
    uint64_t source_offset = (uint64_t)context->source_offset +
                             (uint64_t)blit->in_y * context->source_pitch +
                             (uint64_t)blit->in_x * bpp;
    uint64_t dest_offset = (uint64_t)context->dest_offset +
                           (uint64_t)blit->out_y * context->dest_pitch +
                           (uint64_t)blit->out_x * bpp;
    size_t row_pixels =
        MIN(blit->width, MIN(context->source_pitch, context->dest_pitch) / bpp);
    size_t row_bytes = row_pixels * bpp;
    uint64_t source_end = source_offset +
                          (uint64_t)(blit->height - 1) * context->source_pitch +
                          row_bytes;
    uint64_t dest_end = dest_offset +
                        (uint64_t)(blit->height - 1) * context->dest_pitch +
                        row_bytes;
    if (!source || !dest || source_end > source_length ||
        dest_end > dest_length) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D12: NV2A 2D blit exceeds its DMA object");
        return;
    }

    hwaddr dest_address = dest + dest_offset - d->vram_ptr;
    size_t requested_size =
        (size_t)(blit->height - 1) * context->dest_pitch + row_bytes;
    size_t clipped_size =
        nv_clip_gpu_tile_blit(d, dest_address, requested_size);
    size_t rows = clipped_size ?
                      MIN((clipped_size - 1) / context->dest_pitch + 1,
                          (size_t)blit->height) :
                      0;
    if (rows) {
        size_t last_row_bytes =
            MIN(row_bytes, clipped_size - (rows - 1) * context->dest_pitch);
        uint8_t *source_start = source + source_offset;
        uint8_t *dest_start = dest + dest_offset;
        uint8_t *source_range_end =
            source_start + (rows - 1) * context->source_pitch + row_bytes;
        bool backwards =
            dest_start > source_start && dest_start < source_range_end;
        bool ok = true;
        if (backwards) {
            ok = d3d12_perform_blit(
                blit->operation,
                source_start + (rows - 1) * context->source_pitch,
                dest_start + (rows - 1) * context->dest_pitch,
                last_row_bytes / bpp, 1, bpp, context->source_pitch,
                context->dest_pitch, &pg->beta);
        }
        if (ok && rows > 1) {
            ok = d3d12_perform_blit(
                blit->operation, source_start, dest_start, row_pixels, rows - 1,
                bpp, context->source_pitch, context->dest_pitch, &pg->beta);
        }
        if (ok && !backwards) {
            ok = d3d12_perform_blit(
                blit->operation,
                source_start + (rows - 1) * context->source_pitch,
                dest_start + (rows - 1) * context->dest_pitch,
                last_row_bytes / bpp, 1, bpp, context->source_pitch,
                context->dest_pitch, &pg->beta);
        }
        if (!ok) {
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                               "D3D12: unsupported NV2A 2D blit operation");
            return;
        }
    }

    uint8_t alpha = 0;
    bool patch_alpha = false;
    if (context->color_format == NV062_SET_COLOR_FORMAT_LE_X8R8G8B8) {
        alpha = 0xff;
        patch_alpha = true;
    } else if (context->color_format ==
               NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8) {
        patch_alpha = true;
    }
    if (patch_alpha && rows) {
        if (rows > 1) {
            d3d12_patch_alpha(dest + dest_offset, row_pixels, rows - 1,
                              context->dest_pitch, alpha);
        }
        size_t last_row_bytes =
            MIN(row_bytes, clipped_size - (rows - 1) * context->dest_pitch);
        d3d12_patch_alpha(dest + dest_offset + (rows - 1) * context->dest_pitch,
                          last_row_bytes / 4, 1, 0, alpha);
    }

    pgraph_d3d12_invalidate_surface_range(d, dest_address, clipped_size);
    memory_region_set_client_dirty(d->vram, dest_address, clipped_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, dest_address, clipped_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}
