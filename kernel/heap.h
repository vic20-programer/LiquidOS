// heap.h — a real malloc()/free() implementation: a free-list allocator
// with block coalescing, over a static memory pool (same idea as
// allocator.h's bump allocator, but this one can actually reclaim space).
//
// WHY THIS EXISTS: allocator.h's bump allocator can only grow — it hands
// out increasing offsets and the only way to reclaim ANY memory is to
// reset the entire pool at once, discarding every allocation made so
// far. That's fine for "run one program, then reset everything," which
// is exactly what MiniPy's REPL did. It's NOT fine for anything
// long-running that allocates and frees objects throughout its lifetime
// — which is exactly what a multitasking kernel needs (tasks are created
// and destroyed, each with its own dynamically-sized state).
//
// HOW IT WORKS: the pool is carved into variable-sized BLOCKS. Each block
// has a small header living just before the memory handed back to the
// caller:
//
//   [[BlockHeader][user data...........]] [[BlockHeader][user data...]]
//    ^ this pointer is what malloc() returns, pointing PAST the header
//
// Free blocks are threaded together into a singly-linked list using a
// pointer INSIDE the block itself (in the header) — no separate "list of
// free blocks" data structure is needed, which matters here since we
// don't have a heap to allocate THAT structure from (that would be
// circular).
//
// malloc(): walks the free list, takes the first block big enough
// ("first-fit"), and if it's significantly bigger than requested, splits
// it into "the part we're using" + "a new, smaller free block" so we
// don't waste the excess.
//
// free(): marks the block free and re-links it into the free list, then
// immediately tries to merge ("coalesce") with the physically-adjacent
// block on either side if that neighbor is also free. Without
// coalescing, repeated alloc/free cycles fragment the heap into many
// small free blocks that are individually useless even though their
// total size might be enough for a later large allocation.

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace heap2 {

constexpr size_t POOL_SIZE = 4 * 1024 * 1024; // 4 MB, same size as the bump pool

// Every block (whether free or in-use) starts with this header. `size`
// is the size of the USER-USABLE area only (not counting this header) —
// this matters for pointer arithmetic throughout the allocator, get it
// wrong and you corrupt the next block's header.
struct BlockHeader {
    size_t size;          // usable bytes in this block, NOT including this header
    bool is_free;
    BlockHeader* next_free; // valid only when is_free == true; intrusive free list
    BlockHeader* prev_physical; // the block immediately before this one in memory
                                 // (needed to coalesce backward — see free())
};

constexpr size_t HEADER_SIZE = sizeof(BlockHeader);
// Minimum useful block size: big enough that splitting never produces a
// block too small to even hold its own next allocation's header. Without
// this floor, repeated tiny allocations could fragment the heap into
// blocks too small to ever be reused for anything.
constexpr size_t MIN_BLOCK_SIZE = 16;

class FreeListAllocator {
public:
    FreeListAllocator() : free_list_head(nullptr), initialized(false) {}

    // Must be called once before any alloc()/free() calls. Not done in
    // the constructor because this kernel avoids relying on C++ global
    // constructors running before kmain() (see allocator.h's comment
    // about __cxa_guard — same reasoning applies here, plus it's just
    // clearer to have an explicit init step for something this important).
    void init() {
        BlockHeader* first = reinterpret_cast<BlockHeader*>(&pool[0]);
        first->size = POOL_SIZE - HEADER_SIZE;
        first->is_free = true;
        first->next_free = nullptr;
        first->prev_physical = nullptr;
        free_list_head = first;
        initialized = true;
    }

    void* alloc(size_t requested_size) {
        if (!initialized || requested_size == 0) return nullptr;

        size_t size = (requested_size + 7) & ~size_t(7);
        if (size < MIN_BLOCK_SIZE) size = MIN_BLOCK_SIZE;

        BlockHeader* prev_in_list = nullptr;
        BlockHeader* block = free_list_head;

        while (block != nullptr) {
            if (block->size >= size) {
                size_t leftover = block->size - size;

                if (leftover >= HEADER_SIZE + MIN_BLOCK_SIZE) {
                    // Split off a new free block from the unused tail of
                    // this one. IMPORTANT: split_block() does NOT touch
                    // the free list itself — it only carves up the memory
                    // and sets up the new block's header. We do the list
                    // surgery here, in ONE consistent step: new_block
                    // takes block's exact place in the list (same
                    // prev/next neighbors), since block is about to
                    // become in-use and new_block is what's left over to
                    // remain free. Splitting this into two separate
                    // "list update" steps (one inside split_block, one
                    // here) is what caused the original self-referencing
                    // cycle bug — each step independently assumed the
                    // free list was still in its pre-split state.
                    BlockHeader* new_block = split_block(block, size);
                    new_block->next_free = block->next_free;
                    if (prev_in_list != nullptr) {
                        prev_in_list->next_free = new_block;
                    } else {
                        free_list_head = new_block;
                    }
                } else {
                    remove_from_free_list(block, prev_in_list);
                }

                block->next_free = nullptr;
                block->is_free = false;
                return block_to_user_ptr(block);
            }
            prev_in_list = block;
            block = block->next_free;
        }

        return nullptr; // no block big enough - out of memory
    }

    void free(void* ptr) {
        if (ptr == nullptr) return;

        BlockHeader* block = user_ptr_to_block(ptr);
        block->is_free = true;

        // Coalesce with the NEXT physical block first (simpler — next
        // block's address is just block + HEADER_SIZE + block->size,
        // no list traversal needed to find it).
        BlockHeader* next_physical = get_next_physical(block);
        if (next_physical != nullptr && next_physical->is_free) {
            remove_from_free_list(next_physical, find_prev_in_free_list(next_physical));
            block->size += HEADER_SIZE + next_physical->size;
            BlockHeader* new_next = get_next_physical(block);
            if (new_next != nullptr) {
                new_next->prev_physical = block;
            }
        }

        // Coalesce with the PREVIOUS physical block. This needs
        // prev_physical (stored in the header) since, unlike the next
        // block, there's no arithmetic shortcut to find where the
        // previous block STARTS from where this one starts.
        BlockHeader* prev_physical = block->prev_physical;
        if (prev_physical != nullptr && prev_physical->is_free) {
            remove_from_free_list(prev_physical, find_prev_in_free_list(prev_physical));
            prev_physical->size += HEADER_SIZE + block->size;
            BlockHeader* new_next = get_next_physical(prev_physical);
            if (new_next != nullptr) {
                new_next->prev_physical = prev_physical;
            }
            block = prev_physical; // the merged block is what we add to the free list
        }

        push_to_free_list(block);
    }

    // Diagnostics - useful for verifying coalescing actually happened
    // rather than just trusting the implementation.
    size_t count_free_blocks() const {
        size_t count = 0;
        for (BlockHeader* b = free_list_head; b != nullptr; b = b->next_free) count++;
        return count;
    }

    size_t total_free_bytes() const {
        size_t total = 0;
        for (BlockHeader* b = free_list_head; b != nullptr; b = b->next_free) total += b->size;
        return total;
    }

private:
    static void* block_to_user_ptr(BlockHeader* block) {
        return reinterpret_cast<uint8_t*>(block) + HEADER_SIZE;
    }

    static BlockHeader* user_ptr_to_block(void* ptr) {
        return reinterpret_cast<BlockHeader*>(reinterpret_cast<uint8_t*>(ptr) - HEADER_SIZE);
    }

    BlockHeader* get_next_physical(BlockHeader* block) {
        uint8_t* next_addr = reinterpret_cast<uint8_t*>(block) + HEADER_SIZE + block->size;
        uint8_t* pool_end = &pool[0] + POOL_SIZE;
        if (next_addr >= pool_end) return nullptr; // block runs to the end of the pool
        return reinterpret_cast<BlockHeader*>(next_addr);
    }

    // Splits `block` into a used portion of exactly `used_size` bytes and
    // a new free block holding whatever's left over. Returns the new
    // block. Deliberately does NOT touch the free list — see alloc()'s
    // comment for why that bookkeeping has to happen in one place, not
    // split across this function and its caller.
    BlockHeader* split_block(BlockHeader* block, size_t used_size) {
        size_t original_size = block->size;
        uint8_t* new_block_addr = reinterpret_cast<uint8_t*>(block) + HEADER_SIZE + used_size;
        BlockHeader* new_block = reinterpret_cast<BlockHeader*>(new_block_addr);

        new_block->size = original_size - used_size - HEADER_SIZE;
        new_block->is_free = true;
        new_block->prev_physical = block;

        BlockHeader* after_new = get_next_physical(new_block);
        if (after_new != nullptr) {
            after_new->prev_physical = new_block;
        }

        block->size = used_size;
        return new_block;
    }

    void remove_from_free_list(BlockHeader* block, BlockHeader* prev_in_list) {
        if (prev_in_list != nullptr) {
            prev_in_list->next_free = block->next_free;
        } else {
            free_list_head = block->next_free;
        }
        block->next_free = nullptr;
    }

    BlockHeader* find_prev_in_free_list(BlockHeader* target) {
        if (free_list_head == target) return nullptr;
        for (BlockHeader* b = free_list_head; b != nullptr; b = b->next_free) {
            if (b->next_free == target) return b;
        }
        return nullptr; // target wasn't in the free list (shouldn't happen if called correctly)
    }

    void push_to_free_list(BlockHeader* block) {
        block->next_free = free_list_head;
        free_list_head = block;
    }

    static uint8_t pool[POOL_SIZE];
    BlockHeader* free_list_head;
    bool initialized;
};

inline FreeListAllocator g_heap;

inline void init() { g_heap.init(); }
inline void* alloc(size_t size) { return g_heap.alloc(size); }
inline void free_ptr(void* ptr) { g_heap.free(ptr); }

} // namespace heap2

inline uint8_t heap2::FreeListAllocator::pool[heap2::POOL_SIZE];

// ---------------------------------------------------------------------------
// NOTE ON operator new/delete: allocator.h (the bump allocator) already
// defines the global operator new/delete/new[]/delete[] overloads for
// this kernel. Only ONE allocator can own those global operators at a
// time — defining them twice is a link error, and silently swapping which
// header is included last to "pick" the active allocator is fragile.
//
// Per the README, this milestone makes heap2 (this file) the active
// allocator for `new`/`delete` by REMOVING those overloads from
// allocator.h and defining them HERE instead, backed by heap2::alloc/
// free_ptr. allocator.h's BumpAllocator class and its `heap::alloc()`
// free function still exist and still work if you want to use the bump
// allocator directly and explicitly for something — they just no longer
// own the global `new`/`delete` keywords.
// ---------------------------------------------------------------------------

inline void* operator new(size_t, void* ptr) noexcept { return ptr; } // placement new

inline void* operator new(size_t size) {
    return heap2::alloc(size);
}

inline void* operator new[](size_t size) {
    return heap2::alloc(size);
}

inline void operator delete(void* ptr) noexcept {
    heap2::free_ptr(ptr);
}

inline void operator delete[](void* ptr) noexcept {
    heap2::free_ptr(ptr);
}

inline void operator delete(void* ptr, size_t) noexcept {
    heap2::free_ptr(ptr);
}

inline void operator delete[](void* ptr, size_t) noexcept {
    heap2::free_ptr(ptr);
}
