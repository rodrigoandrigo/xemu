/*
 * GeForce NV2A PGRAPH Direct3D 11 render targets
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"
#include "surfaces.h"

static void d3d11_release_target(PGRAPHD3D11RenderTarget *target)
{
    if (target->access_cb && tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), target->access_cb);
    }
    if (target->dsv) {
        ID3D11DepthStencilView_Release(target->dsv);
    }
    if (target->srv) {
        ID3D11ShaderResourceView_Release(target->srv);
    }
    if (target->rtv) {
        ID3D11RenderTargetView_Release(target->rtv);
    }
    if (target->texture) {
        ID3D11Texture2D_Release(target->texture);
    }
    memset(target, 0, sizeof(*target));
}

static bool d3d11_ranges_overlap(hwaddr first, size_t first_length,
                                 hwaddr second, size_t second_length)
{
    uint64_t first_end = first + first_length;
    uint64_t second_end = second + second_length;
    return first < second_end && second < first_end;
}

static D3D11_COMPARISON_FUNC d3d11_comparison(unsigned int value)
{
    static const D3D11_COMPARISON_FUNC map[] = {
        D3D11_COMPARISON_NEVER,         D3D11_COMPARISON_LESS,
        D3D11_COMPARISON_EQUAL,         D3D11_COMPARISON_LESS_EQUAL,
        D3D11_COMPARISON_GREATER,       D3D11_COMPARISON_NOT_EQUAL,
        D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP d3d11_stencil_op(unsigned int value)
{
    static const D3D11_STENCIL_OP map[] = {
        D3D11_STENCIL_OP_KEEP,     D3D11_STENCIL_OP_KEEP,
        D3D11_STENCIL_OP_ZERO,     D3D11_STENCIL_OP_REPLACE,
        D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT,
        D3D11_STENCIL_OP_INVERT,   D3D11_STENCIL_OP_INCR,
        D3D11_STENCIL_OP_DECR,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D11_STENCIL_OP_KEEP;
}

static D3D11_BLEND d3d11_blend_factor(unsigned int value)
{
    static const D3D11_BLEND map[] = {
        D3D11_BLEND_ZERO,          D3D11_BLEND_ONE,
        D3D11_BLEND_SRC_COLOR,     D3D11_BLEND_INV_SRC_COLOR,
        D3D11_BLEND_SRC_ALPHA,     D3D11_BLEND_INV_SRC_ALPHA,
        D3D11_BLEND_DEST_ALPHA,    D3D11_BLEND_INV_DEST_ALPHA,
        D3D11_BLEND_DEST_COLOR,    D3D11_BLEND_INV_DEST_COLOR,
        D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ZERO,
        D3D11_BLEND_BLEND_FACTOR,  D3D11_BLEND_INV_BLEND_FACTOR,
        D3D11_BLEND_BLEND_FACTOR,  D3D11_BLEND_INV_BLEND_FACTOR,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D11_BLEND_ONE;
}

static D3D11_BLEND d3d11_alpha_blend_factor(D3D11_BLEND value)
{
    switch (value) {
    case D3D11_BLEND_SRC_COLOR:
        return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:
        return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC_ALPHA_SAT:
        return D3D11_BLEND_ONE;
    default:
        return value;
    }
}

static D3D11_BLEND_OP d3d11_blend_op(unsigned int value)
{
    static const D3D11_BLEND_OP map[] = {
        D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
        D3D11_BLEND_OP_ADD,      D3D11_BLEND_OP_MIN,
        D3D11_BLEND_OP_MAX,      D3D11_BLEND_OP_REV_SUBTRACT,
        D3D11_BLEND_OP_ADD,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D11_BLEND_OP_ADD;
}

static D3D11_LOGIC_OP d3d11_logic_op(unsigned int value)
{
    static const D3D11_LOGIC_OP map[] = {
        D3D11_LOGIC_OP_CLEAR,         D3D11_LOGIC_OP_AND,
        D3D11_LOGIC_OP_AND_REVERSE,   D3D11_LOGIC_OP_COPY,
        D3D11_LOGIC_OP_AND_INVERTED,  D3D11_LOGIC_OP_NOOP,
        D3D11_LOGIC_OP_XOR,           D3D11_LOGIC_OP_OR,
        D3D11_LOGIC_OP_NOR,           D3D11_LOGIC_OP_EQUIV,
        D3D11_LOGIC_OP_INVERT,        D3D11_LOGIC_OP_OR_REVERSE,
        D3D11_LOGIC_OP_COPY_INVERTED, D3D11_LOGIC_OP_OR_INVERTED,
        D3D11_LOGIC_OP_NAND,          D3D11_LOGIC_OP_SET,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D11_LOGIC_OP_COPY;
}

static void d3d11_surface_access_callback(void *opaque, MemoryRegion *mr,
                                          hwaddr address, hwaddr length,
                                          bool write)
{
    NV2AState *d = opaque;
    PGRAPHState *pg = &d->pgraph;
    bool synchronize = false;
    (void)mr;

    qemu_mutex_lock(&pg->lock);
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (r) {
        PGRAPHD3D11RenderTarget *targets[] = {
            &r->render_targets.color,
            &r->render_targets.depth_stencil,
        };
        for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
            PGRAPHD3D11RenderTarget *target = targets[i];
            if (!target->texture ||
                !d3d11_ranges_overlap(address, length, target->vram_address,
                                      target->storage_length)) {
                continue;
            }
            Surface *surface = i == 0 ? &pg->surface_color : &pg->surface_zeta;
            synchronize |= surface->draw_dirty;
            if (write) {
                surface->buffer_dirty = true;
            }
        }
    }
    qemu_mutex_unlock(&pg->lock);

    if (synchronize) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&pg->flush_complete);
        qatomic_set(&pg->flush_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&pg->flush_complete);
    }
}

void pgraph_d3d11_surfaces_finalize(PGRAPHD3D11State *r)
{
    d3d11_release_target(&r->render_targets.color);
    d3d11_release_target(&r->render_targets.depth_stencil);
    if (r->render_targets.blend_state) {
        ID3D11BlendState_Release(r->render_targets.blend_state);
        r->render_targets.blend_state = NULL;
    }
    if (r->render_targets.depth_stencil_state) {
        ID3D11DepthStencilState_Release(r->render_targets.depth_stencil_state);
        r->render_targets.depth_stencil_state = NULL;
    }
}

static unsigned int d3d11_color_bytes_per_pixel(unsigned int format)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return 2;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return 1;
    default:
        return 4;
    }
}

static uint8_t d3d11_expand_5_to_8(uint32_t value)
{
    return (value << 3) | (value >> 2);
}

static uint8_t d3d11_expand_6_to_8(uint32_t value)
{
    return (value << 2) | (value >> 4);
}

static uint16_t d3d11_float_to_f16(float value)
{
    value = CLAMP(value, 0.0f, f16_max);
    if (value == 0.0f) {
        return 0;
    }
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits <= 0x3c000000U) {
        return 1;
    }
    return MIN((bits - 0x3c000000U + 0x400U) >> 11, UINT16_MAX);
}

static uint32_t d3d11_float_to_f24(float value)
{
    value = CLAMP(value, 0.0f, f24_max);
    if (value == 0.0f) {
        return 0;
    }
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return MIN((bits + 0x40U) >> 7, 0x00ffffffU);
}

static void d3d11_convert_color_to_bgra8(
    unsigned int format, uint8_t *destination, size_t destination_pitch,
    const uint8_t *source, size_t source_pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = source + y * source_pitch;
        uint8_t *dst = destination + y * destination_pitch;
        for (uint32_t x = 0; x < width; x++, dst += 4) {
            switch (format) {
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5: {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2, sizeof(pixel));
                dst[0] = d3d11_expand_5_to_8(pixel & 0x1f);
                dst[1] = d3d11_expand_5_to_8((pixel >> 5) & 0x1f);
                dst[2] = d3d11_expand_5_to_8((pixel >> 10) & 0x1f);
                dst[3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5 ?
                        0xff :
                        0;
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2, sizeof(pixel));
                dst[0] = d3d11_expand_5_to_8(pixel & 0x1f);
                dst[1] = d3d11_expand_6_to_8((pixel >> 5) & 0x3f);
                dst[2] = d3d11_expand_5_to_8((pixel >> 11) & 0x1f);
                dst[3] = 0xff;
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
                dst[0] = src[x];
                dst[1] = dst[2] = 0;
                dst[3] = 0xff;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
                dst[0] = src[x * 2];
                dst[1] = src[x * 2 + 1];
                dst[2] = 0;
                dst[3] = 0xff;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
                memcpy(dst, src + x * 4, 3);
                dst[3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8 ?
                        0xff :
                        0;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
                memcpy(dst, src + x * 4, 3);
                dst[3] = (src[x * 4 + 3] & 0x7f) * 255 / 127;
                break;
            default:
                memcpy(dst, src + x * 4, 4);
                break;
            }
        }
    }
}

static void d3d11_convert_bgra8_to_color(
    unsigned int format, uint8_t *destination, size_t destination_pitch,
    const uint8_t *source, size_t source_pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = destination + y * destination_pitch;
        const uint8_t *src = source + y * source_pitch;
        for (uint32_t x = 0; x < width; x++, src += 4) {
            switch (format) {
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5: {
                uint16_t pixel = ((src[2] >> 3) << 10) | ((src[1] >> 3) << 5) |
                                 (src[0] >> 3);
                if (format ==
                    NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5) {
                    pixel |= 0x8000;
                }
                memcpy(dst + x * 2, &pixel, sizeof(pixel));
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
                uint16_t pixel = ((src[2] >> 3) << 11) | ((src[1] >> 2) << 5) |
                                 (src[0] >> 3);
                memcpy(dst + x * 2, &pixel, sizeof(pixel));
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
                dst[x] = src[0];
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
                dst[x * 2] = src[0];
                dst[x * 2 + 1] = src[1];
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
                memcpy(dst + x * 4, src, 3);
                dst[x * 4 + 3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8 ?
                        0xff :
                        0;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
                memcpy(dst + x * 4, src, 3);
                dst[x * 4 + 3] = (src[3] * 127 + 127) / 255;
                if (format ==
                    NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8) {
                    dst[x * 4 + 3] |= 0x80;
                }
                break;
            default:
                memcpy(dst + x * 4, src, 4);
                break;
            }
        }
    }
}

static void d3d11_surface_dimensions(PGRAPHState *pg, uint32_t *guest_width,
                                     uint32_t *guest_height, uint32_t *width,
                                     uint32_t *height)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        *guest_width = 1U << pg->surface_shape.log_width;
        *guest_height = 1U << pg->surface_shape.log_height;
    } else {
        *guest_width = pg->surface_shape.clip_width + pg->surface_shape.clip_x;
        *guest_height =
            pg->surface_shape.clip_height + pg->surface_shape.clip_y;
    }
    *guest_width = MAX(*guest_width, 1U);
    *guest_height = MAX(*guest_height, 1U);
    pgraph_apply_anti_aliasing_factor(pg, guest_width, guest_height);
    *width = *guest_width;
    *height = *guest_height;
    pgraph_apply_scaling_factor(pg, width, height);
}

static bool d3d11_target_address(NV2AState *d, bool color, uint32_t width,
                                 uint32_t height, unsigned int bpp,
                                 hwaddr *address, size_t *storage_length,
                                 Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    DMAObject dma = nv_dma_load(d, color ? pg->dma_color : pg->dma_zeta);
    uint64_t row_bytes = (uint64_t)width * bpp;
    uint64_t length =
        (uint64_t)height * MAX((uint64_t)surface->pitch, row_bytes);
    uint64_t end = (uint64_t)surface->offset + length;
    uint64_t vram_size = memory_region_size(d->vram);

    /*
     * Retail software can program render-target DMA objects whose limit does
     * not cover the complete multisampled surface (and can temporarily leave
     * the zeta limit at zero).  The established PGRAPH renderers treat that
     * field as advisory in release builds and validate the resolved VRAM
     * interval instead.  Rejecting it here prevents valid boot surfaces from
     * ever being bound.
     */
    if (dma.dma_class != NV_DMA_IN_MEMORY_CLASS || dma.address > vram_size ||
        end > vram_size - dma.address) {
        error_setg(errp,
                   "D3D11: invalid %s render-target DMA range "
                   "(class=0x%x base=0x%" HWADDR_PRIx " limit=0x%" HWADDR_PRIx
                   " offset=0x%" HWADDR_PRIx " pitch=%u "
                   "size=%ux%u bpp=%u length=0x%" PRIx64 " vram=0x%" PRIx64 ")",
                   color ? "color" : "depth/stencil", dma.dma_class,
                   dma.address, dma.limit, surface->offset, surface->pitch,
                   width, height, bpp, length, vram_size);
        return false;
    }
    *address = dma.address + surface->offset;
    *storage_length = length;
    return true;
}

static void d3d11_upload_color(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *target = &r->render_targets.color;
    if (!target->texture) {
        return;
    }
    unsigned int bpp = d3d11_color_bytes_per_pixel(target->nv2a_format);
    size_t guest_pitch = (size_t)target->guest_width * bpp;
    uint8_t *linear = g_malloc(guest_pitch * target->guest_height);
    const uint8_t *vram = d->vram_ptr + target->vram_address;
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        unswizzle_rect(vram, target->guest_width, target->guest_height, linear,
                       guest_pitch, bpp);
    } else {
        for (uint32_t y = 0; y < target->guest_height; y++) {
            memcpy(linear + (size_t)y * guest_pitch,
                   vram + (size_t)y * target->pitch, guest_pitch);
        }
    }

    size_t source_pitch = (size_t)target->guest_width * 4;
    uint8_t *source = g_malloc(source_pitch * target->guest_height);
    d3d11_convert_color_to_bgra8(target->nv2a_format, source, source_pitch,
                                 linear, guest_pitch, target->guest_width,
                                 target->guest_height);
    g_free(linear);

    size_t host_pitch = (size_t)target->width * 4;
    uint8_t *host = g_malloc(host_pitch * target->height);
    for (uint32_t y = 0; y < target->height; y++) {
        uint32_t sy = (uint64_t)y * target->guest_height / target->height;
        for (uint32_t x = 0; x < target->width; x++) {
            uint32_t sx = (uint64_t)x * target->guest_width / target->width;
            memcpy(host + (size_t)y * host_pitch + x * 4,
                   source + (size_t)sy * source_pitch + sx * 4, 4);
        }
    }
    g_free(source);
    ID3D11DeviceContext_UpdateSubresource(r->context,
                                          (ID3D11Resource *)target->texture, 0,
                                          NULL, host, host_pitch, 0);
    g_free(host);
    pg->surface_color.buffer_dirty = false;
}

static void d3d11_upload_depth_stencil(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *target = &r->render_targets.depth_stencil;
    if (!target->texture) {
        return;
    }
    unsigned int guest_bpp =
        target->nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4;
    size_t guest_pitch = (size_t)target->guest_width * guest_bpp;
    uint8_t *linear = g_malloc(guest_pitch * target->guest_height);
    const uint8_t *vram = d->vram_ptr + target->vram_address;
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        unswizzle_rect(vram, target->guest_width, target->guest_height, linear,
                       guest_pitch, guest_bpp);
    } else {
        for (uint32_t y = 0; y < target->guest_height; y++) {
            memcpy(linear + (size_t)y * guest_pitch,
                   vram + (size_t)y * target->pitch, guest_pitch);
        }
    }

    size_t host_pitch = (size_t)target->width * 4;
    uint8_t *host = g_malloc(host_pitch * target->height);
    for (uint32_t y = 0; y < target->height; y++) {
        uint32_t sy = (uint64_t)y * target->guest_height / target->height;
        for (uint32_t x = 0; x < target->width; x++) {
            uint32_t sx = (uint64_t)x * target->guest_width / target->width;
            if (guest_bpp == 2) {
                uint16_t packed;
                memcpy(&packed, linear + (size_t)sy * guest_pitch + sx * 2,
                       sizeof(packed));
                float depth = target->z_format ?
                                  convert_f16_to_float(packed) / f16_max :
                                  packed / 65535.0f;
                memcpy(host + (size_t)y * host_pitch + x * 4, &depth,
                       sizeof(depth));
            } else {
                uint32_t packed;
                memcpy(&packed, linear + (size_t)sy * guest_pitch + sx * 4,
                       sizeof(packed));
                uint32_t guest_depth = packed >> 8;
                float depth = target->z_format ?
                                  convert_f24_to_float(guest_depth) / f24_max :
                                  guest_depth / 16777215.0f;
                uint32_t native_depth =
                    CLAMP(depth, 0.0f, 1.0f) * 16777215.0f + 0.5f;
                uint32_t native = native_depth | ((packed & 0xff) << 24);
                memcpy(host + (size_t)y * host_pitch + x * 4, &native,
                       sizeof(native));
            }
        }
    }
    g_free(linear);
    ID3D11DeviceContext_UpdateSubresource(r->context,
                                          (ID3D11Resource *)target->texture, 0,
                                          NULL, host, host_pitch, 0);
    g_free(host);
    pg->surface_zeta.buffer_dirty = false;
}

static void d3d11_store_color(PGRAPHState *pg, PGRAPHD3D11RenderTarget *target,
                              uint8_t *vram, const uint8_t *linear,
                              unsigned int bpp)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        swizzle_rect(linear, target->guest_width, target->guest_height, vram,
                     (size_t)target->guest_width * bpp, bpp);
        return;
    }
    for (uint32_t y = 0; y < target->guest_height; y++) {
        memcpy(vram + (size_t)y * target->pitch,
               linear + (size_t)y * target->guest_width * bpp,
               (size_t)target->guest_width * bpp);
    }
}

static void d3d11_store_depth_stencil(PGRAPHState *pg,
                                      PGRAPHD3D11RenderTarget *target,
                                      uint8_t *vram, const uint8_t *linear,
                                      unsigned int bpp)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        swizzle_rect(linear, target->guest_width, target->guest_height, vram,
                     (size_t)target->guest_width * bpp, bpp);
        return;
    }
    for (uint32_t y = 0; y < target->guest_height; y++) {
        memcpy(vram + (size_t)y * target->pitch,
               linear + (size_t)y * target->guest_width * bpp,
               (size_t)target->guest_width * bpp);
    }
}

static bool d3d11_flush_color(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *target = &r->render_targets.color;
    if (!target->texture || !pg->surface_color.draw_dirty) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(target->texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &staging);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: color surface readback allocation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    ID3D11DeviceContext_CopyResource(r->context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)target->texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(r->context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(staging);
        error_setg(errp,
                   "D3D11: color surface readback map failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }

    size_t bgra_pitch = (size_t)target->guest_width * 4;
    uint8_t *bgra = g_malloc(bgra_pitch * target->guest_height);
    for (uint32_t y = 0; y < target->guest_height; y++) {
        uint32_t sy = (uint64_t)y * target->height / target->guest_height;
        for (uint32_t x = 0; x < target->guest_width; x++) {
            uint32_t sx = (uint64_t)x * target->width / target->guest_width;
            memcpy(bgra + (size_t)y * bgra_pitch + x * 4,
                   (uint8_t *)mapped.pData + (size_t)sy * mapped.RowPitch +
                       sx * 4,
                   4);
        }
    }
    ID3D11DeviceContext_Unmap(r->context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);

    unsigned int bpp = d3d11_color_bytes_per_pixel(target->nv2a_format);
    size_t linear_pitch = (size_t)target->guest_width * bpp;
    uint8_t *linear = g_malloc(linear_pitch * target->guest_height);
    d3d11_convert_bgra8_to_color(target->nv2a_format, linear, linear_pitch,
                                 bgra, bgra_pitch, target->guest_width,
                                 target->guest_height);
    g_free(bgra);
    d3d11_store_color(pg, target, d->vram_ptr + target->vram_address, linear,
                      bpp);
    g_free(linear);
    memory_region_set_client_dirty(d->vram, target->vram_address,
                                   target->storage_length, DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, target->vram_address,
                                   target->storage_length,
                                   DIRTY_MEMORY_NV2A_TEX);
    pg->surface_color.draw_dirty = false;
    return true;
}

static bool d3d11_flush_depth_stencil(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *target = &r->render_targets.depth_stencil;
    if (!target->texture || !pg->surface_zeta.draw_dirty) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(target->texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &staging);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: depth/stencil readback allocation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    ID3D11DeviceContext_CopyResource(r->context, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)target->texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(r->context, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(staging);
        error_setg(errp,
                   "D3D11: depth/stencil readback map failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }

    unsigned int bpp =
        target->nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4;
    size_t linear_pitch = (size_t)target->guest_width * bpp;
    uint8_t *linear = g_malloc(linear_pitch * target->guest_height);
    for (uint32_t y = 0; y < target->guest_height; y++) {
        uint32_t sy = (uint64_t)y * target->height / target->guest_height;
        for (uint32_t x = 0; x < target->guest_width; x++) {
            uint32_t sx = (uint64_t)x * target->width / target->guest_width;
            const uint8_t *source =
                (uint8_t *)mapped.pData + (size_t)sy * mapped.RowPitch + sx * 4;
            if (bpp == 2) {
                float depth;
                memcpy(&depth, source, sizeof(depth));
                depth = CLAMP(depth, 0.0f, 1.0f);
                uint16_t packed = target->z_format ?
                                      d3d11_float_to_f16(depth * f16_max) :
                                      depth * 65535.0f + 0.5f;
                memcpy(linear + (size_t)y * linear_pitch + x * 2, &packed,
                       sizeof(packed));
            } else {
                uint32_t native;
                memcpy(&native, source, sizeof(native));
                uint32_t native_depth = native & 0x00ffffffU;
                uint32_t guest_depth =
                    target->z_format ?
                        d3d11_float_to_f24(native_depth / 16777215.0f *
                                           f24_max) :
                        native_depth;
                uint32_t packed = (guest_depth << 8) | (native >> 24);
                memcpy(linear + (size_t)y * linear_pitch + x * 4, &packed,
                       sizeof(packed));
            }
        }
    }
    ID3D11DeviceContext_Unmap(r->context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
    d3d11_store_depth_stencil(pg, target, d->vram_ptr + target->vram_address,
                              linear, bpp);
    g_free(linear);
    memory_region_set_client_dirty(d->vram, target->vram_address,
                                   target->storage_length, DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, target->vram_address,
                                   target->storage_length,
                                   DIRTY_MEMORY_NV2A_TEX);
    pg->surface_zeta.draw_dirty = false;
    return true;
}

bool pgraph_d3d11_surface_flush(NV2AState *d, Error **errp)
{
    if (!d3d11_flush_color(d, errp)) {
        return false;
    }
    return d3d11_flush_depth_stencil(d, errp);
}

void pgraph_d3d11_surface_invalidate_range(NV2AState *d, hwaddr address,
                                           size_t length)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *targets[] = {
        &r->render_targets.color,
        &r->render_targets.depth_stencil,
    };
    Surface *surfaces[] = {
        &pg->surface_color,
        &pg->surface_zeta,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
        if (targets[i]->texture &&
            d3d11_ranges_overlap(address, length, targets[i]->vram_address,
                                 targets[i]->storage_length)) {
            surfaces[i]->buffer_dirty = true;
            surfaces[i]->draw_dirty = false;
        }
    }
}

static bool d3d11_create_target(NV2AState *d, bool color, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTarget *target =
        color ? &r->render_targets.color : &r->render_targets.depth_stencil;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    uint32_t guest_width, guest_height, width, height;
    d3d11_surface_dimensions(pg, &guest_width, &guest_height, &width, &height);
    unsigned int nv2a_format =
        color ? pg->surface_shape.color_format : pg->surface_shape.zeta_format;
    unsigned int bpp =
        color ? d3d11_color_bytes_per_pixel(nv2a_format) :
                (nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4);
    hwaddr address;
    size_t storage_length;
    if (!d3d11_target_address(d, color, guest_width, guest_height, bpp,
                              &address, &storage_length, errp)) {
        return false;
    }
    DXGI_FORMAT format = color ?
                             DXGI_FORMAT_B8G8R8A8_UNORM :
                             (nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ?
                                  DXGI_FORMAT_D32_FLOAT :
                                  DXGI_FORMAT_D24_UNORM_S8_UINT);
    surface->buffer_dirty |= memory_region_test_and_clear_dirty(
        d->vram, address, storage_length, DIRTY_MEMORY_NV2A);

    if (target->texture && target->vram_address == address &&
        target->width == width && target->height == height &&
        target->guest_width == guest_width &&
        target->guest_height == guest_height &&
        target->pitch == surface->pitch && target->format == format &&
        target->nv2a_format == nv2a_format &&
        (color || target->z_format == pg->surface_shape.z_format)) {
        if (surface->buffer_dirty) {
            if (color) {
                d3d11_upload_color(d);
            } else {
                d3d11_upload_depth_stencil(d);
            }
        }
        return true;
    }

    ID3D11DeviceContext_OMSetRenderTargets(r->context, 0, NULL, NULL);
    if (color && target->texture &&
        !pgraph_d3d11_retain_color_scanout(d, errp)) {
        return false;
    }
    d3d11_release_target(target);
    D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = format,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = color ?
                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE :
                         D3D11_BIND_DEPTH_STENCIL,
    };
    HRESULT hr =
        ID3D11Device_CreateTexture2D(r->device, &desc, NULL, &target->texture);
    if (SUCCEEDED(hr) && color) {
        hr = ID3D11Device_CreateRenderTargetView(
            r->device, (ID3D11Resource *)target->texture, NULL, &target->rtv);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateShaderResourceView(
                r->device, (ID3D11Resource *)target->texture, NULL,
                &target->srv);
        }
    } else if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateDepthStencilView(
            r->device, (ID3D11Resource *)target->texture, NULL, &target->dsv);
    }
    if (FAILED(hr)) {
        d3d11_release_target(target);
        error_setg(errp,
                   "D3D11: failed to create %s render target "
                   "(HRESULT 0x%08lx)",
                   color ? "color" : "depth/stencil", (unsigned long)hr);
        return false;
    }
    target->vram_address = address;
    target->width = width;
    target->height = height;
    target->guest_width = guest_width;
    target->guest_height = guest_height;
    target->pitch = surface->pitch;
    target->storage_length = storage_length;
    target->format = format;
    target->nv2a_format = nv2a_format;
    target->z_format = pg->surface_shape.z_format;
    if (color) {
        d3d11_upload_color(d);
    } else {
        d3d11_upload_depth_stencil(d);
    }
    if (tcg_enabled()) {
        target->access_cb = mem_access_callback_insert(
            qemu_get_cpu(0), d->vram, target->vram_address,
            target->storage_length, &d3d11_surface_access_callback, d);
    }
    return true;
}

bool pgraph_d3d11_render_targets_update(NV2AState *d, bool color, bool zeta,
                                        Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);
    if (color && pg->surface_shape.color_format &&
        !d3d11_create_target(d, true, errp)) {
        return false;
    }
    if (zeta && pg->surface_shape.zeta_format &&
        !d3d11_create_target(d, false, errp)) {
        return false;
    }
    pg->draw_time++;
    return true;
}

static bool d3d11_update_blend_state(PGRAPHD3D11State *r,
                                     const D3D11_BLEND_DESC1 *desc,
                                     Error **errp)
{
    PGRAPHD3D11RenderTargets *targets = &r->render_targets;
    if (targets->blend_state_valid &&
        memcmp(&targets->blend_desc, desc, sizeof(*desc)) == 0) {
        return true;
    }

    ID3D11BlendState *state = NULL;
    HRESULT hr;
    if (desc->RenderTarget[0].LogicOpEnable) {
        if (!r->device1 || !r->output_merger_logic_op) {
            error_setg(errp, "D3D11: the active adapter does not support "
                             "output-merger logic operations");
            return false;
        }
        ID3D11BlendState1 *state1 = NULL;
        hr = ID3D11Device1_CreateBlendState1(r->device1, desc, &state1);
        state = (ID3D11BlendState *)state1;
    } else {
        D3D11_BLEND_DESC base = {
            .AlphaToCoverageEnable = desc->AlphaToCoverageEnable,
            .IndependentBlendEnable = desc->IndependentBlendEnable,
        };
        for (unsigned int i = 0; i < ARRAY_SIZE(base.RenderTarget); i++) {
            base.RenderTarget[i].BlendEnable =
                desc->RenderTarget[i].BlendEnable;
            base.RenderTarget[i].SrcBlend = desc->RenderTarget[i].SrcBlend;
            base.RenderTarget[i].DestBlend = desc->RenderTarget[i].DestBlend;
            base.RenderTarget[i].BlendOp = desc->RenderTarget[i].BlendOp;
            base.RenderTarget[i].SrcBlendAlpha =
                desc->RenderTarget[i].SrcBlendAlpha;
            base.RenderTarget[i].DestBlendAlpha =
                desc->RenderTarget[i].DestBlendAlpha;
            base.RenderTarget[i].BlendOpAlpha =
                desc->RenderTarget[i].BlendOpAlpha;
            base.RenderTarget[i].RenderTargetWriteMask =
                desc->RenderTarget[i].RenderTargetWriteMask;
        }
        hr = ID3D11Device_CreateBlendState(r->device, &base, &state);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: blend-state creation failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    if (targets->blend_state) {
        ID3D11BlendState_Release(targets->blend_state);
    }
    targets->blend_state = state;
    targets->blend_desc = *desc;
    targets->blend_state_valid = true;
    return true;
}

static bool d3d11_update_depth_stencil_state(
    PGRAPHD3D11State *r, const D3D11_DEPTH_STENCIL_DESC *desc, Error **errp)
{
    PGRAPHD3D11RenderTargets *targets = &r->render_targets;
    if (targets->depth_stencil_state_valid &&
        memcmp(&targets->depth_stencil_desc, desc, sizeof(*desc)) == 0) {
        return true;
    }
    ID3D11DepthStencilState *state = NULL;
    HRESULT hr = ID3D11Device_CreateDepthStencilState(r->device, desc, &state);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D11: depth/stencil-state creation failed "
                   "(HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    if (targets->depth_stencil_state) {
        ID3D11DepthStencilState_Release(targets->depth_stencil_state);
    }
    targets->depth_stencil_state = state;
    targets->depth_stencil_desc = *desc;
    targets->depth_stencil_state_valid = true;
    return true;
}

bool pgraph_d3d11_update_output_merger(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    PGRAPHD3D11RenderTargets *targets = &r->render_targets;
    uint32_t control0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);

    D3D11_BLEND_DESC1 blend_desc = { 0 };
    D3D11_RENDER_TARGET_BLEND_DESC1 *rt = &blend_desc.RenderTarget[0];
    rt->LogicOpEnable = !!(blend & NV_PGRAPH_BLEND_LOGICOP_ENABLE);
    rt->BlendEnable = !rt->LogicOpEnable && !!(blend & NV_PGRAPH_BLEND_EN);
    rt->SrcBlend = d3d11_blend_factor(GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR));
    rt->DestBlend =
        d3d11_blend_factor(GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR));
    rt->BlendOp = d3d11_blend_op(GET_MASK(blend, NV_PGRAPH_BLEND_EQN));
    rt->SrcBlendAlpha = d3d11_alpha_blend_factor(rt->SrcBlend);
    rt->DestBlendAlpha = d3d11_alpha_blend_factor(rt->DestBlend);
    rt->BlendOpAlpha = rt->BlendOp;
    rt->LogicOp = d3d11_logic_op(GET_MASK(blend, NV_PGRAPH_BLEND_LOGICOP));
    rt->RenderTargetWriteMask =
        ((control0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ?
             D3D11_COLOR_WRITE_ENABLE_RED :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ?
             D3D11_COLOR_WRITE_ENABLE_GREEN :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE) ?
             D3D11_COLOR_WRITE_ENABLE_BLUE :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ?
             D3D11_COLOR_WRITE_ENABLE_ALPHA :
             0);
    switch (pg->surface_shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        break;
    default:
        rt->RenderTargetWriteMask &= ~D3D11_COLOR_WRITE_ENABLE_ALPHA;
        break;
    }
    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                     targets->blend_factor);

    D3D11_DEPTH_STENCIL_DESC depth_desc = {
        .DepthEnable = !!(control0 & NV_PGRAPH_CONTROL_0_ZENABLE),
        .DepthWriteMask = control0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE ?
                              D3D11_DEPTH_WRITE_MASK_ALL :
                              D3D11_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc =
            d3d11_comparison(GET_MASK(control0, NV_PGRAPH_CONTROL_0_ZFUNC)),
        .StencilEnable = !!(control1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE),
        .StencilReadMask =
            GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ),
        .StencilWriteMask =
            GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE),
    };
    targets->stencil_ref = GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
    D3D11_DEPTH_STENCILOP_DESC stencil = {
        .StencilFailOp = d3d11_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL)),
        .StencilDepthFailOp = d3d11_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL)),
        .StencilPassOp = d3d11_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS)),
        .StencilFunc = d3d11_comparison(
            GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC)),
    };
    depth_desc.FrontFace = stencil;
    depth_desc.BackFace = stencil;

    if (!d3d11_update_blend_state(r, &blend_desc, errp) ||
        !d3d11_update_depth_stencil_state(r, &depth_desc, errp) ||
        !pgraph_d3d11_render_targets_update(
            d, rt->RenderTargetWriteMask != 0,
            depth_desc.DepthEnable || depth_desc.StencilEnable, errp)) {
        return false;
    }
    ID3D11DeviceContext_OMSetBlendState(r->context, targets->blend_state,
                                        targets->blend_factor, UINT_MAX);
    ID3D11DeviceContext_OMSetDepthStencilState(
        r->context, targets->depth_stencil_state, targets->stencil_ref);
    return true;
}

void pgraph_d3d11_bind_render_targets(PGRAPHD3D11State *r)
{
    ID3D11RenderTargetView *rtv = r->render_targets.color.rtv;
    ID3D11DeviceContext_OMSetRenderTargets(r->context, rtv ? 1 : 0,
                                           rtv ? &rtv : NULL,
                                           r->render_targets.depth_stencil.dsv);
    PGRAPHD3D11RenderTarget *target =
        rtv ? &r->render_targets.color : &r->render_targets.depth_stencil;
    if (target->texture) {
        D3D11_VIEWPORT viewport = {
            .Width = target->width,
            .Height = target->height,
            .MaxDepth = 1.0f,
        };
        ID3D11DeviceContext_RSSetViewports(r->context, 1, &viewport);
    }
}

bool pgraph_d3d11_render_targets_clear(NV2AState *d, uint32_t parameter,
                                       Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    bool color = parameter & NV097_CLEAR_SURFACE_COLOR;
    bool zeta =
        parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
    pg->clearing = true;
    if (!pgraph_d3d11_render_targets_update(d, color, zeta, errp)) {
        pg->clearing = false;
        return false;
    }
    if (color && r->render_targets.color.rtv) {
        float clear_color[4];
        pgraph_get_clear_color(pg, clear_color);
        unsigned int xmin = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX),
                                     NV_PGRAPH_CLEARRECTX_XMIN);
        unsigned int xmax = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX),
                                     NV_PGRAPH_CLEARRECTX_XMAX);
        unsigned int ymin = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY),
                                     NV_PGRAPH_CLEARRECTY_YMIN);
        unsigned int ymax = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY),
                                     NV_PGRAPH_CLEARRECTY_YMAX);
        unsigned int clear_width = xmax >= xmin ? xmax - xmin + 1 : 0;
        unsigned int clear_height = ymax >= ymin ? ymax - ymin + 1 : 0;
        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &clear_width, &clear_height);
        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &clear_width, &clear_height);

        PGRAPHD3D11RenderTarget *target = &r->render_targets.color;
        D3D11_RECT rect = {
            .left = MIN(xmin, target->width),
            .top = MIN(ymin, target->height),
            .right = MIN((uint64_t)xmin + clear_width, target->width),
            .bottom = MIN((uint64_t)ymin + clear_height, target->height),
        };
        bool full_clear = rect.left == 0 && rect.top == 0 &&
                          rect.right >= target->width &&
                          rect.bottom >= target->height;
        ID3D11DeviceContext1 *context1 = NULL;
        if (!full_clear && rect.right > rect.left && rect.bottom > rect.top &&
            SUCCEEDED(ID3D11DeviceContext_QueryInterface(
                r->context, &IID_ID3D11DeviceContext1, (void **)&context1))) {
            ID3D11DeviceContext1_ClearView(context1, (ID3D11View *)target->rtv,
                                           clear_color, &rect, 1);
            ID3D11DeviceContext1_Release(context1);
        } else if (full_clear) {
            ID3D11DeviceContext_ClearRenderTargetView(r->context, target->rtv,
                                                      clear_color);
        }
        pg->surface_color.draw_dirty = true;
    }
    if (zeta && r->render_targets.depth_stencil.dsv) {
        UINT flags = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            flags |= D3D11_CLEAR_DEPTH;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            r->render_targets.depth_stencil.format != DXGI_FORMAT_D32_FLOAT) {
            flags |= D3D11_CLEAR_STENCIL;
        }
        float depth;
        int stencil;
        pgraph_get_clear_depth_stencil_value(pg, &depth, &stencil);
        if (flags) {
            ID3D11DeviceContext_ClearDepthStencilView(
                r->context, r->render_targets.depth_stencil.dsv, flags, depth,
                stencil);
            pg->surface_zeta.draw_dirty = true;
        }
    }
    pg->clearing = false;
    return true;
}
