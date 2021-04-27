#ifndef RUN_MOVE_H
#define RUN_MOVE_H

#include <time.h>

#include "sicm_low.h"

struct ThreadArgs {
    size_t id;
    sicm_device_list *devs;
    sicm_device *src;
    sicm_device *dst;
    size_t *sizes;
    size_t count;

    struct timespec start;
    struct timespec end;
};

int pin_thread(size_t cpu);

void run(sicm_device_list *devs,
         const size_t thread_count,
         const size_t allocations,
         size_t *sizes,
         int src_numa,
         int dst_numa,
         const char *name,
         void *(*func)(void *));

#endif
