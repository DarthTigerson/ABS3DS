/* Background job threads (network/API work off the UI thread). */
#ifndef ABS3DS_WORKER_H
#define ABS3DS_WORKER_H
#include <stdbool.h>
#include <3ds.h>

typedef void (*JobFn)(void *ud);

typedef struct Worker Worker;
/* core: preferred CPU (1 = system core, falls back to 0) */
Worker *worker_create(int prio, int core, int stack);
void worker_destroy(Worker *w);
/* FIFO. The job owns ud (free it inside fn). */
bool worker_post(Worker *w, JobFn fn, void *ud);
int worker_pending(Worker *w);

/* the shared API worker */
extern Worker *g_api;

/* thread creation on the system core with fallback */
Thread thread_spawn(ThreadFunc fn, void *arg, int stack, int prio, int core);

#endif
