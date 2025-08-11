#include "unity.h"

// 包含所有必要的头文件
#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 包含所有组件的实现文件
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#include <stdlib.h>
#include <string.h>

#define MAX_BLOCK_SIZE 4096

#define TOTAL_NVM_SIZE (10 * NVM_SLAB_SIZE)
#define NUM_SLABS 10

static void* mock_nvm_base = NULL;

// 通过extern声明来访问NvmAllocator.c中的全局静态实例
extern struct NvmAllocator* global_nvm_allocator;

// MODIFIED: setUp 和 tearDown 现在负责管理全局分配器的生命周期
void setUp(void) {
    mock_nvm_base = malloc(TOTAL_NVM_SIZE);
    TEST_ASSERT_NOT_NULL(mock_nvm_base);
    memset(mock_nvm_base, 0, TOTAL_NVM_SIZE);
    
    // 使用您指定的 nvm_allocator_create 初始化全局分配器
    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE));
}

void tearDown(void) {
    // 使用您指定的 nvm_allocator_destroy 销毁全局分配器
    nvm_allocator_destroy();
    
    free(mock_nvm_base);
    mock_nvm_base = NULL;
}

// ============================================================================
//         测试 nvm_allocator_restore_allocation 函数
// ============================================================================

/**
 * @brief 测试基本路径：恢复单个对象，这将触发新Slab的创建。
 */
void test_restore_first_object_in_new_slab(void) {
    const uint64_t obj_offset = 2 * NVM_SLAB_SIZE + 64;
    const size_t obj_size = 60;
    const uint64_t slab_base_offset = 2 * NVM_SLAB_SIZE;
    const SizeClassID sc_id = SC_64B;
    
    void* obj_ptr = (void*)((char*)mock_nvm_base + obj_offset);

    // MODIFIED: 调用新的全局API
    int result = nvm_allocator_restore_allocation(obj_ptr, obj_size);
    TEST_ASSERT_EQUAL_INT(0, result);

    // 白盒验证
    NvmSlab* restored_slab = global_nvm_allocator->slab_lists[sc_id];
    TEST_ASSERT_NOT_NULL(restored_slab);
    TEST_ASSERT_EQUAL_UINT64(slab_base_offset, restored_slab->nvm_base_offset);
    uint32_t block_idx = (obj_offset - slab_base_offset) / restored_slab->block_size;
    TEST_ASSERT_TRUE(IS_BIT_SET(restored_slab->bitmap, block_idx));
}

/**
 * @brief 测试在已存在的Slab中恢复第二个对象。
 */
void test_restore_second_object_in_existing_slab(void) {
    // 先恢复第一个对象
    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_restore_allocation(mock_nvm_base, 32));
    
    // 现在恢复同一Slab中的第二个对象
    const uint64_t obj_offset = 128;
    void* obj_ptr = (void*)((char*)mock_nvm_base + obj_offset);

    int result = nvm_allocator_restore_allocation(obj_ptr, 32);
    TEST_ASSERT_EQUAL_INT(0, result);

    // 白盒验证
    NvmSlab* slab = global_nvm_allocator->slab_lists[SC_32B];
    TEST_ASSERT_EQUAL_UINT32(2, slab->allocated_block_count);
    TEST_ASSERT_TRUE(IS_BIT_SET(slab->bitmap, 0));
    TEST_ASSERT_TRUE(IS_BIT_SET(slab->bitmap, 4));
}

/**
 * @brief 测试恢复一个对象，其Slab正好是整个空闲空间的头部。
 */
void test_restore_object_at_head_of_space(void) {
    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_restore_allocation(mock_nvm_base, 16));
    
    FreeSegmentNode* head = global_nvm_allocator->space_manager->head;
    TEST_ASSERT_EQUAL_UINT64(NVM_SLAB_SIZE, head->nvm_offset);
}

/**
 * @brief 测试恢复一个对象，其Slab正好是整个空闲空间的尾部。
 */
void test_restore_object_at_tail_of_space(void) {
    const uint64_t slab_base_offset = (NUM_SLABS - 1) * NVM_SLAB_SIZE;
    void* obj_ptr = (void*)((char*)mock_nvm_base + slab_base_offset);

    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_restore_allocation(obj_ptr, 16));

    FreeSegmentNode* head = global_nvm_allocator->space_manager->head;
    TEST_ASSERT_EQUAL_UINT64(slab_base_offset, head->size);
    TEST_ASSERT_NULL(head->next);
}

/**
 * @brief 测试恢复流程中的错误处理路径。
 */
void test_restore_error_handling(void) {
    // 1. 无效参数 (注意：NULL allocator 的情况无法测试，因为现在是全局的)
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_restore_allocation(NULL, 10));
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_restore_allocation(mock_nvm_base, 0));

    // 2. 恢复一个大对象
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_restore_allocation(mock_nvm_base, MAX_BLOCK_SIZE + 1));

    // 3. 恢复一个与已存在Slab尺寸冲突的对象
    nvm_allocator_restore_allocation(mock_nvm_base, 16);
    void* conflict_ptr = (void*)((char*)mock_nvm_base + 32);
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_restore_allocation(conflict_ptr, 32));

    // 4. 恢复一个位于已被占用的空间中的对象
    void* occupied_ptr = (void*)((char*)mock_nvm_base + 64);
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_restore_allocation(occupied_ptr, 64));
}

// ============================================================================
//                          压力测试及辅助函数
// ============================================================================

typedef struct {
    uint64_t    slab_base_offset;
    SizeClassID sc_id;
    uint32_t    block_size;
    int         num_objects_to_restore;
} StressTestSlabInfo;

static void restore_single_slab_for_stress_test(const StressTestSlabInfo* info) {
    for (int i = 0; i < info->num_objects_to_restore; ++i) {
        uint64_t block_offset_in_slab = (uint64_t)i * (info->block_size + 7);
        uint64_t obj_offset = info->slab_base_offset + block_offset_in_slab;

        if (obj_offset + info->block_size > info->slab_base_offset + NVM_SLAB_SIZE) {
            continue;
        }
        
        void* obj_ptr = (void*)((char*)mock_nvm_base + obj_offset);
        
        int result = nvm_allocator_restore_allocation(obj_ptr, info->block_size);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, result, "Failed to restore object during stress test");
    }
}

static void verify_restored_slab(const StressTestSlabInfo* info) {
    NvmSlab* slab = slab_hashtable_lookup(global_nvm_allocator->slab_lookup_table, info->slab_base_offset);
    TEST_ASSERT_NOT_NULL(slab);
    TEST_ASSERT_EQUAL_UINT64(info->slab_base_offset, slab->nvm_base_offset);
    TEST_ASSERT_EQUAL_UINT8(info->sc_id, slab->size_type_id);
    
    uint32_t actual_blocks_set = 0;
    for (uint32_t i = 0; i < slab->total_block_count; ++i) {
        if (IS_BIT_SET(slab->bitmap, i)) {
            actual_blocks_set++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(actual_blocks_set, slab->allocated_block_count);
    TEST_ASSERT_GREATER_THAN_INT32(0, slab->allocated_block_count);
}

/**
 * @brief 压力测试：恢复多个不同尺寸的Slab和大量对象
 */
void test_restore_multiple_slabs_and_stress(void) {
    StressTestSlabInfo test_scenario[] = {
        { .slab_base_offset = 1 * NVM_SLAB_SIZE, .sc_id = SC_16B,  .block_size = 16,   .num_objects_to_restore = 2000 },
        { .slab_base_offset = 4 * NVM_SLAB_SIZE, .sc_id = SC_128B, .block_size = 128,  .num_objects_to_restore = 1000 },
        { .slab_base_offset = 8 * NVM_SLAB_SIZE, .sc_id = SC_4K,   .block_size = 4096, .num_objects_to_restore = 511 }
    };
    const int num_scenarios = sizeof(test_scenario) / sizeof(test_scenario[0]);

    for (int i = 0; i < num_scenarios; ++i) {
        restore_single_slab_for_stress_test(&test_scenario[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(num_scenarios, global_nvm_allocator->slab_lookup_table->count);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lists[SC_16B]);

    for (int i = 0; i < num_scenarios; ++i) {
        verify_restored_slab(&test_scenario[i]);
    }

    FreeSegmentNode* current = global_nvm_allocator->space_manager->head;

    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL_UINT64(0 * NVM_SLAB_SIZE, current->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(1 * NVM_SLAB_SIZE, current->size);
    current = current->next;
    
    // ...
}

// ============================================================================
//                          测试执行入口
// ============================================================================
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_restore_first_object_in_new_slab);
    RUN_TEST(test_restore_second_object_in_existing_slab);
    RUN_TEST(test_restore_object_at_head_of_space);
    RUN_TEST(test_restore_object_at_tail_of_space);
    RUN_TEST(test_restore_error_handling);
    RUN_TEST(test_restore_multiple_slabs_and_stress); 

    return UNITY_END();
}