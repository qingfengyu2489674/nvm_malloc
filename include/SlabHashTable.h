#ifndef SLAB_HASH_TABLE_H
#define SLAB_HASH_TABLE_H

#include "NvmDefs.h"
#include "NvmSlab.h"

// 哈希表节点，用于解决哈希冲突 (拉链法)。
typedef struct SlabHashNode {
    uint64_t             nvm_offset;   // Key: Slab的起始偏移量
    NvmSlab*             slab_ptr;     // Value: 指向Slab元数据的指针
    struct SlabHashNode* next;         // 指向冲突链中的下一个节点
} SlabHashNode;

// 用于映射 "NVM偏移量 -> Slab指针" 的哈希表。
typedef struct SlabHashTable {
    SlabHashNode** buckets;      // 哈希桶数组 (指针数组)
    uint32_t       capacity;     // 哈希桶数组的容量
    uint32_t       count;        // 当前存储的元素总数
} SlabHashTable;


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