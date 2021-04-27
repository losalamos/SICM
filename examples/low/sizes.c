#include <stdlib.h>
#include <stdio.h>

#include "sizes.h"

size_t *select_sizes(size_t max_size, size_t count) {
    size_t *sizes = calloc(count, sizeof(size_t));
    for(size_t i = 0; i < count; i++) {
        while ((sizes[i] = (rand() * rand() * rand()) % max_size) == 0);
    }
    return sizes;
}

size_t *read_sizes(char *filename, size_t count) {
    if (!filename) {
        fprintf(stderr, "Bad filename\n");
        return NULL;
    }

    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Could not open file\n");
        return NULL;
    }

    size_t avail = 0;
    if (fscanf(file, "%zu", &avail) != 1) {
        fprintf(stderr, "Could not read available\n");
        return NULL;
    }

    /* file can have more values than needed */
    if (avail < count) {
        fprintf(stderr, "Not enough numbers: Have %zu. Want %zu.\n", avail, count);
        return NULL;
    }

    size_t *sizes = calloc(count, sizeof(size_t));
    if (!sizes) {
        fprintf(stderr, "Could not allocate array for holding sizes\n");
        return NULL;
    }

    size_t i;
    for(i = 0; i < count; i++) {
        if (fscanf(file, "%zu", &sizes[i]) != 1) {
            fprintf(stderr, "Could not read size[%zu]\n", i);
            break;
        }
    }

    fclose(file);

    if (i < count) {
        free(sizes);
        return NULL;
    }

    return sizes;
}

void print_sizes(size_t *sizes, size_t count) {
    printf("%zu\n", count); /* print how many values are available */
    for(size_t i = 0; i < count; i++) {
        printf("%zu\n", sizes[i]);
    }
}
