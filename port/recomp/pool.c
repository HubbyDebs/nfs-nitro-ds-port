/* Grupo fijo de hilos de trabajo del PC para repartir el dibujado (3D, capas 2D, ventana).
 * Se crean una vez; en cada tarea, cada hilo va cogiendo el siguiente trozo libre. */
#include <stdlib.h>
#include <windows.h>

#include "sys.h"

#define MAX_WORKERS 16

static int workers = -1;
static HANDLE start_sem, done_sem;
static volatile LONG next_item, active;
static int job_count;
static void (*job_fn)(void *ctx, int item);
static void *job_ctx;

static void run_items(void) {
    for (;;) {
        LONG i = InterlockedIncrement(&next_item) - 1;
        if (i >= job_count) return;
        job_fn(job_ctx, (int)i);
    }
}

static DWORD WINAPI worker(void *param) {
    (void)param;
    for (;;) {
        WaitForSingleObject(start_sem, INFINITE);
        run_items();
        if (InterlockedDecrement(&active) == 0) ReleaseSemaphore(done_sem, 1, 0);
    }
}

int pool_threads(void) {
    if (workers < 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int n = getenv("NFS_THREADS") ? atoi(getenv("NFS_THREADS")) : (int)si.dwNumberOfProcessors - 2;
        if (n < 1) n = 1;
        if (n > MAX_WORKERS + 1) n = MAX_WORKERS + 1;
        workers = n - 1; /* el hilo que llama tambien trabaja */
        start_sem = CreateSemaphoreA(0, 0, MAX_WORKERS, 0);
        done_sem = CreateSemaphoreA(0, 0, 1, 0);
        for (int i = 0; i < workers; i++) CloseHandle(CreateThread(0, 0, worker, 0, 0, 0));
    }
    return workers + 1;
}

/* Ejecuta fn(ctx, i) para i = 0..count-1 repartido entre todos los hilos, y espera a que acabe */
void pool_run(int count, void (*fn)(void *ctx, int item), void *ctx) {
    pool_threads();
    if (count <= 0) return;
    if (!workers || count == 1) {
        for (int i = 0; i < count; i++) fn(ctx, i);
        return;
    }
    job_fn = fn;
    job_ctx = ctx;
    job_count = count;
    next_item = 0;
    active = workers;
    ReleaseSemaphore(start_sem, workers, 0);
    run_items();
    WaitForSingleObject(done_sem, INFINITE);
}
