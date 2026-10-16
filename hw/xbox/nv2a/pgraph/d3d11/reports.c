/*
 * GeForce NV2A PGRAPH Direct3D 11 reports
 *
 * The queue and accumulated Z-pass behavior follow xemu's PGRAPH report
 * contract. Implemented for D3D11 on 2026-09-26.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-host.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "qemu/host-utils.h"
#include "renderer.h"

static void d3d11_release_queries(ID3D11Query **queries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ID3D11Query_Release(queries[i]);
    }
    g_free(queries);
}

static uint32_t d3d11_report_scaled_count(uint64_t samples,
                                         uint32_t scale_factor)
{
    uint64_t scale = scale_factor ? scale_factor : 1;
    return (uint32_t)(samples / (scale * scale));
}

static void d3d11_report_accumulate_count(uint32_t *total, uint64_t samples,
                                         uint32_t scale_factor)
{
    *total += d3d11_report_scaled_count(samples, scale_factor);
}

bool pgraph_d3d11_reports_init(PGRAPHD3D11State *r)
{
    r->reports = g_new0(PGRAPHD3D11ReportState, 1);
    QSIMPLEQ_INIT(&r->reports->queue);
    r->reports->current_queries = g_ptr_array_new();
    return true;
}

void pgraph_d3d11_reports_finalize(PGRAPHD3D11State *r)
{
    if (!r->reports) {
        return;
    }
    if (r->reports->active_query) {
        ID3D11DeviceContext_End(r->context,
                                (ID3D11Asynchronous *)r->reports->active_query);
        ID3D11Query_Release(r->reports->active_query);
    }
    for (guint i = 0; i < r->reports->current_queries->len; i++) {
        ID3D11Query_Release(g_ptr_array_index(r->reports->current_queries, i));
    }
    g_ptr_array_free(r->reports->current_queries, TRUE);
    PGRAPHD3D11QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->reports->queue))) {
        QSIMPLEQ_REMOVE_HEAD(&r->reports->queue, entry);
        d3d11_release_queries(report->queries, report->query_count);
        g_free(report);
    }
    g_free(r->reports);
    r->reports = NULL;
}

bool pgraph_d3d11_report_draw_begin(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || !r->reports || r->device_lost) {
        return false;
    }
    if (!d->pgraph.zpass_pixel_count_enable || r->reports->active_query) {
        return true;
    }
    D3D11_QUERY_DESC desc = { .Query = D3D11_QUERY_OCCLUSION };
    HRESULT hr =
        ID3D11Device_CreateQuery(r->device, &desc, &r->reports->active_query);
    if (FAILED(hr)) {
        qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                           "D3D11: failed to create occlusion query");
        /* Reports are optional instrumentation; keep rendering the draw. */
        return true;
    }
    ID3D11DeviceContext_Begin(r->context,
                              (ID3D11Asynchronous *)r->reports->active_query);
    return true;
}

void pgraph_d3d11_report_draw_end(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || !r->reports) {
        return;
    }
    if (!r->reports->active_query) {
        return;
    }
    ID3D11DeviceContext_End(r->context,
                            (ID3D11Asynchronous *)r->reports->active_query);
    g_ptr_array_add(r->reports->current_queries, r->reports->active_query);
    r->reports->active_query = NULL;
}

void pgraph_d3d11_clear_report_value(NV2AState *d)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || !r->reports) {
        return;
    }
    pgraph_d3d11_report_draw_end(d);
    for (guint i = 0; i < r->reports->current_queries->len; i++) {
        ID3D11Query_Release(g_ptr_array_index(r->reports->current_queries, i));
    }
    g_ptr_array_set_size(r->reports->current_queries, 0);
    PGRAPHD3D11QueryReport *report = g_new0(PGRAPHD3D11QueryReport, 1);
    report->clear = true;
    QSIMPLEQ_INSERT_TAIL(&r->reports->queue, report, entry);
}

void pgraph_d3d11_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHD3D11State *r = d->pgraph.d3d11_renderer_state;
    if (!r || !r->resources_ready || !r->reports) {
        pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
        return;
    }
    pgraph_d3d11_report_draw_end(d);
    PGRAPHD3D11QueryReport *report = g_new0(PGRAPHD3D11QueryReport, 1);
    report->parameter = parameter;
    report->query_count = r->reports->current_queries->len;
    report->queries =
        (ID3D11Query **)g_ptr_array_free(r->reports->current_queries, FALSE);
    r->reports->current_queries = g_ptr_array_new();
    QSIMPLEQ_INSERT_TAIL(&r->reports->queue, report, entry);
}

static bool d3d11_read_query(PGRAPHD3D11State *r, ID3D11Query *query,
                             uint64_t *samples)
{
    ID3D11DeviceContext_Flush(r->context);
    int64_t deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    for (;;) {
        HRESULT hr = ID3D11DeviceContext_GetData(
            r->context, (ID3D11Asynchronous *)query, samples, sizeof(*samples),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            return true;
        }
        if (FAILED(hr) || g_get_monotonic_time() >= deadline) {
            r->device_lost = true;
            qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                               "D3D11: occlusion-query failed or timed out");
            return false;
        }
        Sleep(1);
    }
}

static bool d3d11_report_accumulate_queries(PGRAPHD3D11State *r,
                                           ID3D11Query **queries,
                                           size_t query_count,
                                           uint32_t scale_factor,
                                           uint32_t *total)
{
    if (r->device_lost) {
        *total = 0;
        return false;
    }
    for (size_t i = 0; i < query_count; i++) {
        uint64_t samples = 0;
        if (!d3d11_read_query(r, queries[i], &samples)) {
            *total = 0;
            return false;
        }
        d3d11_report_accumulate_count(total, samples, scale_factor);
    }
    return true;
}

void pgraph_d3d11_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHD3D11State *r = pg->d3d11_renderer_state;
    if (!r || !r->resources_ready || !r->reports) {
        return;
    }
    PGRAPHD3D11QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->reports->queue))) {
        if (report->clear) {
            r->reports->zpass_pixel_count = 0;
        } else {
            uint8_t type = GET_MASK(report->parameter, NV097_GET_REPORT_TYPE);
            if (type != NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT) {
                qemu_host_emit_log(QEMU_HOST_LOG_ERROR,
                                   "D3D11: unsupported NV2A report type");
            } else {
                bool success = d3d11_report_accumulate_queries(
                    r, report->queries, report->query_count,
                    pg->surface_scale_factor,
                    &r->reports->zpass_pixel_count);
                if (!success) {
                    /* Do not expose an earlier report's count on query loss. */
                    r->reports->zpass_pixel_count = 0;
                }
                pgraph_write_zpass_pixel_cnt_report(
                    d, report->parameter, r->reports->zpass_pixel_count);
            }
        }
        QSIMPLEQ_REMOVE_HEAD(&r->reports->queue, entry);
        d3d11_release_queries(report->queries, report->query_count);
        g_free(report);
    }
}
