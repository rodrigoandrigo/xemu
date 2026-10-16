/*
 * xemu high-resolution performance counters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/xemu-qpc.h"

typedef struct XemuQpcAccumulator {
    int64_t ticks;
    int64_t count;
    int64_t maximum;
} XemuQpcAccumulator;

static XemuQpcAccumulator accumulators[XEMU_QPC__COUNT];
static int64_t report_timestamp;

static int64_t xemu_qpc_frequency(void)
{
#ifdef _WIN32
    static int64_t frequency;
    int64_t value = qatomic_read(&frequency);

    if (!value) {
        LARGE_INTEGER result;
        QueryPerformanceFrequency(&result);
        value = result.QuadPart;
        qatomic_cmpxchg(&frequency, 0, value);
        value = qatomic_read(&frequency);
    }
    return value;
#else
    return NANOSECONDS_PER_SECOND;
#endif
}

int64_t xemu_qpc_start(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
#else
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
#endif
}

void xemu_qpc_end(XemuQpcMetric metric, int64_t start)
{
    int64_t elapsed;

    if (metric >= XEMU_QPC__COUNT || start <= 0) {
        return;
    }
    elapsed = xemu_qpc_start() - start;
    if (elapsed < 0) {
        return;
    }
    qatomic_add(&accumulators[metric].ticks, elapsed);
    qatomic_inc(&accumulators[metric].count);
    int64_t maximum = qatomic_read(&accumulators[metric].maximum);
    while (elapsed > maximum) {
        int64_t previous = qatomic_cmpxchg(&accumulators[metric].maximum,
                                          maximum, elapsed);
        if (previous == maximum) {
            break;
        }
        maximum = previous;
    }
}

bool xemu_qpc_format_if_due(char *buffer, size_t size)
{
    static const char *names[XEMU_QPC__COUNT] = {
        "cpu", "io-r", "io-w", "nv2a", "mesa", "shader", "fence",
        "present",
        "fb-wait", "hud", "shader-cache",
    };
    int64_t now = xemu_qpc_start();
    int64_t frequency = xemu_qpc_frequency();
    int64_t previous = qatomic_read(&report_timestamp);
    size_t used = 0;
    int i;

    if (!buffer || !size || now - previous < frequency ||
        qatomic_cmpxchg(&report_timestamp, previous, now) != previous) {
        return false;
    }

    used += g_snprintf(buffer + used, size - used, "[perf/qpc]");
    for (i = 0; i < XEMU_QPC__COUNT && used < size; i++) {
        int64_t ticks = qatomic_xchg(&accumulators[i].ticks, 0);
        int64_t count = qatomic_xchg(&accumulators[i].count, 0);
        int64_t maximum = qatomic_xchg(&accumulators[i].maximum, 0);
        double total_ms = (double)ticks * 1000.0 / (double)frequency;
        double average_ms = count ? total_ms / (double)count : 0.0;

        used += g_snprintf(buffer + used, size - used,
                           " %s=%.3fms/%" PRId64 "(avg %.3fms,max %.3fms)",
                           names[i], total_ms, count, average_ms,
                           (double)maximum * 1000.0 / (double)frequency);
    }
    buffer[size - 1] = '\0';
    return true;
}
