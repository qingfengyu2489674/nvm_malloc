#ifndef NVM_SLAB_H
#define NVM_SLAB_H

#include <stdbool.h>
#include "NvmDefs.h"

// NVM Slab元数据，管理一个内存块集合。
typedef struct NvmSlab {
    
    struct NvmSlab* next_in_chain;    // 指向同尺寸链表中的下一个Slab

    uint64_t nvm_base_offset;         // Slab在NVM上的起始偏移量 (ID)
    uint8_t  size_type_id;            // 尺寸类别ID
    uint8_t  _padding[3];             // 内存对齐
    uint32_t block_size;              // Slab内块的大小
    uint32_t total_block_count;       // Slab的块总容量
    uint32_t allocated_block_count;   // 已分配的块数量

    // --- 空闲块索引的本地FIFO缓存 ---
    uint32_t cache_head;
    uint32_t cache_tail;
    uint32_t cache_count;
    uint32_t free_block_buffer[SLAB_CACHE_SIZE];

    unsigned char bitmap[];           // 块分配状态的位图 (柔性数组)

} NvmSlab;


// 创建并初始化Slab元数据
NvmSlab* nvm_slab_create(SizeClassID sc_id, uint64_t nvm_base_offset);

// 销毁Slab元数据
void nvm_slab_destroy(NvmSlab* self);

// 从Slab中分配一个块
int nvm_slab_alloc(NvmSlab* self, uint32_t* out_block_idx);

// 向Slab中释放一个块
void nvm_slab_free(NvmSlab* self, uint32_t block_idx);


int nvm_slab_set_bitmap_at_idx(NvmSlab* self, uint32_t block_idx);

// 检查Slab是否已满
bool nvm_slab_is_full(const NvmSlab* self);

// 检查Slab是否完全为空
bool nvm_slab_is_empty(const NvmSlab* self);

#endif // NVM_SLAB_H