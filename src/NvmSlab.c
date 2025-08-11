#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "NvmDefs.h"
#include "NvmSlab.h"


// 位图操作宏
#define IS_BIT_SET(bitmap, n) ((bitmap[(n) / 8] >> ((n) % 8)) & 1)
#define SET_BIT(bitmap, n) (bitmap[(n) / 8] |= (1 << ((n) % 8)))
#define CLEAR_BIT(bitmap, n) (bitmap[(n) / 8] &= ~(1 << ((n) % 8)))



// ============================================================================
//                          内部函数前向声明
// ============================================================================

static uint32_t get_block_size_from_sc_id(SizeClassID sc_id);
static uint32_t refill_cache(NvmSlab* self);
static uint32_t drain_cache(NvmSlab* self);


// ============================================================================
//                          公共 API 函数实现
// ============================================================================

// 创建并初始化Slab元数据
NvmSlab* nvm_slab_create(SizeClassID sc_id, uint64_t nvm_base_offset) {
    uint32_t block_size = get_block_size_from_sc_id(sc_id);
    if (block_size == 0) {
        fprintf(stderr, "Error: Invalid SizeClassID provided to nvm_slab_create.\n");
        return NULL;
    }

    // 计算Slab元数据所需内存大小
    uint32_t total_block_count = NVM_SLAB_SIZE / block_size;
    size_t bitmap_bytes = (total_block_count + 7) / 8;
    size_t total_alloc_size = sizeof(NvmSlab) + bitmap_bytes;

    // 分配内存（包括柔性数组成员）
    NvmSlab* self = (NvmSlab*)calloc(1, total_alloc_size);
    if (self == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for NvmSlab metadata.\n");
        return NULL;
    }

    // 初始化成员
    self->nvm_base_offset = nvm_base_offset;
    self->size_type_id = (uint8_t)sc_id;
    self->block_size = block_size;
    self->total_block_count = total_block_count;

    return self;
}

// 销毁Slab元数据
void nvm_slab_destroy(NvmSlab* self) {
    free(self);
}

// 从Slab中分配一个块
int nvm_slab_alloc(NvmSlab* self, uint32_t* out_block_idx) {
    if (self == NULL || out_block_idx == NULL) {
        return -1;
    }

    // 本地缓存为空，尝试从位图填充
    if (self->cache_count == 0) {
        refill_cache(self);
    }

    // 若仍然为空，则Slab已满
    if (self->cache_count == 0) {
        return -1;
    }

    // 从本地缓存快速获取一个空闲块索引
    uint32_t block_idx = self->free_block_buffer[self->cache_head];
    self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
    self->cache_count--;
    self->allocated_block_count++;

    *out_block_idx = block_idx;
    return 0;
}

// 将一个块释放回Slab
void nvm_slab_free(NvmSlab* self, uint32_t block_idx) {
    if (self == NULL) return;
    if (block_idx >= self->total_block_count) {
        fprintf(stderr, "Error: Attempt to free an out-of-bounds block index (%u).\n", block_idx);
        return;
    }

    // // 检查双重释放
    // // 1. 首先检查权威记录（位图）
    // if (!IS_BIT_SET(self->bitmap, block_idx)) {
    //     fprintf(stderr, "Warning: Double free detected for block index %u.\n", block_idx);
    //     return;
    // }

    // // 2. 然后检查缓存
    // // 遍历当前缓存中的所有有效索引
    // for (uint32_t i = 0; i < self->cache_count; ++i) {
    //     uint32_t cache_index = (self->cache_head + i) % SLAB_CACHE_SIZE;
    //     if (self->free_block_buffer[cache_index] == block_idx) {
    //         // 如果在缓存中找到了这个索引，这也是双重释放
    //         fprintf(stderr, "Warning: Double free on a block already in the free cache (index %u).\n", block_idx);
    //         return;
    //     }
    // }

    if (self->allocated_block_count > 0) {
        self->allocated_block_count--;
    } else {
        fprintf(stderr, "Warning: nvm_slab_free called on an already empty slab.\n");
    }

    // 本地缓存满时，先回写部分数据以腾出空间
    if (self->cache_count >= SLAB_CACHE_SIZE) {
        drain_cache(self);
    }
    
    // 将被释放的块索引放入本地缓存
    self->free_block_buffer[self->cache_tail] = block_idx;
    self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
    self->cache_count++;
}

// 检查Slab是否已满
bool nvm_slab_is_full(const NvmSlab* self) {
    if (self == NULL) return false;
    return self->allocated_block_count == self->total_block_count;
}

// 检查Slab是否完全为空
bool nvm_slab_is_empty(const NvmSlab* self) {
    if (self == NULL) return true;
    return self->allocated_block_count == 0;
}


// ============================================================================
//                          内部函数实现
// ============================================================================

// (内部) 根据尺寸类别ID获取块大小
static uint32_t get_block_size_from_sc_id(SizeClassID sc_id) {
    switch (sc_id) {
        case SC_8B:   return 8;
        case SC_16B:  return 16;
        case SC_32B:  return 32;
        case SC_64B:  return 64;
        case SC_128B: return 128;
        case SC_256B: return 256;
        case SC_512B: return 512;
        case SC_1K:   return 1024;
        case SC_2K:   return 2048;
        case SC_4K:   return 4096;
        default:      return 0;
    }
}

// (内部) 扫描位图，用空闲块索引填充本地缓存
static uint32_t refill_cache(NvmSlab* self) {
    if (self->allocated_block_count >= self->total_block_count) {
        return 0; // Slab已满，无需扫描
    }

    uint32_t filled_count = 0;
    // 扫描位图，最多填充一批 (SLAB_CACHE_BATCH_SIZE)
    for (uint32_t i = 0; i < self->total_block_count && filled_count < SLAB_CACHE_BATCH_SIZE; ++i) {
        if (!IS_BIT_SET(self->bitmap, i)) {
            self->free_block_buffer[self->cache_tail] = i;
            self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
            SET_BIT(self->bitmap, i); // 预先标记为已分配
            filled_count++;
        }
    }

    self->cache_count += filled_count;
    return filled_count;
}

// (内部) 当本地缓存满时，将部分空闲块索引回写到位图
static uint32_t drain_cache(NvmSlab* self) {
    if (self->cache_count <= SLAB_CACHE_BATCH_SIZE) {
        return 0;
    }

    // 计算需要回写的数量，使缓存降至低水位
    uint32_t count_to_drain = self->cache_count - SLAB_CACHE_BATCH_SIZE;
    uint32_t drained_count = 0;

    for (uint32_t i = 0; i < count_to_drain; ++i) {
        if (self->cache_count == 0) break; // 防御性检查
        
        uint32_t block_idx = self->free_block_buffer[self->cache_head];
        self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
        CLEAR_BIT(self->bitmap, block_idx); // 在位图中标记为可用
        self->cache_count--;
        drained_count++;
    }

    return drained_count;
}


int nvm_slab_set_bitmap_at_idx(NvmSlab* self, uint32_t block_idx) {
    // 1. 参数有效性检查
    if (self == NULL) {
        return -1;
    }
    if (block_idx >= self->total_block_count) {
        // 块索引超出了此Slab的管理范围
        return -1;
    }

    // 2. 检查位图，实现幂等性
    //    只有当块当前是空闲时（位为0），才执行标记操作
    if (!IS_BIT_SET(self->bitmap, block_idx)) {
        
        // 3. 标记位图
        SET_BIT(self->bitmap, block_idx);
        
        // 4. 更新已分配块的计数
        self->allocated_block_count++;
    }
    
    // 如果块已经被标记过了（IS_BIT_SET返回true），则什么都不做，直接返回成功。
    return 0;
}