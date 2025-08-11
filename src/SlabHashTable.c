#include "SlabHashTable.h"
#include <stdlib.h>
#include <string.h>


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


// ============================================================================
//                          内部函数前向声明
// ============================================================================

static uint32_t hash_function(const SlabHashTable* table, uint64_t key);
static SlabHashNode* create_hash_node(uint64_t nvm_offset, NvmSlab* slab_ptr);


// ============================================================================
//                          公共 API 函数实现
// ============================================================================

// 创建并初始化哈希表
SlabHashTable* slab_hashtable_create(uint32_t initial_capacity) {
    if (initial_capacity == 0) {
        fprintf(stderr, "ERROR [HashTable]: Cannot create hash table with zero capacity.\n");
        return NULL;
    }

    SlabHashTable* table = (SlabHashTable*)malloc(sizeof(SlabHashTable));
    if (table == NULL) {
        fprintf(stderr, "ERROR [HashTable]: Failed to allocate memory for SlabHashTable struct.\n");
        return NULL;
    }

    table->buckets = (SlabHashNode**)calloc(initial_capacity, sizeof(SlabHashNode*));
    if (table->buckets == NULL) {
        fprintf(stderr, "ERROR [HashTable]: Failed to allocate memory for hash buckets.\n");
        free(table);
        return NULL;
    }

    table->capacity = initial_capacity;
    table->count = 0;
    return table;
}

// 销毁哈希表，释放所有哈希节点和桶数组
void slab_hashtable_destroy(SlabHashTable* table) {
    if (table == NULL) {
        return;
    }
    // 遍历并释放每个桶中的冲突链
    for (uint32_t i = 0; i < table->capacity; ++i) {
        SlabHashNode* current_node = table->buckets[i];
        while (current_node != NULL) {
            SlabHashNode* node_to_free = current_node;
            current_node = current_node->next;
            free(node_to_free);
        }
    }
    free(table->buckets);
    free(table);
}

// 向哈希表中插入一个 "偏移量 -> Slab指针" 映射
int slab_hashtable_insert(SlabHashTable* table, uint64_t nvm_offset, NvmSlab* slab_ptr) {
    if (table == NULL || slab_ptr == NULL) {
        fprintf(stderr, "ERROR [HashTable]: Insert failed due to NULL table or slab_ptr argument.\n");
        return -1;
    }
    
    uint32_t bucket_index = hash_function(table, nvm_offset);

    // 遍历冲突链，检查键是否已存在
    SlabHashNode* current_node = table->buckets[bucket_index];
    while (current_node != NULL) {
        if (current_node->nvm_offset == nvm_offset) {
            fprintf(stderr, "ERROR [HashTable]: Insert failed. Key %llu already exists in the hash table.\n", (unsigned long long)nvm_offset);
            return -1; // 键已存在
        }
        current_node = current_node->next;
    }

    // 创建新节点并使用头插法插入
    SlabHashNode* new_node = create_hash_node(nvm_offset, slab_ptr);
    if (new_node == NULL) {
        fprintf(stderr, "ERROR [HashTable]: Insert failed. Could not create hash node (host memory exhausted).\n");
        return -1; // 内存不足
    }
    new_node->next = table->buckets[bucket_index];
    table->buckets[bucket_index] = new_node;

    table->count++;
    return 0;
}

// 根据偏移量查找Slab指针
NvmSlab* slab_hashtable_lookup(SlabHashTable* table, uint64_t nvm_offset) {
    if (table == NULL) {
        return NULL;
    }

    uint32_t bucket_index = hash_function(table, nvm_offset);

    // 遍历冲突链查找匹配的键
    SlabHashNode* current_node = table->buckets[bucket_index];
    while (current_node != NULL) {
        if (current_node->nvm_offset == nvm_offset) {
            return current_node->slab_ptr; // 找到匹配项
        }
        current_node = current_node->next;
    }

    return NULL; // 未找到
}

// 根据偏移量移除一个Slab映射
NvmSlab* slab_hashtable_remove(SlabHashTable* table, uint64_t nvm_offset) {
    if (table == NULL) {
        return NULL;
    }

    uint32_t bucket_index = hash_function(table, nvm_offset);

    // 遍历冲突链，查找要删除的节点
    SlabHashNode* current_node = table->buckets[bucket_index];
    SlabHashNode* prev_node = NULL;

    while (current_node != NULL) {
        if (current_node->nvm_offset == nvm_offset) {
            // 将节点从链表中解链
            if (prev_node == NULL) {
                table->buckets[bucket_index] = current_node->next; // 移除的是头节点
            } else {
                prev_node->next = current_node->next; // 移除的是中间或尾部节点
            }

            NvmSlab* slab_to_return = current_node->slab_ptr;
            free(current_node);
            table->count--;
            return slab_to_return;
        }
        
        prev_node = current_node;
        current_node = current_node->next;
    }

    fprintf(stderr, "WARN [HashTable]: Attempted to remove non-existent key %llu.\n", (unsigned long long)nvm_offset);
    return NULL; // 未找到
}


// ============================================================================
//                          内部函数实现
// ============================================================================

// (内部) 哈希函数：将NVM偏移量转换为哈希桶索引。
// 利用了key是NVM_SLAB_SIZE倍数的特性来优化。
static uint32_t hash_function(const SlabHashTable* table, uint64_t key) {
    uint64_t slab_index = key / NVM_SLAB_SIZE; // 使用slab索引而非原始地址进行哈希
    return slab_index % table->capacity;
}

// (内部) 创建并初始化一个新的哈希节点
static SlabHashNode* create_hash_node(uint64_t nvm_offset, NvmSlab* slab_ptr) {
    SlabHashNode* node = (SlabHashNode*)malloc(sizeof(SlabHashNode));
    if (node != NULL) {
        node->nvm_offset = nvm_offset;
        node->slab_ptr = slab_ptr;
        node->next = NULL;
    } else {
        fprintf(stderr, "ERROR [HashTable]: malloc failed for SlabHashNode.\n");
    }
    return node;
}