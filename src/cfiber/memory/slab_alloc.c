#include "cfiber/memory/slab_alloc.h"

#include <stdint.h>
#include <string.h>

#define INDEX_BITMAP(x) ((x) / BITMAP_WORD_BITS)
#define INDEX_TO_BIT(x) ((bitmap_t)1 << ((x) & (BITMAP_WORD_BITS - 1)))

static inline uint32_t ptr_to_index(const slab_t* slab, const void* ptr) {
    return (uint32_t)(((uintptr_t)ptr - (uintptr_t)slab->memory) / slab->block_size);
}

static inline void* index_to_ptr(const slab_t* slab, const uint32_t index) {
    return (void*)((uintptr_t)slab->memory + ((uintptr_t)index * slab->block_size));
}

static inline bool bitmap_test(const bitmap_t* bitmap, const uint32_t index) {
    return (bitmap[INDEX_BITMAP(index)] & INDEX_TO_BIT(index)) != 0;
}

static inline void bitmap_set(bitmap_t* bitmap, const uint32_t index) {
    bitmap[INDEX_BITMAP(index)] |= INDEX_TO_BIT(index);
}

static inline void bitmap_clear(bitmap_t* bitmap, const uint32_t index) {
    bitmap[INDEX_BITMAP(index)] &= ~INDEX_TO_BIT(index);
}

static inline int bitmap_find_free(const bitmap_t* bitmap, const uint32_t words, const uint32_t capacity) {
    for (uint32_t i = 0; i < words; i++) {
        const bitmap_t inv = ~bitmap[i];
        if (inv) {
#if BITMAP_WORD_BITS == 64
            const int bit = __builtin_ctzll((unsigned long long)inv);
#else
            const int bit = __builtin_ctz((unsigned int)inv);
#endif
            const uint32_t index = (i * BITMAP_WORD_BITS) + (uint32_t)bit;
            if (index < capacity) {
                return (int)index;
            }
        }
    }
    return -1;
}

int slab_init(slab_t* alloc, size_t block_size, void* memory, size_t memory_size) {
    if (UNLIKELY(!block_size || (block_size % CACHE_LINE_SIZE) || memory_size < block_size)) {
        ASSERT(false && "Invalid block size or memory size");
        return -1;
    }
    const uint32_t count = (uint32_t)(memory_size / block_size);

    if (UNLIKELY(count > MAX_BLOCK_COUNT)) {
        ASSERT(false && "Exceeded max block count for bitmap");
        return -1;
    }

    alloc->memory = memory;
    alloc->block_size = block_size;
    alloc->block_count = count;
    /* calculate how many BITMAP_WORD_BITS words we actually need to iterate through */
    alloc->bitmap_count = (count + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS;

    memset(alloc->bitmap, 0, BITMAP_SIZE * sizeof(bitmap_t));

    return 0;
}

void* slab_alloc(slab_t* slab) {
    const int index = bitmap_find_free(slab->bitmap, slab->bitmap_count, slab->block_count);
    if (UNLIKELY(index < 0)) {
        return nullptr;
    }
    bitmap_set(slab->bitmap, (uint32_t)index);
    return index_to_ptr(slab, (uint32_t)index);
}

void slab_release(slab_t* slab, void* block) {
#if CFIBER_DEFENSIVE
    const size_t total_memory_size = slab->block_size * slab->block_count;
    const uintptr_t b = (uintptr_t)block;
    const uintptr_t base = (uintptr_t)slab->memory;
    if (b < base || b >= base + total_memory_size) {
        ASSERT(false && "not our memory");
        return;
    }

    const uintptr_t offset = b - base;
    if (offset % slab->block_size) {
        ASSERT(false && "not aligned to block boundary");
        return;
    }
#endif

    const uint32_t index = ptr_to_index(slab, block);
#if CFIBER_DEFENSIVE
    if (!bitmap_test(slab->bitmap, index)) {
        ASSERT(false && "double-free");
        return;
    }
#endif

    bitmap_clear(slab->bitmap, index);
}

void slab_reset(slab_t* alloc) {
    memset(alloc->bitmap, 0, BITMAP_SIZE * sizeof(bitmap_t));
}
