#ifndef SLAB_HASH_TABLE_H
#define SLAB_HASH_TABLE_H

#include "NvmDefs.h"
#include "NvmSlab.h"

typedef struct SlabHashTable SlabHashTable;

// 创建并初始化哈希表
SlabHashTable* slab_hashtable_create(uint32_t initial_capacity);

// 销毁哈希表（注意：不释放其存储的Slab指针）
void slab_hashtable_destroy(SlabHashTable* table);

// 向哈希表中插入一个 "偏移量 -> Slab指针" 映射
int slab_hashtable_insert(SlabHashTable* table, uint64_t nvm_offset, NvmSlab* slab_ptr);

// 根据偏移量查找Slab指针
NvmSlab* slab_hashtable_lookup(SlabHashTable* table, uint64_t nvm_offset);

// 根据偏移量移除一个Slab映射
NvmSlab* slab_hashtable_remove(SlabHashTable* table, uint64_t nvm_offset);

#endif // SLAB_HASH_TABLE_H