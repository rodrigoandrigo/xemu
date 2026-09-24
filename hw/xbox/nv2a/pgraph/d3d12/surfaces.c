/*
 * Geforce NV2A PGRAPH Direct3D 12 render targets
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"
#include "surfaces.h"

static bool d3d12_upload_target(NV2AState *d, bool color, Error **errp);
static unsigned int d3d12_color_bytes_per_pixel(unsigned int format);

static void d3d12_release_target(PGRAPHD3D12RenderTarget *target)
{
    if (target->access_cb && tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), target->access_cb);
    }
    if (target->resource) {
        ID3D12Resource_Release(target->resource);
    }
    memset(target, 0, sizeof(*target));
}

static bool d3d12_ranges_overlap(hwaddr first, size_t first_length,
                                 hwaddr second, size_t second_length)
{
    uint64_t first_end = first + first_length;
    uint64_t second_end = second + second_length;
    return first < second_end && second < first_end;
}

static void d3d12_surface_access_callback(void *opaque, MemoryRegion *mr,
                                          hwaddr address, hwaddr length,
                                          bool write)
{
    NV2AState *d = opaque;
    (void)mr;
    PGRAPHState *pg = &d->pgraph;
    bool synchronize = false;

    qemu_mutex_lock(&pg->lock);
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    if (r) {
        PGRAPHD3D12RenderTarget *targets[] = {
            &r->render_targets.color,
            &r->render_targets.depth_stencil,
        };
        for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
            PGRAPHD3D12RenderTarget *target = targets[i];
            size_t target_length = target->storage_length;
            if (!target->resource ||
                !d3d12_ranges_overlap(address, length, target->vram_address,
                                      target_length)) {
                continue;
            }
            Surface *surface = i == 0 ? &pg->surface_color : &pg->surface_zeta;
            synchronize |= surface->draw_dirty;
            if (write) {
                surface->buffer_dirty = true;
            }
        }
    }
    qemu_mutex_unlock(&pg->lock);

    if (synchronize) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&pg->flush_complete);
        qatomic_set(&pg->flush_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&pg->flush_complete);
    }
}

static D3D12_COMPARISON_FUNC d3d12_comparison(unsigned int value)
{
    static const D3D12_COMPARISON_FUNC map[] = {
        D3D12_COMPARISON_FUNC_NEVER,         D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_EQUAL,         D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,       D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D12_COMPARISON_FUNC_ALWAYS;
}

static D3D12_STENCIL_OP d3d12_stencil_op(unsigned int value)
{
    static const D3D12_STENCIL_OP map[] = {
        D3D12_STENCIL_OP_KEEP,     D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_ZERO,     D3D12_STENCIL_OP_REPLACE,
        D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_DECR_SAT,
        D3D12_STENCIL_OP_INVERT,   D3D12_STENCIL_OP_INCR,
        D3D12_STENCIL_OP_DECR,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D12_STENCIL_OP_KEEP;
}

static D3D12_BLEND d3d12_blend_factor(unsigned int value)
{
    static const D3D12_BLEND map[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_ZERO,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_ALPHA_FACTOR,  D3D12_BLEND_INV_ALPHA_FACTOR,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D12_BLEND_ONE;
}

static D3D12_LOGIC_OP d3d12_logic_op(unsigned int value)
{
    static const D3D12_LOGIC_OP map[] = {
        D3D12_LOGIC_OP_CLEAR,         D3D12_LOGIC_OP_AND,
        D3D12_LOGIC_OP_AND_REVERSE,   D3D12_LOGIC_OP_COPY,
        D3D12_LOGIC_OP_AND_INVERTED,  D3D12_LOGIC_OP_NOOP,
        D3D12_LOGIC_OP_XOR,           D3D12_LOGIC_OP_OR,
        D3D12_LOGIC_OP_NOR,           D3D12_LOGIC_OP_EQUIV,
        D3D12_LOGIC_OP_INVERT,        D3D12_LOGIC_OP_OR_REVERSE,
        D3D12_LOGIC_OP_COPY_INVERTED, D3D12_LOGIC_OP_OR_INVERTED,
        D3D12_LOGIC_OP_NAND,          D3D12_LOGIC_OP_SET,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D12_LOGIC_OP_COPY;
}

static D3D12_BLEND d3d12_alpha_blend_factor(D3D12_BLEND value)
{
    switch (value) {
    case D3D12_BLEND_SRC_COLOR:
        return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR:
        return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case D3D12_BLEND_SRC_ALPHA_SAT:
        return D3D12_BLEND_ONE;
    default:
        return value;
    }
}

static D3D12_BLEND_OP d3d12_blend_op(unsigned int value)
{
    static const D3D12_BLEND_OP map[] = {
        D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT,
        D3D12_BLEND_OP_ADD,      D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX,      D3D12_BLEND_OP_REV_SUBTRACT,
        D3D12_BLEND_OP_ADD,
    };
    return value < ARRAY_SIZE(map) ? map[value] : D3D12_BLEND_OP_ADD;
}

static void d3d12_surface_dimensions(PGRAPHState *pg, uint32_t *guest_width,
                                     uint32_t *guest_height, uint32_t *width,
                                     uint32_t *height)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        *guest_width = 1U << pg->surface_shape.log_width;
        *guest_height = 1U << pg->surface_shape.log_height;
    } else {
        *guest_width = pg->surface_shape.clip_width + pg->surface_shape.clip_x;
        *guest_height =
            pg->surface_shape.clip_height + pg->surface_shape.clip_y;
    }
    *guest_width = MAX(*guest_width, 1U);
    *guest_height = MAX(*guest_height, 1U);
    /* NV2A multisample surfaces are physically enlarged in VRAM.  Keep the
     * guest dimensions at that physical size and apply only the user render
     * scale when deriving the D3D12 resource dimensions. */
    pgraph_apply_anti_aliasing_factor(pg, guest_width, guest_height);
    *width = *guest_width;
    *height = *guest_height;
    pgraph_apply_scaling_factor(pg, width, height);
}

static bool d3d12_target_address(NV2AState *d, bool color, uint32_t width,
                                 uint32_t height, unsigned int bpp,
                                 hwaddr *address, size_t *storage_length,
                                 Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    DMAObject dma = nv_dma_load(d, color ? pg->dma_color : pg->dma_zeta);
    uint64_t row_bytes = (uint64_t)width * bpp;
    uint64_t length =
        (uint64_t)height * MAX((uint64_t)surface->pitch, row_bytes);
    uint64_t end = (uint64_t)surface->offset + length;
    uint64_t vram_size = memory_region_size(d->vram);
    if (dma.dma_class != NV_DMA_IN_MEMORY_CLASS || end > dma.limit + 1ULL ||
        dma.address > vram_size || end > vram_size - dma.address) {
        error_setg(errp, "D3D12: invalid %s render-target DMA range",
                   color ? "color" : "depth/stencil");
        return false;
    }
    *address = dma.address + surface->offset;
    *storage_length = length;
    return true;
}

static void d3d12_transition(ID3D12GraphicsCommandList *list,
                             PGRAPHD3D12RenderTarget *target,
                             D3D12_RESOURCE_STATES state)
{
    if (!target->resource || target->state == state) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = target->resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = target->state,
            .StateAfter = state,
        },
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    target->state = state;
}

static bool d3d12_create_target(NV2AState *d, bool color, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12RenderTarget *target =
        color ? &r->render_targets.color : &r->render_targets.depth_stencil;
    Surface *surface = color ? &pg->surface_color : &pg->surface_zeta;
    uint32_t guest_width, guest_height, width, height;
    d3d12_surface_dimensions(pg, &guest_width, &guest_height, &width, &height);
    unsigned int nv2a_format =
        color ? pg->surface_shape.color_format : pg->surface_shape.zeta_format;
    unsigned int guest_bpp =
        color ? d3d12_color_bytes_per_pixel(nv2a_format) :
                (nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4);
    hwaddr address;
    size_t guest_length;
    if (!d3d12_target_address(d, color, guest_width, guest_height, guest_bpp,
                              &address, &guest_length, errp)) {
        return false;
    }
    DXGI_FORMAT format = color ?
                             DXGI_FORMAT_B8G8R8A8_UNORM :
                             (nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ?
                                  DXGI_FORMAT_D32_FLOAT :
                                  r->z24s8_format);
    surface->buffer_dirty |= memory_region_test_and_clear_dirty(
        d->vram, address, guest_length, DIRTY_MEMORY_NV2A);
    if (target->resource && target->vram_address == address &&
        target->width == width && target->height == height &&
        target->guest_width == guest_width &&
        target->guest_height == guest_height &&
        target->pitch == surface->pitch &&
        target->storage_length == guest_length && target->format == format &&
        target->nv2a_format == nv2a_format &&
        (color || target->z_format == pg->surface_shape.z_format)) {
        if (!surface->buffer_dirty) {
            return true;
        }
        if (!d3d12_upload_target(d, color, errp)) {
            return false;
        }
        surface->buffer_dirty = false;
        return true;
    }

    /* The target and its RTV/DSV descriptor may still be referenced by an
     * asynchronous draw. D3D12 does not retain application-owned resource or
     * descriptor storage for us, so retire those draws before replacing it. */
    if (target->resource && !pgraph_d3d12_draw_collect(r, true, errp)) {
        return false;
    }
    d3d12_release_target(target);
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = color ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET :
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
    };
    D3D12_CLEAR_VALUE clear = { .Format = format };
    if (!color) {
        clear.DepthStencil.Depth = 1.0f;
    }
    D3D12_RESOURCE_STATES initial = color ? D3D12_RESOURCE_STATE_RENDER_TARGET :
                                            D3D12_RESOURCE_STATE_DEPTH_WRITE;
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, initial, &clear,
        &IID_ID3D12Resource, (void **)&target->resource);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: %s render target creation failed (0x%08lx)",
                   color ? "color" : "depth/stencil", (unsigned long)hr);
        return false;
    }
    if (!pgraph_d3d12_make_resident(r, (ID3D12Pageable *)target->resource,
                                    errp)) {
        ID3D12Resource_Release(target->resource);
        target->resource = NULL;
        return false;
    }
    target->state = initial;
    target->vram_address = address;
    target->width = width;
    target->height = height;
    target->guest_width = guest_width;
    target->guest_height = guest_height;
    target->pitch = surface->pitch;
    target->storage_length = guest_length;
    target->format = format;
    target->nv2a_format = nv2a_format;
    target->z_format = !color && pg->surface_shape.z_format;

    if (color) {
        D3D12_RENDER_TARGET_VIEW_DESC view = {
            .Format = format,
            .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
        };
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                r->render_target_rtv_heap);
        ID3D12Device_CreateRenderTargetView(r->device, target->resource, &view,
                                            handle);
    } else {
        D3D12_DEPTH_STENCIL_VIEW_DESC view = {
            .Format = format,
            .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
        };
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                r->dsv_heap);
        ID3D12Device_CreateDepthStencilView(r->device, target->resource, &view,
                                            handle);
    }
    if (!d3d12_upload_target(d, color, errp)) {
        d3d12_release_target(target);
        return false;
    }
    if (tcg_enabled()) {
        target->access_cb = mem_access_callback_insert(
            qemu_get_cpu(0), d->vram, target->vram_address,
            target->storage_length, &d3d12_surface_access_callback, d);
    }
    surface->buffer_dirty = false;
    return true;
}

bool pgraph_d3d12_surfaces_init(PGRAPHD3D12State *r, Error **errp)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {
        .Format = DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    HRESULT hr = ID3D12Device_CheckFeatureSupport(
        r->device, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (SUCCEEDED(hr) &&
        (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
        r->z24s8_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    } else {
        support.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        hr = ID3D12Device_CheckFeatureSupport(
            r->device, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (FAILED(hr) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
            error_setg(errp, "D3D12: no compatible Z24S8 depth-stencil format");
            return false;
        }
        r->z24s8_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = 1,
    };
    hr = ID3D12Device_CreateDescriptorHeap(r->device, &rtv,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&r->render_target_rtv_heap);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: render-target RTV heap creation failed");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC dsv = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
    };
    hr = ID3D12Device_CreateDescriptorHeap(
        r->device, &dsv, &IID_ID3D12DescriptorHeap, (void **)&r->dsv_heap);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: depth-stencil heap creation failed");
        ID3D12DescriptorHeap_Release(r->render_target_rtv_heap);
        r->render_target_rtv_heap = NULL;
        return false;
    }
    return true;
}

void pgraph_d3d12_surfaces_finalize(PGRAPHD3D12State *r)
{
    d3d12_release_target(&r->render_targets.color);
    d3d12_release_target(&r->render_targets.depth_stencil);
    if (r->dsv_heap) {
        ID3D12DescriptorHeap_Release(r->dsv_heap);
        r->dsv_heap = NULL;
    }
    if (r->render_target_rtv_heap) {
        ID3D12DescriptorHeap_Release(r->render_target_rtv_heap);
        r->render_target_rtv_heap = NULL;
    }
}

void pgraph_d3d12_invalidate_surface_range(NV2AState *d, hwaddr address,
                                           size_t length)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint64_t end = address + length;
    PGRAPHD3D12RenderTarget *targets[] = {
        &r->render_targets.color,
        &r->render_targets.depth_stencil,
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(targets); i++) {
        PGRAPHD3D12RenderTarget *target = targets[i];
        uint64_t target_end = target->vram_address + target->storage_length;
        if (target->resource && address < target_end &&
            target->vram_address < end) {
            Error *error = NULL;
            if (!pgraph_d3d12_draw_collect(r, true, &error)) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   error ? error_get_pretty(error) :
                                           "D3D12: failed to retire a surface "
                                           "before invalidation");
                error_free(error);
                return;
            }
            d3d12_release_target(target);
            if (i == 0) {
                d->pgraph.surface_color.buffer_dirty = true;
            } else {
                d->pgraph.surface_zeta.buffer_dirty = true;
            }
        }
    }
}

bool pgraph_d3d12_surface_update(NV2AState *d, bool color, bool zeta,
                                 Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);
    if (color && pg->surface_shape.color_format &&
        !d3d12_create_target(d, true, errp)) {
        return false;
    }
    if (zeta && pg->surface_shape.zeta_format &&
        !d3d12_create_target(d, false, errp)) {
        return false;
    }
    pg->draw_time++;
    return true;
}

bool pgraph_d3d12_update_output_merger(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12RenderTargets *targets = &r->render_targets;
    uint32_t control0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);

    memset(&targets->blend, 0, sizeof(targets->blend));
    D3D12_RENDER_TARGET_BLEND_DESC *rt = &targets->blend.RenderTarget[0];
    rt->LogicOpEnable = !!(blend & NV_PGRAPH_BLEND_LOGICOP_ENABLE);
    if (rt->LogicOpEnable && !r->output_merger_logic_op) {
        error_setg(errp, "D3D12: the active adapter does not support "
                         "output-merger logic operations");
        return false;
    }
    /* D3D12 does not permit blending and a logic operation to be enabled on
     * the same render target. NV2A logic operations take precedence. */
    rt->BlendEnable = !rt->LogicOpEnable && !!(blend & NV_PGRAPH_BLEND_EN);
    rt->SrcBlend = d3d12_blend_factor(GET_MASK(blend, NV_PGRAPH_BLEND_SFACTOR));
    rt->DestBlend =
        d3d12_blend_factor(GET_MASK(blend, NV_PGRAPH_BLEND_DFACTOR));
    rt->BlendOp = d3d12_blend_op(GET_MASK(blend, NV_PGRAPH_BLEND_EQN));
    rt->SrcBlendAlpha = d3d12_alpha_blend_factor(rt->SrcBlend);
    rt->DestBlendAlpha = d3d12_alpha_blend_factor(rt->DestBlend);
    rt->BlendOpAlpha = rt->BlendOp;
    rt->LogicOp = d3d12_logic_op(GET_MASK(blend, NV_PGRAPH_BLEND_LOGICOP));
    rt->RenderTargetWriteMask =
        ((control0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) ?
             D3D12_COLOR_WRITE_ENABLE_RED :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) ?
             D3D12_COLOR_WRITE_ENABLE_GREEN :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE) ?
             D3D12_COLOR_WRITE_ENABLE_BLUE :
             0) |
        ((control0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) ?
             D3D12_COLOR_WRITE_ENABLE_ALPHA :
             0);
    switch (pg->surface_shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        break;
    default:
        /* X/R/G/B-only targets have no writable guest alpha channel. Keep
         * their expanded host alpha at the fixed value established during
         * upload so destination-alpha blending remains stable across draws. */
        rt->RenderTargetWriteMask &= ~D3D12_COLOR_WRITE_ENABLE_ALPHA;
        break;
    }
    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                     targets->blend_factor);

    D3D12_DEPTH_STENCIL_DESC *ds = &targets->depth_stencil_desc;
    memset(ds, 0, sizeof(*ds));
    ds->DepthEnable = !!(control0 & NV_PGRAPH_CONTROL_0_ZENABLE);
    ds->DepthWriteMask = control0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE ?
                             D3D12_DEPTH_WRITE_MASK_ALL :
                             D3D12_DEPTH_WRITE_MASK_ZERO;
    ds->DepthFunc =
        d3d12_comparison(GET_MASK(control0, NV_PGRAPH_CONTROL_0_ZFUNC));
    ds->StencilEnable = !!(control1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);
    ds->StencilReadMask =
        GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
    ds->StencilWriteMask =
        GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    targets->stencil_ref = GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
    D3D12_DEPTH_STENCILOP_DESC stencil = {
        .StencilFailOp = d3d12_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL)),
        .StencilDepthFailOp = d3d12_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL)),
        .StencilPassOp = d3d12_stencil_op(
            GET_MASK(control2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS)),
        .StencilFunc = d3d12_comparison(
            GET_MASK(control1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC)),
    };
    ds->FrontFace = stencil;
    ds->BackFace = stencil;

    return pgraph_d3d12_surface_update(d, rt->RenderTargetWriteMask != 0,
                                       ds->DepthEnable || ds->StencilEnable,
                                       errp);
}

void pgraph_d3d12_configure_pipeline(
    PGRAPHD3D12State *r, D3D12_GRAPHICS_PIPELINE_STATE_DESC *pipeline)
{
    pipeline->BlendState = r->render_targets.blend;
    pipeline->DepthStencilState = r->render_targets.depth_stencil_desc;
    pipeline->SampleDesc.Count = 1;
    pipeline->SampleDesc.Quality = 0;

    /* D3D12 validates enum fields even when their corresponding feature is
     * disabled.  NV2A state starts zeroed, while zero is not a valid value for
     * D3D12_BLEND, D3D12_BLEND_OP, D3D12_LOGIC_OP, D3D12_COMPARISON_FUNC or
     * D3D12_STENCIL_OP.  Supply canonical inactive values so the first Xbox
     * draws can create a PSO before all output-merger registers have been
     * programmed. */
    for (unsigned int i = 0;
         i < ARRAY_SIZE(pipeline->BlendState.RenderTarget); i++) {
        D3D12_RENDER_TARGET_BLEND_DESC *blend =
            &pipeline->BlendState.RenderTarget[i];
        if (!blend->SrcBlend) {
            blend->SrcBlend = D3D12_BLEND_ONE;
        }
        if (!blend->DestBlend) {
            blend->DestBlend = D3D12_BLEND_ZERO;
        }
        if (!blend->BlendOp) {
            blend->BlendOp = D3D12_BLEND_OP_ADD;
        }
        if (!blend->SrcBlendAlpha) {
            blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        }
        if (!blend->DestBlendAlpha) {
            blend->DestBlendAlpha = D3D12_BLEND_ZERO;
        }
        if (!blend->BlendOpAlpha) {
            blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        if (!blend->LogicOp) {
            blend->LogicOp = D3D12_LOGIC_OP_NOOP;
        }
    }
    D3D12_DEPTH_STENCIL_DESC *depth = &pipeline->DepthStencilState;
    if (!depth->DepthFunc) {
        depth->DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_DEPTH_STENCILOP_DESC *faces[] = {
        &depth->FrontFace,
        &depth->BackFace,
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(faces); i++) {
        if (!faces[i]->StencilFailOp) {
            faces[i]->StencilFailOp = D3D12_STENCIL_OP_KEEP;
        }
        if (!faces[i]->StencilDepthFailOp) {
            faces[i]->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        }
        if (!faces[i]->StencilPassOp) {
            faces[i]->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        }
        if (!faces[i]->StencilFunc) {
            faces[i]->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
    }
    if (r->render_targets.color.resource) {
        pipeline->NumRenderTargets = 1;
        pipeline->RTVFormats[0] = r->render_targets.color.format;
    } else {
        pipeline->NumRenderTargets = 0;
        pipeline->RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    }
    pipeline->DSVFormat = r->render_targets.depth_stencil.resource ?
                              r->render_targets.depth_stencil.format :
                              DXGI_FORMAT_UNKNOWN;
    if (!r->render_targets.depth_stencil.resource) {
        pipeline->DepthStencilState.DepthEnable = FALSE;
        pipeline->DepthStencilState.StencilEnable = FALSE;
    }
}

void pgraph_d3d12_bind_render_targets(PGRAPHD3D12State *r,
                                      ID3D12GraphicsCommandList *list)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = { 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = { 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE *rtv_ptr = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE *dsv_ptr = NULL;
    if (r->render_targets.color.resource) {
        d3d12_transition(list, &r->render_targets.color,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            r->render_target_rtv_heap);
        rtv_ptr = &rtv;
    }
    if (r->render_targets.depth_stencil.resource) {
        D3D12_RESOURCE_STATES state =
            r->render_targets.depth_stencil_desc.DepthWriteMask ==
                        D3D12_DEPTH_WRITE_MASK_ALL ||
                    r->render_targets.depth_stencil_desc.StencilWriteMask ?
                D3D12_RESOURCE_STATE_DEPTH_WRITE :
                D3D12_RESOURCE_STATE_DEPTH_READ;
        d3d12_transition(list, &r->render_targets.depth_stencil, state);
        dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            r->dsv_heap);
        dsv_ptr = &dsv;
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, rtv_ptr ? 1 : 0, rtv_ptr,
                                                 FALSE, dsv_ptr);
    ID3D12GraphicsCommandList_OMSetBlendFactor(list,
                                               r->render_targets.blend_factor);
    ID3D12GraphicsCommandList_OMSetStencilRef(list,
                                              r->render_targets.stencil_ref);
}

static bool d3d12_execute_clear(PGRAPHD3D12State *r,
                                ID3D12GraphicsCommandList *list, Error **errp)
{
    HRESULT hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: render-target clear command close failed");
        return false;
    }
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    qemu_mutex_lock(&r->queue_lock);
    ID3D12CommandQueue_ExecuteCommandLists(r->queue, 1, lists);
    uint64_t value = r->next_fence_value++;
    hr = ID3D12CommandQueue_Signal(r->queue, r->fence, value);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Fence_SetEventOnCompletion(r->fence, value, r->fence_event);
    }
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObjectEx(r->fence_event, 5000, FALSE) !=
            WAIT_OBJECT_0) {
            hr = ID3D12Device_GetDeviceRemovedReason(r->device);
            if (SUCCEEDED(hr)) {
                hr = DXGI_ERROR_DEVICE_HUNG;
            }
        }
    }
    qemu_mutex_unlock(&r->queue_lock);
    if (FAILED(hr)) {
        pgraph_d3d12_note_device_error(r, hr, "render-target clear");
        error_setg(errp, "D3D12: render-target clear synchronization failed");
        return false;
    }
    return true;
}

static bool d3d12_upload_target(NV2AState *d, bool color, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12RenderTarget *target =
        color ? &r->render_targets.color : &r->render_targets.depth_stencil;
    if (!target->resource) {
        return true;
    }
    unsigned int guest_bpp =
        color ?
            d3d12_color_bytes_per_pixel(target->nv2a_format) :
            (target->nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4);
    size_t guest_linear_pitch = (size_t)target->guest_width * guest_bpp;
    uint8_t *guest_linear = g_malloc(guest_linear_pitch * target->guest_height);
    const uint8_t *vram = d->vram_ptr + target->vram_address;
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        unswizzle_rect(vram, target->guest_width, target->guest_height,
                       guest_linear, guest_linear_pitch, guest_bpp);
    } else {
        for (uint32_t y = 0; y < target->guest_height; y++) {
            memcpy(guest_linear + y * guest_linear_pitch,
                   vram + (size_t)y * target->pitch, guest_linear_pitch);
        }
    }

    unsigned int host_bpp =
        color || target->format == DXGI_FORMAT_D32_FLOAT ?
            4 :
            (target->format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8 : 4);
    size_t host_pitch = (size_t)target->width * host_bpp;
    uint8_t *host = g_malloc0(host_pitch * target->height);
    uint8_t *guest_bgra = NULL;
    if (color) {
        guest_bgra =
            g_malloc((size_t)target->guest_width * target->guest_height * 4);
        pgraph_d3d12_convert_color_to_bgra8(
            target->nv2a_format, guest_bgra, (size_t)target->guest_width * 4,
            guest_linear, guest_linear_pitch, target->guest_width,
            target->guest_height);
    }
    for (uint32_t y = 0; y < target->height; y++) {
        uint32_t sy = (uint64_t)y * target->guest_height / target->height;
        for (uint32_t x = 0; x < target->width; x++) {
            uint32_t sx = (uint64_t)x * target->guest_width / target->width;
            if (color) {
                memcpy(host + (size_t)y * host_pitch + x * 4,
                       guest_bgra + ((size_t)sy * target->guest_width + sx) * 4,
                       4);
            } else if (guest_bpp == 2) {
                uint16_t packed;
                memcpy(&packed,
                       guest_linear +
                           ((size_t)sy * target->guest_width + sx) * 2,
                       sizeof(packed));
                float depth = target->z_format ?
                                  convert_f16_to_float(packed) / f16_max :
                                  packed / 65535.0f;
                memcpy(host + (size_t)y * host_pitch + x * 4, &depth,
                       sizeof(depth));
            } else {
                uint32_t packed;
                memcpy(&packed,
                       guest_linear +
                           ((size_t)sy * target->guest_width + sx) * 4,
                       sizeof(packed));
                uint32_t depth = packed >> 8;
                uint8_t stencil = packed;
                float normalized_depth =
                    target->z_format ? convert_f24_to_float(depth) / f24_max :
                                       depth / 16777215.0f;
                if (target->format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                    uint32_t native_depth =
                        CLAMP(normalized_depth, 0.0f, 1.0f) * 16777215.0f +
                        0.5f;
                    uint32_t native = native_depth | ((uint32_t)stencil << 24);
                    memcpy(host + (size_t)y * host_pitch + x * 4, &native,
                           sizeof(native));
                } else {
                    uint8_t *pixel = host + (size_t)y * host_pitch + x * 8;
                    memcpy(pixel, &normalized_depth, sizeof(normalized_depth));
                    pixel[4] = stencil;
                }
            }
        }
    }
    g_free(guest_bgra);
    g_free(guest_linear);

    D3D12_RESOURCE_DESC texture_desc = ID3D12Resource_GetDesc(target->resource);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 upload_size = 0;
    ID3D12Device_GetCopyableFootprints(r->device, &texture_desc, 0, 1, 0,
                                       &footprint, NULL, NULL, &upload_size);
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC buffer_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = upload_size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    ID3D12Resource *upload = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        r->device, &heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
        (void **)&upload);
    if (FAILED(hr)) {
        g_free(host);
        error_setg(errp, "D3D12: surface upload allocation failed");
        return false;
    }
    uint8_t *mapped = NULL;
    D3D12_RANGE no_read = { 0, 0 };
    hr = ID3D12Resource_Map(upload, 0, &no_read, (void **)&mapped);
    if (SUCCEEDED(hr)) {
        for (uint32_t y = 0; y < target->height; y++) {
            memcpy(mapped + (size_t)y * footprint.Footprint.RowPitch,
                   host + (size_t)y * host_pitch, host_pitch);
        }
        ID3D12Resource_Unmap(upload, 0, NULL);
    }
    g_free(host);
    if (FAILED(hr)) {
        ID3D12Resource_Release(upload);
        error_setg(errp, "D3D12: surface upload map failed");
        return false;
    }
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    hr = ID3D12Device_CreateCommandAllocator(
        r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    }
    if (FAILED(hr)) {
        if (allocator)
            ID3D12CommandAllocator_Release(allocator);
        ID3D12Resource_Release(upload);
        error_setg(errp, "D3D12: surface upload command list failed");
        return false;
    }
    D3D12_RESOURCE_STATES previous = target->state;
    d3d12_transition(list, target, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination = {
        .pResource = target->resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    };
    D3D12_TEXTURE_COPY_LOCATION source = {
        .pResource = upload,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = footprint,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &destination, 0, 0, 0,
                                                &source, NULL);
    d3d12_transition(list, target, previous);
    bool ok = d3d12_execute_clear(r, list, errp);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12Resource_Release(upload);
    return ok;
}

static unsigned int d3d12_color_bytes_per_pixel(unsigned int format)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        return 2;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        return 1;
    default:
        return 4;
    }
}

static uint8_t d3d12_expand_5_to_8(uint32_t value)
{
    return (value << 3) | (value >> 2);
}

static uint8_t d3d12_expand_6_to_8(uint32_t value)
{
    return (value << 2) | (value >> 4);
}

static uint16_t d3d12_float_to_f16(float value)
{
    value = CLAMP(value, 0.0f, f16_max);
    if (value == 0.0f) {
        return 0;
    }
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits <= 0x3c000000U) {
        return 1;
    }
    return MIN((bits - 0x3c000000U + 0x400U) >> 11, UINT16_MAX);
}

static uint32_t d3d12_float_to_f24(float value)
{
    value = CLAMP(value, 0.0f, f24_max);
    if (value == 0.0f) {
        return 0;
    }
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return MIN((bits + 0x40U) >> 7, 0x00ffffffU);
}

void pgraph_d3d12_convert_color_to_bgra8(
    unsigned int format, uint8_t *destination, size_t destination_pitch,
    const uint8_t *source, size_t source_pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = source + y * source_pitch;
        uint8_t *dst = destination + y * destination_pitch;
        for (uint32_t x = 0; x < width; x++, dst += 4) {
            switch (format) {
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5: {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2, sizeof(pixel));
                dst[0] = d3d12_expand_5_to_8(pixel & 0x1f);
                dst[1] = d3d12_expand_5_to_8((pixel >> 5) & 0x1f);
                dst[2] = d3d12_expand_5_to_8((pixel >> 10) & 0x1f);
                dst[3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5 ?
                        0xff :
                        0;
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
                uint16_t pixel;
                memcpy(&pixel, src + x * 2, sizeof(pixel));
                dst[0] = d3d12_expand_5_to_8(pixel & 0x1f);
                dst[1] = d3d12_expand_6_to_8((pixel >> 5) & 0x3f);
                dst[2] = d3d12_expand_5_to_8((pixel >> 11) & 0x1f);
                dst[3] = 0xff;
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
                dst[0] = src[x];
                dst[1] = dst[2] = 0;
                dst[3] = 0xff;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
                dst[0] = src[x * 2];
                dst[1] = src[x * 2 + 1];
                dst[2] = 0;
                dst[3] = 0xff;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
                memcpy(dst, src + x * 4, 3);
                dst[3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8 ?
                        0xff :
                        0;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
                memcpy(dst, src + x * 4, 3);
                dst[3] = (src[x * 4 + 3] & 0x7f) * 255 / 127;
                break;
            default:
                memcpy(dst, src + x * 4, 4);
                break;
            }
        }
    }
}

void pgraph_d3d12_convert_bgra8_to_color(
    unsigned int format, uint8_t *destination, size_t destination_pitch,
    const uint8_t *source, size_t source_pitch, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = destination + y * destination_pitch;
        const uint8_t *src = source + y * source_pitch;
        for (uint32_t x = 0; x < width; x++, src += 4) {
            switch (format) {
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5: {
                uint16_t pixel = ((src[2] >> 3) << 10) | ((src[1] >> 3) << 5) |
                                 (src[0] >> 3);
                if (format ==
                    NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5) {
                    pixel |= 0x8000;
                }
                memcpy(dst + x * 2, &pixel, sizeof(pixel));
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
                uint16_t pixel = ((src[2] >> 3) << 11) | ((src[1] >> 2) << 5) |
                                 (src[0] >> 3);
                memcpy(dst + x * 2, &pixel, sizeof(pixel));
                break;
            }
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
                dst[x] = src[0];
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
                dst[x * 2] = src[0];
                dst[x * 2 + 1] = src[1];
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
                memcpy(dst + x * 4, src, 3);
                dst[x * 4 + 3] =
                    format ==
                            NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8 ?
                        0xff :
                        0;
                break;
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
            case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
                memcpy(dst + x * 4, src, 3);
                dst[x * 4 + 3] = (src[3] * 127 + 127) / 255;
                if (format ==
                    NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8) {
                    dst[x * 4 + 3] |= 0x80;
                }
                break;
            default:
                memcpy(dst + x * 4, src, 4);
                break;
            }
        }
    }
}

void pgraph_d3d12_convert_z16_to_d32(float *destination,
                                     size_t destination_pitch,
                                     const uint8_t *source, size_t source_pitch,
                                     uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = source + y * source_pitch;
        float *dst = (float *)((uint8_t *)destination + y * destination_pitch);
        for (uint32_t x = 0; x < width; x++) {
            uint16_t depth;
            memcpy(&depth, src + x * 2, sizeof(depth));
            dst[x] = depth / 65535.0f;
        }
    }
}

void pgraph_d3d12_convert_d32_to_z16(uint8_t *destination,
                                     size_t destination_pitch,
                                     const float *source, size_t source_pitch,
                                     uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = destination + y * destination_pitch;
        const float *src =
            (const float *)((const uint8_t *)source + y * source_pitch);
        for (uint32_t x = 0; x < width; x++) {
            float value = CLAMP(src[x], 0.0f, 1.0f);
            uint16_t depth = value * 65535.0f + 0.5f;
            memcpy(dst + x * 2, &depth, sizeof(depth));
        }
    }
}

static void d3d12_clear_color_vram_pixel(uint8_t *pixel, unsigned int format,
                                         uint32_t value, uint32_t parameter)
{
    switch (format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5: {
        uint16_t old_value;
        memcpy(&old_value, pixel, sizeof(old_value));
        uint16_t mask = ((parameter & NV097_CLEAR_SURFACE_R) ? 0x7c00 : 0) |
                        ((parameter & NV097_CLEAR_SURFACE_G) ? 0x03e0 : 0) |
                        ((parameter & NV097_CLEAR_SURFACE_B) ? 0x001f : 0);
        uint16_t result = (old_value & ~mask) | ((uint16_t)value & mask);
        if (format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5) {
            result |= 0x8000;
        } else {
            result &= 0x7fff;
        }
        memcpy(pixel, &result, sizeof(result));
        break;
    }
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: {
        uint16_t old_value;
        memcpy(&old_value, pixel, sizeof(old_value));
        uint16_t mask = ((parameter & NV097_CLEAR_SURFACE_R) ? 0xf800 : 0) |
                        ((parameter & NV097_CLEAR_SURFACE_G) ? 0x07e0 : 0) |
                        ((parameter & NV097_CLEAR_SURFACE_B) ? 0x001f : 0);
        uint16_t result = (old_value & ~mask) | ((uint16_t)value & mask);
        memcpy(pixel, &result, sizeof(result));
        break;
    }
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = value;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = value;
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            pixel[1] = value >> 8;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        if (parameter & NV097_CLEAR_SURFACE_B) {
            pixel[0] = value;
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            pixel[1] = value >> 8;
        }
        if (parameter & NV097_CLEAR_SURFACE_R) {
            pixel[2] = value >> 16;
        }
        if (parameter & NV097_CLEAR_SURFACE_A) {
            pixel[3] = value >> 24;
        }
        if (format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8) {
            pixel[3] = 0;
        } else if (format ==
                   NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8) {
            pixel[3] = 0xff;
        } else if (format ==
                   NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8) {
            pixel[3] &= 0x7f;
        } else if (format ==
                   NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8) {
            pixel[3] |= 0x80;
        }
        break;
    default:
        break;
    }
}

static void d3d12_get_clear_color(PGRAPHState *pg, float rgba[4])
{
    uint32_t value = pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE);
    rgba[0] = rgba[1] = rgba[2] = 0.0f;
    rgba[3] = 1.0f;
    switch (pg->surface_shape.color_format) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        rgba[0] = ((value >> 10) & 0x1f) / 31.0f;
        rgba[1] = ((value >> 5) & 0x1f) / 31.0f;
        rgba[2] = (value & 0x1f) / 31.0f;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        rgba[0] = ((value >> 11) & 0x1f) / 31.0f;
        rgba[1] = ((value >> 5) & 0x3f) / 63.0f;
        rgba[2] = (value & 0x1f) / 31.0f;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
        rgba[2] = (value & 0xff) / 255.0f;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        rgba[1] = ((value >> 8) & 0xff) / 255.0f;
        rgba[2] = (value & 0xff) / 255.0f;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        rgba[0] = ((value >> 16) & 0xff) / 255.0f;
        rgba[1] = ((value >> 8) & 0xff) / 255.0f;
        rgba[2] = (value & 0xff) / 255.0f;
        rgba[3] = ((value >> 24) & 0x7f) / 127.0f;
        break;
    default:
        rgba[0] = ((value >> 16) & 0xff) / 255.0f;
        rgba[1] = ((value >> 8) & 0xff) / 255.0f;
        rgba[2] = (value & 0xff) / 255.0f;
        rgba[3] = ((value >> 24) & 0xff) / 255.0f;
        break;
    }
}

static void d3d12_mirror_clear_to_vram(NV2AState *d, uint32_t parameter,
                                       const D3D12_RECT *rect)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool swizzled = pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE;
    if ((parameter & NV097_CLEAR_SURFACE_COLOR) &&
        r->render_targets.color.resource) {
        PGRAPHD3D12RenderTarget *target = &r->render_targets.color;
        unsigned int bpp = d3d12_color_bytes_per_pixel(target->nv2a_format);
        size_t linear_pitch = (size_t)target->guest_width * bpp;
        uint8_t *vram = d->vram_ptr + target->vram_address;
        uint8_t *pixels = vram;
        if (swizzled) {
            pixels = g_malloc(linear_pitch * target->guest_height);
            unswizzle_rect(vram, target->guest_width, target->guest_height,
                           pixels, linear_pitch, bpp);
        }
        uint32_t value = pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE);
        for (LONG y = rect->top; y < rect->bottom; y++) {
            uint8_t *row =
                pixels + (size_t)y * (swizzled ? linear_pitch : target->pitch);
            for (LONG x = rect->left; x < rect->right; x++) {
                d3d12_clear_color_vram_pixel(row + (size_t)x * bpp,
                                             target->nv2a_format, value,
                                             parameter);
            }
        }
        if (swizzled) {
            swizzle_rect(pixels, target->guest_width, target->guest_height,
                         vram, linear_pitch, bpp);
            g_free(pixels);
        }
        size_t length = target->storage_length;
        memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                       DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                       DIRTY_MEMORY_NV2A_TEX);
    }

    if ((parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL)) &&
        r->render_targets.depth_stencil.resource) {
        PGRAPHD3D12RenderTarget *target = &r->render_targets.depth_stencil;
        unsigned int bpp =
            target->nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4;
        size_t linear_pitch = (size_t)target->guest_width * bpp;
        uint8_t *vram = d->vram_ptr + target->vram_address;
        uint8_t *pixels = vram;
        if (swizzled) {
            pixels = g_malloc(linear_pitch * target->guest_height);
            unswizzle_rect(vram, target->guest_width, target->guest_height,
                           pixels, linear_pitch, bpp);
        }
        uint32_t value = pgraph_reg_r(pg, NV_PGRAPH_ZSTENCILCLEARVALUE);
        for (LONG y = rect->top; y < rect->bottom; y++) {
            uint8_t *row =
                pixels + (size_t)y * (swizzled ? linear_pitch : target->pitch);
            for (LONG x = rect->left; x < rect->right; x++) {
                uint8_t *pixel = row + (size_t)x * bpp;
                if (bpp == 2) {
                    if (parameter & NV097_CLEAR_SURFACE_Z) {
                        uint16_t z = value;
                        memcpy(pixel, &z, sizeof(z));
                    }
                } else {
                    uint32_t old_value;
                    memcpy(&old_value, pixel, sizeof(old_value));
                    uint32_t mask =
                        ((parameter & NV097_CLEAR_SURFACE_Z) ? 0xffffff00U :
                                                               0) |
                        ((parameter & NV097_CLEAR_SURFACE_STENCIL) ? 0xffU : 0);
                    uint32_t result = (old_value & ~mask) | (value & mask);
                    memcpy(pixel, &result, sizeof(result));
                }
            }
        }
        if (swizzled) {
            swizzle_rect(pixels, target->guest_width, target->guest_height,
                         vram, linear_pitch, bpp);
            g_free(pixels);
        }
        size_t length = target->storage_length;
        memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                       DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                       DIRTY_MEMORY_NV2A_TEX);
    }
}

bool pgraph_d3d12_clear_surface(NV2AState *d, uint32_t parameter, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    bool color = parameter & NV097_CLEAR_SURFACE_COLOR;
    bool full_color_clear =
        (parameter & NV097_CLEAR_SURFACE_COLOR) == NV097_CLEAR_SURFACE_COLOR;
    bool zeta =
        parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
    pg->clearing = true;
    bool updated = pgraph_d3d12_surface_update(d, color, zeta, errp);
    pg->clearing = false;
    if (!updated) {
        return false;
    }
    if ((!color || !r->render_targets.color.resource) &&
        (!zeta || !r->render_targets.depth_stencil.resource)) {
        return true;
    }
    if (color && !full_color_clear && !pgraph_d3d12_surface_flush(d, errp)) {
        return false;
    }

    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    HRESULT hr = ID3D12Device_CreateCommandAllocator(
        r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: render-target clear command list failed");
        if (allocator) {
            ID3D12CommandAllocator_Release(allocator);
        }
        return false;
    }

    unsigned int left = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX),
                                 NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int top = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY),
                                NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int right = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX),
                                  NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int bottom = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY),
                                   NV_PGRAPH_CLEARRECTY_YMAX);
    if (right < left || bottom < top) {
        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(allocator);
        return true;
    }
    unsigned int width = right - left + 1;
    unsigned int height = bottom - top + 1;
    pgraph_apply_anti_aliasing_factor(pg, &left, &top);
    pgraph_apply_anti_aliasing_factor(pg, &width, &height);
    D3D12_RECT guest_rect = {
        .left = left,
        .top = top,
        .right = left + width,
        .bottom = top + height,
    };
    pgraph_apply_scaling_factor(pg, &left, &top);
    pgraph_apply_scaling_factor(pg, &width, &height);
    D3D12_RECT rect = {
        .left = left,
        .top = top,
        .right = left + width,
        .bottom = top + height,
    };
    if (color && r->render_targets.color.resource) {
        D3D12_RECT color_rect = rect;
        color_rect.left =
            MIN(color_rect.left, (LONG)r->render_targets.color.width);
        color_rect.top =
            MIN(color_rect.top, (LONG)r->render_targets.color.height);
        color_rect.right =
            MIN(color_rect.right, (LONG)r->render_targets.color.width);
        color_rect.bottom =
            MIN(color_rect.bottom, (LONG)r->render_targets.color.height);
        d3d12_transition(list, &r->render_targets.color,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
        float rgba[4];
        d3d12_get_clear_color(pg, rgba);
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                r->render_target_rtv_heap);
        if (color_rect.left < color_rect.right &&
            color_rect.top < color_rect.bottom) {
            ID3D12GraphicsCommandList_ClearRenderTargetView(list, handle, rgba,
                                                            1, &color_rect);
            pg->surface_color.draw_dirty = true;
        }
    }
    if (zeta && r->render_targets.depth_stencil.resource) {
        D3D12_RECT depth_rect = rect;
        depth_rect.left =
            MIN(depth_rect.left, (LONG)r->render_targets.depth_stencil.width);
        depth_rect.top =
            MIN(depth_rect.top, (LONG)r->render_targets.depth_stencil.height);
        depth_rect.right =
            MIN(depth_rect.right, (LONG)r->render_targets.depth_stencil.width);
        depth_rect.bottom = MIN(depth_rect.bottom,
                                (LONG)r->render_targets.depth_stencil.height);
        d3d12_transition(list, &r->render_targets.depth_stencil,
                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
        float depth;
        int stencil;
        pgraph_get_clear_depth_stencil_value(pg, &depth, &stencil);
        D3D12_CLEAR_FLAGS flags = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            flags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            r->render_targets.depth_stencil.format != DXGI_FORMAT_D32_FLOAT) {
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if (flags && depth_rect.left < depth_rect.right &&
            depth_rect.top < depth_rect.bottom) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                    r->dsv_heap);
            ID3D12GraphicsCommandList_ClearDepthStencilView(
                list, handle, flags, depth, stencil, 1, &depth_rect);
            pg->surface_zeta.draw_dirty = true;
        }
    }
    bool result = d3d12_execute_clear(r, list, errp);
    if (result) {
        if (color && r->render_targets.color.resource) {
            D3D12_RECT color_guest_rect = guest_rect;
            color_guest_rect.left =
                MIN(color_guest_rect.left,
                    (LONG)r->render_targets.color.guest_width);
            color_guest_rect.top =
                MIN(color_guest_rect.top,
                    (LONG)r->render_targets.color.guest_height);
            color_guest_rect.right =
                MIN(color_guest_rect.right,
                    (LONG)r->render_targets.color.guest_width);
            color_guest_rect.bottom =
                MIN(color_guest_rect.bottom,
                    (LONG)r->render_targets.color.guest_height);
            d3d12_mirror_clear_to_vram(d, parameter & NV097_CLEAR_SURFACE_COLOR,
                                       &color_guest_rect);
        }
        if (zeta && r->render_targets.depth_stencil.resource) {
            D3D12_RECT depth_guest_rect = guest_rect;
            depth_guest_rect.left =
                MIN(depth_guest_rect.left,
                    (LONG)r->render_targets.depth_stencil.guest_width);
            depth_guest_rect.top =
                MIN(depth_guest_rect.top,
                    (LONG)r->render_targets.depth_stencil.guest_height);
            depth_guest_rect.right =
                MIN(depth_guest_rect.right,
                    (LONG)r->render_targets.depth_stencil.guest_width);
            depth_guest_rect.bottom =
                MIN(depth_guest_rect.bottom,
                    (LONG)r->render_targets.depth_stencil.guest_height);
            d3d12_mirror_clear_to_vram(
                d,
                parameter &
                    (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL),
                &depth_guest_rect);
        }
        if (color && !full_color_clear && !d3d12_upload_target(d, true, errp)) {
            result = false;
        }
    }
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    return result;
}

static bool d3d12_readback_target(PGRAPHD3D12State *r,
                                  PGRAPHD3D12RenderTarget *target,
                                  ID3D12Resource **readback,
                                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint,
                                  Error **errp)
{
    D3D12_RESOURCE_DESC texture_desc = ID3D12Resource_GetDesc(target->resource);
    UINT64 size = 0;
    ID3D12Device_GetCopyableFootprints(r->device, &texture_desc, 0, 1, 0,
                                       footprint, NULL, NULL, &size);
    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_READBACK,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC buffer_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        r->device, &heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void **)readback);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: surface readback buffer failed (0x%08lx)",
                   (unsigned long)hr);
        return false;
    }

    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    hr = ID3D12Device_CreateCommandAllocator(
        r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(
            r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    }
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: surface readback command list failed");
        if (allocator) {
            ID3D12CommandAllocator_Release(allocator);
        }
        ID3D12Resource_Release(*readback);
        *readback = NULL;
        return false;
    }

    D3D12_RESOURCE_STATES previous = target->state;
    d3d12_transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination = {
        .pResource = *readback,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = *footprint,
    };
    D3D12_TEXTURE_COPY_LOCATION source = {
        .pResource = target->resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &destination, 0, 0, 0,
                                                &source, NULL);
    d3d12_transition(list, target, previous);
    bool ok = d3d12_execute_clear(r, list, errp);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    if (!ok) {
        ID3D12Resource_Release(*readback);
        *readback = NULL;
    }
    return ok;
}

static void d3d12_store_linear_or_swizzled(PGRAPHState *pg, uint8_t *vram,
                                           const uint8_t *linear,
                                           uint32_t width, uint32_t height,
                                           uint32_t pitch, unsigned int bpp)
{
    if (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
        swizzle_rect(linear, width, height, vram, (size_t)width * bpp, bpp);
        return;
    }
    for (uint32_t y = 0; y < height; y++) {
        memcpy(vram + (size_t)y * pitch, linear + (size_t)y * width * bpp,
               (size_t)width * bpp);
    }
}

static bool d3d12_flush_color(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12RenderTarget *target = &r->render_targets.color;
    if (!target->resource || !pg->surface_color.draw_dirty) {
        return true;
    }
    ID3D12Resource *readback = NULL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    if (!d3d12_readback_target(r, target, &readback, &footprint, errp)) {
        return false;
    }
    uint8_t *mapped = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)footprint.Footprint.RowPitch *
                                 footprint.Footprint.Height };
    HRESULT hr = ID3D12Resource_Map(readback, 0, &range, (void **)&mapped);
    if (FAILED(hr)) {
        ID3D12Resource_Release(readback);
        error_setg(errp, "D3D12: color surface readback map failed");
        return false;
    }
    size_t bgra_pitch = (size_t)target->guest_width * 4;
    uint8_t *bgra = g_malloc(bgra_pitch * target->guest_height);
    for (uint32_t y = 0; y < target->guest_height; y++) {
        uint32_t sy = (uint64_t)y * target->height / target->guest_height;
        for (uint32_t x = 0; x < target->guest_width; x++) {
            uint32_t sx = (uint64_t)x * target->width / target->guest_width;
            memcpy(bgra + (size_t)y * bgra_pitch + x * 4,
                   mapped + (size_t)sy * footprint.Footprint.RowPitch + sx * 4,
                   4);
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12Resource_Release(readback);
    unsigned int bpp = d3d12_color_bytes_per_pixel(target->nv2a_format);
    uint8_t *linear =
        g_malloc((size_t)target->guest_width * target->guest_height * bpp);
    pgraph_d3d12_convert_bgra8_to_color(
        target->nv2a_format, linear, (size_t)target->guest_width * bpp, bgra,
        bgra_pitch, target->guest_width, target->guest_height);
    g_free(bgra);
    uint8_t *vram = d->vram_ptr + target->vram_address;
    d3d12_store_linear_or_swizzled(pg, vram, linear, target->guest_width,
                                   target->guest_height, target->pitch, bpp);
    g_free(linear);
    size_t length = target->storage_length;
    memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                   DIRTY_MEMORY_NV2A_TEX);
    pg->surface_color.draw_dirty = false;
    return true;
}

static bool d3d12_flush_depth(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    PGRAPHD3D12RenderTarget *target = &r->render_targets.depth_stencil;
    if (!target->resource || !pg->surface_zeta.draw_dirty) {
        return true;
    }
    ID3D12Resource *readback = NULL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    if (!d3d12_readback_target(r, target, &readback, &footprint, errp)) {
        return false;
    }
    uint8_t *mapped = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)footprint.Footprint.RowPitch *
                                 footprint.Footprint.Height };
    HRESULT hr = ID3D12Resource_Map(readback, 0, &range, (void **)&mapped);
    if (FAILED(hr)) {
        ID3D12Resource_Release(readback);
        error_setg(errp, "D3D12: depth surface readback map failed");
        return false;
    }
    unsigned int bpp =
        target->nv2a_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 2 : 4;
    uint8_t *linear =
        g_malloc((size_t)target->guest_width * target->guest_height * bpp);
    for (uint32_t y = 0; y < target->guest_height; y++) {
        uint32_t sy = (uint64_t)y * target->height / target->guest_height;
        const uint8_t *src = mapped + (size_t)sy * footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < target->guest_width; x++) {
            uint32_t sx = (uint64_t)x * target->width / target->guest_width;
            if (bpp == 2) {
                float depth;
                memcpy(&depth, src + sx * sizeof(float), sizeof(depth));
                depth = CLAMP(depth, 0.0f, 1.0f);
                uint16_t value = target->z_format ?
                                     d3d12_float_to_f16(depth * f16_max) :
                                     depth * 65535.0f + 0.5f;
                memcpy(linear + ((size_t)y * target->guest_width + x) * 2,
                       &value, sizeof(value));
            } else {
                uint32_t value;
                if (target->format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                    uint32_t packed;
                    memcpy(&packed, src + sx * 4, sizeof(packed));
                    uint32_t native_depth = packed & 0x00ffffffU;
                    uint32_t guest_depth =
                        target->z_format ?
                            d3d12_float_to_f24(native_depth / 16777215.0f *
                                               f24_max) :
                            native_depth;
                    value = (guest_depth << 8) | (packed >> 24);
                } else {
                    float depth;
                    memcpy(&depth, src + sx * 8, sizeof(depth));
                    uint8_t stencil = src[sx * 8 + 4];
                    depth = CLAMP(depth, 0.0f, 1.0f);
                    uint32_t guest_depth =
                        target->z_format ? d3d12_float_to_f24(depth * f24_max) :
                                           depth * 16777215.0f + 0.5f;
                    value = (guest_depth << 8) | stencil;
                }
                memcpy(linear + ((size_t)y * target->guest_width + x) * 4,
                       &value, sizeof(value));
            }
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12Resource_Release(readback);
    uint8_t *vram = d->vram_ptr + target->vram_address;
    d3d12_store_linear_or_swizzled(pg, vram, linear, target->guest_width,
                                   target->guest_height, target->pitch, bpp);
    g_free(linear);
    size_t length = target->storage_length;
    memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, target->vram_address, length,
                                   DIRTY_MEMORY_NV2A_TEX);
    pg->surface_zeta.draw_dirty = false;
    return true;
}

bool pgraph_d3d12_surface_flush(NV2AState *d, Error **errp)
{
    if (!d3d12_flush_color(d, errp)) {
        return false;
    }
    return d3d12_flush_depth(d, errp);
}
