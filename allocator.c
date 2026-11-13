#include "allocator.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define PAYLOAD_ALIGNMENT 40

static uint8_t *heap_a = NULL;
static size_t heap_size_a = 0;

typedef struct {
    size_t size;
    size_t prev;
    size_t next;
    bool free;
} BlockHeader;

typedef struct {
    size_t head;
    uint8_t pattern[5];
} AllocatorState;

int mm_init(uint8_t *heap, size_t heap_size){
    heap_a = NULL;
    heap_size_a = 0;

    if (heap == NULL || heap_size < 5 || heap_size < sizeof(AllocatorState) + sizeof(BlockHeader)) return -1;

    uint8_t pattern[5];
    memcpy(pattern, heap, sizeof(pattern));

    for (size_t i=0; i<heap_size; i++){
        if (heap[i] != pattern[i%5]) return -1;
    }

    size_t metadata_end = sizeof(AllocatorState) + sizeof(BlockHeader);

    if (metadata_end > SIZE_MAX - (PAYLOAD_ALIGNMENT - 1)) return -1;

    size_t payload_offset = ((metadata_end + PAYLOAD_ALIGNMENT - 1) / PAYLOAD_ALIGNMENT) * PAYLOAD_ALIGNMENT;

    if (payload_offset >= heap_size) return -1;

    size_t header_offset = payload_offset - sizeof(BlockHeader);

    BlockHeader head = {0};

    head.size = heap_size - payload_offset;
    head.prev = SIZE_MAX;
    head.next = SIZE_MAX;
    head.free = true;

    AllocatorState state = {0};

    state.head = header_offset;
    memcpy(state.pattern, pattern, sizeof(pattern));

    memcpy(heap, &state, sizeof(state));
    memcpy(heap + header_offset, &head, sizeof(head));

    heap_a = heap;
    heap_size_a = heap_size;

    return 0;
}

void *mm_malloc(size_t size){
    if (size == 0 || size > heap_size_a || heap_a == NULL) return NULL;

    AllocatorState state = {0};
    memcpy(&state, heap_a, sizeof(AllocatorState));

    size_t current = state.head;
    BlockHeader header = {0};
    while(current < heap_size_a){
        memcpy(&header, heap_a + current, sizeof(BlockHeader));
        if (header.free && header.size >= size){
            header.free = false;
            //split
            if (header.size > size + sizeof(BlockHeader)){
                size_t block_end = current + sizeof(BlockHeader) + size + sizeof(BlockHeader);

                if (block_end > SIZE_MAX - (PAYLOAD_ALIGNMENT - 1)) return NULL;

                size_t payload_offset = ((block_end + PAYLOAD_ALIGNMENT - 1) / PAYLOAD_ALIGNMENT) * PAYLOAD_ALIGNMENT;

                if (payload_offset >= heap_size_a) return NULL;

                size_t header_offset = payload_offset - sizeof(BlockHeader);

                BlockHeader newHeader = {0};
                newHeader.size = current + header.size - header_offset;
                newHeader.next = header.next;
                newHeader.prev = current;
                newHeader.free = true;

                if (header.next != SIZE_MAX){
                    BlockHeader nextHeader = {0};
                    memcpy(&nextHeader, heap_a + header.next, sizeof(BlockHeader));

                    nextHeader.prev = header_offset;

                    memcpy(heap_a + header.next, &nextHeader, sizeof(BlockHeader));
                }

                header.next = header_offset;
                header.size = header_offset - current - sizeof(BlockHeader);
                memcpy(heap_a + header_offset, &newHeader, sizeof(BlockHeader));
            }

            memcpy(heap_a + current, &header, sizeof(BlockHeader));

            return heap_a + current + sizeof(BlockHeader);
        }

        current = header.next;
    }

    return NULL;
}