/*
 * Geforce NV2A PGRAPH Direct3D 12 render targets
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_SURFACES_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_SURFACES_H

#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef WIDL_C_INLINE_WRAPPERS
#define WIDL_C_INLINE_WRAPPERS
#endif
#include <d3d12.h>

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D12State PGRAPHD3D12State;
typedef struct MemAccessCallback MemAccessCallback;

typedef struct PGRAPHD3D12RenderTarget {
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
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
} PGRAPHD3D12RenderTarget;

typedef struct PGRAPHD3D12RenderTargets {
    PGRAPHD3D12RenderTarget color;
    PGRAPHD3D12RenderTarget depth_stencil;
    D3D12_BLEND_DESC blend;
    D3D12_DEPTH_STENCIL_DESC depth_stencil_desc;
    float blend_factor[4];
    UINT stencil_ref;
} PGRAPHD3D12RenderTargets;

bool pgraph_d3d12_surfaces_init(PGRAPHD3D12State *r, Error **errp);
void pgraph_d3d12_surfaces_finalize(PGRAPHD3D12State *r);
bool pgraph_d3d12_surface_update(NV2AState *d, bool color, bool zeta,
                                 Error **errp);
bool pgraph_d3d12_update_output_merger(NV2AState *d, Error **errp);
void pgraph_d3d12_configure_pipeline(
    PGRAPHD3D12State *r, D3D12_GRAPHICS_PIPELINE_STATE_DESC *pipeline);
void pgraph_d3d12_bind_render_targets(PGRAPHD3D12State *r,
                                      ID3D12GraphicsCommandList *list);
void pgraph_d3d12_invalidate_surface_range(NV2AState *d, hwaddr address,
                                           size_t length);
void pgraph_d3d12_convert_color_to_bgra8(unsigned int format,
                                         uint8_t *destination,
                                         size_t destination_pitch,
                                         const uint8_t *source,
                                         size_t source_pitch, uint32_t width,
                                         uint32_t height);
void pgraph_d3d12_convert_bgra8_to_color(unsigned int format,
                                         uint8_t *destination,
                                         size_t destination_pitch,
                                         const uint8_t *source,
                                         size_t source_pitch, uint32_t width,
                                         uint32_t height);
void pgraph_d3d12_convert_z16_to_d32(float *destination,
                                     size_t destination_pitch,
                                     const uint8_t *source, size_t source_pitch,
                                     uint32_t width, uint32_t height);
void pgraph_d3d12_convert_d32_to_z16(uint8_t *destination,
                                     size_t destination_pitch,
                                     const float *source, size_t source_pitch,
                                     uint32_t width, uint32_t height);
bool pgraph_d3d12_clear_surface(NV2AState *d, uint32_t parameter, Error **errp);
bool pgraph_d3d12_surface_flush(NV2AState *d, Error **errp);


#endif
