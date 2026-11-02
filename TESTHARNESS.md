# RetroCore Test Harness

`testharness.c` provides the standard public test environment for the RetroCore memory manager. It creates the simulated heap, calls your allocator only through the required `mm_*` interface, records the data that it expects allocations to contain, and can deliberately alter heap bytes to simulate faults.

You should not modify `testharness.c`.

## Building

Your `Makefile` should provide a `testharness` target and `make all` should create both:

```text
liballocator.so
testharness
```

A typical harness link command is:

```text
gcc -std=c11 -Wall -Wextra -Wpedantic \
    testharness.c -L. -lallocator -Wl,-rpath,'$ORIGIN' \
    -o testharness
```

## Running

Run one scenario file at a time:

```text
./testharness tests/00_smoke.test
```

A scenario contains one command per line. Blank lines are ignored and `#` begins a comment. Command names are not case sensitive, and numbers may be written in decimal or hexadecimal (`0x1F`). A line may have at most 10 arguments.

The harness prints `PASS line N: ...` on standard output and `FAIL line N: ...` on standard error, and ends with a `SUMMARY` line giving the number of passes, failures and allocator calls. Exit status `0` means that the scenario completed without a detected failure, `1` means that at least one check failed, and `2` means that the harness was started incorrectly or could not open the scenario file.

## Heap pattern

Before `mm_init` is called, the complete simulated heap is initialised with the recurring five-byte pattern:

```text
A5 5A C3 3C 96
```

The source buffer supplied by the harness to every normal `WRITE` operation is also initialised with this same recurring pattern, starting from `A5` for each write.

## Guard bytes

The heap you are given is surrounded by 64 guard bytes on each side. After every call into your allocator the harness checks them, and any change is reported as `heap guard bytes changed`. It means that your allocator wrote before the start or after the end of the heap it was given.

## Slots

Successful allocations are stored in numbered harness slots `0` to `255`.

Slots belong to the harness only; they are not part of your allocator.

## Public commands

### `HEAP <bytes>`

Create a fresh heap, fill it with the recurring five-byte pattern, and call:

```c
mm_init(heap, heap_size);
```

`mm_init` must return `0`. Any earlier heap and all slots are discarded.

Example:

```text
HEAP 8192
```

### `MALLOC <slot> <bytes>`

Call `mm_malloc(bytes)` and require success. The slot must not already be in use, and `bytes` must not be `0` (`mm_malloc(0)` must return `NULL`; test that with `EXPECT_NULL 0`). The harness verifies that the returned payload is inside the supplied heap, is 40-byte aligned relative to the heap base, and does not overlap another live allocation.

```text
MALLOC 0 128
```

### `TRY_MALLOC <slot> <bytes>`

Like `MALLOC`, but `NULL` is permitted. If allocation succeeds, the returned pointer is checked and stored in the slot.

```text
TRY_MALLOC 5 4096
```

### `EXPECT_NULL <bytes>`

Require `mm_malloc(bytes)` to return `NULL`.

```text
EXPECT_NULL 0
```

### `EXPECT_NULL_MAX`

Call `mm_malloc(SIZE_MAX)` and require `NULL`. This is useful when checking overflow handling.

### `WRITE <slot> <offset> <length>`

Call `mm_write` using a source buffer containing the recurring five-byte pattern. `mm_write` must return `length`.

`mm_read` and `mm_write` return a `size_t`: the number of bytes transferred. That is `length` on success and `0` when the request is rejected, in which case nothing may be copied. A zero-length request that is otherwise valid also returns `0`.

```text
WRITE 0 20 100
```

The harness stores its own independent copy of the bytes that should now be present.

A zero-length write is written naturally as:

```text
WRITE 0 128 0
```

### `READ <slot> <offset> <length>`

Call `mm_read`. It must return `length`, and bytes that were previously written are compared with the harness's independent expected copy.

```text
READ 0 20 100
```

### `WRITE_FAIL <slot> <offset> <length>`

Require `mm_write` to reject the operation by returning `0`.

```text
WRITE_FAIL 0 95 10
```

### `READ_FAIL <slot> <offset> <length>`

Require `mm_read` to reject the operation by returning `0`.

```text
READ_FAIL 0 95 10
```

### `CHECK <slot>`

Re-read all regions of the allocation for which the harness has known expected data. All checked data must be returned successfully and exactly match the data previously written.

### `CHECK_SAFE <slot>`

Like `CHECK`, except that each known region may either:

- be returned correctly; or
- be rejected by returning `0`.

Returning changed data as though it were valid is always a failure.

This command is useful after deliberate fault injection.

### `CHECK_ALL`

Apply `CHECK` to every active slot.

### `CHECK_ALL_SAFE`

Apply `CHECK_SAFE` to every active slot.

### `FREE <slot>`

Call `mm_free` on an active allocation.

### `FREE_AGAIN <slot>`

Call `mm_free` again using the pointer most recently freed from the slot. This checks safe handling of repeated frees.

### `FREE_NULL`

Call:

```c
mm_free(NULL);
```

The operation must be harmless.

### `FREE_OFFSET <slot> <signed-relative-offset>`

Call `mm_free` using a deliberately invalid pointer derived from a live allocation. The offset must not be `0`.

```text
FREE_OFFSET 0 8
```

The live allocation remains tracked by the harness so that it can subsequently be checked.

### `FLIP_SLOT <slot> <signed-relative-offset> <xor-mask>`

Directly XOR one heap byte relative to a live payload pointer. The mask must be between `1` and `255`; each bit that is set in it is flipped.

```text
FLIP_SLOT 0 12 0x01
FLIP_SLOT 0 -8 0x04
```

Targets outside the supplied heap are rejected by the harness.

### `STORM <seed> <count>`

Inject `count` deterministic single-bit faults at pseudo-random positions across the supplied heap.

```text
STORM 48192 4
```

The same seed, heap size and count produce the same fault sequence in this harness.

## Public and unseen scenarios

The supplied public scenarios are deliberately small and readable. Unseen evaluation scenarios use the same allocator interface and general harness model but may vary heap size, allocation sizes and lifetimes, fragmentation, invalid operations, fault location, memory pressure and workload duration.

Do not rely on a particular slot number, allocation sequence or fault position.
