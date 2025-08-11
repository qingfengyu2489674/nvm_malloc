#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#include "NvmDefs.h"
#include "NvmSpaceManager.h"


// NVM中一个连续的空闲空间块。
// 这些节点构成一个按地址排序的双向链表。
typedef struct FreeSegmentNode {
    uint64_t nvm_offset;
    uint64_t size;
    struct FreeSegmentNode* prev;    // 指向前一个节点 (地址更小)
    struct FreeSegmentNode* next;    // 指向后一个节点 (地址更大)
} FreeSegmentNode;


// NVM大块空闲空间管理器。
typedef struct FreeSpaceManager {
    FreeSegmentNode* head;
    FreeSegmentNode* tail;
} FreeSpaceManager;


// ============================================================================
//                          内部函数前向声明
// ============================================================================

static FreeSegmentNode* create_segment_node(uint64_t offset, uint64_t size);
static void remove_node_from_list(FreeSpaceManager* manager, FreeSegmentNode* node);
static void insert_node_into_list(FreeSpaceManager* manager, FreeSegmentNode* new_node, FreeSegmentNode* prev_node, FreeSegmentNode* next_node);


// ============================================================================
//                          公共 API 函数实现
// ============================================================================

// 创建并初始化空间管理器
FreeSpaceManager* space_manager_create(uint64_t total_nvm_size, uint64_t nvm_start_offset) {
    FreeSpaceManager* manager = (FreeSpaceManager*)malloc(sizeof(FreeSpaceManager));
    if (manager == NULL) {
        return NULL;
    }
    manager->head = NULL;
    manager->tail = NULL;

    // 验证NVM总大小是否有效
    if (total_nvm_size < NVM_SLAB_SIZE) {
        free(manager);
        return NULL;
    }

    // 创建代表整个空间的初始空闲节点
    FreeSegmentNode* initial_node = create_segment_node(nvm_start_offset, total_nvm_size);
    if (initial_node == NULL) {
        free(manager);
        return NULL;
    }

    // 初始化链表
    manager->head = initial_node;
    manager->tail = initial_node;

    return manager;
}

// 销毁空间管理器并释放所有节点
void space_manager_destroy(FreeSpaceManager* manager) {
    if (manager == NULL) {
        return;
    }

    // 遍历并释放链表中的所有节点
    FreeSegmentNode* current = manager->head;
    while (current != NULL) {
        FreeSegmentNode* node_to_free = current;
        current = current->next;
        free(node_to_free);
    }
    
    // 释放管理器自身
    free(manager);
}

// 从空闲空间中分配一个Slab大小的块
uint64_t space_manager_alloc_slab(FreeSpaceManager* manager) {
    if (manager == NULL) {
        return (uint64_t)-1;
    }

    // 遍历链表，查找第一个足够大的空闲块 (First-Fit策略)
    FreeSegmentNode* current = manager->head;
    while (current != NULL) {
        if (current->size >= NVM_SLAB_SIZE) {
            uint64_t allocated_offset = current->nvm_offset;

            // 根据空闲块大小决定是分裂还是直接移除
            if (current->size == NVM_SLAB_SIZE) {
                // 大小正好，从链表中移除并释放该节点
                remove_node_from_list(manager, current);
                free(current);
            } else {
                // 大小过大，分裂节点：更新其起始地址和大小
                current->nvm_offset += NVM_SLAB_SIZE;
                current->size -= NVM_SLAB_SIZE;
            }

            return allocated_offset;
        }
        current = current->next;
    }

    // 未找到足够大的空闲块，返回失败
    return (uint64_t)-1;
}

// 释放一个Slab大小的块，并将其归还给空闲链表
void space_manager_free_slab(FreeSpaceManager* manager, uint64_t offset_to_free) {
    if (manager == NULL) {
        return;
    }

    // 遍历链表，找到新块的正确插入位置 (prev_node 和 next_node)
    FreeSegmentNode* prev_node = NULL;
    FreeSegmentNode* next_node = manager->head;
    while (next_node != NULL && next_node->nvm_offset < offset_to_free) {
        prev_node = next_node;
        next_node = next_node->next;
    }
    
    // 断言：检查双重释放或地址重叠
    assert(next_node == NULL || (offset_to_free + NVM_SLAB_SIZE <= next_node->nvm_offset));
    assert(prev_node == NULL || (prev_node->nvm_offset + prev_node->size <= offset_to_free));

    bool can_merge_prev = (prev_node != NULL && (prev_node->nvm_offset + prev_node->size == offset_to_free));
    bool can_merge_next = (next_node != NULL && (offset_to_free + NVM_SLAB_SIZE == next_node->nvm_offset));

    // 根据能否与前后节点合并来执行不同操作
    if (can_merge_prev && can_merge_next) {
        // 向前向后都能合并：扩展前节点，移除后节点
        prev_node->size += NVM_SLAB_SIZE + next_node->size;
        remove_node_from_list(manager, next_node);
        free(next_node);
    } else if (can_merge_prev) {
        // 只向前合并：扩展前节点大小
        prev_node->size += NVM_SLAB_SIZE;
    } else if (can_merge_next) {
        // 只向后合并：更新后节点的起始地址和大小
        next_node->nvm_offset = offset_to_free;
        next_node->size += NVM_SLAB_SIZE;
    } else {
        // 无法合并：创建并插入一个新的独立节点
        FreeSegmentNode* new_node = create_segment_node(offset_to_free, NVM_SLAB_SIZE);
        if (new_node == NULL) { return; } // 内存不足，记录日志
        insert_node_into_list(manager, new_node, prev_node, next_node);
    }
}


// ============================================================================
//                          内部函数实现
// ============================================================================

// (内部) 创建并初始化一个新的空闲节点
static FreeSegmentNode* create_segment_node(uint64_t offset, uint64_t size) {
    FreeSegmentNode* node = (FreeSegmentNode*)malloc(sizeof(FreeSegmentNode));
    if (node != NULL) {
        node->nvm_offset = offset;
        node->size = size;
        node->prev = NULL;
        node->next = NULL;
    }
    return node;
}

// (内部) 将一个节点从双向链表中安全地解链
static void remove_node_from_list(FreeSpaceManager* manager, FreeSegmentNode* node) {
    assert(manager != NULL && node != NULL);

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        manager->head = node->next; // 移除的是头节点
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        manager->tail = node->prev; // 移除的是尾节点
    }

    node->prev = NULL;
    node->next = NULL;
}

// (内部) 将一个新节点插入到双向链表中的正确位置
static void insert_node_into_list(FreeSpaceManager* manager, FreeSegmentNode* new_node, FreeSegmentNode* prev_node, FreeSegmentNode* next_node) {
    assert(manager != NULL && new_node != NULL);

    new_node->prev = prev_node;
    new_node->next = next_node;

    if (prev_node != NULL) {
        prev_node->next = new_node;
    } else {
        manager->head = new_node; // 插入到链表头部
    }

    if (next_node != NULL) {
        next_node->prev = new_node;
    } else {
        manager->tail = new_node; // 插入到链表尾部
    }
}


int space_manager_alloc_at_offset(FreeSpaceManager* manager, uint64_t offset) {
    if (manager == NULL) return -1;
    
    const uint64_t size_to_alloc = NVM_SLAB_SIZE;
    uint64_t end_offset = offset + size_to_alloc;

    // 遍历查找包含该区域的空闲节点
    FreeSegmentNode* current = manager->head;
    while (current != NULL) {
        if (current->nvm_offset <= offset && (current->nvm_offset + current->size) >= end_offset) {
            
            bool is_head_match = (current->nvm_offset == offset);
            bool is_tail_match = ((current->nvm_offset + current->size) == end_offset);

            if (is_head_match && is_tail_match) {
                // Case 1: 完美匹配，移除节点
                remove_node_from_list(manager, current);
                free(current);

            } else if (is_head_match) {
                // Case 2: 裁切头部
                current->nvm_offset += size_to_alloc;
                current->size -= size_to_alloc;

            } else if (is_tail_match) {
                // Case 3: 裁切尾部
                current->size -= size_to_alloc;

            } else {
                // Case 4: 裁切中间，分裂节点
                uint64_t original_end = current->nvm_offset + current->size;
                current->size = offset - current->nvm_offset; // 修改原节点
                
                FreeSegmentNode* new_tail_node = create_segment_node(end_offset, original_end - end_offset);
                if (new_tail_node == NULL) return -1;
                insert_node_into_list(manager, new_tail_node, current, current->next); // 插入新节点
            }
            
            return 0; // 成功
        }
        
        current = current->next;
    }
    
    // 未找到合适的空闲块
    return -1;
}