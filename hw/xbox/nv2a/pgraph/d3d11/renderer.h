/*
 * GeForce NV2A PGRAPH Direct3D 11 Renderer
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_RENDERER_H

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d10.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>

#include "surfaces.h"
#include "draw.h"
#include "shaders.h"
#include "textures.h"
#include "reports.h"
#include "presentation.h"

struct PGRAPHD3D11State {
    IDXGIFactory4 *factory;
    IDXGIAdapter1 *adapter;
    ID3D11Device *device;
    ID3D11Device1 *device1;
    ID3D11DeviceContext *context;
    IDXGISwapChain3 *swapchain;
    ID3D11Texture2D *backbuffer;
    ID3D11RenderTargetView *backbuffer_rtv;
    ID3D11Query *idle_query;
    PGRAPHD3D11DrawState *draw;
    PGRAPHD3D11ShaderState *shaders;
    PGRAPHD3D11TextureState *textures;
    PGRAPHD3D11ReportState *reports;
    PGRAPHD3D11PresentationState *presentation;
    PGRAPHD3D11RenderTargets render_targets;
    UINT output_width;
    UINT output_height;
    D3D_FEATURE_LEVEL feature_level;
    bool output_merger_logic_op;
    bool device_lost;
    bool resources_ready;
    bool app_suspended;
    bool memory_suspended;
    bool logged_draw_prepared;
    bool logged_draw_submitted;
    bool logged_present_without_color;
    bool logged_present_with_color;
    int64_t next_recovery_us;
};

#endif
