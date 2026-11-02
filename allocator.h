#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

int mm_init(uint8_t *heap, size_t heap_size);
void *mm_malloc(size_t size);
size_t mm_read(void *ptr, size_t offset, void *buf, size_t len);
size_t mm_write(void *ptr, size_t offset, const void *src, size_t len);
void mm_free(void *ptr);

#endif
