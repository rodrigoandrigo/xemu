/*
 * Geforce NV2A PGRAPH Direct3D 12 reports
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "reports.h"

#define D3D12_MAX_OCCLUSION_QUERIES 1024

static bool d3d12_wait_for_query_submission(PGRAPHD3D12State *r,
                                            ID3D12GraphicsCommandList *list,
                                            Error **errp)
{
    HRESULT hr = ID3D12GraphicsCommandList_Close(list);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: query resolve command close failed");
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
        pgraph_d3d12_note_device_error(r, hr, "query resolve");
        error_setg(errp, "D3D12: query resolve synchronization failed");
        return false;
    }
    return true;
}

bool pgraph_d3d12_reports_init(PGRAPHD3D12State *r, Error **errp)
{
    r->max_query_count = D3D12_MAX_OCCLUSION_QUERIES;
    QSIMPLEQ_INIT(&r->report_queue);
    D3D12_QUERY_HEAP_DESC query_desc = {
        .Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION,
        .Count = r->max_query_count,
    };
    HRESULT hr = ID3D12Device_CreateQueryHeap(
        r->device, &query_desc, &IID_ID3D12QueryHeap, (void **)&r->query_heap);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: occlusion query heap creation failed");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap = {
        .Type = D3D12_HEAP_TYPE_READBACK,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    D3D12_RESOURCE_DESC buffer = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = (UINT64)r->max_query_count * sizeof(uint64_t),
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = { 1, 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    hr = ID3D12Device_CreateCommittedResource(
        r->device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void **)&r->query_readback);
    if (FAILED(hr)) {
        error_setg(errp, "D3D12: query readback buffer creation failed");
        ID3D12QueryHeap_Release(r->query_heap);
        r->query_heap = NULL;
        return false;
    }
    return true;
}

void pgraph_d3d12_reports_finalize(PGRAPHD3D12State *r)
{
    PGRAPHD3D12QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue))) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
    if (r->query_readback) {
        ID3D12Resource_Release(r->query_readback);
        r->query_readback = NULL;
    }
    if (r->query_heap) {
        ID3D12QueryHeap_Release(r->query_heap);
        r->query_heap = NULL;
    }
}

void pgraph_d3d12_reports_begin_draw(PGRAPHD3D12State *r,
                                     ID3D12GraphicsCommandList *list)
{
    if (r->query_active || r->query_count >= r->max_query_count) {
        return;
    }
    ID3D12GraphicsCommandList_BeginQuery(
        list, r->query_heap, D3D12_QUERY_TYPE_OCCLUSION, r->query_count);
    r->query_active = true;
}

void pgraph_d3d12_reports_end_draw(PGRAPHD3D12State *r,
                                   ID3D12GraphicsCommandList *list)
{
    if (!r->query_active) {
        return;
    }
    ID3D12GraphicsCommandList_EndQuery(
        list, r->query_heap, D3D12_QUERY_TYPE_OCCLUSION, r->query_count++);
    r->query_active = false;
}

static void d3d12_queue_report(PGRAPHD3D12State *r, bool clear,
                               uint32_t parameter)
{
    PGRAPHD3D12QueryReport *report = g_new0(PGRAPHD3D12QueryReport, 1);
    report->clear = clear;
    report->parameter = parameter;
    report->query_count = r->query_count;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);
}

void pgraph_d3d12_clear_report_value(NV2AState *d)
{
    d3d12_queue_report(d->pgraph.d3d12_renderer_state, true, 0);
}

void pgraph_d3d12_get_report(NV2AState *d, uint32_t parameter)
{
    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    if (type != NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT) {
        qemu_host_emit_log(QEMU_HOST_LOG_WARNING,
                           "D3D12: unsupported NV2A report type");
        pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
        return;
    }
    d3d12_queue_report(d->pgraph.d3d12_renderer_state, false, parameter);
}

bool pgraph_d3d12_process_reports_now(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D12State *r = pg->d3d12_renderer_state;
    uint64_t *results = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    if (r->query_active) {
        error_setg(errp, "D3D12: cannot resolve an active occlusion query");
        return false;
    }
    if (r->query_count) {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(
            r->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&allocator);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateCommandList(
                r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                &IID_ID3D12GraphicsCommandList, (void **)&list);
        }
        if (FAILED(hr)) {
            error_setg(errp, "D3D12: query resolve command list failed");
            if (allocator) {
                ID3D12CommandAllocator_Release(allocator);
            }
            return false;
        }
        ID3D12GraphicsCommandList_ResolveQueryData(
            list, r->query_heap, D3D12_QUERY_TYPE_OCCLUSION, 0, r->query_count,
            r->query_readback, 0);
        bool submitted = d3d12_wait_for_query_submission(r, list, errp);
        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(allocator);
        if (!submitted) {
            return false;
        }
        D3D12_RANGE read_range = { 0,
                                   (SIZE_T)r->query_count * sizeof(uint64_t) };
        hr = ID3D12Resource_Map(r->query_readback, 0, &read_range,
                                (void **)&results);
        if (FAILED(hr)) {
            error_setg(errp, "D3D12: query readback map failed");
            return false;
        }
    }

    uint32_t consumed = 0;
    uint64_t divisor = MAX(pg->surface_scale_factor, 1);
    divisor *= divisor;
    PGRAPHD3D12QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue))) {
        while (consumed < report->query_count && consumed < r->query_count) {
            r->zpass_pixel_count += results[consumed++];
        }
        if (report->clear) {
            r->zpass_pixel_count = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(d, report->parameter,
                                                r->zpass_pixel_count / divisor);
        }
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
    while (consumed < r->query_count) {
        r->zpass_pixel_count += results[consumed++];
    }
    if (results) {
        D3D12_RANGE no_write = { 0, 0 };
        ID3D12Resource_Unmap(r->query_readback, 0, &no_write);
    }
    r->query_count = 0;
    return true;
}

void pgraph_d3d12_process_pending_reports(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    uint32_t dma_get = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t dma_put = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
    if (dma_get != dma_put || QSIMPLEQ_EMPTY(&r->report_queue)) {
        return;
    }
    Error *error = NULL;
    if (!pgraph_d3d12_process_reports_now(d, &error)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           error ? error_get_pretty(error) :
                                   "D3D12: query processing failed");
        error_free(error);
    }
}

void pgraph_d3d12_reports_device_lost(NV2AState *d)
{
    PGRAPHD3D12State *r = d->pgraph.d3d12_renderer_state;
    PGRAPHD3D12QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue))) {
        if (report->clear) {
            r->zpass_pixel_count = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(d, report->parameter, 0);
        }
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
    r->query_count = 0;
    r->query_active = false;
}
