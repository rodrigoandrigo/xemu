/*
 * Geforce NV2A PGRAPH Direct3D 12 Textures
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"
#include "textures.h"

typedef struct D3D12DecodedTexture {
    uint8_t *data;
    size_t size;
    uint32_t row_pitch;
    uint32_t slice_pitch;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    DXGI_FORMAT format;
    UINT component_mapping;
} D3D12DecodedTexture;

typedef struct D3D12TextureLayout {
    D3D12DecodedTexture subresources[6][16];
    unsigned int layers;
    unsigned int levels;
} D3D12TextureLayout;

#define D3D12_MAP_COMPONENT_0 \
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0
#define D3D12_MAP_COMPONENT_1 \
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1
#define D3D12_MAP_COMPONENT_2 \
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2
#define D3D12_MAP_COMPONENT_3 \
    D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3
#define D3D12_MAP_ZERO D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0
#define D3D12_MAP_ONE D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1

static UINT d3d12_component_mapping(UINT r, UINT g, UINT b, UINT a)
{
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a);
}

static void d3d12_release_texture(PGRAPHD3D12Texture *texture)
{
    if (texture->upload) {
        ID3D12Resource_Release(texture->upload);
    }
    if (texture->resource) {
        ID3D12Resource_Release(texture->resource);
    }
    memset(texture, 0, sizeof(*texture));
}

static bool d3d12_decode_texture(PGRAPHState *pg, const TextureShape *shape,
                                 unsigned int width, unsigned int height,
                                 unsigned int depth, uint32_t linear_pitch,
                                 const uint8_t *source, const uint8_t *palette,
                                 D3D12DecodedTexture *decoded)
{
    BasicColorFormatInfo info =
        kelvin_color_format_info_map[shape->color_format];
    uint32_t source_pitch =
        linear_pitch ? linear_pitch : width * info.bytes_per_pixel;
    uint8_t *linear = NULL;
    decoded->width = width;
    decoded->height = height;
    decoded->depth = depth;
    decoded->component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (pgraph_is_texture_format_compressed(pg, shape->color_format)) {
        enum S3TC_DECOMPRESS_FORMAT format =
            shape->color_format ==
                    NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                S3TC_DECOMPRESS_FORMAT_DXT1 :
            shape->color_format ==
                    NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ?
                S3TC_DECOMPRESS_FORMAT_DXT3 :
                S3TC_DECOMPRESS_FORMAT_DXT5;
        decoded->data =
            depth > 1 ?
                s3tc_decompress_3d(format, source, width, height, depth) :
                s3tc_decompress_2d(format, source, width, height);
        decoded->row_pitch = width * 4;
        decoded->slice_pitch = decoded->row_pitch * height;
        decoded->size = (size_t)decoded->slice_pitch * depth;
        decoded->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return decoded->data != NULL;
    }

    if (info.linear) {
        if (depth != 1) {
            return false;
        }
        linear = g_malloc((size_t)source_pitch * height);
        memcpy(linear, source, (size_t)source_pitch * height);
    } else {
        source_pitch = width * info.bytes_per_pixel;
        uint32_t source_slice_pitch = source_pitch * height;
        linear = g_malloc((size_t)source_slice_pitch * depth);
        if (depth > 1) {
            unswizzle_box(source, width, height, depth, linear, source_pitch,
                          source_slice_pitch, info.bytes_per_pixel);
        } else {
            unswizzle_rect(source, width, height, linear, source_pitch,
                           info.bytes_per_pixel);
        }
    }

    size_t converted_size = 0;
    uint8_t *converted = pgraph_convert_texture_data(
        *shape, linear, palette, width, height, depth, source_pitch,
        source_pitch * height, &converted_size);
    if (converted) {
        g_free(linear);
        decoded->data = converted;
        decoded->size = converted_size;
        if (shape->color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            decoded->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            decoded->row_pitch = width * 4;
        } else if (shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
            size_t pixels = (size_t)width * height * depth;
            uint8_t *rgba = g_malloc(pixels * 4);
            for (size_t i = 0; i < pixels; i++) {
                memcpy(rgba + i * 4, converted + i * 3, 3);
                rgba[i * 4 + 3] = 0x7f;
            }
            g_free(converted);
            decoded->data = rgba;
            decoded->size = pixels * 4;
            decoded->format = DXGI_FORMAT_R8G8B8A8_SNORM;
            decoded->row_pitch = width * 4;
        } else {
            decoded->format = DXGI_FORMAT_R8G8B8A8_UNORM;
            decoded->row_pitch = width * 4;
        }
        decoded->slice_pitch = decoded->row_pitch * height;
        return true;
    }

    decoded->data = linear;
    decoded->size = (size_t)source_pitch * height * depth;
    decoded->row_pitch = source_pitch;
    decoded->slice_pitch = source_pitch * height;
    switch (shape->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_ONE);
        decoded->format = DXGI_FORMAT_R8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_ONE, D3D12_MAP_ONE, D3D12_MAP_ONE, D3D12_MAP_COMPONENT_0);
        decoded->format = DXGI_FORMAT_R8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_COMPONENT_0);
        decoded->format = DXGI_FORMAT_R8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_COMPONENT_1);
        decoded->format = DXGI_FORMAT_R8G8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_1, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_COMPONENT_1);
        decoded->format = DXGI_FORMAT_R8G8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_1, D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_COMPONENT_1);
        decoded->format = DXGI_FORMAT_R8G8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_1, D3D12_MAP_COMPONENT_2,
            D3D12_MAP_ONE);
        decoded->format = DXGI_FORMAT_B5G5R5A1_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
        decoded->format = DXGI_FORMAT_B5G5R5A1_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4:
        decoded->format = DXGI_FORMAT_B4G4R4A4_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5:
        decoded->format = DXGI_FORMAT_B5G6R5_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_1, D3D12_MAP_COMPONENT_2,
            D3D12_MAP_ONE);
        decoded->format = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
        decoded->format = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        decoded->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_1, D3D12_MAP_COMPONENT_2, D3D12_MAP_COMPONENT_3,
            D3D12_MAP_COMPONENT_0);
        decoded->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_3, D3D12_MAP_COMPONENT_2, D3D12_MAP_COMPONENT_1,
            D3D12_MAP_COMPONENT_0);
        decoded->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        decoded->component_mapping =
            d3d12_component_mapping(D3D12_MAP_COMPONENT_0, D3D12_MAP_ZERO,
                                    D3D12_MAP_ZERO, D3D12_MAP_ZERO);
        decoded->format = DXGI_FORMAT_R16_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        decoded->component_mapping =
            d3d12_component_mapping(D3D12_MAP_COMPONENT_0, D3D12_MAP_ZERO,
                                    D3D12_MAP_ONE, D3D12_MAP_ZERO);
        decoded->format = DXGI_FORMAT_R16_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16:
        decoded->component_mapping = d3d12_component_mapping(
            D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0, D3D12_MAP_COMPONENT_0,
            D3D12_MAP_ONE);
        decoded->format = DXGI_FORMAT_R16_UNORM;
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT:
        decoded->component_mapping =
            d3d12_component_mapping(D3D12_MAP_COMPONENT_0, D3D12_MAP_ONE,
                                    D3D12_MAP_ZERO, D3D12_MAP_ZERO);
        decoded->format = DXGI_FORMAT_R32_UINT;
        break;
    default:
        g_free(decoded->data);
        memset(decoded, 0, sizeof(*decoded));
        return false;
    }
    return true;
}

static size_t d3d12_cubemap_face_size(PGRAPHState *pg,
                                      const TextureShape *shape)
{
    BasicColorFormatInfo info =
        kelvin_color_format_info_map[shape->color_format];
    bool compressed =
        pgraph_is_texture_format_compressed(pg, shape->color_format);
    unsigned int width = shape->width;
    unsigned int height = shape->height;
    size_t size = 0;

    if (!info.linear && shape->border) {
        width = MAX(16, width * 2);
        height = MAX(16, height * 2);
    }
    for (unsigned int level = 0; level < shape->levels; level++) {
        width = MAX(width, 1);
        height = MAX(height, 1);
        if (compressed) {
            unsigned int block_size =
                shape->color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                    8 :
                    16;
            size += (size_t)ROUND_UP(width, 4) / 4 * (ROUND_UP(height, 4) / 4) *
                    block_size;
        } else {
            size += (size_t)width * height * info.bytes_per_pixel;
        }
        width >>= 1;
        height >>= 1;
    }
    return ROUND_UP(size, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

static void d3d12_free_texture_layout(D3D12TextureLayout *layout)
{
    for (unsigned int layer = 0; layer < layout->layers; layer++) {
        for (unsigned int level = 0; level < layout->levels; level++) {
            g_free(layout->subresources[layer][level].data);
        }
    }
    memset(layout, 0, sizeof(*layout));
}

static bool
d3d12_decode_texture_layout(PGRAPHState *pg, const TextureShape *shape,
                            const uint8_t *source, const uint8_t *palette,
                            D3D12TextureLayout *layout, Error **errp)
{
    BasicColorFormatInfo info =
        kelvin_color_format_info_map[shape->color_format];
    bool compressed =
        pgraph_is_texture_format_compressed(pg, shape->color_format);
    unsigned int width = shape->width;
    unsigned int height = shape->dimensionality >= 2 ? shape->height : 1;
    unsigned int depth = shape->dimensionality >= 3 ? shape->depth : 1;

    if (shape->levels == 0 || shape->levels > 16 ||
        (shape->dimensionality < 1 || shape->dimensionality > 3) ||
        (shape->cubemap && shape->dimensionality != 2) ||
        (info.linear && (shape->dimensionality != 2 || shape->levels != 1)) ||
        (compressed && shape->dimensionality < 2)) {
        error_setg(errp, "D3D12: unsupported NV2A texture layout");
        return false;
    }
    if (!info.linear && shape->border) {
        width = MAX(16, width * 2);
        if (shape->dimensionality >= 2) {
            height = MAX(16, height * 2);
        } else {
            height = 1;
        }
        if (shape->dimensionality >= 3) {
            depth = MAX(16, depth * 2);
        } else {
            depth = 1;
        }
    }

    layout->layers = shape->cubemap ? 6 : 1;
    layout->levels = shape->levels;
    size_t face_size = shape->cubemap ? d3d12_cubemap_face_size(pg, shape) : 0;
    unsigned int block_size =
        shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
            8 :
            16;

    for (unsigned int layer = 0; layer < layout->layers; layer++) {
        const uint8_t *level_source = source + layer * face_size;
        unsigned int level_width = width;
        unsigned int level_height = height;
        unsigned int level_depth = depth;
        for (unsigned int level = 0; level < layout->levels; level++) {
            level_width = MAX(level_width, 1);
            level_height = MAX(level_height, 1);
            level_depth = MAX(level_depth, 1);
            uint32_t pitch = info.linear ? shape->pitch : 0;
            if (!d3d12_decode_texture(pg, shape, level_width, level_height,
                                      shape->dimensionality == 3 ? level_depth :
                                                                   1,
                                      pitch, level_source, palette,
                                      &layout->subresources[layer][level])) {
                error_setg(errp, "D3D12: unsupported texture format 0x%x",
                           shape->color_format);
                d3d12_free_texture_layout(layout);
                return false;
            }

            if (compressed) {
                level_source += (size_t)ROUND_UP(level_width, 4) / 4 *
                                (ROUND_UP(level_height, 4) / 4) *
                                (shape->dimensionality == 3 ? level_depth : 1) *
                                block_size;
            } else if (info.linear) {
                level_source +=
                    (size_t)(shape->pitch ?
                                 shape->pitch :
                                 level_width * info.bytes_per_pixel) *
                    level_height;
            } else {
                level_source += (size_t)level_width * level_height *
                                (shape->dimensionality == 3 ? level_depth : 1) *
                                info.bytes_per_pixel;
            }
            level_width >>= 1;
            level_height >>= 1;
            level_depth >>= 1;
        }
    }
    return true;
}

static D3D12_TEXTURE_ADDRESS_MODE d3d12_address_mode(unsigned int mode)
{
    switch (mode) {
    case NV_PGRAPH_TEXADDRESS0_ADDRU_WRAP:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case NV_PGRAPH_TEXADDRESS0_ADDRU_MIRROR:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static D3D12_FILTER d3d12_filter(unsigned int min_filter,
                                 unsigned int mag_filter,
                                 unsigned int anisotropy)
{
    if (anisotropy > 1) {
        return D3D12_FILTER_ANISOTROPIC;
    }
    D3D12_FILTER_TYPE min =
        (min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
         min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD ||
         min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD ||
         min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0) ?
            D3D12_FILTER_TYPE_LINEAR :
            D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mag = (mag_filter == 2 || mag_filter == 4) ?
                                D3D12_FILTER_TYPE_LINEAR :
                                D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mip =
        (min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD ||
         min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD) ?
            D3D12_FILTER_TYPE_LINEAR :
            D3D12_FILTER_TYPE_POINT;
    return D3D12_ENCODE_BASIC_FILTER(min, mag, mip,
                                     D3D12_FILTER_REDUCTION_TYPE_STANDARD);
}

static bool d3d12_retire_descriptor_users(PGRAPHD3D12State *r, bool *retired,
                                          Error **errp)
{
    if (*retired) {
        return true;
    }
    if (!pgraph_d3d12_draw_collect(r, true, errp)) {
        return false;
    }
    *retired = true;
    return true;
}

static bool d3d12_update_sampler(PGRAPHD3D12State *r, PGRAPHState *pg,
                                 unsigned int index, bool *retired,
                                 Error **errp)
{
    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + index * 4);
    uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + index * 4);
    uint32_t border = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + index * 4);
    unsigned int anisotropy =
        1 << GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + index * 4),
                      NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY);
    TextureShape shape = pgraph_get_texture_shape(pg, index);
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    bool mipmap_enabled =
        !kelvin_color_format_info_map[shape.color_format].linear &&
        min_filter != NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 &&
        min_filter != NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 &&
        min_filter != NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0;
    D3D12_SAMPLER_DESC descriptor = {
        .Filter = d3d12_filter(min_filter,
                               GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG),
                               anisotropy),
        .AddressU = d3d12_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .AddressV = d3d12_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .AddressW = d3d12_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)),
        .MipLODBias = pgraph_convert_lod_bias_to_float(
            GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS)),
        .MaxAnisotropy = MIN(anisotropy, 16),
        .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
        .BorderColor = {
            ((border >> 16) & 0xff) / 255.0f,
            ((border >> 8) & 0xff) / 255.0f,
            (border & 0xff) / 255.0f,
            ((border >> 24) & 0xff) / 255.0f,
        },
        .MinLOD = mipmap_enabled ?
                      MIN(shape.min_mipmap_level, shape.levels - 1) :
                      0.0f,
        .MaxLOD = mipmap_enabled ?
                      MIN(shape.max_mipmap_level, shape.levels - 1) :
                      0.0f,
    };
    PGRAPHD3D12Texture *texture = &r->textures[index];
    if (texture->sampler_valid &&
        !memcmp(&texture->sampler, &descriptor, sizeof(descriptor))) {
        return true;
    }
    /* Shader-visible descriptors must not be overwritten while an earlier
     * command list can still reference them. Texture resources have the same
     * lifetime requirement, so one retirement covers every mutation made by
     * this bind pass. */
    if (!d3d12_retire_descriptor_users(r, retired, errp)) {
        return false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            r->sampler_heap);
    handle.ptr += index * r->sampler_increment;
    ID3D12Device_CreateSampler(r->device, &descriptor, handle);
    texture->sampler = descriptor;
    texture->sampler_valid = true;
    return true;
}

bool pgraph_d3d12_textures_init(PGRAPHD3D12State *r, Error **errp)
{
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = NV2A_MAX_TEXTURES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(
        r->device, &srv_desc, &IID_ID3D12DescriptorHeap, (void **)&r->srv_heap);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: texture SRV heap creation failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC sampler_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
        .NumDescriptors = NV2A_MAX_TEXTURES,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    hr = ID3D12Device_CreateDescriptorHeap(r->device, &sampler_desc,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&r->sampler_heap);
    if (FAILED(hr)) {
        error_setg(errp,
                   "D3D12: texture sampler heap creation failed (0x%08lx)",
                   (unsigned long)hr);
        ID3D12DescriptorHeap_Release(r->srv_heap);
        r->srv_heap = NULL;
        return false;
    }
    r->srv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        r->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r->sampler_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        r->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    r->textures = g_new0(PGRAPHD3D12Texture, NV2A_MAX_TEXTURES);
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC null_view = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 },
        };
        D3D12_CPU_DESCRIPTOR_HANDLE srv =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                r->srv_heap);
        srv.ptr += i * r->srv_increment;
        ID3D12Device_CreateShaderResourceView(r->device, NULL, &null_view, srv);
        D3D12_SAMPLER_DESC null_sampler = {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        D3D12_CPU_DESCRIPTOR_HANDLE sampler =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                r->sampler_heap);
        sampler.ptr += i * r->sampler_increment;
        ID3D12Device_CreateSampler(r->device, &null_sampler, sampler);
    }
    return true;
}

void pgraph_d3d12_textures_finalize(PGRAPHD3D12State *r)
{
    if (r->textures) {
        for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            PGRAPHD3D12Texture *texture = &r->textures[i];
            if (texture->upload) {
                ID3D12Resource_Release(texture->upload);
            }
            if (texture->resource) {
                ID3D12Resource_Release(texture->resource);
            }
        }
        g_free(r->textures);
        r->textures = NULL;
    }
    if (r->sampler_heap) {
        ID3D12DescriptorHeap_Release(r->sampler_heap);
        r->sampler_heap = NULL;
    }
    if (r->srv_heap) {
        ID3D12DescriptorHeap_Release(r->srv_heap);
        r->srv_heap = NULL;
    }
}

void pgraph_d3d12_textures_trim(PGRAPHD3D12State *r)
{
    if (!r->textures) {
        return;
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        PGRAPHD3D12Texture *texture = &r->textures[i];
        if (texture->resource) {
            ID3D12Pageable *resource = (ID3D12Pageable *)texture->resource;
            ID3D12Device_Evict(r->device, 1, &resource);
        }
        d3d12_release_texture(texture);
    }
}

static bool d3d12_texture_dma_range(NV2AState *d, unsigned int index,
                                    hwaddr *address, size_t *available)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t format = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + index * 4);
    bool dma_b = GET_MASK(format, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);
    hwaddr dma_length = 0;
    uint8_t *mapping =
        nv_dma_map(d, dma_b ? pg->dma_b : pg->dma_a, &dma_length);
    hwaddr offset = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + index * 4);
    uint64_t vram_size = memory_region_size(d->vram);
    if (!mapping || offset >= dma_length) {
        return false;
    }
    uintptr_t start = (uintptr_t)mapping + offset;
    uintptr_t vram_start = (uintptr_t)d->vram_ptr;
    if (start < vram_start || start - vram_start >= vram_size) {
        return false;
    }
    *address = start - vram_start;
    *available = MIN((uint64_t)(dma_length - offset), vram_size - *address);
    return true;
}

static bool d3d12_palette_dma_range(NV2AState *d, unsigned int index,
                                    hwaddr *address, size_t *length)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t palette = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + index * 4);
    static const size_t palette_entries[] = { 256, 128, 64, 32 };
    unsigned int length_index = GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_LENGTH);
    if (length_index >= ARRAY_SIZE(palette_entries)) {
        return false;
    }
    bool dma_b = GET_MASK(palette, NV_PGRAPH_TEXPALETTE0_CONTEXT_DMA);
    hwaddr dma_length = 0;
    uint8_t *mapping =
        nv_dma_map(d, dma_b ? pg->dma_b : pg->dma_a, &dma_length);
    hwaddr offset = palette & NV_PGRAPH_TEXPALETTE0_OFFSET;
    size_t bytes = palette_entries[length_index] * sizeof(uint32_t);
    uint64_t vram_size = memory_region_size(d->vram);
    if (!mapping || offset > dma_length || bytes > dma_length - offset) {
        return false;
    }
    uintptr_t start = (uintptr_t)mapping + offset;
    uintptr_t vram_start = (uintptr_t)d->vram_ptr;
    if (start < vram_start || start - vram_start > vram_size ||
        bytes > vram_size - (start - vram_start)) {
        return false;
    }
    *address = start - vram_start;
    *length = bytes;
    return true;
}

static bool d3d12_texture_memory_dirty(NV2AState *d, hwaddr address,
                                       size_t length)
{
    if (!length) {
        return false;
    }
    uint64_t vram_size = memory_region_size(d->vram);
    uint64_t start = address & TARGET_PAGE_MASK;
    uint64_t unaligned_end = (uint64_t)address + length;
    uint64_t end = MIN((uint64_t)TARGET_PAGE_ALIGN(unaligned_end), vram_size);
    return end > start &&
           memory_region_test_and_clear_dirty(d->vram, start, end - start,
                                              DIRTY_MEMORY_NV2A_TEX);
}

static void d3d12_mark_overlapping_textures_dirty(PGRAPHState *pg,
                                                  PGRAPHD3D12State *r,
                                                  hwaddr address, size_t length)
{
    uint64_t end = (uint64_t)address + length;
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        PGRAPHD3D12Texture *texture = &r->textures[i];
        if ((texture->vram_length &&
             address < texture->vram_address + texture->vram_length &&
             texture->vram_address < end) ||
            (texture->palette_length &&
             address < texture->palette_address + texture->palette_length &&
             texture->palette_address < end)) {
            pg->texture_dirty[i] = true;
        }
    }
}

static bool d3d12_upload_texture(PGRAPHD3D12State *r,
                                 PGRAPHD3D12Texture *texture,
                                 unsigned int descriptor_index,
                                 const D3D12TextureLayout *layout, Error **errp)
{
    const D3D12DecodedTexture *first = &layout->subresources[0][0];
    D3D12_HEAP_PROPERTIES default_heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC texture_desc = {
        .Dimension = texture->shape.dimensionality == 3 ?
                         D3D12_RESOURCE_DIMENSION_TEXTURE3D :
                     texture->shape.dimensionality == 2 ?
                         D3D12_RESOURCE_DIMENSION_TEXTURE2D :
                         D3D12_RESOURCE_DIMENSION_TEXTURE1D,
        .Width = first->width,
        .Height = texture->shape.dimensionality >= 2 ? first->height : 1,
        .DepthOrArraySize =
            texture->shape.dimensionality == 3 ? first->depth : layout->layers,
        .MipLevels = layout->levels,
        .Format = first->format,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    };
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        r->device, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void **)&texture->resource);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: texture creation failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }
    if (!pgraph_d3d12_make_resident(r, (ID3D12Pageable *)texture->resource,
                                    errp)) {
        ID3D12Resource_Release(texture->resource);
        texture->resource = NULL;
        return false;
    }
    texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
    texture->format = first->format;
    texture->component_mapping = first->component_mapping;

    UINT subresource_count = layout->levels * layout->layers;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprints =
        g_new0(D3D12_PLACED_SUBRESOURCE_FOOTPRINT, subresource_count);
    UINT *rows = g_new0(UINT, subresource_count);
    UINT64 *row_sizes = g_new0(UINT64, subresource_count);
    UINT64 upload_size = 0;
    ID3D12Device_GetCopyableFootprints(r->device, &texture_desc, 0,
                                       subresource_count, 0, footprints, rows,
                                       row_sizes, &upload_size);
    for (UINT subresource = 0; subresource < subresource_count; subresource++) {
        unsigned int layer = subresource / layout->levels;
        unsigned int level = subresource % layout->levels;
        const D3D12DecodedTexture *decoded =
            &layout->subresources[layer][level];
        if (decoded->format != first->format ||
            decoded->component_mapping != first->component_mapping ||
            decoded->row_pitch > footprints[subresource].Footprint.RowPitch ||
            decoded->row_pitch > row_sizes[subresource] ||
            decoded->height > rows[subresource]) {
            error_setg(errp, "D3D12: inconsistent decoded texture layout");
            g_free(row_sizes);
            g_free(rows);
            g_free(footprints);
            return false;
        }
    }
    D3D12_HEAP_PROPERTIES upload_heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC upload_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = upload_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    hr = ID3D12Device_CreateCommittedResource(
        r->device, &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
        (void **)&texture->upload);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: texture upload buffer failed (0x%08lx)",
                   (unsigned long)hr);
        g_free(row_sizes);
        g_free(rows);
        g_free(footprints);
        return false;
    }
    uint8_t *mapped = NULL;
    D3D12_RANGE no_read = { 0, 0 };
    hr = ID3D12Resource_Map(texture->upload, 0, &no_read, (void **)&mapped);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: texture upload map failed (0x%08lx)",
                   (unsigned long)hr);
        g_free(row_sizes);
        g_free(rows);
        g_free(footprints);
        return false;
    }
    for (UINT subresource = 0; subresource < subresource_count; subresource++) {
        unsigned int layer = subresource / layout->levels;
        unsigned int level = subresource % layout->levels;
        const D3D12DecodedTexture *decoded =
            &layout->subresources[layer][level];
        uint8_t *destination = mapped + footprints[subresource].Offset;
        size_t destination_slice =
            (size_t)footprints[subresource].Footprint.RowPitch *
            footprints[subresource].Footprint.Height;
        for (unsigned int z = 0; z < decoded->depth; z++) {
            for (unsigned int y = 0; y < decoded->height; y++) {
                memcpy(destination + z * destination_slice +
                           (size_t)y *
                               footprints[subresource].Footprint.RowPitch,
                       decoded->data + (size_t)z * decoded->slice_pitch +
                           (size_t)y * decoded->row_pitch,
                       decoded->row_pitch);
            }
        }
    }
    ID3D12Resource_Unmap(texture->upload, 0, NULL);

    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    hr = ID3D12Device_CreateCommandAllocator(
        r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: texture upload command list failed");
        if (allocator) {
            ID3D12CommandAllocator_Release(allocator);
        }
        g_free(row_sizes);
        g_free(rows);
        g_free(footprints);
        return false;
    }
    for (UINT subresource = 0; subresource < subresource_count; subresource++) {
        D3D12_TEXTURE_COPY_LOCATION destination = {
            .pResource = texture->resource,
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = subresource,
        };
        D3D12_TEXTURE_COPY_LOCATION source = {
            .pResource = texture->upload,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = footprints[subresource],
        };
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &destination, 0, 0, 0,
                                                    &source, NULL);
    }
    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = texture->resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
            .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr)) {
        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(allocator);
        g_free(row_sizes);
        g_free(rows);
        g_free(footprints);
        error_setg(errp, "D3D12: texture upload command close failed");
        return false;
    }
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    qemu_mutex_lock(&r->queue_lock);
    ID3D12CommandQueue_ExecuteCommandLists(r->queue, 1, lists);
    uint64_t fence_value = r->next_fence_value++;
    hr = ID3D12CommandQueue_Signal(r->queue, r->fence, fence_value);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Fence_SetEventOnCompletion(r->fence, fence_value,
                                              r->fence_event);
    }
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObjectEx(r->fence_event, 5000, FALSE) !=
            WAIT_OBJECT_0) {
            hr = ID3D12Device_GetDeviceRemovedReason(r->device);
            if (SUCCEEDED(hr)) {
                hr = DXGI_ERROR_DEVICE_HUNG;
            }
        }
    }
    qemu_mutex_unlock(&r->queue_lock);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    g_free(row_sizes);
    g_free(rows);
    g_free(footprints);
    if (FAILED(hr)) {
        pgraph_d3d12_note_device_error(r, hr, "texture upload");
        error_setg(errp, "D3D12: texture upload synchronization failed");
        return false;
    }
    texture->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12Resource_Release(texture->upload);
    texture->upload = NULL;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        .Format = texture->format,
        .Shader4ComponentMapping = texture->component_mapping,
    };
    if (texture->shape.cubemap) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = layout->levels;
    } else if (texture->shape.dimensionality == 3) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MostDetailedMip = 0;
        srv.Texture3D.MipLevels = layout->levels;
    } else if (texture->shape.dimensionality == 2) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = layout->levels;
    } else {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        srv.Texture1D.MostDetailedMip = 0;
        srv.Texture1D.MipLevels = layout->levels;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->srv_heap);
    handle.ptr += descriptor_index * r->srv_increment;
    ID3D12Device_CreateShaderResourceView(r->device, texture->resource, &srv,
                                          handle);
    return true;
}

bool pgraph_d3d12_bind_textures(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool descriptor_users_retired = false;
    /* Resolve GPU-authoritative render targets only when an enabled texture
     * actually aliases one of them. Flushing every target before every draw
     * serializes the GPU and makes the native renderer unusably slow. */
    bool flush_surfaces = false;
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES && !flush_surfaces; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            continue;
        }
        uint32_t raw_format = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        unsigned int raw_color = GET_MASK(raw_format, NV_PGRAPH_TEXFMT0_COLOR);
        unsigned int raw_dimension =
            GET_MASK(raw_format, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
        unsigned int raw_levels =
            GET_MASK(raw_format, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
        bool raw_cubemap =
            GET_MASK(raw_format, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
        if (raw_color >= ARRAY_SIZE(kelvin_color_format_info_map) ||
            !kelvin_color_format_info_map[raw_color].bytes_per_pixel ||
            raw_dimension < 1 || raw_dimension > 3 || !raw_levels ||
            (kelvin_color_format_info_map[raw_color].linear &&
             raw_dimension != 2) ||
            (raw_cubemap && raw_dimension != 2) ||
            (pgraph_is_texture_format_compressed(pg, raw_color) &&
             raw_dimension < 2)) {
            continue;
        }
        TextureShape shape = pgraph_get_texture_shape(pg, i);
        hwaddr texture_address = 0;
        size_t available = 0;
        if (!d3d12_texture_dma_range(d, i, &texture_address, &available)) {
            continue;
        }
        size_t texture_length = pgraph_get_texture_length(pg, &shape);
        texture_length = MIN(texture_length, available);
        PGRAPHD3D12RenderTarget *targets[] = {
            &r->render_targets.color,
            &r->render_targets.depth_stencil,
        };
        bool dirty[] = { pg->surface_color.draw_dirty,
                         pg->surface_zeta.draw_dirty };
        for (unsigned int target_index = 0; target_index < ARRAY_SIZE(targets);
             target_index++) {
            PGRAPHD3D12RenderTarget *target = targets[target_index];
            if (dirty[target_index] && target->resource && texture_length &&
                texture_address <
                    target->vram_address + target->storage_length &&
                target->vram_address < texture_address + texture_length) {
                flush_surfaces = true;
                break;
            }
        }
    }
    if (flush_surfaces && !pgraph_d3d12_surface_flush(d, errp)) {
        return false;
    }
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            continue;
        }
        uint32_t format_register = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        unsigned int color_format =
            GET_MASK(format_register, NV_PGRAPH_TEXFMT0_COLOR);
        unsigned int dimensionality =
            GET_MASK(format_register, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);
        bool cubemap =
            GET_MASK(format_register, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE);
        unsigned int levels =
            GET_MASK(format_register, NV_PGRAPH_TEXFMT0_MIPMAP_LEVELS);
        if (color_format >= ARRAY_SIZE(kelvin_color_format_info_map) ||
            !kelvin_color_format_info_map[color_format].bytes_per_pixel ||
            dimensionality < 1 || dimensionality > 3 ||
            (kelvin_color_format_info_map[color_format].linear &&
             dimensionality != 2) ||
            (cubemap && dimensionality != 2) ||
            (pgraph_is_texture_format_compressed(pg, color_format) &&
             dimensionality < 2) ||
            !levels) {
            error_setg(errp, "D3D12: texture %u has an invalid format/layout",
                       i);
            return false;
        }
        TextureShape shape = pgraph_get_texture_shape(pg, i);
        hwaddr address = 0;
        size_t dma_available = 0;
        if (!d3d12_texture_dma_range(d, i, &address, &dma_available)) {
            error_setg(errp, "D3D12: texture %u has an invalid DMA object", i);
            return false;
        }
        size_t length = pgraph_get_texture_length(pg, &shape);
        uint64_t vram_size = memory_region_size(d->vram);
        if (address > vram_size || length > vram_size - address ||
            length > dma_available) {
            error_setg(errp, "D3D12: texture %u exceeds VRAM", i);
            return false;
        }

        /* Resource upload follows the same cache key used by GL/Vulkan.  The
         * decoded GPU resource is created when the draw pipeline requests its
         * SRV; sampler descriptors are valid immediately. */
        PGRAPHD3D12Texture *texture = &r->textures[i];
        size_t palette_length = 0;
        hwaddr palette_address = 0;
        bool indexed =
            shape.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8;
        bool palette_valid =
            !indexed ||
            d3d12_palette_dma_range(d, i, &palette_address, &palette_length);
        bool changed = !texture->resource || pg->texture_dirty[i] ||
                       texture->vram_address != address ||
                       texture->vram_length != length ||
                       texture->palette_address != palette_address ||
                       texture->palette_length != palette_length ||
                       memcmp(&texture->shape, &shape, sizeof(shape));
        if (!changed && d3d12_texture_memory_dirty(d, address, length)) {
            d3d12_mark_overlapping_textures_dirty(pg, r, address, length);
            changed = true;
        }
        if (!changed && palette_length &&
            d3d12_texture_memory_dirty(d, palette_address, palette_length)) {
            d3d12_mark_overlapping_textures_dirty(pg, r, palette_address,
                                                  palette_length);
            changed = true;
        }
        if (!changed) {
            if (!d3d12_update_sampler(r, pg, i, &descriptor_users_retired,
                                      errp)) {
                return false;
            }
            continue;
        }
        uint64_t hash = fast_hash(d->vram_ptr + address, length);
        uint64_t palette_hash =
            palette_length && palette_valid ?
                fast_hash(d->vram_ptr + palette_address, palette_length) :
                0;
        if (!texture->resource || texture->data_hash != hash ||
            texture->vram_address != address ||
            texture->vram_length != length ||
            texture->palette_hash != palette_hash ||
            texture->palette_address != palette_address ||
            texture->palette_length != palette_length ||
            memcmp(&texture->shape, &shape, sizeof(shape))) {
            if (!d3d12_retire_descriptor_users(r, &descriptor_users_retired,
                                               errp)) {
                return false;
            }
            d3d12_release_texture(texture);
            texture->shape = shape;
            texture->vram_address = address;
            texture->vram_length = length;
            texture->palette_address = palette_address;
            texture->palette_length = palette_length;
            texture->data_hash = hash;
            texture->palette_hash = palette_hash;
            const uint8_t *palette =
                palette_valid ? d->vram_ptr + palette_address : NULL;
            if (indexed && !palette) {
                error_setg(errp, "D3D12: texture %u has an invalid palette", i);
                d3d12_release_texture(texture);
                return false;
            }
            D3D12TextureLayout layout = { 0 };
            if (!d3d12_decode_texture_layout(pg, &shape, d->vram_ptr + address,
                                             palette, &layout, errp) ||
                !d3d12_upload_texture(r, texture, i, &layout, errp)) {
                d3d12_free_texture_layout(&layout);
                d3d12_release_texture(texture);
                return false;
            }
            d3d12_free_texture_layout(&layout);
        }
        pg->texture_dirty[i] = false;
        if (!d3d12_update_sampler(r, pg, i, &descriptor_users_retired, errp)) {
            return false;
        }
    }
    return true;
}
