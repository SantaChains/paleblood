/* Sampling profiler (BB_PROF=1): see runtime_prof.h. Windows implementation —
 * suspend/sample/resume every guest thread, append (rip, slot) into a preallocated
 * ring, and aggregate to stdout from the sampler itself (the only thread touching
 * the report buffers). No heap allocation happens on this thread: the ring, the
 * snapshot slots and the report scratch are all static, so a suspended thread
 * holding the heap lock can never stall the sampler. */
#include "runtime_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define PROF_RING (1u << 17) /* power of two: 131k samples, ~1 MiB static */
#define PROF_MAX_THREADS 64
#define PROF_REPORT_ROWS 12

typedef struct {
    uintptr_t rip;
    uint32_t thread;
} ProfSample;

static ProfSample ring[PROF_RING];
static volatile unsigned long long prof_head, reported;
static RuntimeThreadInfo current[PROF_MAX_THREADS]; /* latest snapshot, for report labels */
static uintptr_t image_base;
static size_t image_size;
static unsigned prof_interval_ms = 10;

/* Sort key: rip (64-byte block), then thread slot — qsort over the report window,
 * aggregated linearly into hot rows, then re-sorted by count for the top list. */
static int sample_cmp(const void *a, const void *b) {
    const ProfSample *x = a, *y = b;
    if (x->rip != y->rip) return x->rip < y->rip ? -1 : 1;
    if (x->thread != y->thread) return x->thread < y->thread ? -1 : 1;
    return 0;
}

typedef struct {
    uintptr_t rip;
    unsigned long long count;
    uint32_t thread; /* the thread with the most samples in this block */
} HotRow;

static HotRow hot[8192];
static int hot_cmp(const void *a, const void *b) {
    const HotRow *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    if (x->rip != y->rip) return x->rip < y->rip ? -1 : 1;
    return 0;
}

static void report(void) {
    unsigned long long from = reported;
    unsigned long long count = prof_head - from;
    if (!count) return;
    reported = prof_head;
    static ProfSample window[PROF_RING / 2];
    unsigned long long n = count > PROF_RING / 2 ? PROF_RING / 2 : count;
    for (unsigned long long i = 0; i < n; ++i) {
        window[i] = ring[(from + i) & (PROF_RING - 1)];
        /* Coarsen to 64-byte blocks: exact RIPs fragment the report across the
         * instruction stream, while one hot basic block should read as one line. */
        window[i].rip &= ~(uintptr_t)63;
    }
    qsort(window, (size_t)n, sizeof window[0], sample_cmp);

    /* Aggregate the sorted window into hot rows; the row's thread label is the first
     * sample's thread (good enough for a report — ties mean shared heat across threads). */
    int hotn = 0;
    unsigned long long run = 0;
    for (unsigned long long i = 0; i < n; ++i) {
        ++run;
        if (i + 1 < n && window[i + 1].rip == window[i].rip) continue;
        if (hotn < (int)(sizeof hot / sizeof hot[0])) {
            hot[hotn].rip = window[i].rip;
            hot[hotn].count = run;
            hot[hotn].thread = window[i].thread;
            ++hotn;
        }
        run = 0;
    }
    qsort(hot, (size_t)hotn, sizeof hot[0], hot_cmp);

    printf("Prof: %llu samples (interval %u ms); top %d hot 64-byte blocks (any thread):\n",
           count, prof_interval_ms, PROF_REPORT_ROWS);
    for (int i = 0; i < hotn && i < PROF_REPORT_ROWS; ++i) {
        const char *name = current[hot[i].thread].name;
        if (image_base && hot[i].rip - image_base < image_size)
            printf("  %6.2f%% %-24s guest+0x%llx\n",
                   100.0 * hot[i].count / n, name && name[0] ? name : "?",
                   (unsigned long long)(hot[i].rip - image_base));
        else
            printf("  %6.2f%% %-24s host+0x%llx\n",
                   100.0 * hot[i].count / n, name && name[0] ? name : "?",
                   (unsigned long long)hot[i].rip);
    }
}

static DWORD WINAPI prof_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        int n = runtime_threads_snapshot(current, PROF_MAX_THREADS);
        for (int i = 0; i < n; ++i) {
            HANDLE h = (HANDLE)current[i].handle;
            if (!h) continue;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(h) == (DWORD)-1) continue;
            if (GetThreadContext(h, &ctx)) {
                unsigned long long slot = prof_head;
                ring[slot & (PROF_RING - 1)] = (ProfSample){ctx.Rip, (uint32_t)i};
                prof_head = slot + 1;
            }
            ResumeThread(h);
        }
        if (prof_head - reported >= 30000) /* 30k samples ≈ 12 s at the default interval */
            report();
        Sleep(prof_interval_ms);
    }
    return 0;
}
#endif

void runtime_prof_start(uintptr_t image_base_, size_t image_size_) {
#ifdef _WIN32
    image_base = image_base_;
    image_size = image_size_;
    const char *ms = getenv("BB_PROF_MS");
    if (ms) {
        unsigned v = (unsigned)atoi(ms);
        if (v >= 1 && v <= 1000) prof_interval_ms = v;
    }
    if (!CreateThread(NULL, 1 << 16, prof_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL))
        printf("Prof: failed to start the sampler thread\n");
    printf("Prof: sampling every %u ms (BB_PROF_MS), report every 30k samples; "
           "image at %llx (%zu bytes)\n",
           prof_interval_ms, (unsigned long long)image_base, image_size);
#else
    (void)image_base_;
    (void)image_size_;
#endif
}
