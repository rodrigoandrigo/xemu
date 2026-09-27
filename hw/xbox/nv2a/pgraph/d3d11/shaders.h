/*
 * GeForce NV2A PGRAPH Direct3D 11 shader translation
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_SHADERS_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_SHADERS_H

#include "qapi/error.h"

typedef struct PGRAPHState PGRAPHState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;

typedef struct PGRAPHD3D11VertexShader {
    ID3D11VertexShader *shader;
    ID3DBlob *bytecode;
    ID3D11InputLayout *input_layout;
    uint64_t last_used;
} PGRAPHD3D11VertexShader;

typedef struct PGRAPHD3D11PixelShader {
    ID3D11PixelShader *shader;
    ID3DBlob *bytecode;
    uint64_t last_used;
} PGRAPHD3D11PixelShader;

typedef struct PGRAPHD3D11ShaderState {
    GHashTable *vertex_cache;
    GHashTable *pixel_cache;
    GHashTable *failed_vertex_cache;
    GHashTable *failed_pixel_cache;
    PGRAPHD3D11VertexShader *vertex;
    PGRAPHD3D11PixelShader *pixel;
    ID3D11Buffer *vertex_constants;
    ID3D11Buffer *pixel_constants;
    char *cache_directory;
    uint64_t use_serial;
} PGRAPHD3D11ShaderState;

bool pgraph_d3d11_shaders_init(PGRAPHD3D11State *r, Error **errp);
void pgraph_d3d11_shaders_finalize(PGRAPHD3D11State *r);
void pgraph_d3d11_shaders_trim(PGRAPHD3D11State *r);
bool pgraph_d3d11_bind_vertex_shader(PGRAPHState *pg, Error **errp);
bool pgraph_d3d11_bind_pixel_shader(PGRAPHState *pg, Error **errp);

#endif
