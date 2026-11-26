#include "allocator.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define PAYLOAD_ALIGNMENT 40

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static uint8_t *heap_a = NULL;
static size_t heap_size_a = 0;

typedef struct {
    size_t size;
    size_t prev;
    size_t next;
    bool free;
    uint32_t crc32;
} BlockHeader;

typedef struct {
    size_t head;
    uint8_t pattern[5];
    uint32_t crc32;
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
    head.crc32 = compute_header_crc(&head);

    AllocatorState state = {0};

    state.head = header_offset;
    memcpy(state.pattern, pattern, sizeof(pattern));
    state.crc32 = compute_state_crc(&state);

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
                size_t overhead = 2 * sizeof(BlockHeader);
                if (current > SIZE_MAX - overhead || size > SIZE_MAX - current - overhead) {
                    header.crc32 = compute_header_crc(&header);
                    memcpy(heap_a + current, &header, sizeof(BlockHeader));
                    return heap_a + current + sizeof(BlockHeader);
                }
                size_t block_end = current + overhead + size;

                if (block_end > SIZE_MAX - (PAYLOAD_ALIGNMENT - 1)){
                    header.crc32 = compute_header_crc(&header);
                    memcpy(heap_a + current, &header, sizeof(BlockHeader));
                    return heap_a + current + sizeof(BlockHeader);
                }

                size_t payload_offset = ((block_end + PAYLOAD_ALIGNMENT - 1) / PAYLOAD_ALIGNMENT) * PAYLOAD_ALIGNMENT;

                if (payload_offset >= heap_size_a || payload_offset >= current + sizeof(BlockHeader) + header.size){
                    header.crc32 = compute_header_crc(&header);
                    memcpy(heap_a + current, &header, sizeof(BlockHeader));
                    return heap_a + current + sizeof(BlockHeader);
                }

                size_t header_offset = payload_offset - sizeof(BlockHeader);

                BlockHeader newHeader = {0};
                newHeader.size = current + header.size - header_offset;
                newHeader.next = header.next;
                newHeader.prev = current;
                newHeader.free = true;
                newHeader.crc32 = compute_header_crc(&newHeader);

                if (header.next != SIZE_MAX){
                    BlockHeader nextHeader = {0};
                    memcpy(&nextHeader, heap_a + header.next, sizeof(BlockHeader));

                    nextHeader.prev = header_offset;
                    nextHeader.crc32 = compute_header_crc(&nextHeader);

                    memcpy(heap_a + header.next, &nextHeader, sizeof(BlockHeader));
                }

                header.next = header_offset;
                header.size = header_offset - current - sizeof(BlockHeader);
                memcpy(heap_a + header_offset, &newHeader, sizeof(BlockHeader));
            }
            
            header.crc32 = compute_header_crc(&header);
            memcpy(heap_a + current, &header, sizeof(BlockHeader));

            return heap_a + current + sizeof(BlockHeader);
        }

        current = header.next;
    }

    return NULL;
}

bool validate_pointer(void *ptr){
    if (ptr == NULL || heap_a == NULL || ptr < heap_a + sizeof(AllocatorState) + sizeof(BlockHeader) || ptr >= heap_a + heap_size_a) return false;
    if (((size_t) ptr - (size_t) heap_a) % PAYLOAD_ALIGNMENT != 0) return false;
    return true;
}

size_t mm_read(void *ptr, size_t offset, void *buf, size_t len){
    if (!validate_pointer(ptr)) return 0;
    int8_t *headerPtr = (int8_t*) ptr - sizeof(BlockHeader);
    BlockHeader header = {0};
    memcpy(&header, headerPtr, sizeof(BlockHeader));

    if (header.free) return 0;

    if (offset >= header.size || offset + len > header.size) return 0;

    memcpy(buf, (int8_t*)ptr + offset, len);

    return len;
}

size_t mm_write(void *ptr, size_t offset, const void *src, size_t len){
    if (!validate_pointer(ptr)) return 0;
    int8_t *headerPtr = (int8_t*) ptr - sizeof(BlockHeader);
    BlockHeader header = {0};
    memcpy(&header, headerPtr, sizeof(BlockHeader));

    if (header.free) return 0;

    if (offset >= header.size || offset + len > header.size) return 0;

    memcpy((int8_t*)ptr + offset, src, len);

    return len;
}

void mm_free(void *ptr){

    if (!validate_pointer(ptr)) return;

    int8_t* headerPtr = (int8_t*) ptr - sizeof(BlockHeader);
    BlockHeader header = {0};

    memcpy(&header, headerPtr, sizeof(BlockHeader));
    if (header.free) return;
    header.free = true;
    
    if (header.next != SIZE_MAX){
        int8_t *nextPtr = heap_a + header.next;
        BlockHeader next = {0};
        memcpy(&next, nextPtr, sizeof(BlockHeader));

        if (next.free){
            header.size += sizeof(BlockHeader) + next.size;
            header.next = next.next;

            if (next.next != SIZE_MAX){
                int8_t *nextNextPtr = heap_a + next.next;
                BlockHeader nextNext = {0};
                memcpy(&nextNext, nextNextPtr, sizeof(BlockHeader));
                nextNext.prev = headerPtr - heap_a;
                nextNext.crc32 = compute_header_crc(&nextNext);
                memcpy(nextNextPtr, &nextNext, sizeof(BlockHeader));
            }
        }
    }

    bool prevMerged = false;
    size_t payloadSize;
    int8_t *payloadAddress;

    if (header.prev != SIZE_MAX){
        int8_t *prevPtr = heap_a + header.prev;
        BlockHeader prev = {0};
        memcpy(&prev, prevPtr, sizeof(BlockHeader));

        if (prev.free){
            prev.size += sizeof(BlockHeader) + header.size;
            prev.next = header.next;

            if (header.next != SIZE_MAX){
                int8_t *nextPtr = heap_a + header.next;
                BlockHeader next = {0};
                memcpy(&next, nextPtr, sizeof(BlockHeader));
                next.prev = prevPtr - heap_a;
                next.crc32 = compute_header_crc(&next);
                memcpy(nextPtr, &next, sizeof(BlockHeader));
            }
            prevMerged = true;
            payloadSize = prev.size;
            payloadAddress = prevPtr + sizeof(BlockHeader);
            prev.crc32 = compute_header_crc(&prev);
            memcpy(prevPtr, &prev, sizeof(BlockHeader));
        }
    }

    if (!prevMerged){
        payloadSize = header.size;
        payloadAddress = headerPtr + sizeof(BlockHeader);
        header.crc32 = compute_header_crc(&header);
        memcpy(headerPtr, &header, sizeof(BlockHeader));   
    }

    AllocatorState state = {0};
    memcpy(&state, heap_a, sizeof(AllocatorState));

    int8_t *payload = (int8_t*) payloadAddress;
    size_t baseOffset = (size_t) (payloadAddress - heap_a);

    for (size_t i = 0; i < payloadSize; i++){
        payload[i] = state.pattern[(baseOffset + i) % 5];
    }
}

static void init_crc32_table(void) {
    if (crc32_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

static uint32_t calc_crc32(const void *data, size_t len) {
    init_crc32_table();
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc ^ bytes[i]) & 0xFFu);
        crc = (crc >> 8) ^ crc32_table[idx];
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t compute_header_crc(const BlockHeader *hdr) {
    BlockHeader copy = *hdr;
    copy.crc32 = 0;
    return calc_crc32(&copy, sizeof(copy));
}

static uint32_t compute_state_crc(const AllocatorState *state) {
    AllocatorState copy = *state;
    copy.crc32 = 0;
    return calc_crc32(&copy, sizeof(copy));
}