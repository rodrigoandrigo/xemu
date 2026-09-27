/*
 * GeForce NV2A PGRAPH Direct3D 11 textures
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_TEXTURES_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_TEXTURES_H

#include "qapi/error.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/pgraph/texture.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;

typedef struct PGRAPHD3D11TextureBinding {
    ID3D11Resource *resource;
    ID3D11ShaderResourceView *view;
    uint64_t data_hash;
    hwaddr address;
    size_t length;
    TextureShape shape;
} PGRAPHD3D11TextureBinding;

typedef struct PGRAPHD3D11TextureState {
    PGRAPHD3D11TextureBinding bindings[NV2A_MAX_TEXTURES];
    ID3D11SamplerState *samplers[NV2A_MAX_TEXTURES];
    D3D11_SAMPLER_DESC sampler_descs[NV2A_MAX_TEXTURES];
    bool sampler_valid[NV2A_MAX_TEXTURES];
} PGRAPHD3D11TextureState;

bool pgraph_d3d11_textures_init(PGRAPHD3D11State *r, Error **errp);
void pgraph_d3d11_textures_finalize(PGRAPHD3D11State *r);
void pgraph_d3d11_textures_trim(PGRAPHD3D11State *r);
bool pgraph_d3d11_bind_textures(NV2AState *d, Error **errp);

#endif
