#include "allocator.h"

static uint8_t *heap_a = NULL;
static size_t heap_size_a = 0;

typedef struct {
    uint8_t *head;
    uint8_t pattern[5];
} AllocatorState;

int mm_init(uint8_t *heap, size_t heap_size){
    if (heap == NULL) return -1;
    if (heap_size < 5 || heap_size < sizeof(AllocatorState)) return -1;

    heap_a = heap;
    heap_size_a = heap_size;

    uint8_t pattern[5];
    memcpy(pattern, heap, 5);

    for (size_t i=0; i<heap_size; i++){
        if (heap[i] != pattern[i%5]) return -1;
    }

    AllocatorState* state = (AllocatorState*) heap;
    state->head = heap + sizeof(AllocatorState);
    memcpy(state->pattern, pattern, 5);
    return 0;
}