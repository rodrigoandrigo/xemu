/*
 * Geforce NV2A PGRAPH Direct3D 12 Renderer
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_RENDERER_H

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d12.h>
#include <dxgi1_4.h>

#include "qemu/thread.h"
#include "reports.h"
#include "shaders.h"
#include "surfaces.h"
#include "draw.h"

typedef struct PGRAPHD3D12Texture PGRAPHD3D12Texture;
typedef struct PGRAPHD3D12DrawState PGRAPHD3D12DrawState;

#define D3D12_FRAME_COUNT 2

typedef struct PGRAPHD3D12Frame {
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *command_list;
    ID3D12Resource *render_target;
    ID3D12Resource *upload_buffer;
    uint8_t *upload_data;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_footprint;
    D3D12_RESOURCE_STATES render_target_state;
    uint64_t fence_value;
} PGRAPHD3D12Frame;

typedef enum PGRAPHD3D12SurfaceFormat {
    PGRAPH_D3D12_SURFACE_INVALID,
    PGRAPH_D3D12_SURFACE_INDEX8,
    PGRAPH_D3D12_SURFACE_X1R5G5B5,
    PGRAPH_D3D12_SURFACE_R5G6B5,
    PGRAPH_D3D12_SURFACE_X8R8G8B8,
} PGRAPHD3D12SurfaceFormat;

typedef struct PGRAPHD3D12Surface {
    const uint8_t *data;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    PGRAPHD3D12SurfaceFormat format;
} PGRAPHD3D12Surface;

struct PGRAPHD3D12State {
    IDXGIFactory4 *factory;
    IDXGIAdapter3 *adapter;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    ID3D12QueryHeap *query_heap;
    ID3D12Resource *query_readback;
    ID3D12DescriptorHeap *rtv_heap;
    ID3D12DescriptorHeap *render_target_rtv_heap;
    ID3D12DescriptorHeap *dsv_heap;
    ID3D12DescriptorHeap *srv_heap;
    ID3D12DescriptorHeap *sampler_heap;
    ID3D12DescriptorHeap *hud_srv_heap;
    IDXGISwapChain3 *swapchain;
    HANDLE fence_event;
    UINT rtv_increment;
    UINT frame_index;
    UINT output_width;
    UINT output_height;
    DXGI_FORMAT z24s8_format;
    uint64_t next_fence_value;
    QemuMutex queue_lock;
    QSIMPLEQ_HEAD(, PGRAPHD3D12QueryReport) report_queue;
    uint32_t query_count;
    uint32_t max_query_count;
    uint64_t zpass_pixel_count;
    bool query_active;
    bool device_lost;
    bool memory_pressure;
    bool hud_initialized;
    bool draw_prepared;
    bool output_merger_logic_op;
    uint64_t memory_budget;
    uint64_t memory_usage;
    PGRAPHD3D12ShaderCache *shader_cache;
    PGRAPHD3D12DrawState *draw;
    PGRAPHD3D12RenderTargets render_targets;
    PGRAPHD3D12Texture *textures;
    UINT srv_increment;
    UINT sampler_increment;
    PGRAPHD3D12Frame frames[D3D12_FRAME_COUNT];
};

bool pgraph_d3d12_note_device_error(PGRAPHD3D12State *r, HRESULT hr,
                                    const char *operation);
bool pgraph_d3d12_make_resident(PGRAPHD3D12State *r, ID3D12Pageable *resource,
                                Error **errp);

#endif
