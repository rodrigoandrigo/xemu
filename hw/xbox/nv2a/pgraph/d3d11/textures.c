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
        dst[0] = dst[2] = src[1];
        dst[1] = dst[3] = src[0];
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
        /* Unsupported/depth formats remain a deterministic black texel. */
        break;
    }
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
    uint8_t *rgba = g_malloc((size_t)width * height * depth * 4);
    for (size_t i = 0; i < (size_t)width * height * depth; i++) {
        if (special && pixel_size == 4 &&
            format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            uint32_t p;
            memcpy(&p, pixels + i * 4, 4);
            rgba[i * 4 + 0] = p >> 16;
            rgba[i * 4 + 1] = p >> 8;
            rgba[i * 4 + 2] = p;
            rgba[i * 4 + 3] = p >> 24;
        } else if (special && pixel_size == 4) {
            memcpy(rgba + i * 4, pixels + i * 4, 4);
        } else if (special && pixel_size == 3) {
            rgba[i * 4 + 0] = (uint8_t)((int8_t)pixels[i * 3 + 0] + 128);
            rgba[i * 4 + 1] = (uint8_t)((int8_t)pixels[i * 3 + 1] + 128);
            rgba[i * 4 + 2] = (uint8_t)((int8_t)pixels[i * 3 + 2] + 128);
            rgba[i * 4 + 3] = 255;
        } else {
            d3d11_decode_pixel(format, pixels + i * pixel_size, rgba + i * 4);
        }
    }
    g_free(special);
    if (linear != source) {
        g_free(linear);
    }
    return rgba;
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

static D3D11_FILTER d3d11_filter(uint32_t filter)
{
    unsigned int min = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int mag = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    bool linear_min = min == 2 || min == 4 || min == 6 || min == 7;
    bool linear_mip = min == 5 || min == 6;
    bool linear_mag = mag == 2 || mag == 4;
    return (D3D11_FILTER)((linear_min ? 0x10 : 0) | (linear_mag ? 0x4 : 0) |
                          (linear_mip ? 0x1 : 0));
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
    D3D11_SAMPLER_DESC desc = {
        .Filter = max_anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC :
                                       d3d11_filter(filter),
        .AddressU =
            d3d11_address_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .AddressV =
            d3d11_address_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .AddressW =
            d3d11_address_mode(GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)),
        .MipLODBias = pgraph_convert_lod_bias_to_float(
            GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS)),
        .MaxAnisotropy = MIN(max_anisotropy, 16),
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .MinLOD = 0,
        .MaxLOD = D3D11_FLOAT32_MAX,
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
    unsigned int levels = MAX(1, shape.levels);
    unsigned int subresource_count = levels * array_size;
    D3D11_SUBRESOURCE_DATA *initial =
        g_new0(D3D11_SUBRESOURCE_DATA, subresource_count);
    uint8_t **converted = g_new0(uint8_t *, subresource_count);
    const uint8_t *source = d->vram_ptr + address;
    size_t face_size =
        pgraph_get_texture_length(&d->pgraph, &shape) / array_size;
    for (unsigned int face = 0; face < array_size; face++) {
        const uint8_t *level_source = source + face * face_size;
        unsigned int width = shape.width, height = shape.height;
        for (unsigned int level = 0; level < levels; level++) {
            unsigned int index = face * levels + level, consumed;
            converted[index] =
                d3d11_convert_level(shape, level_source, palette, MAX(1, width),
                                    MAX(1, height), 1, &consumed);
            initial[index].pSysMem = converted[index];
            initial[index].SysMemPitch = MAX(1, width) * 4;
            initial[index].SysMemSlicePitch =
                MAX(1, width) * MAX(1, height) * 4;
            level_source += consumed;
            width /= 2;
            height /= 2;
        }
    }
    D3D11_TEXTURE2D_DESC desc = {
        .Width = shape.width,
        .Height = shape.dimensionality >= 2 ? shape.height : 1,
        .MipLevels = levels,
        .ArraySize = array_size,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .MiscFlags = shape.cubemap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0,
    };
    ID3D11Texture2D *texture = NULL;
    HRESULT hr =
        ID3D11Device_CreateTexture2D(r->device, &desc, initial, &texture);
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
                                         D3D11_SRV_DIMENSION_TEXTURE2D,
    };
    if (shape.cubemap) {
        view_desc.TextureCube.MostDetailedMip = shape.min_mipmap_level;
        view_desc.TextureCube.MipLevels = levels - shape.min_mipmap_level;
    } else {
        view_desc.Texture2D.MostDetailedMip = shape.min_mipmap_level;
        view_desc.Texture2D.MipLevels = levels - shape.min_mipmap_level;
    }
    ID3D11ShaderResourceView *view = NULL;
    hr = ID3D11Device_CreateShaderResourceView(
        r->device, (ID3D11Resource *)texture, &view_desc, &view);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(texture);
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
    ID3D11ShaderResourceView *views[NV2A_MAX_TEXTURES] = { 0 };
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES] = { 0 };
    for (unsigned int stage = 0; stage < NV2A_MAX_TEXTURES; stage++) {
        if (!pgraph_is_texture_enabled(pg, stage)) {
            continue;
        }
        TextureShape shape = pgraph_get_texture_shape(pg, stage);
        if (shape.dimensionality == 3) {
            error_setg(errp, "D3D11: 3D NV2A textures are not implemented");
            return false;
        }
        hwaddr address = pgraph_get_texture_phys_addr(pg, stage);
        size_t length = pgraph_get_texture_length(pg, &shape);
        if (address > memory_region_size(d->vram) ||
            length > memory_region_size(d->vram) - address) {
            error_setg(errp, "D3D11: texture %u exceeds VRAM", stage);
            return false;
        }
        size_t palette_length = 0;
        hwaddr palette_address = pgraph_get_texture_palette_phys_addr_length(
            pg, stage, &palette_length);
        const uint8_t *palette = d->vram_ptr + palette_address;
        uint64_t hash = fast_hash(d->vram_ptr + address, length);
        if (shape.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
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
