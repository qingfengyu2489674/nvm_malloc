#ifndef NVM_ALLOCATOR_H
#define NVM_ALLOCATOR_H

#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmSlab.h"
#include "NvmDefs.h"

// NVM堆分配器核心结构
typedef struct {
    FreeSpaceManager* space_manager;    // 大块空闲空间管理器
    SlabHashTable* slab_lookup_table;   // 用于通过偏移量快速查找Slab
    NvmSlab* slab_lists[SC_COUNT];      // 按尺寸分类的Slab链表数组
} NvmAllocator;


// 创建并初始化NVM分配器
NvmAllocator* nvm_allocator_create(uint64_t total_nvm_size, uint64_t nvm_start_offset);

// 销毁NVM分配器并释放其资源
void nvm_allocator_destroy(NvmAllocator* allocator);

// 从NVM分配一块内存
uint64_t nvm_malloc(NvmAllocator* allocator, size_t size);

// 释放指定的NVM内存
void nvm_free(NvmAllocator* allocator, uint64_t nvm_offset);

// 恢复一个之前已分配的内存块
uint64_t nvm_allocator_restore_allocation(NvmAllocator* allocator, uint64_t nvm_offset, size_t size);

#endif // NVM_ALLOCATOR_H