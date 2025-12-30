#ifndef SLAB_HASH_TABLE_H
#define SLAB_HASH_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "NvmDefs.h"
#include "NvmSlab.h"

// ============================================================================
//                          类型定义
// ============================================================================

/**
 * @brief 全局 Slab 索引哈希表 (不透明句柄)
 * 
 * 映射关系: NVM Offset (Key) -> Slab Metadata Pointer (Value)
 * 用于在 free() 时根据 NVM 指针快速找到对应的 Slab 元数据。
 * 
 * @note 线程安全：内部操作由读写锁 (RWLock) 保护。
 */
typedef struct SlabHashTable SlabHashTable;

// ============================================================================
//                          生命周期管理
// ============================================================================

/**
 * @brief 创建哈希表
 * @param initial_capacity 初始桶数量 (建议为素数)
 */
SlabHashTable* slab_hashtable_create(uint32_t initial_capacity);

/**
 * @brief 销毁哈希表
 * 注意：只释放哈希表结构本身，不释放其中存储的 Slab 指针。
 */
void slab_hashtable_destroy(SlabHashTable* table);

// ============================================================================
//                          核心操作 API
// ============================================================================

/**
 * @brief 插入映射
 * @return 0 成功, -1 失败 (键已存在或内存不足)
 */
int slab_hashtable_insert(SlabHashTable* table, uint64_t nvm_offset, NvmSlab* slab_ptr);

/**
 * @brief 查找映射
 * @return 成功返回 Slab 指针，未找到返回 NULL
 */
NvmSlab* slab_hashtable_lookup(SlabHashTable* table, uint64_t nvm_offset);

/**
 * @brief 移除映射
 * @return 被移除的 Slab 指针，未找到返回 NULL
 */
NvmSlab* slab_hashtable_remove(SlabHashTable* table, uint64_t nvm_offset);

#ifdef __cplusplus
}
#endif

#endif // SLAB_HASH_TABLE_H