#!/usr/bin/env python3
"""Guard embedded-host API quiescence before QEMU/display teardown."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST = (ROOT / "system/host.c").read_text()
HOST_H = (ROOT / "include/qemu/qemu-host.h").read_text()
UWP_HOST = (ROOT / "UWP-Port/XemuHost.cpp").read_text()
UWP_PAGE = (ROOT / "UWP-Port/DirectXPage.xaml.cpp").read_text()


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


begin = extract_c_function(HOST, "static bool host_api_call_begin(")
end = extract_c_function(HOST, "static void host_api_call_end(")
ensure_cond = extract_c_function(
    HOST, "static void host_api_calls_ensure_cond_locked(")
storage_begin = extract_c_function(
    HOST, "static bool host_storage_api_call_begin(")
cleanup = extract_c_function(HOST, "int qemu_host_cleanup(")
assert begin.index("g_mutex_lock(&host_state_lock)") < begin.index(
        "if (!host_initialized || host_cleaned)") < begin.index(
        "host_api_calls_active++") < begin.rindex("g_mutex_unlock(&host_state_lock)")
assert end.index("--host_api_calls_active") < end.index(
    "g_cond_broadcast(&host_api_calls_drained)")
assert "g_cond_init(&host_api_calls_drained)" in ensure_cond
assert storage_begin.index("if (host_cleaned)") < storage_begin.index(
    "host_api_calls_ensure_cond_locked()") < storage_begin.index(
        "host_api_calls_active++")
assert "!host_initialized" not in storage_begin
assert cleanup.index("host_cleaned = true") < cleanup.index(
    "while (host_api_calls_active)") < cleanup.index(
        "g_cond_wait(&host_api_calls_drained, &host_state_lock)") < cleanup.index(
            "xemu_stop_embedded_display()") < cleanup.index("qemu_cleanup(status)") < cleanup.index(
                "xemu_shutdown_embedded_display()")
assert "GPOINTER_TO_SIZE(g_private_get(&host_api_call_depth))" in cleanup
assert "return -EBUSY" in cleanup
assert "waits for in-flight renderer, graphics" in HOST_H
assert "storage calls before destroying QEMU/display state" in HOST_H

storage_apis = (
    "int qemu_host_storage_open(",
    "int64_t qemu_host_storage_read(",
    "int64_t qemu_host_storage_write(",
    "int qemu_host_storage_close(",
    "int64_t qemu_host_storage_seek(",
    "int qemu_host_storage_stat(",
    "int qemu_host_storage_mkdir(",
    "int qemu_host_storage_unlink(",
    "int qemu_host_storage_rename(",
    "int qemu_host_storage_flush(",
    "int qemu_host_storage_readdir(",
    "int qemu_host_storage_truncate(",
    "bool qemu_host_storage_path_is_brokered(",
)
for signature in storage_apis:
    function = extract_c_function(HOST, signature)
    assert "HOST_STORAGE_API_CALL_OR_RETURN(" in function, signature

# The public API requires serialized frame calls; in this UWP host its sole
# call site is the XAML Rendering event handler on the UI thread.
assert "concurrent frame calls are\n * unsupported." in HOST_H
assert UWP_HOST.count("m_renderFrame()") == 1
assert "CompositionTarget::Rendering +=" in UWP_PAGE
assert "OnRendering(sender, args)" in UWP_PAGE
assert UWP_PAGE.count("m_xemu->RenderFrame();") == 1

# APIs that touch renderer, graphics, control, video metrics or QEMU input must
# hold an admission reference for their entire access to global state.
for name in (
    "qemu_host_render_frame",
    "qemu_host_graphics_suspend",
    "qemu_host_graphics_resume",
    "qemu_host_notify_memory_pressure",
    "qemu_host_get_video_metrics",
    "qemu_host_pause",
    "qemu_host_resume",
    "qemu_host_request_shutdown",
    "qemu_host_request_stop",
    "qemu_host_reset",
    "qemu_host_send_key_number",
    "qemu_host_send_key_qcode",
    "qemu_host_send_pointer_rel",
    "qemu_host_send_pointer_abs",
    "qemu_host_send_pointer_button",
    "qemu_host_pointer_is_absolute",
):
    signature = (
        "bool qemu_host_pointer_is_absolute("
        if name == "qemu_host_pointer_is_absolute"
        else f"int {name}("
    )
    function = extract_c_function(HOST, signature)
    assert "host_api_call_begin()" in function, name
    assert "host_api_call_end()" in function, name

set_target = extract_c_function(
    HOST, "int qemu_host_set_d3d12_present_target(")
assert set_target.index("g_mutex_lock(&host_state_lock)") < set_target.index(
    "if (host_cleaned)") < set_target.index("host_d3d12_present_target.width")

fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef void *gpointer;
typedef size_t gsize;
typedef pthread_mutex_t GMutex;
typedef struct { pthread_cond_t cond; bool initialized; } GCond;
typedef struct { int unused; } GPrivate;
#define GPOINTER_TO_SIZE(p) ((gsize)(uintptr_t)(p))
#define GSIZE_TO_POINTER(s) ((gpointer)(uintptr_t)(s))
#define g_assert(x) assert(x)
static _Thread_local gpointer tls_api_depth;
static gpointer g_private_get(GPrivate *key) { (void)key; return tls_api_depth; }
static void g_private_set(GPrivate *key, gpointer value)
{
    (void)key;
    tls_api_depth = value;
}
static void g_mutex_lock(GMutex *mutex) { assert(pthread_mutex_lock(mutex) == 0); }
static void g_mutex_unlock(GMutex *mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
static void g_cond_init(GCond *cond)
{
    assert(!cond->initialized);
    assert(pthread_cond_init(&cond->cond, NULL) == 0);
    cond->initialized = true;
}
static void g_cond_wait(GCond *cond, GMutex *mutex)
{
    assert(cond->initialized);
    assert(pthread_cond_wait(&cond->cond, mutex) == 0);
}
static void g_cond_broadcast(GCond *cond)
{
    assert(cond->initialized);
    assert(pthread_cond_broadcast(&cond->cond) == 0);
}
static GMutex host_state_lock = PTHREAD_MUTEX_INITIALIZER;
static GCond host_api_calls_drained;
static bool host_api_calls_cond_initialized;
static GPrivate host_api_call_depth;
static bool host_initialized = true;
static bool host_cleaned;
static unsigned int host_api_calls_active;
static atomic_bool worker_entered;
static atomic_bool cleanup_waiting;
static atomic_bool allow_worker_exit;
static atomic_bool worker_exited;
''' + begin + end + ensure_cond + storage_begin + r'''
static void *api_worker(void *unused)
{
    (void)unused;
    assert(host_storage_api_call_begin());
    atomic_store(&worker_entered, true);
    while (!atomic_load(&allow_worker_exit)) {
        sched_yield();
    }
    host_api_call_end();
    atomic_store(&worker_exited, true);
    return NULL;
}
static void *release_worker_when_cleanup_waits(void *unused)
{
    (void)unused;
    while (!atomic_load(&cleanup_waiting)) {
        sched_yield();
    }
    atomic_store(&allow_worker_exit, true);
    return NULL;
}
int main(void)
{
    pthread_t worker, releaser;

    host_initialized = false;
    assert(host_storage_api_call_begin());
    assert(host_api_calls_cond_initialized);
    assert(host_api_calls_active == 1);
    host_api_call_end();
    assert(host_api_calls_active == 0);
    host_initialized = true;

    assert(host_api_call_begin());
    host_api_call_end();

    assert(host_storage_api_call_begin());
    assert(host_storage_api_call_begin());
    host_api_call_end();
    host_api_call_end();

    assert(pthread_create(&worker, NULL, api_worker, NULL) == 0);
    while (!atomic_load(&worker_entered)) {
        sched_yield();
    }
    assert(pthread_create(&releaser, NULL, release_worker_when_cleanup_waits,
                          NULL) == 0);

    g_mutex_lock(&host_state_lock);
    host_cleaned = true;
    if (host_api_calls_active) {
        atomic_store(&cleanup_waiting, true);
    }
    while (host_api_calls_active) {
        g_cond_wait(&host_api_calls_drained, &host_state_lock);
    }
    g_mutex_unlock(&host_state_lock);

    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(releaser, NULL) == 0);
    assert(atomic_load(&worker_exited));
    assert(host_api_calls_active == 0);
    assert(!host_storage_api_call_begin());
    return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c_path = pathlib.Path(directory) / "host-api-quiescence.c"
    exe_path = pathlib.Path(directory) / "host-api-quiescence"
    c_path.write_text(fixture)
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
         str(c_path), "-o", str(exe_path)],
        check=True,
    )
    subprocess.run([str(exe_path)], check=True)

print("embedded host API quiescence checks passed")
