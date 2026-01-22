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

    // 1. 加哈希表读锁，防止扩容或节点删除
    NVM_RWLOCK_READ_LOCK(&table->lock);

    printf("\n=== NVM Allocated Memory Dump ===\n");
    printf("Total Active Slabs: %u\n", table->count);

    uint32_t slab_counter = 0;
    uint64_t total_objects_allocated = 0;

    for (uint32_t i = 0; i < table->capacity; i++) {
        SlabHashNode* curr = table->buckets[i];
        
        while (curr) {
            NvmSlab* slab = curr->slab_ptr;
            if (!slab) { curr = curr->next; continue; }

            // 为了读取准确的 cache 状态，我们需要持有 Slab 的锁
            // 注意：这可能会短暂阻塞分配器，但对于调试打印是可以接受的
            NVM_SPINLOCK_ACQUIRE(&slab->lock);

            uint32_t b_size = slab->block_size;
            // allocated_block_count 是逻辑计数 (用户持有的)
            // bitmap 是物理计数 (用户持有 + 缓存预取)
            uint32_t logical_usage = slab->allocated_block_count; 
            uint32_t total_cnt = slab->total_block_count;
            uint32_t cached_count = slab->cache_count;
            
            // 复制关键的缓存信息出来，以便尽快释放锁（如果不想长时间持锁打印）
            // 但为了打印逻辑简单，我们这里选择在持锁期间进行计算过滤，
            // 考虑到打印本身就是慢操作，持锁也无妨。

            printf("----------------------------------------------------------------\n");
            printf("[Slab #%u] Offset: 0x%-8llx | BlockSize: %-5u | Usage: %u/%u (Cached: %u)\n", 
                   slab_counter++, 
                   (unsigned long long)curr->nvm_offset, 
                   b_size, 
                   logical_usage, 
                   total_cnt,
                   cached_count);

            if (verbose && logical_usage > 0) {
                printf("    Allocated Blocks (Index -> Address):\n");
                
                uint32_t printed_lines = 0;

                // 遍历所有可能的块索引
                for (uint32_t k = 0; k < total_cnt; k++) {
                    // 1. 检查位图：必须是物理上被占用的
                    if (IS_BIT_SET(slab->bitmap, k)) {
                        
                        // 2. 检查是否在 Ring Buffer 缓存中 (预取块/空闲块)
                        bool is_cached = false;
                        for (uint32_t c = 0; c < slab->cache_count; c++) {
                            // Ring Buffer 索引计算： (head + i) % SIZE
                            uint32_t ring_idx = (slab->cache_head + c) % SLAB_CACHE_SIZE;
                            if (slab->free_block_buffer[ring_idx] == k) {
                                is_cached = true;
                                break;
                            }
                        }

                        // 如果在缓存里，说明它虽然位图是1，但不是用户的数据，跳过
                        if (is_cached) {
                            continue; 
                        }

                        // 3. 确认为用户持有，打印地址
                        uint64_t intra_slab_offset = (uint64_t)k * b_size;
                        uint64_t total_offset = curr->nvm_offset + intra_slab_offset;
                        void* block_addr = (char*)base_addr + total_offset;

                        printf("      [%3u] %p (Len: %u)\n", k, block_addr, b_size);
                        
                        total_objects_allocated++;
                        printed_lines++;
                    }
                }

                // 双重校验：打印的行数应该等于逻辑使用量
                if (printed_lines != logical_usage) {
                     printf("      [WARNING] Displayed %u blocks, but logical usage is %u. (Consistency Check Fail)\n", 
                            printed_lines, logical_usage);
                }

            } else if (logical_usage == 0) {
                printf("    (Slab is legally empty, bitmap may have pre-fetches)\n");
            } else if (!verbose) {
                printf("    (Details hidden...)\n");
                total_objects_allocated += logical_usage;
            }

            NVM_SPINLOCK_RELEASE(&slab->lock); // 释放 Slab 锁
            
            curr = curr->next;
        }
    }
    printf("----------------------------------------------------------------\n");
    printf("=== End Dump: %u Slabs, %llu Total Objects ===\n\n", 
           slab_counter, (unsigned long long)total_objects_allocated);

    NVM_RWLOCK_UNLOCK(&table->lock); // 释放哈希表锁
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