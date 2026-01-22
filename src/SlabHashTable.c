#include "SlabHashTable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
//                          核心数据结构
// ============================================================================

// 哈希桶节点 (拉链法)
typedef struct SlabHashNode {
    uint64_t             nvm_offset;   // Key: Slab起始偏移
    NvmSlab*             slab_ptr;     // Value: Slab元数据
    struct SlabHashNode* next;         // 冲突链下一节点
} SlabHashNode;

// 哈希表
typedef struct SlabHashTable {
    SlabHashNode** buckets;      // 桶数组
    uint32_t       capacity;     // 桶容量
    uint32_t       count;        // 元素总数
    nvm_rwlock_t   lock;         // 读写锁
} SlabHashTable;

#define CHECK_BIT(bitmap, idx) ((bitmap)[(idx) / 8] & (1 << ((idx) % 8)))


// ============================================================================
//                          内部函数前向声明
// ============================================================================

static uint32_t      hash_function(const SlabHashTable* table, uint64_t key);
static SlabHashNode* create_hash_node(uint64_t nvm_offset, NvmSlab* slab_ptr);

// ============================================================================
//                          公共 API 实现
// ============================================================================

SlabHashTable* slab_hashtable_create(uint32_t initial_capacity) {
    if (initial_capacity == 0) {
        LOG_ERR("Capacity cannot be zero.");
        return NULL;
    }

    SlabHashTable* table = (SlabHashTable*)malloc(sizeof(SlabHashTable));
    if (!table) {
        LOG_ERR("Failed to allocate table struct.");
        return NULL;
    }

    table->buckets = (SlabHashNode**)calloc(initial_capacity, sizeof(SlabHashNode*));
    if (!table->buckets) {
        LOG_ERR("Failed to allocate buckets.");
        goto err_free_table;
    }

    if (NVM_RWLOCK_INIT(&table->lock) != 0) {
        LOG_ERR("Failed to init rwlock.");
        goto err_free_buckets;
    }

    table->capacity = initial_capacity;
    table->count = 0;
    return table;

err_free_buckets:
    free(table->buckets);
err_free_table:
    free(table);
    return NULL;
}

void slab_hashtable_destroy(SlabHashTable* table) {
    if (!table) return;

    for (uint32_t i = 0; i < table->capacity; ++i) {
        SlabHashNode* curr = table->buckets[i];
        while (curr) {
            SlabHashNode* next = curr->next;
            free(curr);
            curr = next;
        }
    }

    NVM_RWLOCK_DESTROY(&table->lock);
    free(table->buckets);
    free(table);
}

int slab_hashtable_insert(SlabHashTable* table, uint64_t nvm_offset, NvmSlab* slab_ptr) {
    if (!table || !slab_ptr) return -1;

    NVM_RWLOCK_WRITE_LOCK(&table->lock);

    uint32_t idx = hash_function(table, nvm_offset);
    SlabHashNode* curr = table->buckets[idx];

    // 查重
    while (curr) {
        if (curr->nvm_offset == nvm_offset) {
            LOG_ERR("Key %llu already exists.", (unsigned long long)nvm_offset);
            NVM_RWLOCK_UNLOCK(&table->lock);
            return -1;
        }
        curr = curr->next;
    }

    // 插入 (头插法)
    SlabHashNode* new_node = create_hash_node(nvm_offset, slab_ptr);
    if (!new_node) {
        LOG_ERR("Failed to create node.");
        NVM_RWLOCK_UNLOCK(&table->lock);
        return -1;
    }

    new_node->next = table->buckets[idx];
    table->buckets[idx] = new_node;
    table->count++;

    NVM_RWLOCK_UNLOCK(&table->lock);
    return 0;
}

NvmSlab* slab_hashtable_lookup(SlabHashTable* table, uint64_t nvm_offset) {
    if (!table) return NULL;

    NVM_RWLOCK_READ_LOCK(&table->lock);

    uint32_t idx = hash_function(table, nvm_offset);
    SlabHashNode* curr = table->buckets[idx];

    while (curr) {
        if (curr->nvm_offset == nvm_offset) {
            NvmSlab* result = curr->slab_ptr;
            NVM_RWLOCK_UNLOCK(&table->lock);
            return result;
        }
        curr = curr->next;
    }

    NVM_RWLOCK_UNLOCK(&table->lock);
    return NULL;
}

NvmSlab* slab_hashtable_remove(SlabHashTable* table, uint64_t nvm_offset) {
    if (!table) return NULL;

    NVM_RWLOCK_WRITE_LOCK(&table->lock);

    uint32_t idx = hash_function(table, nvm_offset);
    SlabHashNode* curr = table->buckets[idx];
    SlabHashNode* prev = NULL;

    while (curr) {
        if (curr->nvm_offset == nvm_offset) {
            // 解链
            if (prev) prev->next = curr->next;
            else      table->buckets[idx] = curr->next;

            NvmSlab* slab = curr->slab_ptr;
            free(curr);
            table->count--;

            NVM_RWLOCK_UNLOCK(&table->lock);
            return slab;
        }
        prev = curr;
        curr = curr->next;
    }

    LOG_ERR("Key %llu not found for removal.", (unsigned long long)nvm_offset);
    NVM_RWLOCK_UNLOCK(&table->lock);
    return NULL;
}


// ============================================================================
//                          调试工具 API 实现
// ============================================================================

void slab_hashtable_print_layout(SlabHashTable* table, void* base_addr, bool verbose) {
    if (!table) {
        printf("[SlabHashTable] Table is NULL\n");
        return;
    }

    // 加读锁
    NVM_RWLOCK_READ_LOCK(&table->lock);

    printf("\n=== NVM Allocated Memory Dump ===\n");
    printf("Total Active Slabs: %u\n", table->count);

    uint32_t slab_counter = 0;
    uint64_t total_objects_allocated = 0;

    for (uint32_t i = 0; i < table->capacity; i++) {
        SlabHashNode* curr = table->buckets[i];
        
        while (curr) {
            NvmSlab* slab = curr->slab_ptr;
            // 防御性检查
            if (!slab) { curr = curr->next; continue; }

            uint32_t b_size = slab->block_size;
            uint32_t alloc_cnt = slab->allocated_block_count;
            uint32_t total_cnt = slab->total_block_count;

            // 1. 打印 Slab 头部信息
            printf("----------------------------------------------------------------\n");
            printf("[Slab #%u] Offset: 0x%-8llx | BlockSize: %-5u | Usage: %u/%u\n", 
                   slab_counter++, 
                   (unsigned long long)curr->nvm_offset, 
                   b_size, 
                   alloc_cnt, 
                   total_cnt);

            // 2. 如果开启详细模式且该 Slab 有分配对象，则遍历位图打印地址
            if (verbose && alloc_cnt > 0) {
                printf("    Allocated Blocks (Index -> Address):\n");
                
                // 遍历位图
                for (uint32_t k = 0; k < total_cnt; k++) {
                    if (CHECK_BIT(slab->bitmap, k)) {
                        // --- 核心地址计算逻辑 ---
                        // 1. 块在 Slab 内的偏移 = 索引 * 块大小
                        uint64_t intra_slab_offset = k * b_size;
                        // 2. 块在 NVM 中的总偏移 = Slab偏移 + Slab内偏移
                        uint64_t total_offset = curr->nvm_offset + intra_slab_offset;
                        // 3. 块的绝对虚拟地址 = NVM基地址 + 总偏移
                        void* block_addr = (char*)base_addr + total_offset;

                        printf("      [%3u] %p (Len: %u)\n", k, block_addr, b_size);
                        
                        total_objects_allocated++;
                    }
                }
            } else if (alloc_cnt == 0) {
                printf("    (Slab is Empty)\n");
            } else if (!verbose) {
                printf("    (Details hidden, set verbose=true to see addresses)\n");
            }
            
            curr = curr->next;
        }
    }
    printf("----------------------------------------------------------------\n");
    printf("=== End Dump: %u Slabs, %llu Total Objects ===\n\n", 
           slab_counter, (unsigned long long)total_objects_allocated);

    NVM_RWLOCK_UNLOCK(&table->lock);
}

// ============================================================================
//                          内部函数实现
// ============================================================================

static uint32_t hash_function(const SlabHashTable* table, uint64_t key) {
    // 偏移量是 SLAB_SIZE 对齐的，除以 SLAB_SIZE 得到索引以增加离散度
    uint64_t index = key / NVM_SLAB_SIZE;
    return index % table->capacity;
}

static SlabHashNode* create_hash_node(uint64_t nvm_offset, NvmSlab* slab_ptr) {
    SlabHashNode* node = (SlabHashNode*)malloc(sizeof(SlabHashNode));
    if (node) {
        node->nvm_offset = nvm_offset;
        node->slab_ptr   = slab_ptr;
        node->next       = NULL;
    } else {
        LOG_ERR("Malloc failed for node.");
    }
    return node;
}