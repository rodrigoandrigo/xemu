#!/usr/bin/env python3
"""Guard D3D11 memory-callback lifetime and lock-order behavior."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SURFACES = (ROOT / "hw/xbox/nv2a/pgraph/d3d11/surfaces.c").read_text()
RENDERER = (ROOT / "hw/xbox/nv2a/pgraph/d3d11/renderer.c").read_text()
PGRAPH = (ROOT / "hw/xbox/nv2a/pgraph/pgraph.c").read_text()
PGRAPH_H = (ROOT / "hw/xbox/nv2a/pgraph/pgraph.h").read_text()
PHYSMEM = (ROOT / "system/physmem.c").read_text()
CPUTLB = (ROOT / "accel/tcg/cputlb.c").read_text()
NV2A = (ROOT / "hw/xbox/nv2a/nv2a.c").read_text()
NV2A_INT = (ROOT / "hw/xbox/nv2a/nv2a_int.h").read_text()
PFIFO = (ROOT / "hw/xbox/nv2a/pfifo.c").read_text()
CPU_H = (ROOT / "include/hw/core/cpu.h").read_text()
CPU_COMMON = (ROOT / "cpu-common.c").read_text()


def extract_c_function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


release_target = extract_c_function(
    SURFACES, "static void d3d11_release_target(")
assert "mem_access_callback_remove_by_ref_async" in release_target
assert release_target.index("target->access_cb = NULL") < release_target.index(
    "mem_access_callback_remove_by_ref_async")
assert "mem_access_callback_remove_by_ref(" not in SURFACES

callback = extract_c_function(
    SURFACES, "static void d3d11_surface_access_callback(")
assert callback.index("vram_access_callbacks_disabled") < callback.index(
    "qemu_mutex_lock(&pg->lock)")
assert callback.index("pg->renderer->type") < callback.index(
    "pg->d3d11_renderer_state")
assert callback.index("qemu_mutex_unlock(&pg->lock)") < callback.index(
    "qemu_mutex_lock(&d->pfifo.lock)")
assert callback.index("qemu_mutex_unlock(&pg->lock)") < callback.index(
    "qemu_event_wait(&pg->flush_complete)")
after_unlock = callback[callback.index("qemu_mutex_unlock(&pg->lock)"):]
assert "r->" not in after_unlock

async_remove = extract_c_function(
    PHYSMEM, "void mem_access_callback_remove_by_ref_async(")
async_work = extract_c_function(
    PHYSMEM, "static void do_mem_access_callback_remove_by_ref_async(")
assert "async_run_on_cpu" in async_remove
assert "async_safe_run_on_cpu" not in async_remove
assert "tlb_flush(cpu)" in async_work
assert "mem_access_callback_remove_by_ref_async" in CPU_H

insert = extract_c_function(
    PHYSMEM, "MemAccessCallback *mem_access_callback_insert(")
assert "async_safe_run_on_cpu" in insert
assert "tlb_flush_all_cpus_synced(cpu)" in insert
assert not re.search(r"(?<![A-Za-z0-9_])run_on_cpu\(", insert)
assert "qemu_event_wait" not in insert
tlb_flush_all = extract_c_function(
    CPUTLB, "void tlb_flush_all_cpus_synced(")
assert "tlb_flush_by_mmuidx_all_cpus_synced" in tlb_flush_all
tlb_flush_all_workers = extract_c_function(
    CPUTLB, "void tlb_flush_by_mmuidx_all_cpus_synced(")
assert "async_safe_run_on_cpu" in tlb_flush_all_workers

shutdown = extract_c_function(NV2A, "static void nv2a_exitfn(")
shutdown_order = [
    shutdown.index("nv2a_d3d11_callbacks_shutdown(d)"),
    shutdown.index("qemu_mutex_lock(&d->pfifo.lock)"),
    shutdown.index("d->exiting = true"),
    shutdown.index("qemu_cond_broadcast(&d->pfifo.fifo_cond)"),
    shutdown.index("qemu_mutex_unlock(&d->pfifo.lock)"),
    shutdown.index("bql_unlock()"),
    shutdown.index("qemu_thread_join"),
    shutdown.index("bql_lock()"),
    shutdown.index("pgraph_destroy(&d->pgraph)"),
]
assert shutdown_order == sorted(shutdown_order)
destroy = extract_c_function(PGRAPH, "void pgraph_destroy(")
assert destroy.index("pg->renderer->ops.finalize(d)") < \
       destroy.index("nv2a_d3d11_callbacks_shutdown(d)") < \
       destroy.index("qemu_mutex_destroy(&pg->lock)")
drain = extract_c_function(
    NV2A, "void nv2a_d3d11_callbacks_shutdown(")
assert drain.index("qatomic_set(&d->vram_access_callbacks_disabled, true)") < \
       drain.index("run_on_cpu(cpu, nv2a_d3d11_callback_barrier")
assert "run_on_cpu(" in drain and "async_run_on_cpu(" not in drain
assert "cpu && cpu->created" in drain
assert "vram_access_callbacks_disabled" in NV2A_INT

puller = extract_c_function(PFIFO, "static ssize_t pfifo_run_puller(")
assert puller.count("qemu_mutex_lock(&d->pgraph.lock)") == 2
assert puller.count("qemu_mutex_unlock(&d->pgraph.lock)") == 2
assert puller.count("pgraph_method(d,") == 2
fifo_thread = extract_c_function(PFIFO, "void *pfifo_thread(")
assert fifo_thread.count("if (d->exiting)") == 2
assert fifo_thread.index("pgraph_process_pending_reports(d)") < \
       fifo_thread.index("if (d->exiting)") < \
       fifo_thread.index("if (!d->pfifo.fifo_kick)") < \
       fifo_thread.rindex("if (d->exiting)")

# Synchronous run_on_cpu and async_run_on_cpu share the same tail-queued FIFO.
queue_work = extract_c_function(CPU_COMMON, "static void queue_work_on_cpu(")
run_on_cpu = extract_c_function(CPU_COMMON, "void do_run_on_cpu(")
async_run_on_cpu = extract_c_function(CPU_COMMON, "void async_run_on_cpu(")
process_work = extract_c_function(CPU_COMMON, "void process_queued_cpu_work(")
assert "QSIMPLEQ_INSERT_TAIL(&cpu->work_list, wi, node)" in queue_work
assert "queue_work_on_cpu(cpu, &wi)" in run_on_cpu
assert "queue_work_on_cpu(cpu, wi)" in async_run_on_cpu
assert process_work.index("wi = QSIMPLEQ_FIRST(&cpu->work_list)") < \
       process_work.index("wi->func(cpu, wi->data)") < \
       process_work.index("qatomic_store_release(&wi->done, true)")
assert "while (!QSIMPLEQ_EMPTY(&cpu->work_list))" in process_work

memory_pressure = extract_c_function(
    PGRAPH, "void nv2a_renderer_memory_pressure(")
assert memory_pressure.index("qemu_mutex_lock(&pg->renderer_lock)") < \
       memory_pressure.index("qemu_mutex_lock(&pg->lock)") < \
       memory_pressure.index("ops.memory_pressure(d, level)") < \
       memory_pressure.index("qemu_mutex_unlock(&pg->lock)") < \
       memory_pressure.index("qemu_mutex_unlock(&pg->renderer_lock)")

suspend = extract_c_function(PGRAPH, "void nv2a_renderer_suspend(")
assert suspend.index("qemu_mutex_lock(&pg->renderer_lock)") < \
       suspend.index("qemu_mutex_lock(&pg->lock)") < \
       suspend.index("ops.suspend(d)") < \
       suspend.index("qemu_mutex_unlock(&pg->lock)") < \
       suspend.index("qemu_mutex_unlock(&pg->renderer_lock)")
resume = extract_c_function(PGRAPH, "bool nv2a_renderer_resume(")
assert resume.index("qemu_mutex_lock(&pg->renderer_lock)") < \
       resume.index("qemu_mutex_lock(&pg->lock)") < \
       resume.index("ops.resume(d)") < \
       resume.index("qemu_mutex_unlock(&pg->lock)") < \
       resume.index("qemu_mutex_unlock(&pg->renderer_lock)")
assert ".suspend_requires_pgraph_lock = true" in RENDERER
assert ".resume_requires_pgraph_lock = true" in RENDERER
assert "suspend_requires_pgraph_lock" in PGRAPH_H
assert "resume_requires_pgraph_lock" in PGRAPH_H

pending_reports = extract_c_function(
    PGRAPH, "void pgraph_process_pending_reports(")
assert "process_pending_reports_requires_pgraph_lock" in pending_reports
assert pending_reports.index("qemu_mutex_lock(&pg->lock)") < \
       pending_reports.index("ops.process_pending_reports(d)") < \
       pending_reports.index("qemu_mutex_unlock(&pg->lock)")
assert ".memory_pressure_requires_pgraph_lock = true" in RENDERER
assert ".process_pending_reports_requires_pgraph_lock = true" in RENDERER
assert "memory_pressure_requires_pgraph_lock" in PGRAPH_H

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct MemAccessCallback { int unused; } MemAccessCallback;
typedef struct {
    void *dsv;
    void *srv;
    void *rtv;
    void *texture;
    MemAccessCallback *access_cb;
} PGRAPHD3D11RenderTarget;
static int callback_removed;
static int releases;
static bool tcg_enabled(void) { return true; }
static void mem_access_callback_remove_by_ref_async(void *cpu,
                                                     MemAccessCallback *cb)
{
    (void)cpu;
    assert(cb != NULL);
    callback_removed++;
}
static void ID3D11DepthStencilView_Release(void *p)
{
    assert(p != NULL && callback_removed == 1);
    releases++;
}
static void ID3D11ShaderResourceView_Release(void *p)
{
    assert(p != NULL && callback_removed == 1);
    releases++;
}
static void ID3D11RenderTargetView_Release(void *p)
{
    assert(p != NULL && callback_removed == 1);
    releases++;
}
static void ID3D11Texture2D_Release(void *p)
{
    assert(p != NULL && callback_removed == 1);
    releases++;
}
static void *qemu_get_cpu(unsigned int index)
{
    assert(index == 0);
    return NULL;
}
''' + release_target + r'''
int main(void)
{
    MemAccessCallback callback = { 0 };
    PGRAPHD3D11RenderTarget target = {
        .dsv = (void *)1,
        .srv = (void *)2,
        .rtv = (void *)3,
        .texture = (void *)4,
        .access_cb = &callback,
    };
    d3d11_release_target(&target);
    assert(callback_removed == 1);
    assert(releases == 4);
    assert(target.access_cb == NULL && target.dsv == NULL &&
           target.srv == NULL && target.rtv == NULL && target.texture == NULL);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c_path = pathlib.Path(directory) / "callback-lifetime.c"
    exe_path = pathlib.Path(directory) / "callback-lifetime"
    c_path.write_text(fixture)
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_path),
         "-o", str(exe_path)],
        check=True,
    )
    subprocess.run([str(exe_path)], check=True)

remove_work = extract_c_function(
    PHYSMEM, "static void do_mem_access_callback_remove_by_ref(")
async_work = extract_c_function(
    PHYSMEM, "static void do_mem_access_callback_remove_by_ref_async(")
async_remove = extract_c_function(
    PHYSMEM, "void mem_access_callback_remove_by_ref_async(")
async_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
typedef struct MemAccessCallback { int unused; } MemAccessCallback;
typedef union { void *host_ptr; } run_on_cpu_data;
typedef struct CPUState CPUState;
typedef void (*run_on_cpu_func)(CPUState *, run_on_cpu_data);
struct CPUState {
    int mem_access_callbacks;
    run_on_cpu_func work;
    run_on_cpu_data data;
    int queued;
    int tlb_flushes;
};
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){ .host_ptr = (p) })
#define QTAILQ_REMOVE(head, elm, field) do { (void)(head); (void)(elm); } while (0)
static int frees;
static void g_free(void *p) { frees++; free(p); }
static void tlb_flush(CPUState *cpu) { cpu->tlb_flushes++; }
static void async_run_on_cpu(CPUState *cpu, run_on_cpu_func func,
                             run_on_cpu_data data)
{
    assert(cpu->work == NULL);
    cpu->work = func;
    cpu->data = data;
    cpu->queued++;
}
''' + remove_work + async_work + async_remove + r'''
int main(void)
{
    CPUState cpu = { 0 };
    MemAccessCallback *cb = malloc(sizeof(*cb));
    assert(cb != NULL);
    mem_access_callback_remove_by_ref_async(&cpu, cb);
    assert(cpu.queued == 1 && frees == 0 && cpu.tlb_flushes == 0);
    cpu.work(&cpu, cpu.data);
    assert(frees == 1 && cpu.tlb_flushes == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c_path = pathlib.Path(directory) / "async-remove.c"
    exe_path = pathlib.Path(directory) / "async-remove"
    c_path.write_text(async_fixture)
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_path),
         "-o", str(exe_path)],
        check=True,
    )
    subprocess.run([str(exe_path)], check=True)

callback_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONFIG_DISPLAY_RENDERER_D3D11 7
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
typedef uint64_t hwaddr;
typedef struct MemoryRegion { int unused; } MemoryRegion;
typedef struct QemuMutex { int unused; } QemuMutex;
typedef struct QemuEvent { int unused; } QemuEvent;
typedef struct PGRAPHRenderer { int type; } PGRAPHRenderer;
typedef struct PGRAPHD3D11RenderTarget {
    void *texture;
    hwaddr vram_address;
    size_t storage_length;
} PGRAPHD3D11RenderTarget;
typedef struct PGRAPHD3D11State {
    struct {
        PGRAPHD3D11RenderTarget color;
        PGRAPHD3D11RenderTarget depth_stencil;
    } render_targets;
} PGRAPHD3D11State;
typedef struct {
    bool draw_dirty;
    bool buffer_dirty;
} Surface;
typedef struct PGRAPHState {
    QemuMutex lock;
    PGRAPHRenderer *renderer;
    PGRAPHD3D11State *d3d11_renderer_state;
    Surface surface_color;
    Surface surface_zeta;
    QemuEvent flush_complete;
    bool flush_pending;
} PGRAPHState;
typedef struct NV2AState {
    bool vram_access_callbacks_disabled;
    PGRAPHState pgraph;
    struct { QemuMutex lock; } pfifo;
} NV2AState;
static int lock_calls;
static int unlock_calls;
static void qemu_mutex_lock(QemuMutex *mutex)
{
    assert(mutex != NULL);
    lock_calls++;
}
static void qemu_mutex_unlock(QemuMutex *mutex)
{
    assert(mutex != NULL);
    unlock_calls++;
}
static void qemu_event_reset(QemuEvent *event) { (void)event; }
static void qemu_event_wait(QemuEvent *event) { (void)event; }
static void pfifo_kick(NV2AState *d) { (void)d; }
''' + extract_c_function(
    SURFACES, "static bool d3d11_ranges_overlap(") + callback + r'''
int main(void)
{
    NV2AState d = { 0 };
    PGRAPHRenderer other = { .type = 99 };
    PGRAPHRenderer d3d11 = { .type = CONFIG_DISPLAY_RENDERER_D3D11 };

    d.pgraph.renderer = &other;
    d.pgraph.d3d11_renderer_state = (PGRAPHD3D11State *)1;
    d3d11_surface_access_callback(&d, NULL, 0x100, 4, true);
    assert(lock_calls == 1 && unlock_calls == 1);

    d.vram_access_callbacks_disabled = true;
    d.pgraph.renderer = &d3d11;
    d3d11_surface_access_callback(&d, NULL, 0x100, 4, true);
    assert(lock_calls == 1 && unlock_calls == 1);

    d.vram_access_callbacks_disabled = false;
    PGRAPHD3D11State r = {
        .render_targets.color = {
            .texture = (void *)1,
            .vram_address = 0x100,
            .storage_length = 0x100,
        },
    };
    d.pgraph.d3d11_renderer_state = &r;
    d3d11_surface_access_callback(&d, NULL, 0x100, 4, true);
    assert(lock_calls == 2 && unlock_calls == 2);
    assert(d.pgraph.surface_color.buffer_dirty);
    return 0;
}
'''

destroy_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct CPUState { bool created; } CPUState;
typedef union { void *host_ptr; } run_on_cpu_data;
typedef void (*run_on_cpu_func)(CPUState *, run_on_cpu_data);
typedef struct NV2AState NV2AState;
typedef struct PGRAPHState PGRAPHState;
typedef struct PGRAPHRenderer PGRAPHRenderer;
struct PGRAPHRenderer {
    int type;
    struct { void (*finalize)(NV2AState *); } ops;
};
struct PGRAPHState {
    PGRAPHRenderer *renderer;
    int lock;
};
struct NV2AState {
    PGRAPHState pgraph;
    bool vram_access_callbacks_disabled;
};
typedef struct { int unused; } QemuMutex;
#define CONFIG_DISPLAY_RENDERER_D3D11 7
#define RUN_ON_CPU_NULL ((run_on_cpu_data){ .host_ptr = NULL })
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define qatomic_set(p, v) (*(p) = (v))
static CPUState cpu0 = { .created = true };
static int pending_removals;
static int finalized;
static int destroyed;
static int run_calls;
static bool tcg_enabled(void) { return true; }
static CPUState *qemu_get_cpu(int index)
{
    assert(index == 0);
    return &cpu0;
}
static void run_on_cpu(CPUState *cpu, run_on_cpu_func func,
                       run_on_cpu_data data)
{
    assert(cpu == &cpu0);
    run_calls++;
    pending_removals = 0;
    func(cpu, data);
}
static void qemu_mutex_destroy(int *lock)
{
    assert(lock != NULL && finalized == 1 && pending_removals == 0);
    destroyed++;
}
static void finalize_renderer(NV2AState *d)
{
    assert(d != NULL);
    finalized++;
    pending_removals = 1;
}
''' + extract_c_function(
    NV2A, "static void nv2a_d3d11_callback_barrier(") + extract_c_function(
    NV2A, "void nv2a_d3d11_callbacks_shutdown(") + destroy + r'''
int main(void)
{
    NV2AState early = { 0 };
    cpu0.created = false;
    nv2a_d3d11_callbacks_shutdown(&early);
    assert(early.vram_access_callbacks_disabled && run_calls == 0);
    cpu0.created = true;

    PGRAPHRenderer renderer = {
        .type = CONFIG_DISPLAY_RENDERER_D3D11,
        .ops.finalize = finalize_renderer,
    };
    NV2AState d = { .pgraph.renderer = &renderer };
    pgraph_destroy(&d.pgraph);
    assert(finalized == 1 && destroyed == 1);
    assert(run_calls == 1);
    assert(d.vram_access_callbacks_disabled);
    return 0;
}
'''

for name, source in (("callback-switch", callback_fixture),
                     ("callback-teardown", destroy_fixture)):
    with tempfile.TemporaryDirectory() as directory:
        c_path = pathlib.Path(directory) / f"{name}.c"
        exe_path = pathlib.Path(directory) / name
        c_path.write_text(source)
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(c_path), "-o", str(exe_path)],
            check=True,
        )
        subprocess.run([str(exe_path)], check=True)

print("PASS: callback retirement, renderer guard, FIFO teardown drain, TLB flush, "
      "and D3D11 lock order")


fifo_exit_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct QemuMutex { bool locked; } QemuMutex;
typedef struct QemuCond { int unused; } QemuCond;
typedef struct NV2AState {
    bool exiting;
    struct {
        QemuMutex lock;
        QemuCond fifo_cond;
        QemuCond fifo_idle_cond;
        bool fifo_kick;
        bool halt;
    } pfifo;
} NV2AState;
static int pending_calls;
static void qemu_mutex_lock(QemuMutex *mutex)
{
    assert(!mutex->locked);
    mutex->locked = true;
}
static void qemu_mutex_unlock(QemuMutex *mutex)
{
    assert(mutex->locked);
    mutex->locked = false;
}
static void qemu_cond_broadcast(QemuCond *cond) { (void)cond; }
static void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    (void)cond;
    (void)mutex;
    assert(!"PFIFO must not sleep after exit was published");
}
static void pgraph_init_thread(NV2AState *d) { (void)d; }
static void rcu_register_thread(void) { }
static void rcu_unregister_thread(void) { }
static void request_exit(NV2AState *d)
{
    qemu_mutex_lock(&d->pfifo.lock);
    d->exiting = true;
    qemu_cond_broadcast(&d->pfifo.fifo_cond);
    qemu_mutex_unlock(&d->pfifo.lock);
}
static void pgraph_process_pending(NV2AState *d)
{
    if (pending_calls++ == 0) {
        /* Model PGRAPH work dropping PFIFO.lock while the exit path signals. */
        qemu_mutex_unlock(&d->pfifo.lock);
        request_exit(d);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}
static void pfifo_run_pusher(NV2AState *d) { (void)d; }
static void pgraph_process_pending_reports(NV2AState *d) { (void)d; }
''' + fifo_thread + r'''
int main(void)
{
    NV2AState d = { 0 };
    pfifo_thread(&d);
    assert(d.exiting && pending_calls == 1);
    assert(!d.pfifo.lock.locked);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    c_path = pathlib.Path(directory) / "pfifo-exit.c"
    exe_path = pathlib.Path(directory) / "pfifo-exit"
    c_path.write_text(fifo_exit_fixture)
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_path),
         "-o", str(exe_path)],
        check=True,
    )
    subprocess.run([str(exe_path)], check=True)

print("PASS: PFIFO exit handoff does not sleep after exit publication")
