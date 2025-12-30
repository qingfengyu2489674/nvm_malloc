#ifndef NVM_ALLOCATOR_H
#define NVM_ALLOCATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmSlab.h"
#include "NvmDefs.h"

// ============================================================================
//                          NVM Allocator Public API
// ============================================================================

/**
 * @brief 初始化 NVM 分配器
 * 
 * 这是一个单例模式的初始化函数。它接管指定的一块 NVM 物理内存区域，
 * 并初始化内部的中心堆、Per-CPU 缓存和元数据索引。
 * 
 * @param nvm_base_addr NVM 物理内存映射到进程空间的起始地址
 * @param nvm_size_bytes NVM 区域的总大小 (字节)
 * @return 0 成功, -1 失败 (如已初始化、内存不足等)
 */
int nvm_allocator_create(void* nvm_base_addr, uint64_t nvm_size_bytes);

/**
 * @brief 销毁 NVM 分配器
 * 
 * 释放所有 DRAM 元数据 (Slab 描述符、哈希表、空间管理链表)。
 * 注意：不会修改 NVM 物理内存中的数据。
 */
void nvm_allocator_destroy(void);

/**
 * @brief 分配 NVM 内存
 * 
 * 优先从当前 CPU 的本地缓存 (L1) 分配，无锁操作。
 * 若缓存未命中，则从中心堆 (L2) 分配并回填缓存。
 * 
 * @param size 请求大小 (字节)
 * @return 指向 NVM 内存的指针，若分配失败返回 NULL
 */
void* nvm_malloc(size_t size);

/**
 * @brief 释放 NVM 内存
 * 
 * 支持本地释放 (Local Free) 和跨线程释放 (Remote Free)。
 * 
 * @param nvm_ptr nvm_malloc 返回的指针
 */
void nvm_free(void* nvm_ptr);

// ============================================================================
//                          故障恢复 API
// ============================================================================

/**
 * @brief 恢复已分配内存块的元数据
 * 
 * 在系统崩溃重启后，用于根据持久化日志或扫描结果，重建分配器的内存视图。
 * 它会在内部 Slab 中将对应的块标记为“已占用”。
 * 
 * @param nvm_ptr 指向已分配块的指针
 * @param size 原分配大小
 * @return 0 成功, -1 失败
 */
int nvm_allocator_restore_allocation(void* nvm_ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // NVM_ALLOCATOR_H