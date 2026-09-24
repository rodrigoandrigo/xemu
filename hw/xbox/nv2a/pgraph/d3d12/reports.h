/*
 * Geforce NV2A PGRAPH Direct3D 12 reports
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef HW_XBOX_NV2A_PGRAPH_D3D12_REPORTS_H
#define HW_XBOX_NV2A_PGRAPH_D3D12_REPORTS_H

#include "qapi/error.h"

typedef struct NV2AState NV2AState;
typedef struct PGRAPHD3D12State PGRAPHD3D12State;

typedef struct PGRAPHD3D12QueryReport {
    bool clear;
    uint32_t parameter;
    uint32_t query_count;
    QSIMPLEQ_ENTRY(PGRAPHD3D12QueryReport) entry;
} PGRAPHD3D12QueryReport;

bool pgraph_d3d12_reports_init(PGRAPHD3D12State *r, Error **errp);
void pgraph_d3d12_reports_finalize(PGRAPHD3D12State *r);
void pgraph_d3d12_clear_report_value(NV2AState *d);
void pgraph_d3d12_get_report(NV2AState *d, uint32_t parameter);
void pgraph_d3d12_process_pending_reports(NV2AState *d);
bool pgraph_d3d12_process_reports_now(NV2AState *d, Error **errp);
void pgraph_d3d12_reports_device_lost(NV2AState *d);
void pgraph_d3d12_reports_begin_draw(PGRAPHD3D12State *r,
                                     ID3D12GraphicsCommandList *list);
void pgraph_d3d12_reports_end_draw(PGRAPHD3D12State *r,
                                   ID3D12GraphicsCommandList *list);

#endif
