/*
 * Geforce NV2A PGRAPH Direct3D 12 Textures
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_TEXTURES_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_TEXTURES_H

#include "hw/xbox/nv2a/pgraph/texture.h"

typedef struct PGRAPHD3D12Texture {
    ID3D12Resource *resource;
    ID3D12Resource *upload;
    D3D12_RESOURCE_STATES state;
    TextureShape shape;
    hwaddr vram_address;
    size_t vram_length;
    hwaddr palette_address;
    size_t palette_length;
    uint64_t data_hash;
    uint64_t palette_hash;
    DXGI_FORMAT format;
    UINT component_mapping;
    D3D12_SAMPLER_DESC sampler;
    bool sampler_valid;
} PGRAPHD3D12Texture;

bool pgraph_d3d12_textures_init(PGRAPHD3D12State *r, Error **errp);
void pgraph_d3d12_textures_finalize(PGRAPHD3D12State *r);
void pgraph_d3d12_textures_trim(PGRAPHD3D12State *r);
bool pgraph_d3d12_bind_textures(NV2AState *d, Error **errp);

#endif
