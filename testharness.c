/*
 * testharness.c - RetroCore allocator test harness (student version)
 * ==================================================================
 *
 * WHAT THIS PROGRAM IS
 *   A small program that tests your allocator. It does not contain any tests itself: it reads
 *   a "scenario" file, which is a list of simple commands such as
 *
 *       HEAP 4096          create a simulated heap of 4096 bytes and call your mm_init
 *       MALLOC 0 128       call your mm_malloc(128) and remember the result in "slot" 0
 *       WRITE 0 0 32       call your mm_write to store 32 bytes in slot 0
 *       READ 0 0 32        call your mm_read and check you give back the same 32 bytes
 *       FREE 0             call your mm_free on slot 0
 *
 *   and carries them out one line at a time, checking every answer your allocator gives.
 *   TESTHARNESS.md lists every command.
 *
 * HOW TO RUN IT
 *       ./testharness tests/00_smoke.test
 *
 * HOW TO READ ITS OUTPUT
 *   - "PASS line 5: ..." (printed on stdout) means the command on line 5 of the scenario
 *     behaved correctly.
 *   - "FAIL line 5: ..." (printed on stderr) means it did not, and says why.
 *   - The last line is a summary. The program's exit status is 0 only if nothing failed.
 *
 * WHAT IT CHECKS FOR YOU
 *   - Pointers returned by mm_malloc are inside the heap, 40-byte aligned (measured from the
 *     start of the heap) and do not overlap a block that is still in use.
 *   - Whatever you were asked to store (WRITE) is remembered by the harness in its own copy, and
 *     compared with what mm_read and CHECK give back.
 *   - "Guard bytes" placed just before and just after the heap reveal any write that strays
 *     outside the memory you were given.
 *
 * NOTES FOR READERS NEW TO C
 *   - Every function and variable below is declared `static`. That means "visible only inside
 *     this file". It stops a name used here (such as `check` or `g_heap`) from clashing with the
 *     same name in your allocator.c, which is linked into the same program.
 *   - Do not modify this file. Your submission is always graded with the official copy.
 */

/* Ask the system headers to provide the standard POSIX functions (such as strtok). */
#define _POSIX_C_SOURCE 200809L

/* Your header: it declares mm_init, mm_malloc, mm_read, mm_write and mm_free. */
#include "allocator.h"

/* Standard library headers, and what this file uses each one for. */
#include <ctype.h>   /* toupper: scenario command names are not case sensitive */
#include <errno.h>   /* errno: did a string-to-number conversion fail? */
#include <stdarg.h>  /* va_list: functions that take printf-style arguments */
#include <stdint.h>  /* uint8_t, SIZE_MAX: fixed-size integer types and limits */
#include <stdio.h>   /* printf, fprintf, fopen, fgets: input and output */
#include <stdlib.h>  /* malloc, calloc, free, strtoull: the HARNESS's own memory use */
#include <string.h>  /* memset, memcpy, memcmp, strcmp, strchr, strtok: strings and bytes */

/* Named constants. A name is easier to understand than a bare number. */
#define MAX_SLOTS 256    /* slot numbers are 0..255 */
#define LINE_SIZE 65536  /* the longest scenario line we accept, in characters */
#define MAX_ARGS 10      /* the most arguments one scenario line may have */
#define GUARD_SIZE 64    /* number of guard bytes on EACH side of the heap */
#define GUARD_BYTE 0xD7  /* the value stored in every guard byte (an arbitrary choice) */
#define ALIGNMENT 40     /* returned pointers must be a multiple of 40 from the heap's start */

/*
 * The five-byte pattern from the coursework text. The heap is filled with it (repeating) before
 * your mm_init is called, and so is the data that WRITE sends to your mm_write.
 */
static uint8_t PATTERN[5] = {0xA5, 0x5A, 0xC3, 0x3C, 0x96};

/*
 * A "slot" is one numbered place where the harness keeps track of an allocation: the pointer
 * your mm_malloc returned and what the harness knows about the bytes stored behind it.
 * Scenario commands refer to allocations by slot number, never by address.
 */
typedef struct {
    uint8_t *ptr;      /* the payload pointer your mm_malloc returned */
    size_t size;       /* how many bytes were requested */
    uint8_t *expected; /* the harness's own copy of the bytes that should be stored */
    uint8_t *known;    /* known[i] is 1 once expected[i] has been written, else 0 */
    uint8_t *last_ptr; /* the pointer most recently freed from this slot (for FREE_AGAIN) */
    int active;        /* 1 while the allocation is live, 0 when the slot is unused or freed */
} Slot;

/* The harness's global state: the single heap, the slot table and some counters. */
static uint8_t *g_block;  /* the whole memory block: guard + heap + guard (see create_heap) */
static uint8_t *g_heap;   /* the middle part of g_block: the heap handed to your mm_init */
static size_t g_size;     /* the size of the heap in bytes (not counting the guards) */
static size_t g_line;     /* the scenario line currently being run, used in messages */
static Slot g_slots[MAX_SLOTS];                         /* one entry per slot number */
static unsigned long g_passes, g_failures, g_calls;     /* totals shown in the final summary */

/* ---- reporting ------------------------------------------------------------------------ */

/*
 * Prints "PASS line N: " followed by a message. The message is written exactly as for printf:
 * pass("HEAP size=%zu", size) works like printf("HEAP size=%zu", size). The "..." in the
 * parameter list means "any number of further arguments"; va_list, va_start and va_end are the
 * standard way to pass them on to vprintf.
 */
static void pass(char *format, ...) {
    va_list args;

    g_passes++;                       /* count it for the summary */
    printf("PASS line %zu: ", g_line);
    va_start(args, format);           /* start reading the extra arguments */
    vprintf(format, args);            /* print the message, filling in the %-codes */
    va_end(args);                     /* finished with the extra arguments */
    printf("\n");
}

/*
 * Same as pass, but for a failure: counts it, and prints "FAIL line N: ..." on stderr.
 * It always returns 0, so a function can report a problem and give up in one line:
 *         return fail("something went wrong");
 */
static int fail(char *format, ...) {
    va_list args;

    g_failures++;
    fprintf(stderr, "FAIL line %zu: ", g_line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    return 0;
}

/* ---- heap and guards -------------------------------------------------------------------- */

/* Fills `size` bytes starting at `memory` with the repeating five-byte PATTERN. */
static void fill_pattern(uint8_t *memory, size_t size) {
    size_t i;

    for (i = 0; i < size; i++) {
        memory[i] = PATTERN[i % 5];   /* i % 5 cycles through 0,1,2,3,4,0,1,2,... */
    }
}

/*
 * Returns 1 if every guard byte still has its original value, 0 if any has changed.
 *
 * Memory layout (g_block):
 *       [ 64 guard bytes ][ the heap you are given ][ 64 guard bytes ]
 * Your allocator is only allowed to touch the middle part. If a guard byte changes, your code
 * has written before the start or after the end of the heap.
 */
static int guards_ok(void) {
    size_t i;

    for (i = 0; g_block != NULL && i < GUARD_SIZE; i++) {
        /* g_block[i] is in the guard before the heap; g_block[GUARD_SIZE + g_size + i] is in
           the guard after it. */
        if (g_block[i] != GUARD_BYTE || g_block[GUARD_SIZE + g_size + i] != GUARD_BYTE) {
            return 0;
        }
    }

    return 1;
}

/* Like guards_ok, but also prints a FAIL line if a guard byte has changed. */
static int guards(void) {
    if (guards_ok()) {
        return 1;
    }

    return fail("heap guard bytes changed: allocator wrote outside the supplied heap");
}

/*
 * Called straight after EVERY call into your allocator. It counts the call (for the summary)
 * and checks that the call did not scribble over the guard bytes.
 * Returns 1 if all is well, 0 if a failure was reported.
 */
static int after_call(void) {
    g_calls++;
    return guards();
}

/*
 * Forgets an allocation: releases the harness's two tracking arrays and marks the slot unused.
 * (This frees the harness's own memory. It never calls your mm_free.)
 */
static void clear_active(Slot *slot) {
    free(slot->expected);
    free(slot->known);
    slot->expected = NULL;
    slot->known = NULL;
    slot->ptr = NULL;
    slot->size = 0;
    slot->active = 0;
}

/* Throws away the current heap and every slot, ready for a new HEAP command (or the end). */
static void destroy_heap(void) {
    size_t i;

    for (i = 0; i < MAX_SLOTS; i++) {
        clear_active(&g_slots[i]);
        g_slots[i].last_ptr = NULL;
    }

    free(g_block);
    g_block = NULL;
    g_heap = NULL;
    g_size = 0;
}

/*
 * Returns 1 if the `size` bytes starting at `ptr` lie entirely inside the heap, else 0.
 * Written so that no arithmetic can overflow, even for huge sizes.
 */
static int in_heap(uint8_t *ptr, size_t size) {
    /* No heap, a NULL pointer, or a pointer before the start or beyond the end: outside. */
    if (g_heap == NULL || ptr == NULL || ptr < g_heap || (size_t)(ptr - g_heap) > g_size) {
        return 0;
    }

    /* ptr - g_heap is how far into the heap ptr points; g_size minus that is the room left. */
    return size <= g_size - (size_t)(ptr - g_heap);
}

/*
 * Returns 1 if the `size` bytes starting at `ptr` overlap any allocation that is still live.
 * Two ranges overlap exactly when each one starts before the other one ends.
 */
static int overlaps_live(uint8_t *ptr, size_t size) {
    size_t i;

    for (i = 0; i < MAX_SLOTS; i++) {
        Slot *slot = &g_slots[i];

        if (slot->active && ptr < slot->ptr + slot->size && slot->ptr < ptr + size) {
            return 1;
        }
    }

    return 0;
}

/*
 * HEAP command: build a fresh simulated heap and call your mm_init on it.
 * Returns 1 on success, 0 if a failure was reported.
 */
static int create_heap(size_t size) {
    int result;

    if (size == 0) {
        return fail("HEAP size must be greater than zero");
    }

    /* Refuse sizes so large that adding the guards would wrap around. */
    if (size > SIZE_MAX - 2 * GUARD_SIZE) {
        return fail("HEAP size is too large");
    }

    destroy_heap();   /* forget any earlier heap and slots */

    /* One block big enough for: guard + heap + guard. */
    g_block = malloc(size + 2 * GUARD_SIZE);

    if (g_block == NULL) {
        return fail("host could not allocate simulated heap");
    }

    memset(g_block, GUARD_BYTE, GUARD_SIZE);                       /* the guard before */
    memset(g_block + GUARD_SIZE + size, GUARD_BYTE, GUARD_SIZE);   /* the guard after */
    g_heap = g_block + GUARD_SIZE;                                 /* the heap is in between */
    g_size = size;
    fill_pattern(g_heap, size);   /* the heap starts out holding the repeating pattern */

    result = mm_init(g_heap, size);   /* YOUR CODE RUNS HERE: your allocator sets itself up */

    if (!after_call()) {
        return 0;
    }

    if (result != 0) {   /* mm_init must return 0 for success */
        return fail("mm_init returned failure");
    }

    pass("HEAP size=%zu", size);
    return 1;
}

/* ---- allocation ------------------------------------------------------------------------- */

/*
 * Calls your mm_malloc for `size` bytes and stores what it returned in *ptr.
 * `command` is only used in the error message. Returns 0 if a failure was reported (there is
 * no heap yet, or your mm_malloc wrote outside the heap); returns 1 otherwise, even when
 * mm_malloc itself returned NULL - the caller decides whether NULL is acceptable.
 */
static int call_malloc(size_t size, char *command, uint8_t **ptr) {
    if (g_heap == NULL) {
        return fail("no heap: use HEAP before %s", command);
    }

    *ptr = mm_malloc(size);   /* YOUR CODE RUNS HERE */
    return after_call();
}

/*
 * MALLOC and TRY_MALLOC commands: call mm_malloc and record the result in slot `id`.
 * MALLOC requires success. TRY_MALLOC (allow_null = 1) also accepts NULL, which is the right
 * answer when the heap is full.
 */
static int allocate(size_t id, size_t size, int allow_null) {
    Slot *slot = &g_slots[id];   /* a pointer to this slot's entry in the table */
    uint8_t *ptr;

    if (slot->active) {
        return fail("slot is already active");
    }

    if (!call_malloc(size, "MALLOC", &ptr)) {
        return 0;
    }

    /* NULL is a legitimate answer only for TRY_MALLOC. */
    if (ptr == NULL) {
        if (!allow_null) {
            return fail("mm_malloc returned NULL unexpectedly");
        }

        pass("TRY_MALLOC slot=%zu size=%zu returned NULL", id, size);
        return 1;
    }

    /* The coursework says mm_malloc(0) returns NULL, so reaching here with size 0 is wrong. */
    if (size == 0) {
        return fail("mm_malloc(0) returned non-NULL");
    }

    /* The whole block you promised must lie inside the heap. */
    if (!in_heap(ptr, size)) {
        return fail("allocator returned a payload outside the supplied heap");
    }

    /* The distance from the start of the heap must be a multiple of 40. (Remainder is zero.) */
    if ((size_t)(ptr - g_heap) % ALIGNMENT != 0) {
        return fail("allocator returned a pointer that is not 40-byte aligned "
                    "relative to the heap base");
    }

    /* Two live allocations must never share any bytes. */
    if (overlaps_live(ptr, size)) {
        return fail("allocator returned a payload overlapping another live allocation");
    }

    /*
     * All good. Make the harness's own bookkeeping for this allocation: one array for the
     * bytes we expect to be stored, one saying which of them we have written so far. calloc
     * sets everything to zero, so at first nothing is "known".
     */
    slot->expected = calloc(size, 1);
    slot->known = calloc(size, 1);

    if (slot->expected == NULL || slot->known == NULL) {
        clear_active(slot);
        return fail("host could not allocate shadow tracking");
    }

    slot->ptr = ptr;
    slot->size = size;
    slot->active = 1;
    pass("%s slot=%zu size=%zu offset=%zu", allow_null ? "TRY_MALLOC" : "MALLOC", id, size,
         (size_t)(ptr - g_heap));
    return 1;
}

/*
 * EXPECT_NULL and EXPECT_NULL_MAX commands: your mm_malloc must refuse this request by
 * returning NULL. (Nothing is stored in a slot because nothing should have been allocated.)
 */
static int expect_null(size_t size) {
    uint8_t *ptr;

    if (!call_malloc(size, "EXPECT_NULL", &ptr)) {
        return 0;
    }

    if (ptr != NULL) {
        return fail("EXPECT_NULL received a non-NULL pointer");
    }

    pass("EXPECT_NULL size=%zu", size);
    return 1;
}

/* ---- reading and writing ---------------------------------------------------------------- */

/*
 * The four commands WRITE, WRITE_FAIL, READ and READ_FAIL share this function.
 *
 *   is_write = 1  call your mm_write: store `length` bytes of the repeating pattern at `offset`
 *                 inside the allocation, and remember them in the harness's own copy.
 *   is_write = 0  call your mm_read: fetch `length` bytes from `offset`, and compare them with
 *                 the harness's copy of anything that was written there earlier.
 *   must_fail = 1 the request is deliberately invalid (for example it runs past the end of the
 *                 block), so your function must return 0 (no bytes transferred) and change
 *                 nothing.
 *   must_fail = 0 the request is valid, so your function must return `length`.
 */
static int transfer(size_t id, size_t offset, size_t length, int is_write, int must_fail) {
    Slot *slot = &g_slots[id];
    char *op = is_write ? "WRITE" : "READ";   /* only used in messages */
    uint8_t dummy = 0;
    uint8_t *buffer = &dummy;   /* for a zero-length request we still need a valid pointer */
    size_t result;
    int ok = 0;                 /* becomes 1 only if everything below succeeds */

    if (!slot->active) {
        return fail("%s refers to an inactive slot", op);
    }

    /* A temporary buffer in the harness: the data we send (write) or receive (read). */
    if (length > 0) {
        buffer = malloc(length);

        if (buffer == NULL) {
            return fail(is_write ? "host could not allocate WRITE source buffer"
                                 : "host could not allocate READ buffer");
        }

        if (is_write) {
            fill_pattern(buffer, length);   /* the data to store */
        } else {
            /* Start with a recognisable junk value so that a mm_read that copies nothing
               cannot pass by accident. */
            memset(buffer, 0xEE, length);
        }
    }

    /* YOUR CODE RUNS HERE */
    if (is_write) {
        result = mm_write(slot->ptr, offset, buffer, length);
    } else {
        result = mm_read(slot->ptr, offset, buffer, length);
    }

    if (!after_call()) {
        /* A guard byte changed. after_call has already printed the FAIL line. */
    } else if (must_fail) {
        /* The request was invalid: 0 (nothing transferred) is the only acceptable answer. */
        if (result != 0) {
            fail("%s_FAIL expected 0", op);
        } else {
            pass("%s_FAIL slot=%zu offset=%zu length=%zu returned 0", op, id, offset, length);
            ok = 1;
        }
    } else if (result != length) {
        /* A valid request must return the number of bytes transferred. */
        fail("mm_%s did not return the requested length", is_write ? "write" : "read");
    } else if (offset > slot->size || length > slot->size - offset) {
        /* Your function said "success" for a range outside the block. The scenario author
           expected success here, so the scenario itself is wrong; we cannot track the data. */
        fail("scenario expected %s success outside the tracked payload", op);
    } else {
        size_t i;

        /*
         * For a read, compare each byte with the harness's copy. Bytes never written are
         * "unknown" (their value is up to you) and are skipped. `break` stops at the first
         * mismatch, leaving i < length.
         */
        for (i = 0; !is_write && i < length; i++) {
            if (slot->known[offset + i] && buffer[i] != slot->expected[offset + i]) {
                break;
            }
        }

        if (i < length && !is_write) {
            fail("mm_read returned data that differs from previously written data");
        } else {
            if (is_write && length > 0) {
                /* Remember what we stored, and mark those bytes as known. */
                memcpy(slot->expected + offset, buffer, length);
                memset(slot->known + offset, 1, length);
            }

            pass("%s slot=%zu offset=%zu length=%zu", op, id, offset, length);
            ok = 1;
        }
    }

    if (length > 0) {
        free(buffer);   /* the temporary buffer (the dummy byte was never malloc'd) */
    }

    return ok;
}

/*
 * CHECK and CHECK_SAFE commands: read back EVERYTHING the harness has written to slot `id` and
 * compare it with the harness's copy. Bytes are read in "runs": each stretch of consecutive
 * known bytes is fetched with one call to your mm_read.
 *
 *   CHECK       (safe = 0)  every run must be returned correctly.
 *   CHECK_SAFE  (safe = 1)  used after deliberately damaging the heap. For each run you may
 *               either return the correct data, or refuse by returning 0. What you must never
 *               do is return DIFFERENT data as though it were fine.
 */
static int check(size_t id, int safe) {
    Slot *slot = &g_slots[id];
    size_t start = 0;
    int saw_known = 0;   /* did we find at least one written byte to compare? */

    if (!slot->active) {
        return fail("CHECK refers to an inactive slot");
    }

    while (start < slot->size) {
        size_t end;
        size_t length;
        uint8_t *buffer;
        size_t result;

        /* Skip bytes that were never written. */
        if (!slot->known[start]) {
            start++;
            continue;
        }

        /* `start` is the first byte of a run. Find where the run ends. */
        saw_known = 1;
        end = start;

        while (end < slot->size && slot->known[end]) {
            end++;
        }

        length = end - start;
        buffer = malloc(length);

        if (buffer == NULL) {
            return fail("host could not allocate CHECK buffer");
        }

        result = mm_read(slot->ptr, start, buffer, length);   /* YOUR CODE RUNS HERE */

        if (!after_call()) {
            free(buffer);
            return 0;
        }

        /* A 0 is only acceptable for CHECK_SAFE. Anything else is examined. */
        if (result != 0 || !safe) {
            if (result != length) {
                free(buffer);
                return fail(safe ? "CHECK_SAFE received neither valid data nor 0"
                                 : "CHECK could not read expected live data");
            }

            /* memcmp returns 0 when the two byte ranges are identical. */
            if (memcmp(buffer, slot->expected + start, length) != 0) {
                free(buffer);
                return fail("allocator returned corrupted data as though it were valid");
            }
        }

        free(buffer);
        start = end;   /* continue after this run */
    }

    pass("%s slot=%zu%s", safe ? "CHECK_SAFE" : "CHECK", id,
         saw_known ? "" : " (no written bytes to compare)");
    return 1;
}

/* ---- freeing ---------------------------------------------------------------------------- */

/* Calls your mm_free on `ptr`, then counts the call and checks the guards as usual. */
static int call_free(void *ptr) {
    mm_free(ptr);   /* YOUR CODE RUNS HERE */
    return after_call();
}

/*
 * FREE command: give slot `id`'s block back to your allocator, then forget it.
 * The pointer is remembered in last_ptr so that FREE_AGAIN can free it a second time.
 */
static int release(size_t id) {
    Slot *slot = &g_slots[id];

    if (!slot->active) {
        return fail("FREE refers to an inactive slot");
    }

    slot->last_ptr = slot->ptr;

    if (!call_free(slot->ptr)) {
        return 0;
    }

    clear_active(slot);   /* the slot is now unused and may be reused by a later MALLOC */
    pass("FREE slot=%zu", id);
    return 1;
}

/*
 * FREE_AGAIN command: free the pointer that was already freed from this slot. This is an
 * invalid request (a "double free"), so your allocator must cope without damaging anything.
 */
static int free_again(size_t id) {
    if (g_slots[id].last_ptr == NULL) {
        return fail("FREE_AGAIN requires a slot that has previously been freed");
    }

    if (!call_free(g_slots[id].last_ptr)) {
        return 0;
    }

    pass("FREE_AGAIN slot=%zu", id);
    return 1;
}

/* FREE_NULL command: mm_free(NULL) is allowed and must do nothing. */
static int free_null(void) {
    if (!call_free(NULL)) {
        return 0;
    }

    pass("FREE_NULL");
    return 1;
}

/*
 * FREE_OFFSET command: free a pointer that is `relative` bytes away from the start of a live
 * block. That address is not the start of any allocation, so freeing it is invalid and your
 * allocator must ignore it. The slot stays live so the block can still be checked afterwards.
 */
static int free_offset(size_t id, long long relative) {
    if (!g_slots[id].active) {
        return fail("FREE_OFFSET refers to an inactive slot");
    }

    if (relative == 0) {
        return fail("FREE_OFFSET requires a non-zero relative offset");
    }

    /* Pointer arithmetic: ptr + relative is the address `relative` bytes from ptr. */
    if (!call_free(g_slots[id].ptr + relative)) {
        return 0;
    }

    pass("FREE_OFFSET slot=%zu relative=%lld", id, relative);
    return 1;
}

/* ---- fault injection -------------------------------------------------------------------- */

/*
 * FLIP_SLOT command: simulate a hardware fault by damaging the heap behind your back.
 * The target byte is `relative` bytes from the start of the block in slot `id` (it may be
 * negative, which reaches the bookkeeping in front of the block). Every bit set in `mask` is
 * flipped in that byte. This does not call your allocator.
 */
static int flip_slot(size_t id, long long relative, uint8_t mask) {
    Slot *slot = &g_slots[id];
    long long start;    /* where the block starts, counted from the start of the heap */
    long long target;   /* the byte to damage, counted from the start of the heap */

    if (!slot->active) {
        return fail("FLIP_SLOT refers to an inactive slot");
    }

    start = slot->ptr - g_heap;

    /* The target must stay inside the heap: 0 <= start + relative < g_size. The test is
       rearranged so that it cannot overflow. */
    if (relative < -start || relative >= (long long)g_size - start) {
        return fail("FLIP_SLOT target is outside the heap");
    }

    target = start + relative;
    g_heap[target] ^= mask;   /* ^= is XOR: each 1-bit in mask flips the matching bit */
    pass("FLIP_SLOT slot=%zu relative=%lld target=%lld xor=0x%02X", id, relative, target, mask);
    return 1;
}

/*
 * A tiny "random" number generator (xorshift). Given the same starting `seed` it always
 * produces the same sequence, so a STORM is repeatable: the same faults every time.
 * `state` holds the generator's current value and is updated on each call. (<< and >> shift
 * the bits left and right; ^ is XOR.)
 */
static unsigned int next_random(unsigned int *state) {
    unsigned int x = *state;

    if (x == 0) {
        x = 0x6D2B79F5;   /* the generator cannot work from zero, so use another start value */
    }

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/*
 * STORM command: flip `count` single bits at pseudo-random places in the heap, a bit like a
 * burst of cosmic rays. The seed decides which places, so a scenario is repeatable.
 */
static int storm(unsigned int seed, size_t count) {
    unsigned int state = seed;
    size_t i;

    if (g_heap == NULL) {
        return fail("no heap: use HEAP before STORM");
    }

    for (i = 0; i < count; i++) {
        size_t offset = next_random(&state) % g_size;   /* a random byte in the heap */

        /* A random bit position 0..7 within that byte: 1 << n is the byte with only bit n set. */
        g_heap[offset] ^= 1 << (next_random(&state) % 8);
    }

    pass("STORM seed=%u count=%zu", seed, count);
    return 1;
}

/* ---- commands --------------------------------------------------------------------------- */

/*
 * Every command is described by one entry in the table below:
 *   name   the word used in the scenario file
 *   kinds  one letter per argument the command takes, saying what kind of number it is:
 *            s  a slot number (0-255)
 *            z  an ordinary non-negative number, such as a size, offset or length
 *            i  a signed number (may be negative), such as a relative offset
 *            m  an XOR mask: 1 to 255
 *            e  a 32-bit seed for the random number generator
 *   usage  the text shown when the arguments are wrong
 */
typedef struct {
    char *name;
    char *kinds;
    char *usage;
} Command;

static Command COMMANDS[] = {
    {"HEAP", "z", "HEAP <bytes>"},
    {"MALLOC", "sz", "MALLOC <slot> <bytes>"},
    {"TRY_MALLOC", "sz", "TRY_MALLOC <slot> <bytes>"},
    {"EXPECT_NULL", "z", "EXPECT_NULL <bytes>"},
    {"EXPECT_NULL_MAX", "", "EXPECT_NULL_MAX"},
    {"WRITE", "szz", "WRITE <slot> <offset> <length>"},
    {"WRITE_FAIL", "szz", "WRITE_FAIL <slot> <offset> <length>"},
    {"READ", "szz", "READ <slot> <offset> <length>"},
    {"READ_FAIL", "szz", "READ_FAIL <slot> <offset> <length>"},
    {"CHECK", "s", "CHECK <slot>"},
    {"CHECK_SAFE", "s", "CHECK_SAFE <slot>"},
    {"CHECK_ALL", "", "CHECK_ALL"},
    {"CHECK_ALL_SAFE", "", "CHECK_ALL_SAFE"},
    {"FREE", "s", "FREE <slot>"},
    {"FREE_AGAIN", "s", "FREE_AGAIN <slot>"},
    {"FREE_NULL", "", "FREE_NULL"},
    {"FREE_OFFSET", "si", "FREE_OFFSET <slot> <signed-relative-offset>"},
    {"FLIP_SLOT", "sim", "FLIP_SLOT <slot> <signed-relative-offset> <xor-mask>"},
    {"STORM", "ez", "STORM <seed> <count>"},
};

/*
 * Converts the text of one scenario argument (such as "128" or "0x80") into a number, and
 * stores it in *value. `kind` is one of the letters above. Returns 1 if the text is a valid
 * number of that kind, 0 if not (for example "abc", "-5" for a size, or slot 300).
 */
static int parse(char kind, char *text, size_t *value) {
    unsigned long long number;
    char *end = NULL;   /* strtoull sets this to the first character it could not convert */

    if (*text == '\0') {
        return 0;
    }

    errno = 0;   /* strtoull reports "too big" through errno, so clear it first */

    if (kind == 'i') {
        number = (unsigned long long)strtoll(text, &end, 0);   /* signed: a minus sign is fine */
    } else if (*text == '-') {
        return 0;   /* every other kind must not be negative */
    } else {
        number = strtoull(text, &end, 0);   /* base 0 accepts decimal (128) and hex (0x80) */
    }

    /* Reject numbers that overflowed, empty numbers, and text with trailing junk like "12abc". */
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }

    *value = (size_t)number;

    /* Finally apply the extra limits for each kind. */
    if (kind == 's') {
        return number < MAX_SLOTS;
    }

    if (kind == 'm') {
        return number >= 1 && number <= 0xFF;
    }

    if (kind == 'e') {
        return number <= UINT32_MAX;
    }

    if (kind == 'z') {
        return number <= SIZE_MAX;
    }

    return 1;
}

/*
 * Runs one scenario line. `name` is the command word, args[0..argc-1] are the argument words.
 * Steps: find the command in the table, convert the arguments to numbers, then call the
 * function that does the work.
 */
static void execute(char *name, char **args, size_t argc) {
    Command *command = NULL;
    size_t v[3] = {0, 0, 0};   /* the converted arguments: v[0] is the first, and so on */
    size_t i;

    /* Step 1: look the command up by name. */
    for (i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(name, COMMANDS[i].name) == 0) {   /* strcmp returns 0 for equal strings */
            command = &COMMANDS[i];
        }
    }

    if (command == NULL) {
        fail("unknown command");
        return;
    }

    /* Step 2: convert each argument, stopping at the first one that is not valid. */
    for (i = 0; i < argc && i < strlen(command->kinds); i++) {
        if (!parse(command->kinds[i], args[i], &v[i])) {
            break;
        }
    }

    /* The line is wrong if it has too few or too many arguments, or one failed to convert. */
    if (argc != strlen(command->kinds) || i != argc) {
        fail("usage: %s", command->usage);
        return;
    }

    /* Step 3: run the command. Its arguments are in v[0], v[1], v[2]. */
    if (strcmp(name, "HEAP") == 0) {
        create_heap(v[0]);
    } else if (strcmp(name, "MALLOC") == 0) {
        allocate(v[0], v[1], 0);
    } else if (strcmp(name, "TRY_MALLOC") == 0) {
        allocate(v[0], v[1], 1);
    } else if (strcmp(name, "EXPECT_NULL") == 0) {
        expect_null(v[0]);
    } else if (strcmp(name, "EXPECT_NULL_MAX") == 0) {
        expect_null(SIZE_MAX);   /* the biggest request there can be */
    } else if (strcmp(name, "WRITE") == 0) {
        transfer(v[0], v[1], v[2], 1, 0);   /* is_write = 1, must_fail = 0 */
    } else if (strcmp(name, "WRITE_FAIL") == 0) {
        transfer(v[0], v[1], v[2], 1, 1);
    } else if (strcmp(name, "READ") == 0) {
        transfer(v[0], v[1], v[2], 0, 0);
    } else if (strcmp(name, "READ_FAIL") == 0) {
        transfer(v[0], v[1], v[2], 0, 1);
    } else if (strcmp(name, "CHECK") == 0) {
        check(v[0], 0);
    } else if (strcmp(name, "CHECK_SAFE") == 0) {
        check(v[0], 1);
    } else if (strcmp(name, "CHECK_ALL") == 0 || strcmp(name, "CHECK_ALL_SAFE") == 0) {
        /* Run CHECK (or CHECK_SAFE) on every slot that is currently in use. */
        for (i = 0; i < MAX_SLOTS; i++) {
            if (g_slots[i].active) {
                check(i, strcmp(name, "CHECK_ALL_SAFE") == 0);
            }
        }
    } else if (strcmp(name, "FREE") == 0) {
        release(v[0]);
    } else if (strcmp(name, "FREE_AGAIN") == 0) {
        free_again(v[0]);
    } else if (strcmp(name, "FREE_NULL") == 0) {
        free_null();
    } else if (strcmp(name, "FREE_OFFSET") == 0) {
        free_offset(v[0], (long long)v[1]);   /* v[1] holds a signed number: convert it back */
    } else if (strcmp(name, "FLIP_SLOT") == 0) {
        flip_slot(v[0], (long long)v[1], (uint8_t)v[2]);
    } else if (strcmp(name, "STORM") == 0) {
        storm((unsigned int)v[0], v[1]);
    }
}

/* ---- scenario file ---------------------------------------------------------------------- */

/*
 * Reads the scenario file line by line and runs each line. Rules of the file format:
 *   - everything from a '#' to the end of the line is a comment and is ignored;
 *   - blank lines are ignored;
 *   - the first word is the command (in any mixture of upper and lower case), the rest are
 *     its arguments, separated by spaces or tabs.
 * Returns 1 if the whole file was read, 0 if reading had to stop early.
 */
static int run_scenario(FILE *file) {
    char line[LINE_SIZE];   /* room for one line of the file */

    while (fgets(line, sizeof(line), file) != NULL) {   /* read the next line; NULL = no more */
        char *token;
        char *name;
        char *args[MAX_ARGS];
        size_t argc = 0;

        g_line++;   /* so messages can say which line they are about */

        /* A line with no newline that is not the end of the file did not fit in `line`. */
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fail("scenario line is too long");
            return 0;
        }

        /* Cut the line off at a '#': overwriting it with '\0' ends the string there. */
        token = strchr(line, '#');

        if (token != NULL) {
            *token = '\0';
        }

        /*
         * strtok splits a string into words. The first call returns the first word (here the
         * command name); each later call with NULL returns the next word, and NULL at the end.
         */
        name = strtok(line, " \t\r\n");

        if (name == NULL) {
            continue;   /* nothing on this line: skip to the next one */
        }

        /* Make the command name upper case, so "malloc" and "MALLOC" mean the same. */
        for (token = name; *token != '\0'; token++) {
            *token = toupper((unsigned char)*token);
        }

        /* Collect the remaining words as the arguments. */
        while ((token = strtok(NULL, " \t\r\n")) != NULL && argc < MAX_ARGS) {
            args[argc++] = token;
        }

        if (token != NULL) {
            fail("too many arguments");   /* the loop stopped because args was full */
        } else {
            execute(name, args, argc);
        }
    }

    if (ferror(file)) {   /* fgets stopped because of a read error, not the end of the file */
        fprintf(stderr, "ERROR: failed while reading scenario file\n");
        return 0;
    }

    return 1;
}

/*
 * Where the program starts. argc is the number of words on the command line and argv holds
 * them: argv[0] is the program name and argv[1] must be the scenario file.
 */
int main(int argc, char **argv) {
    FILE *file;
    int read_ok;

    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <scenario.test>\n\n"
                "See TESTHARNESS.md for the public scenario format.\n",
                argv[0]);
        return 2;
    }

    file = fopen(argv[1], "r");   /* open the scenario file for reading */

    if (file == NULL) {
        fprintf(stderr, "ERROR: could not open scenario '%s': %s\n", argv[1], strerror(errno));
        return 2;
    }

    read_ok = run_scenario(file);   /* this runs the whole scenario */
    fclose(file);

    /* One last guard check, in case the final command did not make one. */
    if (!guards_ok()) {
        g_failures++;
        fprintf(stderr, "FAIL final: heap guard bytes changed; "
                        "allocator wrote outside the supplied heap\n");
    }

    printf("\nSUMMARY: %lu passed, %lu failed, %lu allocator calls\n",
           g_passes, g_failures, g_calls);
    destroy_heap();

    /* The exit status tells scripts and the autograder the outcome: 0 = no failures. */
    if (!read_ok || g_failures != 0) {
        return 1;
    }

    return 0;
}
