/*
 * GeForce NV2A PGRAPH Direct3D 11 render targets
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_SURFACES_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_SURFACES_H

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;
typedef struct MemAccessCallback MemAccessCallback;

typedef struct PGRAPHD3D11RenderTarget {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11DepthStencilView *dsv;
    hwaddr vram_address;
    uint32_t width;
    uint32_t height;
    uint32_t guest_width;
    uint32_t guest_height;
    uint32_t pitch;
    size_t storage_length;
    DXGI_FORMAT format;
    unsigned int nv2a_format;
    bool z_format;
    MemAccessCallback *access_cb;
} PGRAPHD3D11RenderTarget;

typedef struct PGRAPHD3D11RenderTargets {
    PGRAPHD3D11RenderTarget color;
    PGRAPHD3D11RenderTarget depth_stencil;
    ID3D11BlendState *blend_state;
    ID3D11DepthStencilState *depth_stencil_state;
    D3D11_BLEND_DESC1 blend_desc;
    D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
    float blend_factor[4];
    UINT stencil_ref;
    bool blend_state_valid;
    bool depth_stencil_state_valid;
} PGRAPHD3D11RenderTargets;

void pgraph_d3d11_surfaces_finalize(PGRAPHD3D11State *r);
bool pgraph_d3d11_render_targets_update(NV2AState *d, bool color, bool zeta,
                                        Error **errp);
bool pgraph_d3d11_update_output_merger(NV2AState *d, Error **errp);
void pgraph_d3d11_bind_render_targets(PGRAPHD3D11State *r);
bool pgraph_d3d11_render_targets_clear(NV2AState *d, uint32_t parameter,
                                       Error **errp);
bool pgraph_d3d11_surface_flush(NV2AState *d, Error **errp);
void pgraph_d3d11_surface_invalidate_range(NV2AState *d, hwaddr address,
                                           size_t length);
#endif
