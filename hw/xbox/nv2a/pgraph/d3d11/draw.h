/*
 * GeForce NV2A PGRAPH Direct3D 11 draw path
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_DRAW_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_DRAW_H

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;

typedef struct PGRAPHD3D11DrawState {
    ID3D11RasterizerState *rasterizer_state;
    D3D11_RASTERIZER_DESC rasterizer_desc;
    bool rasterizer_state_valid;
    bool skip_draw;
    bool prepared;
    ID3D11Buffer *vertex_buffer;
    ID3D11Buffer *index_buffer;
    size_t vertex_buffer_size;
    size_t index_buffer_size;
} PGRAPHD3D11DrawState;

bool pgraph_d3d11_draw_init(PGRAPHD3D11State *r, Error **errp);
void pgraph_d3d11_draw_finalize(PGRAPHD3D11State *r);
void pgraph_d3d11_draw_trim(PGRAPHD3D11State *r);
bool pgraph_d3d11_update_rasterizer(NV2AState *d, Error **errp);
bool pgraph_d3d11_flush_draw(NV2AState *d, Error **errp);

#endif
