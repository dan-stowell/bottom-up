// Machine limits for the build-trace floors: memory bandwidth, 1 thread and all threads.
// gcc -O2 -march=native -pthread -o build/limits hw/limits.c && build/limits
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#define SZ (256u << 20)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static pthread_barrier_t bar;
struct job { int cpu; char *a, *b; double best; };
static void *work(void *p) {
    struct job *j = p; cpu_set_t s; CPU_ZERO(&s); CPU_SET(j->cpu, &s); sched_setaffinity(0, sizeof s, &s);
    memset(j->a, 1, SZ); memset(j->b, 2, SZ);
    j->best = 1e9;
    for (int r = 0; r < 5; r++) {
        pthread_barrier_wait(&bar);
        double t = now(); memcpy(j->b, j->a, SZ); t = now() - t;
        if (t < j->best) j->best = t;
    }
    return 0;
}
static double run(int n) {
    pthread_t th[64]; struct job j[64];
    pthread_barrier_init(&bar, 0, n);
    for (int i = 0; i < n; i++) { j[i].cpu = i; j[i].a = malloc(SZ); j[i].b = malloc(SZ); pthread_create(&th[i], 0, work, &j[i]); }
    double worst = 0;
    for (int i = 0; i < n; i++) { pthread_join(th[i], 0); if (j[i].best > worst) worst = j[i].best; free(j[i].a); free(j[i].b); }
    return (double)SZ * n / worst;   // bytes copied per second, all threads together
}
int main(void) {
    int n = sysconf(_SC_NPROCESSORS_ONLN);
    printf("memcpy_1t_bytes_per_s %.0f\n", run(1));
    printf("memcpy_8t_bytes_per_s %.0f\n", run(n));
    return 0;
}
