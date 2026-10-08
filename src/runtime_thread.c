/* Guest threads on host threads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. On Windows GS is the TEB: the loader points the instruction at a TEB
 * TLS slot that holds the guest TCB instead.
 * Priorities/affinity are recorded, not enforced by a PS4 scheduler. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_prof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#ifdef _WIN32
#include <malloc.h>
/* pthread_exit unwinds guest frames that have no unwind data: see RUNTIME_RECOVER_SET. */
#define EXIT_SET(buf) RUNTIME_RECOVER_SET(buf)
#define EXIT_JUMP(buf) RUNTIME_RECOVER_JUMP(buf)
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#define EXIT_SET(buf) setjmp(buf)
#define EXIT_JUMP(buf) longjmp(buf,1)
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    HostThread host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
#ifdef _WIN32
    RuntimeRecoverBuf exit_jump;
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static HostMutex lock=HOST_MUTEX_INIT;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
static void set_gs(void *base) {
#ifdef _WIN32
    runtime_win_set_tcb(base);
#else
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
#endif
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
#ifdef _WIN32
    unsigned char *block=_aligned_malloc((total+63)&~(size_t)63,64);
#else
    unsigned char *block=aligned_alloc(64,(total+63)&~(size_t)63);
#endif
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    host_lock(&lock); t->next=threads; threads=t; host_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
int runtime_threads_snapshot(RuntimeThreadInfo *out, int max) {
    int n=0;
    host_lock(&lock);
    for (GuestThread *it=threads; it && n<max; it=it->next) {
        if (it->finished || !it->host) continue;
        out[n].handle=(void *)it->host;
        snprintf(out[n].name,sizeof(out[n].name),"%s",it->name);
        ++n;
    }
    host_unlock(&lock);
    return n;
}
/* Lockless membership probe for sections that already hold the lock: records are freed
 * on reap, so every handle dereference must share one lock section with the test. */
static int thread_listed(const GuestThread *t) {
    for (const GuestThread *it=threads;it;it=it->next) if (it==t) return 1;
    return 0;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    host_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    host_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    host_lock(&lock); a->next=attributes; attributes=a; host_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    host_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    host_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    /* The thread record is freed by join's reap: copy its fields inside the lock, then
     * write the attr outside it (find_attr validated `a`, but keep its list linkage). */
    ThreadAttr copy;
    int detached;
    host_lock(&lock);
    if (!thread_listed(thread)) { host_unlock(&lock); return ERR(3); }
    copy=((const GuestThread *)thread)->attr;
    detached=((const GuestThread *)thread)->detached;
    host_unlock(&lock);
    ThreadAttr *next=a->next;
    *a=copy; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
#ifdef _WIN32
    runtime_win_set_thread_name(host);
#else
    pthread_setname_np(pthread_self(),host);
#endif
}
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    set_host_name(t->name);
    if (!EXIT_SET(t->exit_jump)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    host_lock(&lock); t->finished=1; ++exited; host_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
#ifdef _WIN32
    /* The stack is committed at creation (guest code does not probe guard pages), and the
     * loader's bottom-up layout keeps it below 1 TiB as on PS4. */
    int e=host_thread_start(&t->host,stack_bytes,host_start,t);
#else
    pthread_attr_t host;
    pthread_attr_init(&host);
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
#endif
    if (e) { fprintf(stderr,"STOP: host thread creation failed: %d\n",e); exit(21); }
    /* A detached-attr thread is born claimed (detached=1 from the attr above), which also
     * refuses guest detach/join at the gate, so exactly one initial host detach happens
     * here. Joinable threads keep detached=0: join and detach stay legal as on POSIX. */
    int initial_detached;
    host_lock(&lock);
    initial_detached=t->detached;
    host_unlock(&lock);
    if (initial_detached) host_thread_detach(t->host);
    host_lock(&lock); ++created; host_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!t) return ERR(3);
    if (t==current) return ERR(11);
    /* Claim the join under the lock, then join outside it: two guest threads joining the
     * same target would otherwise both pass the check and call host_thread_join twice —
     * the second returns non-zero and exits the process. The lock also serializes against
     * thread_detach, which is a POSIX-legal concurrent pairing; claiming is what makes it
     * safe. Joining while holding the lock itself would deadlock: the target thread takes
     * the same lock to mark itself finished. */
    host_lock(&lock);
    const int listed=thread_listed(t);
    if (!listed) { host_unlock(&lock); return ERR(3); }
    if (t->host_owned) { host_unlock(&lock); return ERR(3); }
    if (t->detached || t->joined) { host_unlock(&lock); return ERR(22); }
    t->joined=1;
    host_unlock(&lock);
    int e=host_thread_join(t->host);
    if (e) { fprintf(stderr,"STOP: host thread join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    /* Reap: unlink first so any later handle dereference through thread_listed returns
     * ESRCH, then release the TLS block and the record itself (joined once, unreferenced). */
    host_lock(&lock);
    for (GuestThread **p=&threads;*p;p=&(*p)->next) if (*p==t) { *p=t->next; break; }
    ++joined_count;
    host_unlock(&lock);
#ifdef _WIN32
    if (t->tls_block) _aligned_free(t->tls_block);
#else
    free(t->tls_block);
#endif
    free(t);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!t) return ERR(3);
    /* Same claim discipline as thread_join, with membership and state validated in one
     * locked section (records are freed on reap): refused once a join has been claimed
     * (the host handle is about to be reaped), and double-detach becomes a clean EINVAL
     * instead of a second host_thread_detach on the same handle. */
    host_lock(&lock);
    const int listed=thread_listed(t);
    if (!listed) { host_unlock(&lock); return ERR(3); }
    if (t->detached || t->joined) { host_unlock(&lock); return ERR(22); }
    t->detached=1;
    host_unlock(&lock);
    if (!t->host_owned) host_thread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    EXIT_JUMP(t->exit_jump);
}
static ABI int32_t thread_yield(void) { host_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    int value;
    host_lock(&lock);
    if (!thread_listed(t)) { host_unlock(&lock); return ERR(3); }
    value=t->attr.prio;
    host_unlock(&lock);
    if (!prio) return ERR(22);
    *prio=value; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    host_lock(&lock);
    if (!thread_listed(t)) { host_unlock(&lock); return ERR(3); }
    t->attr.prio=prio;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    host_lock(&lock);
    if (!thread_listed(t)) { host_unlock(&lock); return ERR(3); }
    t->attr.affinity=mask;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    uint64_t value;
    host_lock(&lock);
    if (!thread_listed(t)) { host_unlock(&lock); return ERR(3); }
    value=t->attr.affinity;
    host_unlock(&lock);
    if (!mask) return ERR(22);
    *mask=value; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    /* The record may be reaped concurrently: write the field under the lock, run the
     * host rename API outside it from a local copy. */
    char copy[sizeof t->name];
    host_lock(&lock);
    if (!thread_listed(t)) { host_unlock(&lock); return ERR(3); }
    if (!name) { host_unlock(&lock); return ERR(22); }
    snprintf(t->name,sizeof(t->name),"%s",name);
    memcpy(copy,t->name,sizeof copy);
    host_unlock(&lock);
    if (t==current) set_host_name(copy);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#p#J",thread_self}, {"EotR8a3ASf4#I#J",thread_self},
        {"9BcDykPmo1I#p#J",guest_error},
        {"nsYoNRywwNg#p#J",attr_init}, {"62KCwEMmzcM#p#J",attr_destroy},
        {"x1X76arYMxU#p#J",attr_get}, {"8+s5BzZjxSg#p#J",attr_affinity},
        {"UTXzJbWhhTE#p#J",attr_set_stack}, {"-Wreprtu0Qs#p#J",attr_set_detach},
        {"4+h9EzwKF4I#p#J",attr_set_policy}, {"DzES9hQF4f4#p#J",attr_set_param},
        {"3qxgM4ezETA#p#J",attr_set_affinity}, {"eXbUSpEaTsA#p#J",attr_set_inherit},
        {"JaRMy+QcpeU#p#J",attr_get_detach}, {"-fA+7ZlGDQs#p#J",attr_get_stack},
        {"FXPWHNk8Of0#p#J",attr_get_param}, {"El+cQ20DynU#p#J",attr_set_guard},
        {"6UgtwV+0zb4#p#J",thread_create}, {"onNY9Byn-W8#p#J",thread_join},
        {"4qGrR6eoP9Y#p#J",thread_detach}, {"3kg7rT0NQIs#p#J",thread_exit},
        {"T72hz6ffq08#p#J",thread_yield}, {"1tKyG7RlMJo#p#J",thread_get_prio},
        {"W0Hpm2X0uPE#p#J",thread_set_prio}, {"bt3CTBKmGyI#p#J",thread_set_affinity}, {"rcrVFJsQWRY#p#J",thread_get_affinity},
        {"GBUY7ywdULE#p#J",thread_rename}, {"3PtV6p3QNX4#p#J",thread_equal},
        {"wtkt-teR1so#I#J",posix_attr_init}, {"zHchY8ft5pk#I#J",posix_attr_destroy},
        {"E+tyo3lp5Lw#I#J",posix_attr_set_detach}, {"2Q0z6rnBrTE#I#J",posix_attr_set_stack},
        {"euKRgm0Vn2M#I#J",posix_attr_set_param}, {"OxhIB8LB-PQ#I#J",posix_create},
        {"Jmi+9w9u0E4#I#J",posix_create_name}, {"h9CcP3J0oVM#I#J",posix_join},
        {"FJrT5LuUBAU#I#J",thread_exit},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    host_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    host_unlock(&lock);
}
