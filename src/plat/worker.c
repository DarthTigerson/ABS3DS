#include "worker.h"

#include <stdlib.h>

#define QMAX 64

struct Worker {
    Thread th;
    LightLock lock;
    LightEvent ev;
    struct { JobFn fn; void *ud; } q[QMAX];
    int head, n;
    volatile bool quit;
    volatile int running;
};

Worker *g_api;

Thread thread_spawn(ThreadFunc fn, void *arg, int stack, int prio, int core) {
    Thread t = threadCreate(fn, arg, stack, prio, core, false);
    if (!t && core != 0) t = threadCreate(fn, arg, stack, prio, 0, false);
    return t;
}

static void run(void *arg) {
    Worker *w = arg;
    while (!w->quit) {
        LightEvent_Wait(&w->ev);
        for (;;) {
            LightLock_Lock(&w->lock);
            if (!w->n || w->quit) { LightLock_Unlock(&w->lock); break; }
            JobFn fn = w->q[w->head].fn;
            void *ud = w->q[w->head].ud;
            w->head = (w->head + 1) % QMAX;
            w->n--;
            w->running = 1;
            LightLock_Unlock(&w->lock);
            fn(ud);
            w->running = 0;
        }
    }
}

Worker *worker_create(int prio, int core, int stack) {
    Worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    LightLock_Init(&w->lock);
    LightEvent_Init(&w->ev, RESET_ONESHOT);
    w->th = thread_spawn(run, w, stack, prio, core);
    if (!w->th) { free(w); return NULL; }
    return w;
}

void worker_destroy(Worker *w) {
    if (!w) return;
    w->quit = true;
    LightEvent_Signal(&w->ev);
    threadJoin(w->th, U64_MAX);
    threadFree(w->th);
    /* drop jobs that never ran */
    while (w->n) { free(w->q[w->head].ud); w->head = (w->head + 1) % QMAX; w->n--; }
    free(w);
}

bool worker_post(Worker *w, JobFn fn, void *ud) {
    LightLock_Lock(&w->lock);
    bool ok = w->n < QMAX;
    if (ok) {
        w->q[(w->head + w->n) % QMAX].fn = fn;
        w->q[(w->head + w->n) % QMAX].ud = ud;
        w->n++;
    }
    LightLock_Unlock(&w->lock);
    if (ok) LightEvent_Signal(&w->ev);
    return ok;
}

int worker_pending(Worker *w) { return w->n + w->running; }
