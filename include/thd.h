#ifndef __NK_THD_H__
#define __NK_THD_H__

#include "kernel.h"
#include "queue.h"
#include "databuf.h"
#include "alloc.h"

// typedefs.
typedef struct nk_schob nk_schob;
typedef struct nk_thd_attrs nk_thd_attrs;
typedef struct nk_thd nk_thd;
typedef struct nk_dpc_attrs nk_dpc_attrs;
typedef struct nk_dpc nk_dpc;
typedef struct nk_host nk_host;
typedef struct nk_hostthd nk_hostthd;

// -------------- schobs: schedulable entities. --------------

typedef enum nk_schob_state {
  NK_SCHOB_STATE_READY,   // on a scheduler run queue.
  NK_SCHOB_STATE_RUNNING, // actually running in a host thread.
  NK_SCHOB_STATE_WAITING, // waiting at a port/semaphore.
  NK_SCHOB_STATE_ZOMBIE,  // zombie, now waiting to be freed.
} nk_schob_state;

typedef enum nk_schob_type {
  NK_SCHOB_TYPE_THD, // cooperatively-scheduled thread with own stack.
  NK_SCHOB_TYPE_DPC, // deferred procedure call to execute only once.
} nk_schob_type;

struct nk_schob {
  nk_schob_type type;
  nk_schob_state state;

  // on a global scheduler queue, host-thread queue, msg or sem queue, join
  // queue, or cleanup queue.
  queue_entry runq;

  // running on a host thread?
  nk_hostthd *hostthd;

  uint32_t prio;
};

QUEUE_DEFINE(nk_schob, runq);

#define NK_PRIO_MIN 0
#define NK_PRIO_DEFAULT 0x80000000
#define NK_PRIO_MAX 0xffffffff

// Internal -- used by msg code.
void nk_schob_enqueue(nk_host *host, nk_schob *schob, int new_schob);

// ----------------- thds: conventional green threads. ------------

struct nk_thd_attrs {
  // Stack size.
  uint32_t stacksize;
  uint32_t prio;
};

#define NK_THD_ATTRS_INIT                                                      \
  { NK_THD_STACKSIZE_DEFAULT, NK_PRIO_DEFAULT, 0 }

#define NK_THD_STACKSIZE_MIN 4096
#define NK_THD_STACKSIZE_DEFAULT (2 * 1024 * 1024)
#define NK_THD_STACKSIZE_MAX (16 * 1024 * 1024)

struct nk_thd {
  nk_schob schob; // parent class
  void *stack;
  void *stacktop;
  size_t stacklen; // actual stacklen, as opposed to attrs-specified len.
  void *recvslot;  // received msg when woken up from a port recv queue.
};

typedef void (*nk_thd_entrypoint)(nk_thd *self, void *data);

/**
 * Creates a new thread. Must only be called from within a DPC or thread
 * context. `attrs` may be NULL.
 */
nk_status nk_thd_create(nk_thd **ret, nk_thd_entrypoint entry, void *data,
                        const nk_thd_attrs *attrs);
/**
 * Operates like nk_thd_create(), but allows insertion of a thread into an
 * nk_host instance from outside that instance.
 */
nk_status nk_thd_create_ext(nk_host *host, nk_thd **ret,
                            nk_thd_entrypoint entry, void *data,
                            const nk_thd_attrs *attrs);

/**
 * Yields to the scheduler. Control may return at any time.
 */
void nk_thd_yield();

// Internal only.
typedef enum {
  NK_THD_YIELD_REASON_READY,
  NK_THD_YIELD_REASON_ZOMBIE,
  NK_THD_YIELD_REASON_WAITING,
} nk_thd_yield_reason;

// Internal only.
void nk_thd_yield_ext(nk_thd_yield_reason r);

/**
 * Exits the thread. Control will never return.
 */
void __attribute__((noreturn)) nk_thd_exit();
/**
 * Returns the current thread, or NULL if in DPC or other context.
 */
nk_thd *nk_thd_self();

// ------------- dpcs: deferred procedure calls. ----------------

struct nk_dpc_attrs {
  uint32_t prio;
};

#define NK_DPC_ATTRS_INIT                                                      \
  { NK_PRIO_DEFAULT, 0 }

typedef void (*nk_dpc_func)(void *data);

struct nk_dpc {
  nk_schob schob; // parent class
  nk_dpc_func func;
  void *data;
};

/**
 * Create a new DPC (deferred procedure call). It will run exactly once, at some
 * later time, outside the context of the calling DPC/thread. This function must
 * be called in the context of another DPC or thread.
 */
nk_status nk_dpc_create(nk_dpc **ret, nk_dpc_func func, void *data,
                        const nk_dpc_attrs *attrs);
/**
 * Operates like nk_dpc_create(), but allows insertion of a DPC into a host
 * from outside that host's context.
 */
nk_status nk_dpc_create_ext(nk_host *h, nk_dpc **ret, nk_dpc_func func,
                            void *data, const nk_dpc_attrs *attrs);

/**
 * Returns the current DPC context, if any, or NULL if in thread or other
 * context.
 */
nk_dpc *nk_dpc_self(); // get the current dpc, if any.

// ---------- host threads: these run thds and dpcs. ---------------

struct nk_hostthd {
  // Host that owns this thread.
  nk_host *host;
  // List of all host-threads.
  queue_entry list;
  // running schob -- thd or dpc.
  nk_schob *running;
  // corresponding system thread.
  pthread_t pthread;
  // system thread stack on which scheduler and dpcs run.
  void *hoststack;
};

QUEUE_DEFINE(nk_hostthd, list);

// Internal use only.
nk_hostthd *nk_hostthd_self();

// Global host context.
struct nk_host {
  // Global runqueue.
  pthread_mutex_t runq_mutex;
  pthread_cond_t runq_cond;
  queue_head runq;
  // Wakeup queue. Separate spinlock -- acquired under port locks on recv
  // wakeup.
  queue_head wakeupq;
  pthread_spinlock_t wakeup_lock;
  // How many threads and DPCs exist in total?
  int schob_count;
  // How many host-threads exist? Protected by runq_mutex.
  int hostthd_count;
  // Host-thread list. Protected by hostthd_mutex.
  queue_head hostthds;
  // Shutdown flag. Protected under, and signaled by, runq_lock / runq_cond.
  int shutdown;
  // Freelists.
  nk_freelist thd_freelist;
  nk_freelist dpc_freelist;
  nk_freelist hostthd_freelist;
};

/**
 * Creates a new host instance. This is the kernel context that runs all
 * threads/DPCs. Multiple host contexts may exist within one program.
 */
nk_status nk_host_create(nk_host **ret);
/**
 * Runs the host instance, returning after the instance is shut down. The given
 * DPC function/arg is executed as a DPC within the host context, and is the
 * only point from which other threads/DPCs may be created. The host instance
 * will run as long as at least one thread or DPC exists, unless
 * `nk_host_shutdown()` is called.
 */
void nk_host_run(nk_host *host, int workers, nk_dpc_func main, void *data);
/**
 * Destroys the host instance. Must be called only after `nk_host_run()`
 * returns.
 */
void nk_host_destroy(nk_host *host);

/**
 * Initiates a shutdown on the given host instance. Should be called while the
 * host is running. All workers will exit when their schedulers next have
 * control, i.e., when their current thread yields or DPC returns.
 */
void nk_host_shutdown(nk_host *host);

// --------------- arch-specific stuff. ------------------

// Returns new top-of-stack.
void *nk_arch_create_ctx(void *stacktop,
                         void (*entry)(void *data1, void *data2, void *data3),
                         void *data1, void *data2, void *data3);
nk_thd_yield_reason nk_arch_switch_ctx(void **fromstack, void *tostack,
                                       nk_thd_yield_reason r);

#endif // __NK_THD_H__
