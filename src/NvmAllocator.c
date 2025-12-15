#include "NvmAllocator.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// ============================================================================
//                          核心数据结构定义
// ============================================================================

// 1. 中心堆 (Central Heap)
// 所有 CPU 共享的资源，必须加锁保护
typedef struct NvmCentralHeap {
    void*             nvm_base_addr;     // NVM 基地址
    FreeSpaceManager* space_manager;     // 大块内存管理器
    SlabHashTable*    slab_lookup_table; // 全局 Slab 查找表
    nvm_mutex_t       global_lock;       // 中心大锁
} NvmCentralHeap;

// 2. CPU 堆 (Per-CPU Heap)
// 每个 CPU 独享的资源，无需加锁
typedef struct NvmCpuHeap {
    NvmSlab* slab_lists[SC_COUNT];
    // 填充至缓存行对齐
    char _padding[CACHE_LINE_SIZE - ((sizeof(NvmSlab*) * SC_COUNT) % CACHE_LINE_SIZE)];
} __attribute__((aligned(CACHE_LINE_SIZE))) NvmCpuHeap;


// 3. 顶层分配器容器
typedef struct NvmAllocator {
    NvmCentralHeap central_heap;         // 只有一个中心堆实例
    NvmCpuHeap     cpu_heaps[MAX_CPUS];  // 每个 CPU 一个堆实例
} NvmAllocator;


static struct NvmAllocator* global_nvm_allocator = NULL;


// ============================================================================
//                          内部函数前向声明
// ============================================================================

static SizeClassID map_size_to_sc_id(size_t size);
static void remove_slab_from_list(NvmSlab** list_head, NvmSlab* slab_to_remove);

static NvmAllocator* nvm_allocator_create_impl(void* nvm_base_addr, uint64_t nvm_size_bytes);
static void nvm_allocator_destroy_impl(NvmAllocator* allocator);

static void* nvm_malloc_impl(NvmAllocator* allocator, size_t size);
static void nvm_free_impl(NvmAllocator* allocator, void* nvm_ptr);
static int nvm_allocator_restore_allocation_impl(NvmAllocator* allocator, void* nvm_ptr, size_t size);


// ============================================================================
//                          公共 API 函数实现
// ============================================================================

int nvm_allocator_create(void* nvm_base_addr, uint64_t nvm_size_bytes) {
    if (global_nvm_allocator != NULL) {
        fprintf(stderr, "WARN: [nvm_allocator_create] Allocator already initialized. Ignoring new create request.\n");
        return -1;
    }
    
    global_nvm_allocator = nvm_allocator_create_impl(nvm_base_addr, nvm_size_bytes);

    if (global_nvm_allocator == NULL) {
        fprintf(stderr, "ERROR: [nvm_allocator_create] Failed to create NVM allocator instance.\n");
        return -1;
    }

    return 0;
}

void nvm_allocator_destroy(void) {
    if (global_nvm_allocator != NULL) {
        nvm_allocator_destroy_impl(global_nvm_allocator);
        global_nvm_allocator = NULL;
    }
}

void* nvm_malloc(size_t size) {
    assert(global_nvm_allocator != NULL && "NVM Allocator has not been initialized. Call nvm_allocator_init() first.");
    
    if (global_nvm_allocator == NULL) {
        fprintf(stderr, "ERROR: [nvm_malloc] Allocator not initialized.\n");
        return NULL;
    }

    return nvm_malloc_impl(global_nvm_allocator, size);
}


void nvm_free(void* nvm_ptr) {
    if (global_nvm_allocator == NULL) {
        assert(!"NVM Allocator has not been initialized. Call nvm_allocator_init() first.");
        return;
    }

    nvm_free_impl(global_nvm_allocator, nvm_ptr);
}

int nvm_allocator_restore_allocation(void* nvm_ptr, size_t size) {
    if (global_nvm_allocator == NULL) {
        assert(!"NVM Allocator has not been initialized. Call nvm_allocator_init() first.");
        return -1;
    }

    return nvm_allocator_restore_allocation_impl(global_nvm_allocator, nvm_ptr, size);
}


// ============================================================================
//                          内部函数实现
// ============================================================================

// (内部) 将请求大小映射到最合适的尺寸类别ID
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
    return SC_COUNT; // 请求大小超过Slab处理范围
}

// (内部) 从Slab链表中移除指定节点
static void remove_slab_from_list(NvmSlab** list_head, NvmSlab* slab_to_remove) {
    if (list_head == NULL || *list_head == NULL || slab_to_remove == NULL) {
        return;
    }

    // 处理头节点
    if (*list_head == slab_to_remove) {
        *list_head = slab_to_remove->next_in_chain;
        return;
    }

    // 查找并移除中间或尾部节点
    NvmSlab* current = *list_head;
    while (current->next_in_chain != NULL && current->next_in_chain != slab_to_remove) {
        current = current->next_in_chain;
    }

    if (current->next_in_chain == slab_to_remove) {
        current->next_in_chain = slab_to_remove->next_in_chain;
    }
}

static NvmAllocator* nvm_allocator_create_impl(void* nvm_base_addr, uint64_t nvm_size_bytes) {

    if(nvm_base_addr == NULL) {
        fprintf(stderr, "ERROR: [nvm_allocator_create_impl] nvm_base_addr cannot be NULL.\n");
        return NULL;
    }

    NvmAllocator* allocator = (NvmAllocator*)malloc(sizeof(NvmAllocator));
    if (allocator == NULL) {
        fprintf(stderr, "ERROR: [nvm_allocator_create_impl] Failed to allocate memory for NvmAllocator struct.\n");
        return NULL;
    }

    // --- 1. 初始化中心堆 ---
    allocator->central_heap.nvm_base_addr = nvm_base_addr;

    // 初始化中心大锁
    if (NVM_MUTEX_INIT(&allocator->central_heap.global_lock) != 0) {
        fprintf(stderr, "ERROR: [nvm_allocator_create_impl] Failed to initialize global mutex.\n");
        free(allocator);
        return NULL;
    }

    // 创建底层组件
    allocator->central_heap.space_manager = space_manager_create(nvm_size_bytes, NVM_START_OFFSET);
    allocator->central_heap.slab_lookup_table = slab_hashtable_create(INITIAL_HASHTABLE_CAPACITY);

    // 错误处理
    if (allocator->central_heap.space_manager == NULL || allocator->central_heap.slab_lookup_table == NULL) {
        fprintf(stderr, "ERROR: [nvm_allocator_create_impl] Failed to create space_manager or slab_lookup_table.\n");
        if (allocator->central_heap.space_manager) space_manager_destroy(allocator->central_heap.space_manager);
        if (allocator->central_heap.slab_lookup_table) slab_hashtable_destroy(allocator->central_heap.slab_lookup_table);
        NVM_MUTEX_DESTROY(&allocator->central_heap.global_lock);
        free(allocator);
        return NULL;
    }

    // --- 2. 初始化 CPU 堆 ---
    for (int i = 0; i < MAX_CPUS; ++i) {
        for (int j = 0; j < SC_COUNT; ++j) {
            allocator->cpu_heaps[i].slab_lists[j] = NULL;
        }
    }

    return allocator;
}

static void nvm_allocator_destroy_impl(NvmAllocator* allocator) {
    if (allocator == NULL) {
        return;
    }

    // 1. 遍历所有 CPU 堆，销毁挂在上面的 Slab
    for (int cpu_idx = 0; cpu_idx < MAX_CPUS; ++cpu_idx) {
        for (int i = 0; i < SC_COUNT; ++i) {
            NvmSlab* current_slab = allocator->cpu_heaps[cpu_idx].slab_lists[i];
            while (current_slab != NULL) {
                NvmSlab* slab_to_destroy = current_slab;
                current_slab = current_slab->next_in_chain;
                nvm_slab_destroy(slab_to_destroy);
            }
        }
    }

    // 2. 销毁中心堆组件
    space_manager_destroy(allocator->central_heap.space_manager);
    slab_hashtable_destroy(allocator->central_heap.slab_lookup_table);
    
    // 销毁锁
    NVM_MUTEX_DESTROY(&allocator->central_heap.global_lock);
 
    // 3. 释放分配器自身
    free(allocator);
}

static void* nvm_malloc_impl(NvmAllocator* allocator, size_t size) {
    if (allocator == NULL || size == 0) {
        return NULL;
    }

    // 1. 映射尺寸类别
    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) {    
        fprintf(stderr, "ERROR: [nvm_malloc_impl] Requested size %zu is too large for slab allocation.\n", size);
        return NULL;
    }

    // 2. 获取当前 CPU ID 和对应的 CPU 堆
    int cpu_id = NVM_GET_CURRENT_CPU_ID();
    NvmCpuHeap* current_cpu_heap = &allocator->cpu_heaps[cpu_id];

    // 3. [快速路径] 在本地堆中查找可用的 Slab
    // 注意：这里不需要加锁，因为只有当前 CPU 会操作 slab_lists 链表的结构。
    // 虽然其他线程可能会 Remote Free 导致 Slab 内部状态变化，但不会改变 next_in_chain 指针。
    NvmSlab* target_slab = current_cpu_heap->slab_lists[sc_id];
    
    // 遍历链表寻找一个未满的 Slab
    while (target_slab != NULL && nvm_slab_is_full(target_slab)) {
        target_slab = target_slab->next_in_chain;
    }

    // 4. [慢速路径] 未找到可用 Slab，需要向中心堆申请
    if (target_slab == NULL) {
        
        // --- 进入临界区 (中心大锁) ---
        NVM_MUTEX_ACQUIRE(&allocator->central_heap.global_lock);

        // 4.1 申请 NVM 空间
        uint64_t new_slab_offset = space_manager_alloc_slab(allocator->central_heap.space_manager);
        
        if (new_slab_offset == (uint64_t)-1) {
            // NVM 空间耗尽
            NVM_MUTEX_RELEASE(&allocator->central_heap.global_lock);
            fprintf(stderr, "ERROR: [nvm_malloc_impl] NVM space exhausted.\n");
            return NULL; 
        }

        // 4.2 创建 Slab 元数据 (DRAM)
        // 注意：slab_create 只是分配 DRAM 内存，理论上可以放在锁外，
        // 但为了出错处理简单（避免回滚已分配的 NVM 空间），放在锁内也无妨。
        target_slab = nvm_slab_create(sc_id, new_slab_offset);
        if (target_slab == NULL) {
            space_manager_free_slab(allocator->central_heap.space_manager, new_slab_offset);
            NVM_MUTEX_RELEASE(&allocator->central_heap.global_lock);
            fprintf(stderr, "ERROR: [nvm_malloc_impl] Failed to create slab metadata.\n");
            return NULL;
        }

        // 4.3 注册到全局哈希表 (用于 Remote Free)
        if (slab_hashtable_insert(allocator->central_heap.slab_lookup_table, new_slab_offset, target_slab) != 0) {
            nvm_slab_destroy(target_slab);
            space_manager_free_slab(allocator->central_heap.space_manager, new_slab_offset);
            NVM_MUTEX_RELEASE(&allocator->central_heap.global_lock);
            fprintf(stderr, "ERROR: [nvm_malloc_impl] Failed to insert slab into hashtable.\n");
            return NULL;
        }

        // --- 离开临界区 ---
        NVM_MUTEX_RELEASE(&allocator->central_heap.global_lock);

        // 4.4 将新 Slab 挂载到当前 CPU 堆的链表头部
        // (这是 CPU 私有操作，无需加锁)
        target_slab->next_in_chain = current_cpu_heap->slab_lists[sc_id];
        current_cpu_heap->slab_lists[sc_id] = target_slab;
    }

    // 5. 执行分配
    // nvm_slab_alloc 内部有自旋锁保护，处理位图和缓存
    uint32_t block_idx;
    if (nvm_slab_alloc(target_slab, &block_idx) == 0) {
        uint64_t final_offset = target_slab->nvm_base_offset + (block_idx * target_slab->block_size);
        return (void*)((char*)allocator->central_heap.nvm_base_addr + final_offset);
    }

    fprintf(stderr, "ERROR: [nvm_malloc_impl] nvm_slab_alloc failed unexpectedly after finding/creating a non-full slab.\n");
    return NULL;
}

static void nvm_free_impl(NvmAllocator* allocator, void* nvm_ptr) {
    if (allocator == NULL || nvm_ptr == NULL) {
        return;
    }

    // 1. 计算 NVM 偏移量
    uint64_t nvm_offset = (uint64_t)((char*)nvm_ptr - (char*)allocator->central_heap.nvm_base_addr);
    uint64_t slab_base_offset = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    // 2. [全局查表] 查找 Slab 元数据
    // 注意：Hash 表是全局共享的，读写都需要加锁保护
    NVM_MUTEX_ACQUIRE(&allocator->central_heap.global_lock);
    NvmSlab* target_slab = slab_hashtable_lookup(allocator->central_heap.slab_lookup_table, slab_base_offset);
    NVM_MUTEX_RELEASE(&allocator->central_heap.global_lock);

    if (target_slab == NULL) {
        // 这是一个严重错误，通常意味着释放了野指针或未被管理的地址
        // 在生产环境中可能需要 assert 或 log
        // assert(!"Attempting to free an unmanaged memory offset!");
        return;
    }

    // 3. [释放块] 调用 Slab 的线程安全释放函数
    // nvm_slab_free 内部持有自旋锁，保证位图和缓存的一致性
    uint32_t block_idx = (nvm_offset - target_slab->nvm_base_offset) / target_slab->block_size;
    nvm_slab_free(target_slab, block_idx);

    /*
     * [并发改造关键点 - Slab 回收策略]
     * 
     * 在单线程版本中，如果 nvm_slab_is_empty(target_slab) 为真，我们会立即：
     * 1. 从链表中移除 Slab
     * 2. 从哈希表中移除 Slab
     * 3. 归还 SpaceManager
     * 4. 销毁 Slab 元数据
     * 
     * 在多线程版本中，Slab 属于某个 CPU 的私有链表 (cpu_heaps[X].slab_lists)。
     * 如果当前线程不是 CPU X (即发生了 Remote Free)，我们无法安全地操作 CPU X 的链表
     * (除非给每个 CPU 链表也加锁，但这会增加 malloc 的开销)。
     * 
     * 策略：延迟回收 (Deferred Reclaim)
     * - 即使 Slab 全空了，我们也不将其从链表中移除，也不销毁。
     * - 它依然挂在 CPU X 的链表上。
     * - 当 CPU X 下次调用 malloc 时，会遍历链表，发现这个 Slab 是空的，直接复用它。
     * - 缺点：可能会占用一些 NVM 空间不释放。
     * - 优点：无需复杂的跨 CPU 锁，性能最高，实现最简单。
     * 
     * 未来优化：可以引入“Slab 归还队列”，将空 Slab 推送给 Owner CPU。
     */
     
     // 目前：什么都不做。
}


static int nvm_allocator_restore_allocation_impl(NvmAllocator* allocator, void* nvm_ptr, size_t size) {
    if (allocator == NULL || nvm_ptr == NULL || size == 0) return -1;

    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) {
        fprintf(stderr, "ERROR: [nvm_allocator_restore_allocation_impl] Restore size %zu is too large for slab allocation.\n", size);
        return -1;
    }

    uint64_t nvm_offset = (uint64_t)((char*)nvm_ptr - (char*)allocator->central_heap.nvm_base_addr);

    uint64_t slab_base_offset = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    NvmSlab* target_slab = slab_hashtable_lookup(allocator->central_heap.slab_lookup_table, slab_base_offset);

    if (target_slab == NULL) {
        // Slab 不存在: 创建新的
        if (space_manager_alloc_at_offset(allocator->central_heap.space_manager, slab_base_offset) != 0) {
            fprintf(stderr, "ERROR: [nvm_allocator_restore_allocation_impl] Failed to reserve NVM space at offset %llu.\n", (unsigned long long)slab_base_offset);
            return -1;
        }

        target_slab = nvm_slab_create(sc_id, slab_base_offset);
        if (target_slab == NULL) {
            space_manager_free_slab(allocator->central_heap.space_manager, slab_base_offset); // 回滚
            fprintf(stderr, "ERROR: [nvm_allocator_restore_allocation_impl] Failed to create metadata for new slab (DRAM exhausted?).\n");
            return -1;
        }

        // 注册并链接新 Slab
        slab_hashtable_insert(allocator->central_heap.slab_lookup_table, slab_base_offset, target_slab);
        target_slab->next_in_chain = allocator->cpu_heaps[0].slab_lists[sc_id];
        allocator->cpu_heaps[0].slab_lists[sc_id] = target_slab;
        
    } else {
        // Slab 已存在: 检查尺寸类别是否一致
        if (target_slab->size_type_id != sc_id) {
            fprintf(stderr, "ERROR: [nvm_allocator_restore_allocation_impl] Size class mismatch for existing slab at offset %llu. Expected SC_ID for size %zu, but found existing SC_ID %d.\n", (unsigned long long)slab_base_offset, size, target_slab->size_type_id);
            return -1;
        }
    }

    // 在Slab内标记此块为已分配
    uint32_t block_idx = (nvm_offset - slab_base_offset) / target_slab->block_size;
    if (nvm_slab_set_bitmap_at_idx(target_slab, block_idx) != 0) {
        fprintf(stderr, "ERROR: [nvm_allocator_restore_allocation_impl] Failed to mark block %u as allocated in slab at offset %llu. Block might be already allocated.\n", block_idx, (unsigned long long)slab_base_offset);
        return -1;
    }

    return 0;
}
