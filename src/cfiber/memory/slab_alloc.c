#include "cfiber/memory/slab_alloc.h"

#include "cfiber/core/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INDEX_BITMAP(x) ((x) / CFIBER_BITMAP_WORD_BITS)
#define INDEX_TO_BIT(x) ((cfiber_bitmap_t)1 << ((x) & (CFIBER_BITMAP_WORD_BITS - 1)))

static inline uint32_t ptr_to_index(const cfiber_slab_t* slab, const void* ptr) {
    return (uint32_t)(((uintptr_t)ptr - (uintptr_t)slab->memory) / slab->block_size);
}

static inline void* index_to_ptr(const cfiber_slab_t* slab, const uint32_t index) {
    return (void*)((uintptr_t)slab->memory + ((uintptr_t)index * slab->block_size));
}

static inline bool bitmap_test(const cfiber_bitmap_t* bitmap, const uint32_t index) {
    return !!(bitmap[INDEX_BITMAP(index)] & INDEX_TO_BIT(index));
}

static inline void bitmap_set(cfiber_bitmap_t* bitmap, const uint32_t index) {
    bitmap[INDEX_BITMAP(index)] |= INDEX_TO_BIT(index);
}

static inline void bitmap_clear(cfiber_bitmap_t* bitmap, const uint32_t index) {
    bitmap[INDEX_BITMAP(index)] &= ~INDEX_TO_BIT(index);
}

static inline int bitmap_find_free(const cfiber_bitmap_t* bitmap, const uint32_t words, const uint32_t capacity) {
    for (uint32_t i = 0; i < words; i++) {
        const cfiber_bitmap_t inv = ~bitmap[i];
        if (inv) {
#if CFIBER_BITMAP_WORD_BITS == 64
            const int bit = __builtin_ctzll((unsigned long long)inv);
#else
            const int bit = __builtin_ctz((unsigned int)inv);
#endif
            const uint32_t index = (i * CFIBER_BITMAP_WORD_BITS) + (uint32_t)bit;
            if (index < capacity) {
                return (int)index;
            }
        }
    }
    return -1;
}

int cfiber_slab_init(cfiber_slab_t* alloc, size_t block_size, void* memory, size_t memory_size) {
    if (UNLIKELY(!block_size || (block_size % CFIBER_CACHE_LINE_SIZE) || memory_size < block_size)) {
        ASSERT(false && "Invalid block size or memory size");
        return -1;
    }
    /* The canary and cfiber_init rely on the base alignment of every block. */
    if (UNLIKELY((uintptr_t)memory % alignof(max_align_t))) {
        ASSERT(false && "slab memory must be aligned to max_align_t");
        return -1;
    }
    const size_t count = memory_size / block_size; /* checked before narrowing */
    if (UNLIKELY(count > CFIBER_SLAB_MAX_BLOCKS)) {
        ASSERT(false && "Exceeded max block count for bitmap");
        return -1;
    }

    alloc->memory = memory;
    alloc->block_size = block_size;
    alloc->block_count = (uint32_t)count;
    /* calculate how many CFIBER_BITMAP_WORD_BITS words we actually need to iterate through */
    alloc->bitmap_count = (alloc->block_count + CFIBER_BITMAP_WORD_BITS - 1) / CFIBER_BITMAP_WORD_BITS;

    memset(alloc->bitmap, 0, CFIBER_BITMAP_SIZE * sizeof(cfiber_bitmap_t));

    return 0;
}

void* cfiber_slab_alloc(cfiber_slab_t* slab) {
    const int index = bitmap_find_free(slab->bitmap, slab->bitmap_count, slab->block_count);
    if (UNLIKELY(index < 0)) {
        return nullptr;
    }
    bitmap_set(slab->bitmap, (uint32_t)index);
    return index_to_ptr(slab, (uint32_t)index);
}

bool cfiber_slab_release(cfiber_slab_t* slab, void* block) {
#if CFIBER_DEFENSIVE
    const size_t total_memory_size = slab->block_size * slab->block_count;
    const uintptr_t b = (uintptr_t)block;
    const uintptr_t base = (uintptr_t)slab->memory;
    if (b < base || b >= base + total_memory_size) {
        ASSERT(false && "not our memory");
        return false;
    }

    const uintptr_t offset = b - base;
    if (offset % slab->block_size) {
        ASSERT(false && "not aligned to block boundary");
        return false;
    }
#endif

    const uint32_t index = ptr_to_index(slab, block);
#if CFIBER_DEFENSIVE
    if (!bitmap_test(slab->bitmap, index)) {
        ASSERT(false && "double-free");
        return false;
    }
#endif

    bitmap_clear(slab->bitmap, index);
    return true;
}

void cfiber_slab_reset(cfiber_slab_t* alloc) {
    memset(alloc->bitmap, 0, CFIBER_BITMAP_SIZE * sizeof(cfiber_bitmap_t));
}
