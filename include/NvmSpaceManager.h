#ifndef NVM_SPACE_MANAGER_H
#define NVM_SPACE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// ============================================================================
//                          类型定义
// ============================================================================

/**
 * @brief NVM 空闲空间管理器 (不透明句柄)
 * 
 * 负责管理大块连续的 NVM 物理空间。
 * 内部维护一个按地址排序的双向链表，支持合并与分割。
 * 
 * @note 线程安全：内部操作由互斥锁 (Mutex) 保护。
 */
typedef struct FreeSpaceManager FreeSpaceManager;

// ============================================================================
//                          生命周期管理
// ============================================================================

/**
 * @brief 创建并初始化空间管理器
 * @param total_nvm_size NVM 总大小 (字节)
 * @param nvm_start_offset NVM 起始偏移量
 * @return 成功返回管理器句柄，失败返回 NULL
 */
FreeSpaceManager* space_manager_create(uint64_t total_nvm_size, uint64_t nvm_start_offset);

/**
 * @brief 销毁空间管理器
 * 释放链表节点内存和锁资源。
 */
void space_manager_destroy(FreeSpaceManager* manager);

// ============================================================================
//                          核心操作 API
// ============================================================================

/**
 * @brief 分配一个标准 Slab 大小的 NVM 块
 * 采用 First-Fit 策略。
 * @return 成功返回 NVM 偏移量，失败返回 (uint64_t)-1
 */
uint64_t space_manager_alloc_slab(FreeSpaceManager* manager);

/**
 * @brief 释放并归还一个 Slab 大小的块
 * 自动尝试与相邻的空闲块合并。
 * @param offset_to_free 要释放的 NVM 偏移量
 */
void space_manager_free_slab(FreeSpaceManager* manager, uint64_t offset_to_free);

/**
 * @brief [故障恢复] 在指定偏移处强制占位
 * 用于在系统重启后，根据持久化数据恢复已分配的块状态。
 * @return 0 成功, -1 失败 (已被占用或无效)
 */
int space_manager_alloc_at_offset(FreeSpaceManager* manager, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif // NVM_SPACE_MANAGER_H