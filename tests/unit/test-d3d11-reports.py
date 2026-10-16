#!/usr/bin/env python3
"""Exercise D3D11 report scaling and query-failure handling portably."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "hw/xbox/nv2a/pgraph/d3d11/reports.c").read_text(
    encoding="utf-8")


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for end in range(opening, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


report_begin = extract_function(
    SOURCE, "bool pgraph_d3d11_report_draw_begin(NV2AState *d)")
report_accumulate = extract_function(
    SOURCE, "static void d3d11_report_accumulate_count(")
report_scaled_count = extract_function(
    SOURCE, "static uint32_t d3d11_report_scaled_count(")
report_accumulate_queries = extract_function(
    SOURCE, "static bool d3d11_report_accumulate_queries(")
pending_reports = extract_function(
    SOURCE, "void pgraph_d3d11_process_pending_reports(NV2AState *d)")
assert "r->reports->zpass_pixel_count = 0" in pending_reports
assert "d3d11_report_accumulate_queries" in pending_reports
assert "pg->surface_scale_factor" in pending_reports
assert "samples / (scale * scale)" in report_scaled_count
assert "(uint32_t)samples" not in report_scaled_count
assert "if (!success)" in pending_reports

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int HRESULT;
typedef struct ID3D11Device { int unused; } ID3D11Device;
typedef struct ID3D11DeviceContext { int unused; } ID3D11DeviceContext;
typedef struct ID3D11Query { int id; } ID3D11Query;
typedef void ID3D11Asynchronous;
typedef struct { unsigned int Query; } D3D11_QUERY_DESC;
typedef struct PGRAPHD3D11QueryReport {
    bool clear;
    uint32_t parameter;
    ID3D11Query **queries;
    size_t query_count;
    struct PGRAPHD3D11QueryReport *next;
} PGRAPHD3D11QueryReport;
typedef struct {
    PGRAPHD3D11QueryReport *first;
    PGRAPHD3D11QueryReport *last;
} QueryQueue;
typedef struct {
    QueryQueue queue;
    ID3D11Query *active_query;
    uint32_t zpass_pixel_count;
} PGRAPHD3D11ReportState;
typedef struct {
    bool resources_ready;
    bool device_lost;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    PGRAPHD3D11ReportState *reports;
} PGRAPHD3D11State;
typedef struct {
    PGRAPHD3D11State *d3d11_renderer_state;
    bool zpass_pixel_count_enable;
    uint32_t surface_scale_factor;
} PGRAPHState;
typedef struct { PGRAPHState pgraph; } NV2AState;
#define QSIMPLEQ_FIRST(head) ((head)->first)
#define QSIMPLEQ_REMOVE_HEAD(head, field) \
    do { \
        (head)->first = (head)->first->next; \
        if (!(head)->first) { (head)->last = NULL; } \
    } while (0)
#define GET_MASK(value, mask) ((value) & (mask))
#define NV097_GET_REPORT_TYPE 0xffU
#define NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT 1U
#define D3D11_QUERY_OCCLUSION 1
#define QEMU_HOST_LOG_ERROR 2
#define FAILED(hr) ((hr) < 0)
#define g_free free
static int create_result;
static int create_calls;
static int begin_calls;
static int log_calls;
static bool mock_query_succeeds;
static uint64_t mock_query_samples;
static uint32_t written_reports[8];
static unsigned int written_report_count;
static ID3D11Query query = { 17 };
static HRESULT mock_create_query(ID3D11Device *device,
                                 const D3D11_QUERY_DESC *desc,
                                 ID3D11Query **out)
{
    (void)device;
    (void)desc;
    create_calls++;
    if (create_result < 0) {
        *out = NULL;
    } else {
        *out = &query;
    }
    return create_result;
}
#define ID3D11Device_CreateQuery(device, desc, out) \
    mock_create_query((device), (desc), (out))
static void mock_begin(ID3D11DeviceContext *context,
                       ID3D11Asynchronous *query_async)
{
    (void)context;
    assert(query_async == (ID3D11Asynchronous *)&query);
    begin_calls++;
}
#define ID3D11DeviceContext_Begin(context, query_async) \
    mock_begin((context), (query_async))
static void qemu_host_emit_log(int level, const char *message)
{
    assert(level == QEMU_HOST_LOG_ERROR);
    assert(message != NULL);
    log_calls++;
}
static bool d3d11_read_query(PGRAPHD3D11State *renderer, ID3D11Query *query,
                             uint64_t *samples)
{
    (void)query;
    if (!mock_query_succeeds) {
        renderer->device_lost = true;
        return false;
    }
    *samples = mock_query_samples;
    return true;
}
static void d3d11_release_queries(ID3D11Query **queries, size_t count)
{
    (void)count;
    free(queries);
}
static void pgraph_write_zpass_pixel_cnt_report(NV2AState *d,
                                                uint32_t parameter,
                                                uint32_t count)
{
    (void)d;
    assert(parameter == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);
    assert(written_report_count < sizeof(written_reports) /
                                     sizeof(written_reports[0]));
    written_reports[written_report_count++] = count;
}
static void enqueue_report(PGRAPHD3D11ReportState *reports)
{
    PGRAPHD3D11QueryReport *report = calloc(1, sizeof(*report));
    assert(report);
    report->parameter = NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT;
    report->query_count = 1;
    report->queries = malloc(sizeof(*report->queries));
    assert(report->queries);
    report->queries[0] = &query;
    if (reports->queue.last) {
        reports->queue.last->next = report;
    } else {
        reports->queue.first = report;
    }
    reports->queue.last = report;
}
''' + report_scaled_count + "\n" + report_accumulate + r'''
''' + report_accumulate_queries + "\n" + report_begin + "\n" + \
    pending_reports + r'''
int main(void)
{
    uint32_t count = 0;
    d3d11_report_accumulate_count(&count, 3, 2);
    d3d11_report_accumulate_count(&count, 3, 2);
    assert(count == 0); /* GL divides each query before accumulating. */
    d3d11_report_accumulate_count(&count, 4, 2);
    d3d11_report_accumulate_count(&count, 8, 2);
    assert(count == 3);

    /* Preserve the high sample bits until after scale correction. */
    assert(d3d11_report_scaled_count((uint64_t)1 << 32, 2) ==
           ((uint32_t)1 << 30));
    assert(d3d11_report_scaled_count((uint64_t)UINT32_MAX + 1 + 4, 2) ==
           ((uint32_t)1 << 30) + 1);
    assert(d3d11_report_scaled_count(100, 0) == 100);
    count = UINT32_MAX;
    d3d11_report_accumulate_count(&count, 1, 1);
    assert(count == 0); /* unsigned 32-bit accumulation wraps like GL. */

    ID3D11Device device = {0};
    ID3D11DeviceContext context = {0};
    PGRAPHD3D11ReportState reports = {.zpass_pixel_count = 777};
    PGRAPHD3D11State renderer = {
        .resources_ready = true,
        .device = &device,
        .context = &context,
        .reports = &reports,
    };
    NV2AState d = { .pgraph = {
        .d3d11_renderer_state = &renderer,
        .zpass_pixel_count_enable = true,
        .surface_scale_factor = 2,
    } };

    /* A failed query and every later queued GET_REPORT must publish zero. */
    enqueue_report(&reports);
    enqueue_report(&reports);
    mock_query_succeeds = false;
    pgraph_d3d11_process_pending_reports(&d);
    assert(renderer.device_lost);
    assert(reports.zpass_pixel_count == 0);
    assert(reports.queue.first == NULL && reports.queue.last == NULL);
    assert(written_report_count == 2);
    assert(written_reports[0] == 0 && written_reports[1] == 0);

    renderer.device_lost = false;
    create_result = -1;
    assert(pgraph_d3d11_report_draw_begin(&d));
    assert(create_calls == 1 && log_calls == 1 && begin_calls == 0);
    assert(reports.active_query == NULL);

    create_result = 0;
    assert(pgraph_d3d11_report_draw_begin(&d));
    assert(create_calls == 2 && begin_calls == 1);
    assert(reports.active_query == &query);
    assert(pgraph_d3d11_report_draw_begin(&d));
    assert(create_calls == 2 && begin_calls == 1);

    reports.active_query = NULL;
    d.pgraph.zpass_pixel_count_enable = false;
    assert(pgraph_d3d11_report_draw_begin(&d));
    assert(create_calls == 2 && begin_calls == 1);
    renderer.resources_ready = false;
    assert(!pgraph_d3d11_report_draw_begin(&d));
    puts("PASS: full-width Z-pass samples are scaled before uint32 conversion");
    puts("PASS: failed occlusion queries drain pending reports with zero");
    puts("PASS: occlusion-query allocation failure is non-fatal to rendering");
}
'''
with tempfile.TemporaryDirectory(prefix="xemu-d3d11-reports-") as directory:
    cfile = pathlib.Path(directory) / "reports.c"
    exe = pathlib.Path(directory) / "reports"
    cfile.write_text(fixture, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
