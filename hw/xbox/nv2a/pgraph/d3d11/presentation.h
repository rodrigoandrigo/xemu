/*
 * GeForce NV2A PGRAPH Direct3D 11 presentation
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_PRESENTATION_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_PRESENTATION_H

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;

typedef struct PGRAPHD3D11PresentationState {
    ID3D11VertexShader *vertex_shader;
    ID3D11PixelShader *pixel_shader;
    ID3D11SamplerState *linear_sampler;
    ID3D11SamplerState *nearest_sampler;
    ID3D11Texture2D *scanout_texture;
    ID3D11ShaderResourceView *scanout_srv;
    uint32_t scanout_width;
    uint32_t scanout_height;
} PGRAPHD3D11PresentationState;

bool pgraph_d3d11_presentation_init(PGRAPHD3D11State *r, Error **errp);
void pgraph_d3d11_presentation_finalize(PGRAPHD3D11State *r);
bool pgraph_d3d11_present_color(NV2AState *d, Error **errp);

#endif
