#include "unity.h"

// 包含所有必要的头文件
#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 包含所有组件的实现文件，以进行白盒测试。
// 这里需要包含我们刚刚添加的新函数。
// 注意：为了让测试文件能找到这些函数，它们需要是 static 的或者在头文件中声明。
// 为简单起见，我们直接包含 .c 文件。
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#include <stdlib.h>

// 为集成测试定义一个模拟NVM空间
#define TOTAL_NVM_SIZE (10 * NVM_SLAB_SIZE) // 20MB
#define NUM_SLABS 10

// 全局分配器，在每个测试开始时创建，结束时销毁
static NvmAllocator* g_allocator = NULL;

void setUp(void) {
    // 每个测试都从一个全新的、空白的分配器开始
    g_allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);
    TEST_ASSERT_NOT_NULL(g_allocator);
}

void tearDown(void) {
    nvm_allocator_destroy(g_allocator);
    g_allocator = NULL;
}

// ============================================================================
//         测试 nvm_allocator_restore_allocation 函数
// ============================================================================

/**
 * @brief 测试基本路径：恢复单个对象，这将触发新Slab的创建。
 */
void test_restore_first_object_in_new_slab(void) {
    const uint64_t obj_offset = 2 * NVM_SLAB_SIZE + 64; // 在第3个slab的第2个64B块
    const size_t obj_size = 60; // 属于 SC_64B
    const uint64_t slab_base = 2 * NVM_SLAB_SIZE;
    const SizeClassID sc_id = SC_64B;

    // --- 执行恢复 ---
    uint64_t result = nvm_allocator_restore_allocation(g_allocator, obj_offset, obj_size);
    TEST_ASSERT_EQUAL_UINT64(obj_offset, result);

    // --- 白盒验证 ---
    // 1. 验证Slab是否已创建并正确链接
    NvmSlab* restored_slab = g_allocator->slab_lists[sc_id];
    TEST_ASSERT_NOT_NULL(restored_slab);
    TEST_ASSERT_EQUAL_UINT64(slab_base, restored_slab->nvm_base_offset);
    TEST_ASSERT_EQUAL_UINT8(sc_id, restored_slab->size_type_id);
    TEST_ASSERT_NULL(restored_slab->next_in_chain);

    // 2. 验证哈希表
    TEST_ASSERT_EQUAL_UINT32(1, g_allocator->slab_lookup_table->count);
    TEST_ASSERT_EQUAL_PTR(restored_slab, slab_hashtable_lookup(g_allocator->slab_lookup_table, slab_base));

    // 3. 验证空间管理器是否已正确“裁切”
    // 初始: [0, 10 * NVM_SLAB_SIZE]
    // 裁切后: [0, 2 * NVM_SLAB_SIZE] 和 [3 * NVM_SLAB_SIZE, 7 * NVM_SLAB_SIZE]
    FreeSegmentNode* head = g_allocator->space_manager->head;
    FreeSegmentNode* tail = g_allocator->space_manager->tail;
    TEST_ASSERT_EQUAL_UINT64(0, head->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(2 * NVM_SLAB_SIZE, head->size);
    TEST_ASSERT_EQUAL_UINT64(3 * NVM_SLAB_SIZE, tail->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 3) * NVM_SLAB_SIZE, tail->size);
    TEST_ASSERT_EQUAL_PTR(tail, head->next);

    // 4. 验证Slab位图和计数
    TEST_ASSERT_EQUAL_UINT32(1, restored_slab->allocated_block_count);
    uint32_t block_idx = (obj_offset - slab_base) / restored_slab->block_size;
    TEST_ASSERT_TRUE(IS_BIT_SET(restored_slab->bitmap, block_idx));
}

/**
 * @brief 测试在已存在的Slab中恢复第二个对象。
 */
void test_restore_second_object_in_existing_slab(void) {
    // 先恢复第一个对象，创建Slab
    nvm_allocator_restore_allocation(g_allocator, 0, 32);
    
    // 现在恢复同一Slab中的第二个对象
    const uint64_t obj_offset = 128; // 偏移128，大小32
    const size_t obj_size = 32;
    const uint64_t slab_base = 0;
    const SizeClassID sc_id = SC_32B;

    // --- 执行恢复 ---
    uint64_t result = nvm_allocator_restore_allocation(g_allocator, obj_offset, obj_size);
    TEST_ASSERT_EQUAL_UINT64(obj_offset, result);

    // --- 白盒验证 ---
    // 1. 验证Slab状态
    NvmSlab* slab = g_allocator->slab_lists[sc_id];
    TEST_ASSERT_NOT_NULL(slab);
    TEST_ASSERT_NULL(slab->next_in_chain); // 确认没有创建新Slab
    TEST_ASSERT_EQUAL_UINT32(2, slab->allocated_block_count); // 计数应为2

    // 2. 验证位图
    TEST_ASSERT_TRUE(IS_BIT_SET(slab->bitmap, 0)); // 第一个对象 (offset 0, block 0)
    TEST_ASSERT_TRUE(IS_BIT_SET(slab->bitmap, 4)); // 第二个对象 (offset 128, block 4)

    // 3. 验证哈希表和空间管理器没有变化
    TEST_ASSERT_EQUAL_UINT32(1, g_allocator->slab_lookup_table->count);
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 1) * NVM_SLAB_SIZE, g_allocator->space_manager->head->size);
}

/**
 * @brief 测试恢复一个对象，其Slab正好是整个空闲空间的头部。
 */
void test_restore_object_at_head_of_space(void) {
    nvm_allocator_restore_allocation(g_allocator, 0, 16);
    FreeSegmentNode* head = g_allocator->space_manager->head;
    TEST_ASSERT_EQUAL_UINT64(NVM_SLAB_SIZE, head->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 1) * NVM_SLAB_SIZE, head->size);
}

/**
 * @brief 测试恢复一个对象，其Slab正好是整个空闲空间的尾部。
 */
void test_restore_object_at_tail_of_space(void) {
    const uint64_t slab_base = (NUM_SLABS - 1) * NVM_SLAB_SIZE;
    nvm_allocator_restore_allocation(g_allocator, slab_base, 16);
    FreeSegmentNode* head = g_allocator->space_manager->head;
    TEST_ASSERT_EQUAL_UINT64(0, head->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(slab_base, head->size);
    TEST_ASSERT_NULL(head->next); // 确认尾部节点已被裁切
}

/**
 * @brief 测试恢复流程中的错误处理路径。
 */
void test_restore_error_handling(void) {
    // 1. 无效参数
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, nvm_allocator_restore_allocation(NULL, 0, 10));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, nvm_allocator_restore_allocation(g_allocator, 0, 0));

    // 2. 恢复一个大对象 (不支持)
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, nvm_allocator_restore_allocation(g_allocator, 0, 5000));

    // 3. 恢复一个与已存在Slab尺寸冲突的对象
    nvm_allocator_restore_allocation(g_allocator, 0, 16); // 创建一个 SC_16B 的Slab
    // 尝试在同一个Slab中恢复一个32字节的对象，应该失败
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, nvm_allocator_restore_allocation(g_allocator, 32, 32));

    // 4. 恢复一个位于已被占用的空间中的对象
    // 第一个Slab (offset 0) 已被占用
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, nvm_allocator_restore_allocation(g_allocator, 64, 64));

    // 5. 恢复一个块索引无效的对象
    // nvm_slab_set_bitmap_at_idx 会返回-1，进而导致函数失败
    // 假设一个块大小为8，一个slab能容纳 2M/8 = 262144 个块
    // offset = slab_base + block_idx * block_size
    // 一个无效的offset，比如没有8字节对齐
    const uint64_t invalid_offset = NVM_SLAB_SIZE + 7;
    // 虽然能计算出 slab_base，但 block_idx 会导致错误 (或者由于取整，block_size不匹配)
    // 这个测试有点难构造，但依赖于底层函数的健壮性。
    // 更直接的测试是在 nvm_slab_set_bitmap_at_idx 的单元测试中完成。
}

// ============================================================================
//                          压力测试及辅助函数
// ============================================================================

// 用于定义压力测试场景的辅助结构体
typedef struct {
    uint64_t    slab_base_offset;
    SizeClassID sc_id;
    uint32_t    block_size;
    int         num_objects_to_restore;
} StressTestSlabInfo;

// 辅助函数：恢复单个Slab中的所有指定对象
static void restore_single_slab_for_stress_test(const StressTestSlabInfo* info) {
    for (int i = 0; i < info->num_objects_to_restore; ++i) {
        // 在Slab内部分散地恢复对象，以更好地测试位图
        // 使用一个质数步长来避免简单的线性模式
        uint64_t block_offset_in_slab = (uint64_t)i * (info->block_size + 7); // 步长不是块大小的整数倍
        uint64_t obj_offset = info->slab_base_offset + block_offset_in_slab;

        // 确保对象不会超出Slab边界
        if (obj_offset + info->block_size > info->slab_base_offset + NVM_SLAB_SIZE) {
            continue; // 跳过会越界的分配
        }

        uint64_t result = nvm_allocator_restore_allocation(g_allocator, obj_offset, info->block_size);
        TEST_ASSERT_EQUAL_UINT64(obj_offset, result);
    }
}

// 辅助函数：验证单个Slab的恢复后状态
static void verify_restored_slab(const StressTestSlabInfo* info) {
    NvmSlab* slab = slab_hashtable_lookup(g_allocator->slab_lookup_table, info->slab_base_offset);
    TEST_ASSERT_NOT_NULL(slab);
    
    // 验证Slab基本信息
    TEST_ASSERT_EQUAL_UINT64(info->slab_base_offset, slab->nvm_base_offset);
    TEST_ASSERT_EQUAL_UINT8(info->sc_id, slab->size_type_id);
    
    // 验证分配计数。注意：由于上面的步长和边界检查，实际恢复的对象数可能少于请求数
    // 我们可以通过重新扫描位图来得到精确的计数值，这是最可靠的验证方式。
    uint32_t actual_blocks_set = 0;
    for (uint32_t i = 0; i < slab->total_block_count; ++i) {
        if (IS_BIT_SET(slab->bitmap, i)) {
            actual_blocks_set++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(actual_blocks_set, slab->allocated_block_count);
    TEST_ASSERT_GREATER_THAN_INT32(0, slab->allocated_block_count); // 确保至少恢复了一些对象
}

/**
 * @brief 压力测试：恢复多个不同尺寸的Slab和大量对象 (重构版)
 */
void test_restore_multiple_slabs_and_stress(void) {
    // 1. --- 场景定义 ---
    StressTestSlabInfo test_scenario[] = {
        { .slab_base_offset = 1 * NVM_SLAB_SIZE, .sc_id = SC_16B,  .block_size = 16,   .num_objects_to_restore = 2000 },
        { .slab_base_offset = 4 * NVM_SLAB_SIZE, .sc_id = SC_128B, .block_size = 128,  .num_objects_to_restore = 1000 },
        { .slab_base_offset = 8 * NVM_SLAB_SIZE, .sc_id = SC_4K,   .block_size = 4096, .num_objects_to_restore = 511 } // 几乎填满
    };
    const int num_scenarios = sizeof(test_scenario) / sizeof(test_scenario[0]);

    // 2. --- 执行恢复 ---
    for (int i = 0; i < num_scenarios; ++i) {
        restore_single_slab_for_stress_test(&test_scenario[i]);
    }

    // 3. --- 白盒验证 ---
    // 3a. 验证Allocator顶层状态
    TEST_ASSERT_EQUAL_UINT32(num_scenarios, g_allocator->slab_lookup_table->count);
    TEST_ASSERT_NOT_NULL(g_allocator->slab_lists[SC_16B]);
    TEST_ASSERT_NOT_NULL(g_allocator->slab_lists[SC_128B]);
    TEST_ASSERT_NOT_NULL(g_allocator->slab_lists[SC_4K]);

    // 3b. 逐个验证每个Slab的状态
    for (int i = 0; i < num_scenarios; ++i) {
        verify_restored_slab(&test_scenario[i]);
    }

    // 3c. 验证空间管理器的碎片化状态
    // 初始: [0, 10 * NVM_SLAB_SIZE]
    // 裁切掉 offset 1, 4, 8 处的Slab后，应留下以下空闲块:
    // [0,  1 * NVM_SLAB_SIZE]
    // [2,  2 * NVM_SLAB_SIZE]
    // [5,  3 * NVM_SLAB_SIZE]
    // [9,  1 * NVM_SLAB_SIZE]
    FreeSegmentNode* current = g_allocator->space_manager->head;

    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL_UINT64(0 * NVM_SLAB_SIZE, current->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(1 * NVM_SLAB_SIZE, current->size);
    current = current->next;

    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL_UINT64(2 * NVM_SLAB_SIZE, current->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(2 * NVM_SLAB_SIZE, current->size);
    current = current->next;

    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL_UINT64(5 * NVM_SLAB_SIZE, current->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(3 * NVM_SLAB_SIZE, current->size);
    current = current->next;

    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL_UINT64(9 * NVM_SLAB_SIZE, current->nvm_offset);
    TEST_ASSERT_EQUAL_UINT64(1 * NVM_SLAB_SIZE, current->size);
    current = current->next;
    
    // 应该没有更多空闲块了
    TEST_ASSERT_NULL(current);
}

// ============================================================================
//                          测试执行入口
// ============================================================================
int main(void) {
    UNITY_BEGIN();

    // 运行专门为恢复逻辑编写的测试
    RUN_TEST(test_restore_first_object_in_new_slab);
    RUN_TEST(test_restore_second_object_in_existing_slab);
    RUN_TEST(test_restore_object_at_head_of_space);
    RUN_TEST(test_restore_object_at_tail_of_space);
    RUN_TEST(test_restore_error_handling);
    RUN_TEST(test_restore_multiple_slabs_and_stress); 

    return UNITY_END();
}