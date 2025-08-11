#ifndef NVM_ALLOCATOR_H
#define NVM_ALLOCATOR_H

#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmSlab.h"
#include "NvmDefs.h"

typedef struct NvmAllocator NvmAllocator;

// 创建并初始化NVM分配器
int nvm_allocator_create(void* nvm_base_addr, uint64_t nvm_size_bytes);

// 销毁NVM分配器并释放其资源
void nvm_allocator_destroy();

// 从NVM分配一块内存
void* nvm_malloc(size_t size);

// 释放指定的NVM内存
void nvm_free(void* nvm_ptr);

// 恢复一个之前已分配的内存块
int nvm_allocator_restore_allocation(void* nvm_ptr, size_t size);

#endif // NVM_ALLOCATOR_H