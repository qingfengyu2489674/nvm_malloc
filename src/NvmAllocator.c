#include "NvmAllocator.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

// ============================================================================
//                          核心数据结构
// ============================================================================

// 中心堆：全局共享，组件内部自带锁保护
typedef struct NvmCentralHeap {
    void*             nvm_base_addr;
    FreeSpaceManager* space_manager;
    SlabHashTable*    slab_lookup_table;
} NvmCentralHeap;

// CPU 堆：每个 CPU 独享，无锁访问，填充以避免伪共享
typedef struct NvmCpuHeap {
    NvmSlab* slab_lists[SC_COUNT];
    char     _padding[CACHE_LINE_SIZE - ((sizeof(NvmSlab*) * SC_COUNT) % CACHE_LINE_SIZE)];
} __attribute__((aligned(CACHE_LINE_SIZE))) NvmCpuHeap;

// 顶层分配器结构
typedef struct NvmAllocator {
    NvmCentralHeap central_heap;
    NvmCpuHeap     cpu_heaps[MAX_CPUS];
} NvmAllocator;

static struct NvmAllocator* global_nvm_allocator = NULL;

// ============================================================================
//                          内部函数前向声明
// ============================================================================

static SizeClassID   map_size_to_sc_id(size_t size);
static void          remove_slab_from_list(NvmSlab** list_head, NvmSlab* slab_to_remove);
static NvmAllocator* nvm_allocator_create_impl(void* nvm_base_addr, uint64_t nvm_size_bytes);
static void          nvm_allocator_destroy_impl(NvmAllocator* allocator);
static void*         nvm_malloc_impl(NvmAllocator* allocator, size_t size);
static void          nvm_free_impl(NvmAllocator* allocator, void* nvm_ptr);
static int           nvm_allocator_restore_allocation_impl(NvmAllocator* allocator, void* nvm_ptr, size_t size);

// ============================================================================
//                          公共 API 实现
// ============================================================================

int nvm_allocator_create(void* nvm_base_addr, uint64_t nvm_size_bytes) {
    if (global_nvm_allocator != NULL) {
        LOG_ERR("Allocator already initialized.");
        return -1;
    }
    
    global_nvm_allocator = nvm_allocator_create_impl(nvm_base_addr, nvm_size_bytes);
    return (global_nvm_allocator == NULL) ? -1 : 0;
}

void nvm_allocator_destroy(void) {
    if (global_nvm_allocator != NULL) {
        nvm_allocator_destroy_impl(global_nvm_allocator);
        global_nvm_allocator = NULL;
    }
}

void* nvm_malloc(size_t size) {
    if (global_nvm_allocator == NULL) {
        LOG_ERR("Allocator not initialized.");
        return NULL;
    }
    return nvm_malloc_impl(global_nvm_allocator, size);
}

void nvm_free(void* nvm_ptr) {
    if (global_nvm_allocator == NULL) {
        LOG_ERR("Allocator not initialized.");
        return;
    }
    nvm_free_impl(global_nvm_allocator, nvm_ptr);
}

int nvm_allocator_restore_allocation(void* nvm_ptr, size_t size) {
    if (global_nvm_allocator == NULL) {
        LOG_ERR("Allocator not initialized.");
        return -1;
    }
    return nvm_allocator_restore_allocation_impl(global_nvm_allocator, nvm_ptr, size);
}

// ============================================================================
//                          内部函数实现
// ============================================================================

static SizeClassID map_size_to_sc_id(size_t size) {
    if (size <= 8)    return SC_8B;
    if (size <= 16)   return SC_16B;
    if (size <= 32)   return SC_32B;
    if (size <= 64)   return SC_64B;
    if (size <= 128)  return SC_128B;
    if (size <= 256)  return SC_256B;
    if (size <= 512)  return SC_512B;
    if (size <= 1024) return SC_1K;
    if (size <= 2048) return SC_2K;
    if (size <= 4096) return SC_4K;
    return SC_COUNT;
}

static void remove_slab_from_list(NvmSlab** list_head, NvmSlab* slab_to_remove) {
    if (!list_head || !*list_head || !slab_to_remove) return;

    if (*list_head == slab_to_remove) {
        *list_head = slab_to_remove->next_in_chain;
        return;
    }

    NvmSlab* current = *list_head;
    while (current->next_in_chain && current->next_in_chain != slab_to_remove) {
        current = current->next_in_chain;
    }

    if (current->next_in_chain == slab_to_remove) {
        current->next_in_chain = slab_to_remove->next_in_chain;
    }
}

static NvmAllocator* nvm_allocator_create_impl(void* nvm_base_addr, uint64_t nvm_size_bytes) {
    if (!nvm_base_addr) return NULL;

    // 使用 calloc 自动初始化为 0，省去手动循环初始化 CPU Heaps
    NvmAllocator* allocator = (NvmAllocator*)calloc(1, sizeof(NvmAllocator));
    if (!allocator) {
        LOG_ERR("Failed to allocate allocator struct.");
        return NULL;
    }

    // 初始化中心堆组件
    allocator->central_heap.nvm_base_addr = nvm_base_addr;
    allocator->central_heap.space_manager = space_manager_create(nvm_size_bytes, NVM_START_OFFSET);
    allocator->central_heap.slab_lookup_table = slab_hashtable_create(INITIAL_HASHTABLE_CAPACITY);

    if (!allocator->central_heap.space_manager || !allocator->central_heap.slab_lookup_table) {
        LOG_ERR("Failed to create central heap components.");
        nvm_allocator_destroy_impl(allocator);
        return NULL;
    }

    return allocator;
}

static void nvm_allocator_destroy_impl(NvmAllocator* allocator) {
    if (!allocator) return;

    // 销毁所有 CPU 堆中的 Slab
    for (int i = 0; i < MAX_CPUS; ++i) {
        for (int j = 0; j < SC_COUNT; ++j) {
            NvmSlab* curr = allocator->cpu_heaps[i].slab_lists[j];
            while (curr) {
                NvmSlab* next = curr->next_in_chain;
                nvm_slab_destroy(curr);
                curr = next;
            }
        }
    }

    // 销毁中心堆组件
    if (allocator->central_heap.space_manager) 
        space_manager_destroy(allocator->central_heap.space_manager);
    if (allocator->central_heap.slab_lookup_table) 
        slab_hashtable_destroy(allocator->central_heap.slab_lookup_table);

    free(allocator);
}

static void* nvm_malloc_impl(NvmAllocator* allocator, size_t size) {
    if (!allocator || size == 0) return NULL;

    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) {
        LOG_ERR("Size too large for slab allocation: %zu", size);
        return NULL;
    }

    // 获取当前 CPU 堆
    int cpu_id = NVM_GET_CURRENT_CPU_ID();
    NvmCpuHeap* current_cpu_heap = &allocator->cpu_heaps[cpu_id];
    NvmSlab* target_slab = current_cpu_heap->slab_lists[sc_id];

    // [Fast Path] 查找本地缓存的可用 Slab
    while (target_slab && nvm_slab_is_full(target_slab)) {
        target_slab = target_slab->next_in_chain;
    }

    // [Slow Path] 需要从中心堆分配
    if (!target_slab) {
        // 1. 申请 NVM 空间
        uint64_t offset = space_manager_alloc_slab(allocator->central_heap.space_manager);
        if (offset == (uint64_t)-1) return NULL;

        // 2. 创建 DRAM 元数据
        target_slab = nvm_slab_create(sc_id, offset);
        if (!target_slab) {
            space_manager_free_slab(allocator->central_heap.space_manager, offset);
            LOG_ERR("Failed to create slab metadata.");
            return NULL;
        }

        // 3. 注册到全局哈希表
        if (slab_hashtable_insert(allocator->central_heap.slab_lookup_table, offset, target_slab) != 0) {
            nvm_slab_destroy(target_slab);
            space_manager_free_slab(allocator->central_heap.space_manager, offset);
            LOG_ERR("Failed to insert slab into hashtable.");
            return NULL;
        }

        // 4. 挂载到本地堆 (头插法)
        target_slab->next_in_chain = current_cpu_heap->slab_lists[sc_id];
        current_cpu_heap->slab_lists[sc_id] = target_slab;
    }

    // 执行分配 (Slab 内部自旋锁保护)
    uint32_t block_idx;
    if (nvm_slab_alloc(target_slab, &block_idx) == 0) {
        uint64_t final_offset = target_slab->nvm_base_offset + (block_idx * target_slab->block_size);
        return (char*)allocator->central_heap.nvm_base_addr + final_offset;
    }

    LOG_ERR("Unexpected allocation failure in slab.");
    return NULL;
}

static void nvm_free_impl(NvmAllocator* allocator, void* nvm_ptr) {
    if (!allocator || !nvm_ptr) return;

    // 计算相对偏移并对齐到 Slab 边界
    uint64_t nvm_offset = (uint64_t)((char*)nvm_ptr - (char*)allocator->central_heap.nvm_base_addr);
    uint64_t slab_base = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    // 全局查表获取元数据
    NvmSlab* target_slab = slab_hashtable_lookup(allocator->central_heap.slab_lookup_table, slab_base);
    if (!target_slab) return;

    // 计算块索引并释放
    uint32_t block_idx = (nvm_offset - target_slab->nvm_base_offset) / target_slab->block_size;
    nvm_slab_free(target_slab, block_idx);
}

static int nvm_allocator_restore_allocation_impl(NvmAllocator* allocator, void* nvm_ptr, size_t size) {
    if (!allocator || !nvm_ptr || size == 0) return -1;

    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) return -1;

    uint64_t nvm_offset = (uint64_t)((char*)nvm_ptr - (char*)allocator->central_heap.nvm_base_addr);
    uint64_t slab_base = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    NvmCentralHeap* central = &allocator->central_heap;
    NvmSlab* slab = slab_hashtable_lookup(central->slab_lookup_table, slab_base);

    if (!slab) {
        // Slab 不存在：重建并占位
        if (space_manager_alloc_at_offset(central->space_manager, slab_base) != 0) {
            LOG_ERR("Restore failed: Space occupied.");
            return -1;
        }

        slab = nvm_slab_create(sc_id, slab_base);
        if (!slab) {
            space_manager_free_slab(central->space_manager, slab_base);
            return -1;
        }

        // 注册并挂载到默认 CPU 0
        slab_hashtable_insert(central->slab_lookup_table, slab_base, slab);
        slab->next_in_chain = allocator->cpu_heaps[0].slab_lists[sc_id];
        allocator->cpu_heaps[0].slab_lists[sc_id] = slab;
    } else {
        // Slab 已存在：校验一致性
        if (slab->size_type_id != sc_id) {
            LOG_ERR("Restore mismatch: Size class conflict.");
            return -1;
        }
    }

    // 标记位图
    uint32_t block_idx = (nvm_offset - slab_base) / slab->block_size;
    return nvm_slab_set_bitmap_at_idx(slab, block_idx);
}


// ============================================================================
//                          调试与监控 API 实现
// ============================================================================

void nvm_allocator_debug_print(void) {
    if (!global_nvm_allocator) {
        printf("[NvmAllocator] Error: Allocator is not initialized.\n");
        return;
    }

    NvmCentralHeap* central = &global_nvm_allocator->central_heap;

    printf("================================================================\n");
    printf("                  NVM Allocator Debug Dump                      \n");
    printf("================================================================\n");
    
    printf("Global Info:\n");
    printf("  NVM Base Address : %p\n", central->nvm_base_addr);
    
    // 修改处：传入基地址，并且 verbose 设为 true
    if (central->slab_lookup_table) {
        slab_hashtable_print_layout(central->slab_lookup_table, central->nvm_base_addr, true);
    } else {
        printf("[NvmAllocator] Warning: Hash table is NULL.\n");
    }

    printf("================================================================\n");
}