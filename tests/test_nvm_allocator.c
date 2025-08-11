#include "unity.h"

// 包含所有必要的头文件
#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 包含所有组件的实现文件，以进行白盒测试并创建单个测试可执行文件。
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#include <stdlib.h>
#include <string.h>

#define MAX_BLOCK_SIZE 4096

// 为集成测试定义一个较大的模拟NVM空间
#define TOTAL_NVM_SIZE (10 * NVM_SLAB_SIZE)
#define NUM_SLABS 10

static void* mock_nvm_base = NULL;

// 通过extern声明来访问NvmAllocator.c中的全局静态实例以进行白盒测试。
extern struct NvmAllocator* global_nvm_allocator;

// MODIFIED: setUp 和 tearDown 现在负责调用您指定的 create/destroy。
void setUp(void) {
    // 1. 准备模拟NVM内存
    mock_nvm_base = malloc(TOTAL_NVM_SIZE);
    TEST_ASSERT_NOT_NULL(mock_nvm_base);
    memset(mock_nvm_base, 0, TOTAL_NVM_SIZE);
    
    // 2. 使用您指定的 nvm_allocator_create 初始化全局分配器
    int result = nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void tearDown(void) {
    // 1. 使用您指定的 nvm_allocator_destroy 销毁全局分配器
    nvm_allocator_destroy();
    
    // 2. 释放模拟NVM内存
    free(mock_nvm_base);
    mock_nvm_base = NULL;
}

// ============================================================================
//                          测试用例
// ============================================================================

/**
 * @brief 测试 NvmAllocator 的创建和销毁。
 */
void test_allocator_lifecycle(void) {
    // --- 子测试 1: 正常创建 ---
    // setUp已经完成了一次成功的创建，我们在这里验证其内部状态
    TEST_ASSERT_NOT_NULL(global_nvm_allocator);

    // 白盒检查:
    TEST_ASSERT_EQUAL_PTR(mock_nvm_base, global_nvm_allocator->nvm_base_addr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->space_manager);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lookup_table);
    TEST_ASSERT_EQUAL_UINT64(TOTAL_NVM_SIZE, global_nvm_allocator->space_manager->head->size);
    for (int i = 0; i < SC_COUNT; ++i) {
        TEST_ASSERT_NULL(global_nvm_allocator->slab_lists[i]);
    }

    // --- 子测试 2: 无效参数创建 ---
    nvm_allocator_destroy(); // 先销毁当前的
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(NULL, TOTAL_NVM_SIZE)); // 无效地址
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(mock_nvm_base, NVM_SLAB_SIZE - 1)); // 无效大小

    // --- 子测试 3: 销毁 (通过重复销毁来模拟) ---
    nvm_allocator_destroy(); // 第一次销毁（此时全局实例已为NULL）
    nvm_allocator_destroy(); // 第二次销毁，不应崩溃
}

/**
 * @brief 测试基本的 malloc/free 和内部状态变化。
 */
void test_basic_malloc_and_free(void) {
    // --- 子测试 1: 首次分配 (应创建新Slab) ---
    void* ptr = nvm_malloc(30);
    TEST_ASSERT_NOT_NULL(ptr);
    
    // 白盒检查:
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lists[SC_32B]);
    TEST_ASSERT_EQUAL_UINT32(1, global_nvm_allocator->slab_lookup_table->count);
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 1) * NVM_SLAB_SIZE, global_nvm_allocator->space_manager->head->size);

    // --- 子测试 2: 释放 ---
    nvm_free(ptr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lists[SC_32B]);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(global_nvm_allocator->slab_lists[SC_32B]));
}

/**
 * @brief 测试Slab的创建和复用逻辑。
 */
void test_slab_creation_and_reuse(void) {
    // 1. 分配一个 60 字节的对象
    void* ptr1 = nvm_malloc(60);
    TEST_ASSERT_NOT_NULL(ptr1);
    NvmSlab* slab64_ptr = global_nvm_allocator->slab_lists[SC_64B];
    TEST_ASSERT_NOT_NULL(slab64_ptr);

    // 2. 再次分配 60 字节，应复用同一个Slab
    void* ptr2 = nvm_malloc(60);
    TEST_ASSERT_NOT_NULL(ptr2);
    TEST_ASSERT_EQUAL_PTR(slab64_ptr, global_nvm_allocator->slab_lists[SC_64B]);

    // 3. 分配一个不同尺寸的对象 (8字节)，应创建新的Slab
    void* ptr3 = nvm_malloc(8);
    TEST_ASSERT_NOT_NULL(ptr3);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lists[SC_8B]);
    TEST_ASSERT_EQUAL_UINT32(2, global_nvm_allocator->slab_lookup_table->count);
}

/**
 * @brief 测试空Slab被回收的核心逻辑。
 */
void test_empty_slab_recycling(void) {
    size_t alloc_size = 128;
    SizeClassID sc_id = SC_128B;
    uint32_t blocks_per_slab = NVM_SLAB_SIZE / alloc_size;

    void** ptrs = malloc(sizeof(void*) * (blocks_per_slab + 1));
    TEST_ASSERT_NOT_NULL(ptrs);
    
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        ptrs[i] = nvm_malloc(alloc_size);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    
    ptrs[blocks_per_slab] = nvm_malloc(alloc_size);
    TEST_ASSERT_NOT_NULL(ptrs[blocks_per_slab]);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->slab_lists[sc_id]->next_in_chain);
    
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        nvm_free(ptrs[i]);
    }

    TEST_ASSERT_NULL(global_nvm_allocator->slab_lists[sc_id]->next_in_chain);
    TEST_ASSERT_EQUAL_UINT32(1, global_nvm_allocator->slab_lookup_table->count);
    
    nvm_free(ptrs[blocks_per_slab]);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(global_nvm_allocator->slab_lists[sc_id]));
    
    free(ptrs);
}

void test_parameter_and_error_handling(void) {
    TEST_ASSERT_NULL(nvm_malloc(0));
    TEST_ASSERT_NULL(nvm_malloc(MAX_BLOCK_SIZE + 1));
    nvm_free(NULL); // 不应崩溃
}

void test_nvm_space_exhaustion(void) {
    // MODIFIED: 为此测试重新创建分配器
    nvm_allocator_destroy();
    const size_t small_nvm_size = 2 * NVM_SLAB_SIZE;
    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_create(mock_nvm_base, small_nvm_size));

    for (int i = 0; i < NVM_SLAB_SIZE / 8; ++i) nvm_malloc(8);
    for (int i = 0; i < NVM_SLAB_SIZE / 16; ++i) nvm_malloc(16);

    TEST_ASSERT_NULL(global_nvm_allocator->space_manager->head);
    TEST_ASSERT_NULL(nvm_malloc(32));
}

void test_mixed_load_and_fragmentation(void) {
    const int num_allocs = 1000;
    void** ptrs = malloc(sizeof(void*) * num_allocs);
    TEST_ASSERT_NOT_NULL(ptrs);

    for (int i = 0; i < num_allocs; ++i) {
        ptrs[i] = nvm_malloc((i % 100) + 1);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    for (int i = 0; i < num_allocs; i += 2) {
        nvm_free(ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 0; i < num_allocs / 2; ++i) {
        TEST_ASSERT_NOT_NULL(nvm_malloc((i % 50) + 1));
    }
    
    free(ptrs);
}

// ============================================================================
//                          测试执行入口
// ============================================================================
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_allocator_lifecycle);
    RUN_TEST(test_basic_malloc_and_free);
    RUN_TEST(test_slab_creation_and_reuse);
    RUN_TEST(test_empty_slab_recycling);
    RUN_TEST(test_parameter_and_error_handling);
    RUN_TEST(test_nvm_space_exhaustion);
    RUN_TEST(test_mixed_load_and_fragmentation);

    return UNITY_END();
}