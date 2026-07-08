// allocator.h — the simplest possible heap: a "bump allocator" over a
// static array baked into the kernel binary at compile time.
//
// There's no malloc/new available in freestanding C++ (no libc), and
// writing a real allocator (free lists, fragmentation handling, etc) is
// a project on its own. A bump allocator just hands out increasing
// offsets into a fixed buffer and never frees individual objects —
// good enough for an interpreter that allocates AST nodes and runs for
// one session, then the user reboots/reruns.

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace heap {

constexpr size_t POOL_SIZE = 4 * 1024 * 1024; // 4 MB static pool

class BumpAllocator {
public:
    BumpAllocator() : offset(0) {}

    void* alloc(size_t size) {
        // 8-byte align every allocation — some types (e.g. anything with
        // a pointer member) need this on x86_64, and misaligned access
        // can be slow or, for certain instructions, fault.
        size_t aligned = (size + 7) & ~size_t(7);

        if (offset + aligned > POOL_SIZE) {
            // Out of memory. We have no exceptions, so there's nowhere
            // graceful to throw this — return nullptr and let callers
            // decide. In this project, callers should treat this as
            // unrecoverable (print an error and reset the interpreter).
            return nullptr;
        }

        void* ptr = &pool[offset];
        offset += aligned;
        return ptr;
    }

    // Wipes the whole pool back to empty. Used between REPL sessions if
    // we ever want a hard reset rather than running until OOM.
    void reset() {
        offset = 0;
    }

    size_t used() const { return offset; }
    size_t capacity() const { return POOL_SIZE; }

private:
    static uint8_t pool[POOL_SIZE];
    size_t offset;
};

// Single global instance — the rest of the interpreter calls heap::alloc()
inline BumpAllocator g_allocator;

inline void* alloc(size_t size) {
    return g_allocator.alloc(size);
}

} // namespace heap

// Out-of-line definition of the static pool member (required in C++ for
// static class members — this actually reserves the 4MB of space in the
// kernel's .bss section).
inline uint8_t heap::BumpAllocator::pool[heap::POOL_SIZE];

// NOTE: operator new/delete are NOT defined here. As of the heap milestone,
// heap.h's free-list allocator owns the global `new`/`delete` keywords
// (see heap.h's comment for why only one allocator can own them). This
// bump allocator still works fine via heap::alloc() directly if you
// explicitly want bump-style "never free, just reset" behavior for
// something — it's just not what plain `new SomeType(...)` calls anymore.
