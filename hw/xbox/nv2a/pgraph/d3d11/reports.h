/*
 * GeForce NV2A PGRAPH Direct3D 11 reports
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D11_REPORTS_H
#define HW_XBOX_NV2A_PGRAPH_D3D11_REPORTS_H

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D11State PGRAPHD3D11State;

typedef struct PGRAPHD3D11QueryReport {
    QSIMPLEQ_ENTRY(PGRAPHD3D11QueryReport) entry;
    bool clear;
    uint32_t parameter;
    ID3D11Query **queries;
    size_t query_count;
} PGRAPHD3D11QueryReport;

typedef struct PGRAPHD3D11ReportState {
    QSIMPLEQ_HEAD(, PGRAPHD3D11QueryReport) queue;
    GPtrArray *current_queries;
    ID3D11Query *active_query;
    uint64_t zpass_pixel_count;
} PGRAPHD3D11ReportState;

bool pgraph_d3d11_reports_init(PGRAPHD3D11State *r);
void pgraph_d3d11_reports_finalize(PGRAPHD3D11State *r);
bool pgraph_d3d11_report_draw_begin(NV2AState *d);
void pgraph_d3d11_report_draw_end(NV2AState *d);
void pgraph_d3d11_clear_report_value(NV2AState *d);
void pgraph_d3d11_get_report(NV2AState *d, uint32_t parameter);
void pgraph_d3d11_process_pending_reports(NV2AState *d);

#endif
