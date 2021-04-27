#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "nano.h"
#include "run_move.h"

int pin_thread(size_t cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

sicm_device *find_dev(sicm_device_list *devs, int numa) {
    for(unsigned int i = 0; i < devs->count; i += 3) {
        if (devs->devices[i]->node == numa) {
            return devs->devices[i];
        }
    }

    return NULL;
}

void run(sicm_device_list *devs,
         const size_t thread_count,
         const size_t count,
         size_t *sizes,
         int src_numa,
         int dst_numa,
         const char *name,
         void *(*func)(void *)) {
    sicm_device *src = find_dev(devs, src_numa);
    sicm_device *dst = find_dev(devs, dst_numa);

    if (!src || !dst) {
        fprintf(stderr, "Invalid (src, dst) pair: (%d, %d)\n", src_numa, dst_numa);
        return;
    }

    /* use the same arrays for all runs */
    pthread_t *threads      = calloc(thread_count, sizeof(pthread_t));
    struct ThreadArgs *args = calloc(thread_count, sizeof(struct ThreadArgs));

    double total_threadtime = 0;
    double total_realtime = 0;

    for(size_t i = 0; i < thread_count; i++) {
        args[i].id = i;
        args[i].devs = devs;
        args[i].src = src;
        args[i].dst = dst;
        args[i].count = count;
        args[i].sizes = sizes;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for(size_t i = 0; i < thread_count; i++) {
        if (pthread_create(&threads[i], NULL, func, &args[i]) != 0) {
            fprintf(stderr, "Could not create thread %zu of %s\n", i, name);
            for(int j = 0; j < thread_count; j++) {
                pthread_join(threads[j], NULL);
                threads[j] = 0;
            }
            break;
        }
    }

    for(size_t i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    total_realtime = nano(&start, &end);

    for(size_t i = 0; i < thread_count; i++) {
        total_threadtime += nano(&args[i].start, &args[i].end);
    }

    /* printf(" %.3f", total_realtime / 1e9); */
    printf("%.3f\n", total_threadtime / 1e9);
    fflush(stdout);

    free(args);
    free(threads);
}
