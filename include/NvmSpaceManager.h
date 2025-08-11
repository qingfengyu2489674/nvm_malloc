#ifndef NVM_SPACE_MANAGER_H
#define NVM_SPACE_MANAGER_H

#include <stdint.h>
#include <stddef.h>

typedef struct FreeSpaceManager FreeSpaceManager;

// 创建并初始化空间管理器
FreeSpaceManager* space_manager_create(uint64_t total_nvm_size, uint64_t nvm_start_offset);

// 销毁空间管理器并释放所有节点
void space_manager_destroy(FreeSpaceManager* manager);

// 从空闲空间中分配一个Slab大小的块
uint64_t space_manager_alloc_slab(FreeSpaceManager* manager);

// 释放一个Slab大小的块，并将其归还给空闲链表
void space_manager_free_slab(FreeSpaceManager* manager, uint64_t offset_to_free);

int space_manager_alloc_at_offset(FreeSpaceManager* manager, uint64_t offset);

#endif // NVM_SPACE_MANAGER_H