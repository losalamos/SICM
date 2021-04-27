#ifndef EXAMPLE_SIZES_H
#define EXAMPLE_SIZES_H

#include <stddef.h>

size_t *select_sizes(size_t max_size, size_t count);
size_t *read_sizes(char *filename, size_t count);
void print_sizes(size_t *sizes, size_t count);

#endif
