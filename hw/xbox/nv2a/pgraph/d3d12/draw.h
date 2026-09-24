/*
 * Geforce NV2A PGRAPH Direct3D 12 draw path
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_DRAW_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_DRAW_H

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D12State PGRAPHD3D12State;

bool pgraph_d3d12_draw_init(PGRAPHD3D12State *r, Error **errp);
void pgraph_d3d12_draw_finalize(PGRAPHD3D12State *r);
bool pgraph_d3d12_flush_draw(NV2AState *d, Error **errp);
bool pgraph_d3d12_draw_collect(PGRAPHD3D12State *r, bool wait, Error **errp);

#endif
