#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <numa.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "run_move.h"
#include "sicm_low.h"
#include "sizes.h"

size_t PAGE_SIZE = 0;

void *device2int(sicm_device *dev)  {
    return (void *) (uintptr_t) (dev->node);
}

int void2int(void *dst) {
    return (int) (uintptr_t) dst;
}

int move_ptrs(void **ptrs, size_t *sizes, size_t count, void *src, void *dst_ptr) {
    const int dst = void2int(dst_ptr);

    /* int *dsts     = malloc(count * sizeof(int)); */
    /* int *statuses = malloc(count * sizeof(int)); */

    /* for(size_t i = 0; i < count; i++) { */
    /*     dsts[i] = dst; */
    /*     statuses[i] = -1; */
    /* } */

    /* const int rc = numa_move_pages(0, count, ptrs, dsts, statuses, 0); */
    /* const int err = errno; */

    /* free(dsts); */
    /* free(statuses); */

    size_t page_count = 0;                                /* total number of pages across all ptrs */
    size_t *page_counts = malloc(count * sizeof(size_t)); /* number of pages each ptr[i] has */

    for(size_t i = 0; i < count; i++) {
        const size_t rem = sizes[i] % PAGE_SIZE;
        page_counts[i] = (sizes[i] / PAGE_SIZE) + !!rem;
        page_count += page_counts[i];
    }

    size_t p = 0;
    void **pages = malloc(page_count * sizeof(void *));
    int *dsts = malloc(page_count * sizeof(int));
    int *statuses = malloc(page_count * sizeof(int));

    for(size_t i = 0; i < count; i++) {
        /* first page */
        pages[p] = ptrs[i];
        dsts[p] = dst;
        statuses[p] = dst;

        p++;

        /* rest of pages */
        for(size_t j = 1; j < page_counts[i]; j++) {
            pages[p] = (void *) (((uintptr_t) pages[p - 1]) + PAGE_SIZE);
            dsts[p] = dst;
            statuses[p] = dst;
            p++;
        }
    }

    /* do the move */
    const int rc = numa_move_pages(0, page_count, pages, dsts, statuses, 0);
    const int err = errno;

    p = 0;
    for(size_t i = 0; i < count; i++) {
        int good = 1;

        for(size_t j = 0; j < page_counts[i]; j++) {
            if (statuses[p + j] != dst) {
                good = 0;
                break;
            }
        }

        if (!good){
            fprintf(stderr, "allocated %p %zu %zu\n", ptrs[i], sizes[i], page_counts[i]);
            for(size_t j = 0; j < page_counts[i]; j++) {
                fprintf(stderr, "    %zu %p %d\n", j, pages[p + j], statuses[p + j]);
            }
        }

        p += page_counts[i];
    }

    free(statuses);
    free(dsts);
    free(pages);
    free(page_counts);

    if (rc < 0) {
        fprintf(stderr, "Could not move pages to %d: %d %s\n",
                dst, err, strerror(err));
        return -1;
    }

    return 0;
}

void *alloc_posix_memalign(size_t size, void *src) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, PAGE_SIZE, size) != 0) {
        const int err = errno;
        fprintf(stderr, "posix_memalign failed: %s (%d)\n", strerror(err), err);
        return NULL;
    }

    memset(ptr, 42, size);

    if (move_ptrs(&ptr, &size, 1, NULL, src) != 0) {
        free(ptr);
        return NULL;
    }

    return ptr;
}

void free_posix_memalign(void **ptrs, size_t *sizes, size_t count) {
    for(size_t i = 0; (i < count) && ptrs[i]; i++) {
        free(ptrs[i]);
    }
}

void *alloc_mmap(size_t size, void *src) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }

    memset(ptr, 42, size);

    if (move_ptrs(&ptr, &size, 1, NULL, src) != 0) {
        munmap(ptr, size);
        return NULL;
    }

    return ptr;
}

void free_mmap(void **ptrs, size_t *sizes, size_t count) {
    for(size_t i = 0; (i < count) && ptrs[i]; i++) {
        munmap(ptrs[i], sizes[i]);
    }
}

void *alloc_numa(size_t size, void *src) {
    void *ptr = numa_alloc_onnode(size, void2int(src));
    memset(ptr, 42, size);
    return ptr;
}

void free_numa(void **ptrs, size_t *sizes, size_t count) {
    for(size_t i = 0; (i < count) && ptrs[i]; i++) {
        numa_free(ptrs[i], sizes[i]);
    }
}

void *device2arena(sicm_device *src) {
    sicm_device_list devs;
    devs.count = 1;
    devs.devices = &src;
    return (void *) sicm_arena_create(0, 0, &devs);
}

sicm_arena void2arena(void *arena) {
    return (sicm_arena) arena;
}

void *alloc_sicm(size_t size, void *arena) {
    void *ptr = sicm_arena_alloc(arena, size);
    memset(ptr, 42, size);
    return ptr;
}

void *device2device(sicm_device *dst) {
    return dst;
}

int move_sicm(void **ptrs, size_t *sizes, size_t count, void *src, void *dst) {
    return sicm_arena_set_device(src, dst);
}

void cleanup_sicm(void *arena) {
    sicm_arena_destroy(void2arena(arena));
}

void *common(void *data,
             void *(*get_src)(sicm_device *dst),
             void *(*get_dst)(sicm_device *dst),
             void *(*alloc)(size_t size, void *src),
             int  (*move)(void **ptrs, size_t *sizes, size_t count, void *src, void *dst),
             void (*free_ptrs)(void **ptr, size_t *size, size_t count),
             void (*cleanup)(void *data)) {
    struct ThreadArgs *args = (struct ThreadArgs *) data;

    if (pin_thread(args->id) != 0) {
        fprintf(stderr, "Could not pin thread to cpu %zu\n", args->id);
        return NULL;
    }

    void *src = get_src(args->src);

    /* allocate pointers and move them to their source NUMA nodes */
    void **ptrs = numa_alloc_onnode(args->count * sizeof(void *), args->src->node);
    for(size_t i = 0; i < args->count; i++) {
        ptrs[i] = alloc(args->sizes[i], src);

        if (!ptrs[i]) {
            fprintf(stderr, "Could not allocate ptrs[%zu]\n", i);
            goto cleanup_ptrs;
        }
    }

    /* move the pointers to their destination NUMA nodes */
    clock_gettime(CLOCK_MONOTONIC, &args->start);
    move(ptrs, args->sizes, args->count, src, get_dst(args->dst));
    clock_gettime(CLOCK_MONOTONIC, &args->end);

  cleanup_ptrs:
    if (free_ptrs) {
        free_ptrs(ptrs, args->sizes, args->count);
    }
    numa_free(ptrs, args->count * sizeof(void *));

    if (cleanup) {
        cleanup(src);
    }

    return NULL;
}

void *posix_memalign_thread(void *args) {
    return common(args, device2int, device2int, alloc_posix_memalign, move_ptrs, free_posix_memalign, NULL);
}

void *mmap_thread(void *args) {
    return common(args, device2int, device2int, alloc_mmap, move_ptrs, free_mmap, NULL);
}

void *numa_thread(void *args) {
    return common(args, device2int, device2int, alloc_numa, move_ptrs, free_numa, NULL);
}

void *sicm_thread(void *args) {
    return common(args, device2arena, device2device, alloc_sicm, move_sicm, NULL, cleanup_sicm);
}

int main(int argc, char *argv[]) {
    if (argc < 7) {
        fprintf(stderr, "Syntax: %s alloc_func threads size_count sizes src dst\n", argv[0]);
        return 1;
    }

    char *alloc_func = argv[1];

    size_t thread_count = 0;
    if (sscanf(argv[2], "%zu", &thread_count) != 1) {
        fprintf(stderr, "Bad thread count: %s\n", argv[2]);
        return 1;
    }

    size_t count = 0;
    if (sscanf(argv[3], "%zu", &count) != 1) {
        fprintf(stderr, "Bad allocation count: %s\n", argv[3]);
        return 1;
    }

    size_t *sizes = read_sizes(argv[4], count);
    if (!sizes) {
        fprintf(stderr, "Bad size file: %s\n", argv[4]);
        return 1;
    }

    sicm_device_list devs = sicm_init();

    int src = -1;
    if ((sscanf(argv[5], "%d", &src) != 1) || (src < 0)) {
        fprintf(stderr, "Bad source NUMA node: %s\n", argv[5]);
        return 1;
    }

    int dst = 0;
    if ((sscanf(argv[6], "%d", &dst) != 1) || (dst < 0)) {
        fprintf(stderr, "Bad destination NUMA node: %s\n", argv[6]);
        return 1;
    }

    PAGE_SIZE = getpagesize();

    void *(*thread)(void *) = NULL;
    const size_t len = strlen(alloc_func);
    if ((len == 14) && (strncmp(alloc_func, "posix_memalign", len) == 0)) {
        thread = posix_memalign_thread;
    }
    else if ((len == 4) && (strncmp(alloc_func, "mmap", len) == 0)) {
        thread = mmap_thread;
    }
    else if ((len == 4) && (strncmp(alloc_func, "numa", len) == 0)) {
        thread = numa_thread;
    }
    else if ((len == 4) && (strncmp(alloc_func, "sicm", len) == 0)) {
        thread = sicm_thread;
    }
    else {
        fprintf(stderr, "Bad allocator function: %s\n", alloc_func);
    }

    if (thread) {
        run(&devs, thread_count, count, sizes, src, dst, alloc_func, thread);
    }

    sicm_fini();

    free(sizes);

    return 0;

}
