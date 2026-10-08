#ifndef BB_RUNTIME_PROF_H
#define BB_RUNTIME_PROF_H
#include <stdint.h>
#include <stddef.h>

/* Sampling profiler (BB_PROF=1): a lowest-priority thread periodically suspends every
 * guest thread, samples its RIP, and aggregates to stdout every half ring. The sampler
 * performs zero heap allocation — the ring is preallocated, so it cannot deadlock on a
 * heap lock held by a suspended thread. Interval: BB_PROF_MS (default 10). */
void runtime_prof_start(uintptr_t image_base, size_t image_size);

/* runtime_thread.c: snapshot the live guest threads (handle + name copies, so the
 * sampler never holds the thread-table lock while suspending). Returns the count. */
typedef struct {
    void *handle; /* HostThread (HANDLE on Windows), kept type-erased for the header */
    char name[32];
} RuntimeThreadInfo;
int runtime_threads_snapshot(RuntimeThreadInfo *out, int max);
#endif
