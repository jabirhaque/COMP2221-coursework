#include "allocator.h"

static uint8_t *heap_a = NULL;
static size_t heap_size_a = 0;

typedef struct {
    uint8_t *head;
    char pattern[5];
} AllocatorState;

int mm_init(uint8_t *heap, size_t heap_size){
    heap_a = heap;
    heap_size_a = heap_size;

    AllocatorState* state = (AllocatorState*) heap;
    state->head = heap + sizeof(AllocatorState);
    return 0;
}