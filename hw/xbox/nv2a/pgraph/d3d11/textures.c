/*
 * GeForce NV2A PGRAPH Direct3D 11 textures
 *
 * Texture addressing and conversion follow xemu's common NV2A texture state
 * and swizzle helpers. Modified for the D3D11 renderer on 2026-09-26.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qapi/error.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "renderer.h"

static void d3d11_release_texture(PGRAPHD3D11TextureBinding *binding)
{
    if (binding->view) {
        ID3D11ShaderResourceView_Release(binding->view);
    }
    if (binding->resource) {
        ID3D11Resource_Release(binding->resource);
    }
    memset(binding, 0, sizeof(*binding));
}

bool pgraph_d3d11_textures_init(PGRAPHD3D11State *r, Error **errp)
{
    (void)errp;
    r->textures = g_new0(PGRAPHD3D11TextureState, 1);
    return true;
}

void pgraph_d3d11_textures_finalize(PGRAPHD3D11State *r)
{
    if (!r->textures) {
        return;
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        d3d11_release_texture(&r->textures->bindings[i]);
        if (r->textures->samplers[i]) {
            ID3D11SamplerState_Release(r->textures->samplers[i]);
        }
    }
    g_free(r->textures);
    r->textures = NULL;
}

void pgraph_d3d11_textures_trim(PGRAPHD3D11State *r)
{
    if (!r->textures) {
        return;
    }
    ID3D11ShaderResourceView *views[NV2A_MAX_TEXTURES] = { 0 };
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0, NV2A_MAX_TEXTURES,
                                             views);
    ID3D11DeviceContext_PSSetSamplers(r->context, 0, NV2A_MAX_TEXTURES,
                                      samplers);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        d3d11_release_texture(&r->textures->bindings[i]);
        if (r->textures->samplers[i]) {
            ID3D11SamplerState_Release(r->textures->samplers[i]);
            r->textures->samplers[i] = NULL;
        }
        r->textures->sampler_valid[i] = false;
    }
}

static uint8_t expand_bits(uint32_t value, unsigned int bits)
{
    return value * 255 / ((1u << bits) - 1);
}

static void d3d11_decode_pixel(unsigned int format, const uint8_t *src,
                               uint8_t dst[4])
{
    uint16_t p16;
    uint32_t p32;
    dst[0] = dst[1] = dst[2] = 0;
    dst[3] = 255;
    switch (format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        dst[0] = dst[1] = dst[2] = src[0];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        dst[0] = dst[1] = dst[2] = dst[3] = src[0];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        dst[0] = dst[1] = dst[2] = 255;
        dst[3] = src[0];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        dst[0] = dst[1] = dst[2] = src[0];
        dst[3] = src[1];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        dst[0] = dst[2] = src[0];
        dst[1] = dst[3] = src[1];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        dst[0] = dst[3] = src[1];
        dst[1] = dst[2] = src[0];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        memcpy(&p16, src, 2);
        dst[0] = expand_bits((p16 >> 10) & 31, 5);
        dst[1] = expand_bits((p16 >> 5) & 31, 5);
        dst[2] = expand_bits(p16 & 31, 5);
        dst[3] = (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5 ||
                  format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5) ?
                     255 :
                     ((p16 & 0x8000) ? 255 : 0);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        memcpy(&p16, src, 2);
        dst[0] = expand_bits((p16 >> 11) & 31, 5);
        dst[1] = expand_bits((p16 >> 5) & 63, 6);
        dst[2] = expand_bits(p16 & 31, 5);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        memcpy(&p16, src, 2);
        dst[0] = expand_bits((p16 >> 8) & 15, 4);
        dst[1] = expand_bits((p16 >> 4) & 15, 4);
        dst[2] = expand_bits(p16 & 15, 4);
        dst[3] = expand_bits((p16 >> 12) & 15, 4);
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        memcpy(&p32, src, 4);
        dst[0] = p32 >> 16;
        dst[1] = p32 >> 8;
        dst[2] = p32;
        dst[3] = (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8 ||
                  format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8) ?
                     255 :
                     p32 >> 24;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        memcpy(&p32, src, 4);
        dst[0] = p32;
        dst[1] = p32 >> 8;
        dst[2] = p32 >> 16;
        dst[3] = p32 >> 24;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        dst[0] = src[3];
        dst[1] = src[2];
        dst[2] = src[1];
        dst[3] = src[0];
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        memcpy(dst, src, 4);
        break;
    default:
        /* The format validator rejects unmapped formats before conversion. */
        break;
    }
}

static bool d3d11_is_depth_texture_format(unsigned int format)
{
    switch (format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        return true;
    default:
        return false;
    }
}

static unsigned int d3d11_texture_pixel_bytes(unsigned int format)
{
    if (d3d11_is_depth_texture_format(format)) {
        return 4;
    }
    return format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16 ? 8 : 4;
}

static bool d3d11_vram_ranges_overlap(hwaddr a, size_t a_length, hwaddr b,
                                     size_t b_length)
{
    if (!a_length || !b_length) {
        return false;
    }
    return a <= b ? b - a < a_length : a - b < b_length;
}

static bool d3d11_dma_range(NV2AState *d, hwaddr dma_object, hwaddr offset,
                            hwaddr *address, size_t *available)
{
    hwaddr dma_length = 0;
    uint8_t *mapping = NULL;
    if (!nv_dma_map_checked(d, dma_object, &mapping, &dma_length)) {
        return false;
    }
    uint64_t vram_size = memory_region_size(d->vram);
    if (offset >= dma_length) {
        return false;
    }

    uintptr_t mapping_address = (uintptr_t)mapping;
    if (offset > UINTPTR_MAX - mapping_address) {
        return false;
    }
    uintptr_t start = mapping_address + (uintptr_t)offset;
    uintptr_t vram_start = (uintptr_t)d->vram_ptr;
    if (start < vram_start || start - vram_start >= vram_size) {
        return false;
    }

    uint64_t relative = start - vram_start;
    uint64_t dma_available = dma_length - offset;
    uint64_t vram_available = vram_size - relative;
    uint64_t range_available = MIN(dma_available, vram_available);
    *address = relative;
    *available = MIN(range_available, (uint64_t)SIZE_MAX);
    return true;
}

static bool d3d11_texture_dma_range(NV2AState *d, unsigned int stage,
                                    hwaddr *address, size_t *available)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t format = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4);
    bool dma_b = GET_MASK(format, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);
    hwaddr dma_object = dma_b ? pg->dma_b : pg->dma_a;
    hwaddr offset = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + stage * 4);
    return d3d11_dma_range(d, dma_object, offset, address, available);
}

static bool d3d11_palette_dma_range(NV2AState *d, unsigned int stage,
                                    hwaddr *address, size_t *length)
{
    static const size_t palette_entries[] = { 256, 128, 64, 32 };
    PGRAPHState *pg = &d->pgraph;
    uint32_t palette = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + stage * 4);
    unsigned int length_index = GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_LENGTH);
    if (length_index >= ARRAY_SIZE(palette_entries)) {
        return false;
    }

    bool dma_b = GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA);
    hwaddr dma_object = dma_b ? pg->dma_b : pg->dma_a;
    hwaddr offset = palette & NV_PGRAPH_TEXPALETTE0_OFFSET;
    size_t palette_size = palette_entries[length_index] * sizeof(uint32_t);
    size_t available = 0;
    if (!d3d11_dma_range(d, dma_object, offset, address, &available) ||
        palette_size > available) {
        return false;
    }
    *length = palette_size;
    return true;
}

static bool d3d11_texture_data_overlaps_target(
    hwaddr address, size_t length, bool uses_palette, hwaddr palette_address,
    size_t palette_length, hwaddr target_address, size_t target_length)
{
    return d3d11_vram_ranges_overlap(address, length, target_address,
                                     target_length) ||
           (uses_palette &&
            d3d11_vram_ranges_overlap(palette_address, palette_length,
                                     target_address, target_length));
}

static DXGI_FORMAT d3d11_texture_dxgi_format(unsigned int format)
{
    if (d3d11_is_depth_texture_format(format)) {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    }
    return format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16 ?
               DXGI_FORMAT_R16G16B16A16_UNORM :
               DXGI_FORMAT_R8G8B8A8_UNORM;
}

static bool d3d11_texture_format_supported(unsigned int format)
{
    if (format >= ARRAY_SIZE(kelvin_color_format_info_map)) {
        return false;
    }
    BasicColorFormatInfo info = kelvin_color_format_info_map[format];
    return info.bytes_per_pixel != 0 &&
           (!info.depth || d3d11_is_depth_texture_format(format));
}

bool pgraph_d3d11_validate_texture_registers(PGRAPHState *pg, Error **errp)
{
    unsigned int combiner_stages =
        pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xff;
    if (combiner_stages > 8) {
        error_setg(errp,
                   "D3D11: invalid pixel combiner stage count %u (maximum 8)",
                   combiner_stages);
        return false;
    }

    for (unsigned int stage = 0; stage < NV2A_MAX_TEXTURES; stage++) {
        if (!pgraph_is_texture_enabled(pg, stage)) {
            continue;
        }

        uint32_t texture_format =
            pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + stage * 4);
        unsigned int color_format =
            GET_MASK(texture_format, NV_PGRAPH_TEXFMT0_COLOR);
        if (!d3d11_texture_format_supported(color_format)) {
            error_setg(errp, "D3D11: texture %u has unsupported format 0x%x",
                       stage, color_format);
            return false;
        }

        BasicColorFormatInfo format =
            kelvin_color_format_info_map[color_format];
        unsigned int dimensionality =
            GET_MASK(texture_format, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
        if (dimensionality < 1 || dimensionality > 3) {
            error_setg(errp, "D3D11: texture %u has invalid dimensionality %u",
                       stage, dimensionality);
            return false;
        }
        if (format.linear && dimensionality != 2) {
            error_setg(errp, "D3D11: linear texture %u must be 2D", stage);
            return false;
        }
        if (!format.linear &&
            !GET_MASK(texture_format, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS)) {
            error_setg(errp, "D3D11: texture %u has no mip levels", stage);
            return false;
        }

        uint32_t texture_filter =
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4);
        if (GET_MASK(texture_filter, NV_PGRAPH_TEXFILTER0_MIN) ==
            NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0) {
            bool cubemap =
                GET_MASK(texture_format, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
            if (dimensionality != 2 || cubemap) {
                error_setg(errp,
                           "D3D11: convolution filter requires a non-cubemap 2D texture at stage %u",
                           stage);
                return false;
            }
            unsigned int kernel =
                GET_MASK(texture_filter,
                         NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL);
            if (kernel != NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_QUINCUNX &&
                kernel != NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL_GAUSSIAN_3) {
                error_setg(errp,
                           "D3D11: texture %u has invalid convolution kernel %u",
                           stage, kernel);
                return false;
            }
        }
    }
    return true;
}

static float d3d11_half_to_float(uint16_t value)
{
    uint32_t sign = (uint32_t)(value & 0x8000) << 16;
    uint32_t exponent = (value >> 10) & 0x1f;
    uint32_t mantissa = value & 0x03ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int32_t unbiased = -14;
            while (!(mantissa & 0x0400)) {
                mantissa <<= 1;
                unbiased--;
            }
            mantissa &= 0x03ff;
            bits = sign | ((uint32_t)(unbiased + 127) << 23) |
                   (mantissa << 13);
        }
    } else if (exponent == 0x1f) {
        bits = sign | 0x7f800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static uint8_t *d3d11_convert_level(TextureShape shape, const uint8_t *source,
                                    const uint8_t *palette, unsigned int width,
                                    unsigned int height, unsigned int depth,
                                    unsigned int *source_size)
{
    unsigned int format = shape.color_format;
    if (format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ||
        format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
        format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
        unsigned int block =
            format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 : 16;
        *source_size =
            ROUND_UP(width, 4) / 4 * (ROUND_UP(height, 4) / 4) * depth * block;
        enum S3TC_DECOMPRESS_FORMAT decompress =
            format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                S3TC_DECOMPRESS_FORMAT_DXT1 :
            format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ?
                S3TC_DECOMPRESS_FORMAT_DXT3 :
                S3TC_DECOMPRESS_FORMAT_DXT5;
        return s3tc_decompress_3d(decompress, source, width, height, depth);
    }
    BasicColorFormatInfo info = kelvin_color_format_info_map[format];
    unsigned int row_pitch =
        info.linear ? shape.pitch : width * info.bytes_per_pixel;
    unsigned int slice_pitch = row_pitch * height;
    *source_size = slice_pitch * depth;
    uint8_t *linear = (uint8_t *)source;
    if (!info.linear) {
        linear = g_malloc(width * height * depth * info.bytes_per_pixel);
        unswizzle_box(
            source, width, height, depth, linear, width * info.bytes_per_pixel,
            width * height * info.bytes_per_pixel, info.bytes_per_pixel);
        row_pitch = width * info.bytes_per_pixel;
        slice_pitch = row_pitch * height;
    }
    uint8_t *special =
        pgraph_convert_texture_data(shape, linear, palette, width, height,
                                    depth, row_pitch, slice_pitch, NULL);
    const uint8_t *pixels = special ? special : linear;
    unsigned int pixel_size =
        special ? (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5 ? 3 : 4) :
                  info.bytes_per_pixel;
    if (special) {
        row_pitch = width * pixel_size;
        slice_pitch = row_pitch * height;
    }
    unsigned int destination_pixel_size = d3d11_texture_pixel_bytes(format);
    uint8_t *rgba = g_malloc((size_t)width * height * depth *
                             destination_pixel_size);
    for (size_t i = 0; i < (size_t)width * height * depth; i++) {
        const uint8_t *pixel = pixels + (i / (width * height)) * slice_pitch +
                               ((i / width) % height) * row_pitch +
                               (i % width) * pixel_size;
        if (d3d11_is_depth_texture_format(format)) {
            float decoded_depth;
            uint32_t depth_bits;
            bool depth24 =
                format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
                format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT;
            bool depth_float =
                format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT;
            if (depth24) {
                uint32_t packed;
                memcpy(&packed, pixel, sizeof(packed));
                depth_bits = packed >> 8;
            } else {
                uint16_t packed;
                memcpy(&packed, pixel, sizeof(packed));
                if (depth_float) {
                    decoded_depth = d3d11_half_to_float(packed);
                    decoded_depth = CLAMP(decoded_depth, 0.0f, 1.0f);
                    depth_bits = (uint32_t)(decoded_depth * 65535.0f + 0.5f);
                } else {
                    depth_bits = packed;
                }
            }
            /* Keep depth as high-to-low byte channels in filterable UNORM.
             * R32_FLOAT filtering is optional at D3D feature level 10.0;
             * reconstructing the weighted bytes after sampling preserves the
             * linear interpolation of the original normalized depth value. */
            uint8_t *encoded = rgba + i * destination_pixel_size;
            if (depth24) {
                encoded[0] = (depth_bits >> 16) & 0xff;
                encoded[1] = (depth_bits >> 8) & 0xff;
                encoded[2] = depth_bits & 0xff;
            } else {
                encoded[0] = (depth_bits >> 8) & 0xff;
                encoded[1] = depth_bits & 0xff;
                encoded[2] = 0;
            }
            encoded[3] = 0xff;
        } else if (format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16) {
            uint16_t luminance;
            uint16_t rgba16[4];
            memcpy(&luminance, pixel, sizeof(luminance));
            rgba16[0] = rgba16[1] = rgba16[2] = luminance;
            rgba16[3] = UINT16_MAX;
            memcpy(rgba + i * destination_pixel_size, rgba16, sizeof(rgba16));
        } else if (special && pixel_size == 4 &&
            format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            uint32_t p;
            memcpy(&p, pixel, 4);
            rgba[i * 4 + 0] = p >> 16;
            rgba[i * 4 + 1] = p >> 8;
            rgba[i * 4 + 2] = p;
            rgba[i * 4 + 3] = p >> 24;
        } else if (special && pixel_size == 4) {
            memcpy(rgba + i * 4, pixel, 4);
        } else if (special && pixel_size == 3) {
            rgba[i * 4 + 0] = pixel[0];
            rgba[i * 4 + 1] = pixel[1];
            rgba[i * 4 + 2] = pixel[2];
            rgba[i * 4 + 3] = 127;
        } else {
            d3d11_decode_pixel(format, pixel, rgba + i * 4);
        }
    }
    g_free(special);
    if (linear != source) {
        g_free(linear);
    }
    return rgba;
}

static bool d3d11_crop_cubemap_border(uint8_t *pixels,
                                      unsigned int source_width,
                                      unsigned int source_height,
                                      unsigned int width,
                                      unsigned int height,
                                      unsigned int pixel_size)
{
    const size_t border = 4;
    size_t source_row_bytes, destination_row_bytes, source_size;
    size_t source_x, source_y, source_last_row, source_end;
    if (!pixels || !width || !height || !pixel_size ||
        source_width < border || source_height < border ||
        width > source_width - border || height > source_height - border ||
        __builtin_mul_overflow((size_t)source_width, (size_t)pixel_size,
                               &source_row_bytes) ||
        __builtin_mul_overflow((size_t)width, (size_t)pixel_size,
                               &destination_row_bytes) ||
        __builtin_mul_overflow(source_row_bytes, (size_t)source_height,
                               &source_size) ||
        __builtin_mul_overflow(border, (size_t)pixel_size, &source_x) ||
        __builtin_mul_overflow(border, source_row_bytes, &source_y) ||
        __builtin_mul_overflow((size_t)height - 1, source_row_bytes,
                               &source_last_row) ||
        __builtin_add_overflow(source_y, source_last_row, &source_end) ||
        __builtin_add_overflow(source_end, source_x, &source_end) ||
        __builtin_add_overflow(source_end, destination_row_bytes,
                               &source_end) ||
        source_end > source_size) {
        return false;
    }

    /* Compact in place; destination rows always precede unread source rows. */
    for (unsigned int y = 0; y < height; y++) {
        memmove(pixels + (size_t)y * destination_row_bytes,
                pixels + ((size_t)y + border) * source_row_bytes + source_x,
                destination_row_bytes);
    }
    return true;
}

/* Swizzled border storage expands each active axis to max(16, 2 * logical). */
static bool d3d11_border_resource_dimensions(
    bool border, bool linear_format, unsigned int dimensionality,
    unsigned int width, unsigned int height, unsigned int depth,
    unsigned int *resource_width, unsigned int *resource_height,
    unsigned int *resource_depth)
{
    if (dimensionality < 1 || dimensionality > 3 || !width ||
        (dimensionality >= 2 && !height) ||
        (dimensionality == 3 && !depth)) {
        return false;
    }

    unsigned int expanded_width = width;
    unsigned int expanded_height = dimensionality >= 2 ? height : 1;
    unsigned int expanded_depth = dimensionality == 3 ? depth : 1;
    if (border && !linear_format) {
        if (expanded_width > UINT_MAX / 2 ||
            (dimensionality >= 2 && expanded_height > UINT_MAX / 2) ||
            (dimensionality == 3 && expanded_depth > UINT_MAX / 2)) {
            return false;
        }
        expanded_width = MAX(16u, expanded_width * 2);
        if (dimensionality >= 2) {
            expanded_height = MAX(16u, expanded_height * 2);
        }
        if (dimensionality == 3) {
            expanded_depth = MAX(16u, expanded_depth * 2);
        }
    }

    *resource_width = expanded_width;
    *resource_height = expanded_height;
    *resource_depth = expanded_depth;
    return true;
}

static D3D11_TEXTURE_ADDRESS_MODE d3d11_address_mode(unsigned int mode)
{
    static const D3D11_TEXTURE_ADDRESS_MODE modes[] = {
        D3D11_TEXTURE_ADDRESS_CLAMP,  D3D11_TEXTURE_ADDRESS_WRAP,
        D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP,
    };
    return mode < ARRAY_SIZE(modes) ? modes[mode] : D3D11_TEXTURE_ADDRESS_CLAMP;
}

static D3D11_FILTER d3d11_filter(unsigned int min, unsigned int mag)
{
    bool linear_min =
        min == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
        min == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD ||
        min == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD ||
        min == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0;
    bool linear_mip = min == NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD ||
                      min == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD;
    bool linear_mag = mag == 2 || mag == 4;
    return (D3D11_FILTER)((linear_min ? 0x10 : 0) | (linear_mag ? 0x4 : 0) |
                          (linear_mip ? 0x1 : 0));
}

static bool d3d11_mipmap_enabled(bool linear_format, unsigned int min_filter)
{
    return !linear_format &&
           min_filter != NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 &&
           min_filter != NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 &&
           min_filter != NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0;
}
static unsigned int d3d11_effective_mip_levels(bool linear_format,
                                               unsigned int levels)
{
    return linear_format ? 1 : MAX(1, levels);
}
static unsigned int d3d11_effective_min_mipmap_level(bool linear_format,
                                                     unsigned int min_level)
{
    return linear_format ? 0 : min_level;
}

static bool d3d11_cubemap_shape_valid(bool cubemap, bool linear_format,
                                     unsigned int dimensionality,
                                     unsigned int width,
                                     unsigned int height)
{
    return !cubemap || (!linear_format && dimensionality == 2 &&
                        width == height);
}

static bool d3d11_cubemap_border_mips_supported(bool cubemap, bool border,
                                                unsigned int levels)
{
    return !cubemap || !border || levels <= 1;
}

static bool d3d11_texture_shape_valid(TextureShape shape)
{
    if (shape.dimensionality < 1 || shape.dimensionality > 3 ||
        !shape.width || !shape.height ||
        !d3d11_texture_format_supported(shape.color_format)) {
        return false;
    }

    BasicColorFormatInfo format =
        kelvin_color_format_info_map[shape.color_format];
    bool linear_format = format.linear;
    if (linear_format && shape.dimensionality != 2) {
        return false;
    }
    if (shape.dimensionality == 3 && !shape.depth) {
        return false;
    }
    if (!d3d11_cubemap_shape_valid(shape.cubemap, linear_format,
                                   shape.dimensionality, shape.width,
                                   shape.height)) {
        return false;
    }

    unsigned int levels =
        d3d11_effective_mip_levels(linear_format, shape.levels);
    unsigned int min_mipmap_level = d3d11_effective_min_mipmap_level(
        linear_format, shape.min_mipmap_level);
    if ((!linear_format && !shape.levels) || min_mipmap_level >= levels ||
        !d3d11_cubemap_border_mips_supported(
            shape.cubemap, shape.border, levels)) {
        return false;
    }
    if (linear_format &&
        shape.pitch < (size_t)shape.width * format.bytes_per_pixel) {
        return false;
    }

    unsigned int resource_width, resource_height, resource_depth;
    return d3d11_border_resource_dimensions(
        shape.border, linear_format, shape.dimensionality, shape.width,
        shape.height, shape.depth, &resource_width, &resource_height,
        &resource_depth);
}

static bool d3d11_update_sampler(PGRAPHD3D11State *r, PGRAPHState *pg,
                                 unsigned int stage, Error **errp)
{
    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + stage * 4);
    uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + stage * 4);
    uint32_t border = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + stage * 4);
    unsigned int max_anisotropy =
        1u << GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + stage * 4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY);
    TextureShape shape = pgraph_get_texture_shape(pg, stage);
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    bool mipmap_enabled = d3d11_mipmap_enabled(
        kelvin_color_format_info_map[shape.color_format].linear, min_filter);
    D3D11_SAMPLER_DESC desc = {
        .Filter = max_anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC :
                                       d3d11_filter(min_filter, mag_filter),
        .AddressU = d3d11_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .AddressV = d3d11_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .AddressW = d3d11_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)),
        .MipLODBias = pgraph_convert_lod_bias_to_float(
            GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS)),
        .MaxAnisotropy = MIN(max_anisotropy, 16),
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MinLOD = 0,
        .MaxLOD = mipmap_enabled ? D3D11_FLOAT32_MAX : 0,
    };
    pgraph_argb_pack32_to_rgba_float(border, desc.BorderColor);
    if (!r->textures->sampler_valid[stage] ||
        memcmp(&desc, &r->textures->sampler_descs[stage], sizeof(desc))) {
        ID3D11SamplerState *sampler = NULL;
        HRESULT hr =
            ID3D11Device_CreateSamplerState(r->device, &desc, &sampler);
        if (FAILED(hr)) {
            error_setg(errp, "D3D11: sampler creation failed (HRESULT 0x%08lx)",
                       (unsigned long)hr);
            return false;
        }
        if (r->textures->samplers[stage]) {
            ID3D11SamplerState_Release(r->textures->samplers[stage]);
        }
        r->textures->samplers[stage] = sampler;
        r->textures->sampler_descs[stage] = desc;
        r->textures->sampler_valid[stage] = true;
    }
    return true;
}

static bool d3d11_create_texture(NV2AState *d, unsigned int stage,
                                 TextureShape shape, hwaddr address,
                                 const uint8_t *palette, Error **errp)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    PGRAPHD3D11TextureBinding *binding = &r->textures->bindings[stage];
    unsigned int array_size = shape.cubemap ? 6 : 1;
    if (!d3d11_texture_format_supported(shape.color_format)) {
        error_setg(errp, "D3D11: unsupported or depth texture format 0x%x",
                   shape.color_format);
        return false;
    }
    if (!d3d11_texture_shape_valid(shape)) {
        error_setg(errp, "D3D11: invalid texture shape or row pitch");
        return false;
    }
    bool linear_format =
        kelvin_color_format_info_map[shape.color_format].linear;
    unsigned int levels =
        d3d11_effective_mip_levels(linear_format, shape.levels);
    unsigned int min_mipmap_level = d3d11_effective_min_mipmap_level(
        linear_format, shape.min_mipmap_level);
    if (!d3d11_cubemap_border_mips_supported(
            shape.cubemap, shape.border, levels)) {
        error_setg(errp,
                   "D3D11: bordered cubemap mip chains have no defined NV2A upload layout");
        return false;
    }
    if (!d3d11_cubemap_shape_valid(shape.cubemap, linear_format,
                                   shape.dimensionality, shape.width,
                                   shape.height)) {
        error_setg(errp,
                   "D3D11: cubemaps require square, swizzled 2D faces");
        return false;
    }
    if (!shape.width || !shape.height || min_mipmap_level >= levels ||
        (shape.dimensionality == 3 && (!shape.depth || shape.cubemap))) {
        error_setg(errp, "D3D11: invalid texture dimensions or mip range");
        return false;
    }
    unsigned int resource_width, resource_height, resource_depth;
    if (!d3d11_border_resource_dimensions(
            shape.border, linear_format, shape.dimensionality, shape.width,
            shape.height, shape.depth, &resource_width, &resource_height,
            &resource_depth)) {
        error_setg(errp, "D3D11: invalid texture border extent");
        return false;
    }
    unsigned int source_width = resource_width;
    unsigned int source_height = resource_height;
    unsigned int source_depth = resource_depth;
    /* The GL cubemap loader discards the four-texel border.  Its mip-chain
     * handling does not provide a well-defined D3D-compatible layout, so this
     * exact crop is limited to the unambiguous single-level case. */
    bool discard_cube_border = shape.cubemap && shape.border &&
                                !linear_format && levels == 1;
    if (discard_cube_border) {
        resource_width = shape.width;
        resource_height = shape.height;
    }
    unsigned int subresource_count = levels * array_size;
    D3D11_SUBRESOURCE_DATA *initial =
        g_new0(D3D11_SUBRESOURCE_DATA, subresource_count);
    uint8_t **converted = g_new0(uint8_t *, subresource_count);
    const uint8_t *source = d->vram_ptr + address;
    size_t face_size =
        pgraph_get_texture_length(&d->pgraph, &shape) / array_size;
    for (unsigned int face = 0; face < array_size; face++) {
        const uint8_t *level_source = source + face * face_size;
        unsigned int width = source_width;
        unsigned int height = source_height;
        unsigned int depth = source_depth;
        for (unsigned int level = 0; level < levels; level++) {
            unsigned int index = face * levels + level, consumed;
            converted[index] =
                d3d11_convert_level(shape, level_source, palette, MAX(1, width),
                                    MAX(1, height), MAX(1, depth), &consumed);
            unsigned int upload_width = MAX(1, width);
            unsigned int upload_height = MAX(1, height);
            if (discard_cube_border) {
                if (!d3d11_crop_cubemap_border(
                        converted[index], width, height, resource_width,
                        resource_height,
                        d3d11_texture_pixel_bytes(shape.color_format))) {
                    for (unsigned int i = 0; i < subresource_count; i++) {
                        g_free(converted[i]);
                    }
                    g_free(converted);
                    g_free(initial);
                    error_setg(errp,
                               "D3D11: invalid single-level cubemap border extent");
                    return false;
                }
                upload_width = resource_width;
                upload_height = resource_height;
            }
            initial[index].pSysMem = converted[index];
            initial[index].SysMemPitch =
                upload_width * d3d11_texture_pixel_bytes(shape.color_format);
            initial[index].SysMemSlicePitch =
                upload_width * upload_height *
                d3d11_texture_pixel_bytes(shape.color_format);
            level_source += consumed;
            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {
        .Width = resource_width,
        .Height = resource_height,
        .MipLevels = levels,
        .ArraySize = array_size,
        .Format = d3d11_texture_dxgi_format(shape.color_format),
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags = shape.cubemap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0,
    };
    ID3D11Resource *texture = NULL;
    HRESULT hr;
    if (shape.dimensionality == 3) {
        D3D11_TEXTURE3D_DESC volume_desc = {
            .Width = resource_width,
            .Height = resource_height,
            .Depth = resource_depth,
            .MipLevels = levels,
            .Format = desc.Format,
            .Usage = D3D11_USAGE_IMMUTABLE,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };
        ID3D11Texture3D *volume = NULL;
        hr = ID3D11Device_CreateTexture3D(r->device, &volume_desc, initial,
                                          &volume);
        texture = (ID3D11Resource *)volume;
    } else {
        ID3D11Texture2D *image = NULL;
        hr = ID3D11Device_CreateTexture2D(r->device, &desc, initial, &image);
        texture = (ID3D11Resource *)image;
    }
    for (unsigned int i = 0; i < subresource_count; i++) {
        g_free(converted[i]);
    }
    g_free(converted);
    g_free(initial);
    if (FAILED(hr)) {
        error_setg(errp, "D3D11: texture creation failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = {
        .Format = desc.Format,
        .ViewDimension = shape.cubemap ? D3D11_SRV_DIMENSION_TEXTURECUBE :
                         shape.dimensionality == 3 ?
                                         D3D11_SRV_DIMENSION_TEXTURE3D :
                                         D3D11_SRV_DIMENSION_TEXTURE2D,
    };
    if (shape.cubemap) {
        view_desc.TextureCube.MostDetailedMip = min_mipmap_level;
        view_desc.TextureCube.MipLevels = levels - min_mipmap_level;
    } else if (shape.dimensionality == 3) {
        view_desc.Texture3D.MostDetailedMip = min_mipmap_level;
        view_desc.Texture3D.MipLevels = levels - min_mipmap_level;
    } else {
        view_desc.Texture2D.MostDetailedMip = min_mipmap_level;
        view_desc.Texture2D.MipLevels = levels - min_mipmap_level;
    }
    ID3D11ShaderResourceView *view = NULL;
    hr = ID3D11Device_CreateShaderResourceView(
        r->device, (ID3D11Resource *)texture, &view_desc, &view);
    if (FAILED(hr)) {
        ID3D11Resource_Release(texture);
        error_setg(errp, "D3D11: texture SRV creation failed (HRESULT 0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    d3d11_release_texture(binding);
    binding->resource = (ID3D11Resource *)texture;
    binding->view = view;
    binding->shape = shape;
    binding->address = address;
    binding->length = pgraph_get_texture_length(&d->pgraph, &shape);
    return true;
}

bool pgraph_d3d11_bind_textures(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (!pgraph_d3d11_validate_texture_registers(pg, errp)) {
        return false;
    }
    ID3D11ShaderResourceView *views[NV2A_MAX_TEXTURES] = { 0 };
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES] = { 0 };
    for (unsigned int stage = 0; stage < NV2A_MAX_TEXTURES; stage++) {
        if (!pgraph_is_texture_enabled(pg, stage)) {
            continue;
        }
        TextureShape shape = pgraph_get_texture_shape(pg, stage);
        if (!d3d11_cubemap_border_mips_supported(
                shape.cubemap, shape.border, shape.levels)) {
            error_setg(
                errp,
                "D3D11: bordered cubemap mip chains have no defined NV2A upload layout");
            return false;
        }
        if (!d3d11_texture_shape_valid(shape)) {
            error_setg(errp, "D3D11: invalid texture shape or row pitch");
            return false;
        }
        hwaddr address;
        size_t dma_available = 0;
        if (!d3d11_texture_dma_range(d, stage, &address, &dma_available)) {
            error_setg(errp, "D3D11: texture %u has an invalid DMA range",
                       stage);
            return false;
        }
        size_t length = pgraph_get_texture_length(pg, &shape);
        if (length > dma_available) {
            error_setg(errp,
                       "D3D11: texture %u exceeds its DMA range or VRAM",
                       stage);
            return false;
        }
        bool uses_palette =
            shape.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8;
        size_t palette_length = 0;
        hwaddr palette_address = 0;
        if (uses_palette) {
            if (!d3d11_palette_dma_range(d, stage, &palette_address,
                                         &palette_length)) {
                error_setg(
                    errp,
                    "D3D11: texture %u palette exceeds its DMA range or VRAM",
                    stage);
                return false;
            }
        }
        /* Resolve an active render-to-texture surface before reading VRAM.
         * Include indexed palettes: their VRAM range may alias a dirty target
         * even when the texel range itself does not.
         */
        PGRAPHD3D11RenderTarget *targets[] = {
            &r->render_targets.color,
            &r->render_targets.depth_stencil,
        };
        Surface *surfaces[] = { &pg->surface_color, &pg->surface_zeta };
        bool resolve = false;
        for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
            if (surfaces[i]->draw_dirty && targets[i]->texture &&
                d3d11_texture_data_overlaps_target(
                    address, length, uses_palette, palette_address,
                    palette_length, targets[i]->vram_address,
                    targets[i]->storage_length)) {
                resolve = true;
            }
        }
        if (resolve && !pgraph_d3d11_surface_flush(d, errp)) {
            return false;
        }
        const uint8_t *palette = uses_palette ?
            d->vram_ptr + palette_address : NULL;
        uint64_t hash = fast_hash(d->vram_ptr + address, length);
        if (uses_palette) {
            hash ^= fast_hash(palette, palette_length);
        }
        PGRAPHD3D11TextureBinding *binding = &r->textures->bindings[stage];
        if (!binding->view || binding->data_hash != hash ||
            binding->address != address || binding->length != length ||
            memcmp(&binding->shape, &shape, sizeof(shape))) {
            if (!d3d11_create_texture(d, stage, shape, address, palette,
                                      errp)) {
                return false;
            }
            binding->data_hash = hash;
        }
        if (!d3d11_update_sampler(r, pg, stage, errp)) {
            return false;
        }
        views[stage] = binding->view;
        samplers[stage] = r->textures->samplers[stage];
        pg->texture_dirty[stage] = false;
    }
    ID3D11DeviceContext_PSSetShaderResources(r->context, 0, NV2A_MAX_TEXTURES,
                                             views);
    ID3D11DeviceContext_PSSetSamplers(r->context, 0, NV2A_MAX_TEXTURES,
                                      samplers);
    return true;
}
