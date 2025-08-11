#include "NvmAllocator.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// 初始哈希表容量，素数有助于减少哈希冲突
#define INITIAL_HASHTABLE_CAPACITY 101

// ============================================================================
//                          内部函数前向声明
// ============================================================================

static SizeClassID map_size_to_sc_id(size_t size);
static void remove_slab_from_list(NvmSlab** list_head, NvmSlab* slab_to_remove);


// ============================================================================
//                          公共 API 函数实现
// ============================================================================

NvmAllocator* nvm_allocator_create(uint64_t total_nvm_size, uint64_t nvm_start_offset) {
    NvmAllocator* allocator = (NvmAllocator*)malloc(sizeof(NvmAllocator));
    if (allocator == NULL) {
        return NULL;
    }

    // 创建底层组件
    allocator->space_manager = space_manager_create(total_nvm_size, nvm_start_offset);
    allocator->slab_lookup_table = slab_hashtable_create(INITIAL_HASHTABLE_CAPACITY);

    // 任一组件创建失败，则清理并返回NULL
    if (allocator->space_manager == NULL || allocator->slab_lookup_table == NULL) {
        space_manager_destroy(allocator->space_manager);
        slab_hashtable_destroy(allocator->slab_lookup_table);
        free(allocator);
        return NULL;
    }

    // 初始化Slab链表头指针
    for (int i = 0; i < SC_COUNT; ++i) {
        allocator->slab_lists[i] = NULL;
    }

    return allocator;
}

void nvm_allocator_destroy(NvmAllocator* allocator) {
    if (allocator == NULL) {
        return;
    }

    // 遍历并销毁所有Slab元数据
    for (int i = 0; i < SC_COUNT; ++i) {
        NvmSlab* current_slab = allocator->slab_lists[i];
        while (current_slab != NULL) {
            NvmSlab* slab_to_destroy = current_slab;
            current_slab = current_slab->next_in_chain;
            nvm_slab_destroy(slab_to_destroy);
        }
    }

    // 销毁底层组件
    space_manager_destroy(allocator->space_manager);
    slab_hashtable_destroy(allocator->slab_lookup_table);

    // 释放分配器自身
    free(allocator);
}

uint64_t nvm_malloc(NvmAllocator* allocator, size_t size) {
    if (allocator == NULL || size == 0) {
        return (uint64_t)-1;
    }

    // 映射请求大小到尺寸类别
    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) {
        // TODO: 为大对象增加直接分配逻辑
        return (uint64_t)-1;
    }

    // 在对应链表中查找有空闲空间的Slab
    NvmSlab* target_slab = allocator->slab_lists[sc_id];
    while (target_slab != NULL && nvm_slab_is_full(target_slab)) {
        target_slab = target_slab->next_in_chain;
    }

    // 未找到可用Slab，则创建新的
    if (target_slab == NULL) {
        uint64_t new_slab_offset = space_manager_alloc_slab(allocator->space_manager);
        if (new_slab_offset == (uint64_t)-1) {
            return (uint64_t)-1; // NVM空间耗尽
        }

        target_slab = nvm_slab_create(sc_id, new_slab_offset);
        if (target_slab == NULL) {
            space_manager_free_slab(allocator->space_manager, new_slab_offset); // DRAM不足，归还NVM空间
            return (uint64_t)-1;
        }

        // 注册新Slab并加入链表头部
        slab_hashtable_insert(allocator->slab_lookup_table, new_slab_offset, target_slab);
        target_slab->next_in_chain = allocator->slab_lists[sc_id];
        allocator->slab_lists[sc_id] = target_slab;
    }

    // 从Slab中分配一个块
    uint32_t block_idx;
    if (nvm_slab_alloc(target_slab, &block_idx) == 0) {
        return target_slab->nvm_base_offset + (block_idx * target_slab->block_size);
    }

    return (uint64_t)-1; // 理论上不应发生
}

void nvm_free(NvmAllocator* allocator, uint64_t nvm_offset) {
    if (allocator == NULL || nvm_offset == (uint64_t)-1) {
        return;
    }

    // 根据地址计算Slab基地址
    uint64_t slab_base_offset = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    // 查找Slab元数据
    NvmSlab* target_slab = slab_hashtable_lookup(allocator->slab_lookup_table, slab_base_offset);
    if (target_slab == NULL) {
        assert(!"Attempting to free an unmanaged memory offset!");
        return;
    }

    // 计算块在Slab内的索引并释放
    uint32_t block_idx = (nvm_offset - target_slab->nvm_base_offset) / target_slab->block_size;
    nvm_slab_free(target_slab, block_idx);

    // 如果Slab变空，则回收
    if (nvm_slab_is_empty(target_slab)) {
        // 为避免抖动，保留每种尺寸至少一个Slab
        SizeClassID sc_id = target_slab->size_type_id;
        if (allocator->slab_lists[sc_id] != target_slab || target_slab->next_in_chain != NULL) {
            remove_slab_from_list(&allocator->slab_lists[sc_id], target_slab);
            slab_hashtable_remove(allocator->slab_lookup_table, target_slab->nvm_base_offset);
            space_manager_free_slab(allocator->space_manager, target_slab->nvm_base_offset);
            nvm_slab_destroy(target_slab);
        }
    }
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


uint64_t nvm_allocator_restore_allocation(NvmAllocator* allocator, uint64_t nvm_offset, size_t size) {
    if (allocator == NULL || size == 0) return (uint64_t)-1;

    SizeClassID sc_id = map_size_to_sc_id(size);
    if (sc_id == SC_COUNT) return (uint64_t)-1;

    uint64_t slab_base_offset = (nvm_offset / NVM_SLAB_SIZE) * NVM_SLAB_SIZE;

    NvmSlab* target_slab = slab_hashtable_lookup(allocator->slab_lookup_table, slab_base_offset);

    if (target_slab == NULL) {
        // Slab 不存在: 创建新的
        if (space_manager_alloc_at_offset(allocator->space_manager, slab_base_offset) != 0) return (uint64_t)-1;

        target_slab = nvm_slab_create(sc_id, slab_base_offset);
        if (target_slab == NULL) {
            space_manager_free_slab(allocator->space_manager, slab_base_offset); // 回滚
            return (uint64_t)-1;
        }

        // 注册并链接新 Slab
        slab_hashtable_insert(allocator->slab_lookup_table, slab_base_offset, target_slab);
        target_slab->next_in_chain = allocator->slab_lists[sc_id];
        allocator->slab_lists[sc_id] = target_slab;
        
    } else {
        // Slab 已存在: 检查尺寸类别是否一致
        if (target_slab->size_type_id != sc_id) return (uint64_t)-1;
    }

    // 在Slab内标记此块为已分配
    uint32_t block_idx = (nvm_offset - slab_base_offset) / target_slab->block_size;
    if (nvm_slab_set_bitmap_at_idx(target_slab, block_idx) != 0) return (uint64_t)-1;

    return nvm_offset;
}

