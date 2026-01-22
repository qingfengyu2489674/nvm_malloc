#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "NvmDefs.h"
#include "NvmSlab.h"

// ============================================================================
//                          内部函数前向声明
// ============================================================================

static uint32_t get_block_size_from_sc_id(SizeClassID sc_id);
static uint32_t refill_cache(NvmSlab* self);
static uint32_t drain_cache(NvmSlab* self);

// ============================================================================
//                          公共 API 实现
// ============================================================================

NvmSlab* nvm_slab_create(SizeClassID sc_id, uint64_t nvm_base_offset) {
    uint32_t block_size = get_block_size_from_sc_id(sc_id);
    if (block_size == 0) {
        LOG_ERR("Invalid SizeClassID: %d", sc_id);
        return NULL;
    }

    uint32_t total_block_count = NVM_SLAB_SIZE / block_size;
    size_t bitmap_bytes = (total_block_count + 7) / 8;
    
    // 分配元数据 (含柔性数组)
    NvmSlab* self = (NvmSlab*)calloc(1, sizeof(NvmSlab) + bitmap_bytes);
    if (!self) {
        LOG_ERR("Failed to allocate metadata.");
        return NULL;
    }

    self->nvm_base_offset   = nvm_base_offset;
    self->size_type_id      = (uint8_t)sc_id;
    self->block_size        = block_size;
    self->total_block_count = total_block_count;

    if (NVM_SPINLOCK_INIT(&self->lock) != 0) {
        LOG_ERR("Failed to init spinlock.");
        free(self);
        return NULL;
    }

    return self;
}

void nvm_slab_destroy(NvmSlab* self) {
    if (!self) return;
    NVM_SPINLOCK_DESTROY(&self->lock);
    free(self);
}

int nvm_slab_alloc(NvmSlab* self, uint32_t* out_block_idx) {
    if (!self || !out_block_idx) return -1;

    NVM_SPINLOCK_ACQUIRE(&self->lock);

    // 缓存为空时尝试填充
    if (self->cache_count == 0) {
        refill_cache(self);
    }

    // 仍为空说明已满
    if (self->cache_count == 0) {
        NVM_SPINLOCK_RELEASE(&self->lock);
        return -1;
    }

    // 从缓存分配
    *out_block_idx = self->free_block_buffer[self->cache_head];
    self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
    self->cache_count--;
    __atomic_fetch_add(&self->allocated_block_count, 1, __ATOMIC_RELAXED);

    NVM_SPINLOCK_RELEASE(&self->lock);
    return 0;
}

void nvm_slab_free(NvmSlab* self, uint32_t block_idx) {
    if (!self) return;
    if (block_idx >= self->total_block_count) {
        LOG_ERR("Block index out of bounds: %u", block_idx);
        return;
    }

    NVM_SPINLOCK_ACQUIRE(&self->lock);

    if (self->allocated_block_count > 0) {
        __atomic_fetch_sub(&self->allocated_block_count, 1, __ATOMIC_RELAXED);
    }

    // 缓存满时回写位图
    if (self->cache_count >= SLAB_CACHE_SIZE) {
        drain_cache(self);
    }
    
    // 放入缓存
    self->free_block_buffer[self->cache_tail] = block_idx;
    self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
    self->cache_count++;

    NVM_SPINLOCK_RELEASE(&self->lock);
}

bool nvm_slab_is_full(const NvmSlab* self) {
    if (!self) return false;
    
    uint32_t cnt = __atomic_load_n(&self->allocated_block_count, __ATOMIC_RELAXED);
    return cnt >= self->total_block_count;
}

bool nvm_slab_is_empty(const NvmSlab* self) {
    if (!self) return true;

    uint32_t cnt = __atomic_load_n(&self->allocated_block_count, __ATOMIC_RELAXED);
    return cnt == 0;
}

int nvm_slab_set_bitmap_at_idx(NvmSlab* self, uint32_t block_idx) {
    if (!self || block_idx >= self->total_block_count) return -1;

    NVM_SPINLOCK_ACQUIRE(&self->lock);
    
    if (!IS_BIT_SET(self->bitmap, block_idx)) {
        SET_BIT(self->bitmap, block_idx);    
        __atomic_fetch_add(&self->allocated_block_count, 1, __ATOMIC_RELAXED);
    }
    
    NVM_SPINLOCK_RELEASE(&self->lock);
    return 0;
}

// ============================================================================
//                          内部函数实现
// ============================================================================

static uint32_t get_block_size_from_sc_id(SizeClassID sc_id) {
    static const uint32_t sizes[] = {
        8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };
    if (sc_id >= 0 && sc_id < (sizeof(sizes)/sizeof(sizes[0]))) {
        return sizes[sc_id];
    }
    return 0;
}

// 假设已持锁
static uint32_t refill_cache(NvmSlab* self) {
    if (self->allocated_block_count >= self->total_block_count) {
        return 0;
    }

    uint32_t filled = 0;
    // 批量填充缓存
    for (uint32_t i = 0; i < self->total_block_count && filled < SLAB_CACHE_BATCH_SIZE; ++i) {
        if (!IS_BIT_SET(self->bitmap, i)) {
            self->free_block_buffer[self->cache_tail] = i;
            self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
            SET_BIT(self->bitmap, i); // 预标记
            filled++;
        }
    }
    self->cache_count += filled;
    return filled;
}

// 假设已持锁
static uint32_t drain_cache(NvmSlab* self) {
    if (self->cache_count <= SLAB_CACHE_BATCH_SIZE) {
        return 0;
    }

    uint32_t to_drain = self->cache_count - SLAB_CACHE_BATCH_SIZE;
    uint32_t drained = 0;

    for (uint32_t i = 0; i < to_drain; ++i) {
        uint32_t idx = self->free_block_buffer[self->cache_head];
        self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
        CLEAR_BIT(self->bitmap, idx); // 回写位图
        drained++;
    }
    
    self->cache_count -= drained;
    return drained;
}